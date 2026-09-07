/* GIP security handshake, host side. Port of xone auth/auth.c + auth/crypto.c (GPL-2.0-or-later, (C) 2023 Severin von Wnuck-Lipinski)
 * to userspace using Apple CommonCrypto + Security.framework (no third-party crypto dependency). */
#include "gipauth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>

#define TRAILER_LEN 8
#define RANDOM_LEN 32
#define CERT_MAX_LEN 1024
#define PUBKEY_LEN 270
#define SECRET_LEN 48
#define ENC_PMS_LEN 256
#define TRANSCRIPT_LEN 32
#define SESSION_KEY_LEN 16
#define AUTH2_PUBKEY_LEN 64
#define AUTH2_SECRET_LEN 32

enum { CTX_HANDSHAKE = 0x00, CTX_CONTROL = 0x01 };
enum { CMD_HOST_HELLO = 0x01, CMD_CLIENT_HELLO = 0x02, CMD_CLIENT_CERT = 0x03, CMD_HOST_SECRET = 0x05, CMD_HOST_FINISH = 0x07, CMD_CLIENT_FINISH = 0x08,
       CMD2_HOST_HELLO = 0x21, CMD2_CLIENT_HELLO = 0x22, CMD2_CLIENT_CERT = 0x23, CMD2_CLIENT_PUBKEY = 0x24, CMD2_HOST_PUBKEY = 0x25, CMD2_HOST_FINISH = 0x26, CMD2_CLIENT_FINISH = 0x27 };
enum { CTRL_COMPLETE = 0x00, CTRL_RESET = 0x01 };
enum { OPT_ACKNOWLEDGE = 1, OPT_REQUEST = 2, OPT_FROM_HOST = 0x40, OPT_FROM_CLIENT = 0xC0 };

#pragma pack(push, 1)
typedef struct { uint8_t context, options, error, command; uint8_t length_be[2]; } hdr_hs_t;   /* 6 */
typedef struct { uint8_t command, version; uint8_t length_be[2]; } hdr_data_t;                  /* 4 */
typedef struct { hdr_hs_t hs; hdr_data_t data; } hdr_full_t;                                     /* 10 */
#pragma pack(pop)

struct gip_auth {
    gip_auth_send_fn send; void *user;
    CC_SHA256_CTX transcript;
    uint8_t last_sent;
    uint8_t random_host[RANDOM_LEN], random_client[RANDOM_LEN];
    uint8_t pubkey_client[PUBKEY_LEN];
    uint8_t pubkey_client2[AUTH2_PUBKEY_LEN];
    uint8_t master_secret[SECRET_LEN];
    int complete, failed, v2;
};

static void be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static uint16_t rd16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

static int transcript_get(gip_auth_t *a, uint8_t out[32]) {
    CC_SHA256_CTX c = a->transcript;   /* copy so the running hash continues */
    CC_SHA256_Final(out, &c); return 0;
}
static void transcript_update(gip_auth_t *a, const void *d, size_t n) { CC_SHA256_Update(&a->transcript, d, (CC_LONG)n); }
static void transcript_reset(gip_auth_t *a) { CC_SHA256_Init(&a->transcript); }
static void hmac256(const uint8_t *key, int key_len, const uint8_t *d1, int l1, const uint8_t *d2, int l2, uint8_t out[32]) {
    CCHmacContext c; CCHmacInit(&c, kCCHmacAlgSHA256, key, key_len);
    if (l1) CCHmacUpdate(&c, d1, l1); if (l2) CCHmacUpdate(&c, d2, l2); CCHmacFinal(&c, out);
}
static void random_bytes(uint8_t *b, int n) { arc4random_buf(b, n); }

/* TLS-1.2-style P_SHA256: A(1)=HMAC(label||seed); out_i = HMAC(A(i)||label||seed); A(i+1)=HMAC(A(i)) */
static int compute_prf(const char *label, const uint8_t *key, int key_len, const uint8_t *seed, int seed_len, uint8_t *out, int out_len) {
    uint8_t A[32], blk[32];
    int ll = (int)strlen(label);
    uint8_t *ls = malloc(ll + seed_len); memcpy(ls, label, ll); memcpy(ls + ll, seed, seed_len);
    int ls_len = ll + seed_len;
    hmac256(key, key_len, ls, ls_len, NULL, 0, A);
    while (out_len > 0) {
        hmac256(key, key_len, A, 32, ls, ls_len, blk);
        int c = out_len < 32 ? out_len : 32; memcpy(out, blk, c); out += c; out_len -= c;
        hmac256(key, key_len, A, 32, NULL, 0, A);
    }
    free(ls); return 0;
}

static int send_pkt(gip_auth_t *a, uint8_t cmd, uint8_t *pkt, int len) {
    hdr_full_t *h = (hdr_full_t *)pkt;
    uint16_t data_len = len - sizeof(hdr_hs_t) - TRAILER_LEN;
    h->hs.context = CTX_HANDSHAKE; h->hs.options = OPT_ACKNOWLEDGE | OPT_FROM_HOST; h->hs.error = 0; h->hs.command = cmd; be16(h->hs.length_be, data_len);
    h->data.command = cmd; h->data.version = cmd >= CMD2_HOST_HELLO ? 2 : 1; be16(h->data.length_be, data_len - sizeof(hdr_data_t));
    a->last_sent = cmd;
    transcript_update(a, pkt + sizeof(hdr_hs_t), data_len);
    return a->send(a->user, pkt, len, 1);
}

static int request_pkt(gip_auth_t *a, uint8_t cmd, uint16_t len) {
    uint8_t req[sizeof(hdr_hs_t) + TRAILER_LEN] = {0}; hdr_hs_t *h = (hdr_hs_t *)req;
    h->context = CTX_HANDSHAKE; h->options = OPT_REQUEST | OPT_FROM_HOST; h->command = cmd; be16(h->length_be, len + sizeof(hdr_data_t));
    return a->send(a->user, req, sizeof req, 1);
}

static int send_hello(gip_auth_t *a) {
    uint8_t pkt[sizeof(hdr_full_t) + RANDOM_LEN + 4 + 4 + TRAILER_LEN] = {0};
    random_bytes(a->random_host, RANDOM_LEN); memcpy(pkt + sizeof(hdr_full_t), a->random_host, RANDOM_LEN);
    return send_pkt(a, CMD_HOST_HELLO, pkt, sizeof pkt);
}
static int send_hello2(gip_auth_t *a) {
    uint8_t pkt[sizeof(hdr_full_t) + RANDOM_LEN + 4 + TRAILER_LEN] = {0};
    random_bytes(a->random_host, RANDOM_LEN); memcpy(pkt + sizeof(hdr_full_t), a->random_host, RANDOM_LEN);
    return send_pkt(a, CMD2_HOST_HELLO, pkt, sizeof pkt);
}

static int send_finish(gip_auth_t *a, uint8_t cmd) {
    uint8_t pkt[sizeof(hdr_full_t) + TRANSCRIPT_LEN + TRAILER_LEN] = {0}, tr[32];
    if (transcript_get(a, tr)) return -1;
    if (compute_prf("Host Finished", a->master_secret, SECRET_LEN, tr, 32, pkt + sizeof(hdr_full_t), TRANSCRIPT_LEN)) return -1;
    return send_pkt(a, cmd, pkt, sizeof pkt);
}

static int exchange_rsa(gip_auth_t *a) {
    uint8_t randoms[RANDOM_LEN * 2], pms[SECRET_LEN];
    uint8_t pkt[sizeof(hdr_full_t) + ENC_PMS_LEN + TRAILER_LEN] = {0};
    memcpy(randoms, a->random_host, RANDOM_LEN); memcpy(randoms + RANDOM_LEN, a->random_client, RANDOM_LEN);
    random_bytes(pms, SECRET_LEN);
    CFErrorRef err = NULL;
    CFDataRef der = CFDataCreate(NULL, a->pubkey_client, PUBKEY_LEN);
    const void *ak[] = { kSecAttrKeyType, kSecAttrKeyClass }; const void *av[] = { kSecAttrKeyTypeRSA, kSecAttrKeyClassPublic };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SecKeyRef pk = SecKeyCreateWithData(der, attrs, &err);
    CFRelease(der); CFRelease(attrs);
    if (!pk) { fprintf(stderr, "auth: cannot parse RSA public key\n"); if (err) CFRelease(err); return -1; }
    CFDataRef plain = CFDataCreate(NULL, pms, SECRET_LEN);
    CFDataRef enc = SecKeyCreateEncryptedData(pk, kSecKeyAlgorithmRSAEncryptionPKCS1, plain, &err);
    CFRelease(plain); CFRelease(pk);
    if (!enc || CFDataGetLength(enc) != ENC_PMS_LEN) { fprintf(stderr, "auth: RSA encrypt failed\n"); if (enc) CFRelease(enc); if (err) CFRelease(err); return -1; }
    memcpy(pkt + sizeof(hdr_full_t), CFDataGetBytePtr(enc), ENC_PMS_LEN); CFRelease(enc);
    if (compute_prf("Master Secret", pms, SECRET_LEN, randoms, sizeof randoms, a->master_secret, SECRET_LEN)) return -1;
    return send_pkt(a, CMD_HOST_SECRET, pkt, sizeof pkt);
}

static int exchange_ecdh(gip_auth_t *a) {
    uint8_t randoms[RANDOM_LEN * 2], secret[32], secret_hash[32];
    uint8_t pkt[sizeof(hdr_full_t) + AUTH2_PUBKEY_LEN + TRAILER_LEN] = {0};
    memcpy(randoms, a->random_host, RANDOM_LEN); memcpy(randoms + RANDOM_LEN, a->random_client, RANDOM_LEN);
    CFErrorRef err = NULL;
    int bits = 256; CFNumberRef nbits = CFNumberCreate(NULL, kCFNumberIntType, &bits);
    const void *gk[] = { kSecAttrKeyType, kSecAttrKeySizeInBits }; const void *gv[] = { kSecAttrKeyTypeECSECPrimeRandom, nbits };
    CFDictionaryRef gattrs = CFDictionaryCreate(NULL, gk, gv, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SecKeyRef priv = SecKeyCreateRandomKey(gattrs, &err); CFRelease(gattrs);
    if (!priv) { fprintf(stderr, "auth: EC keygen failed\n"); return -1; }
    SecKeyRef pub = SecKeyCopyPublicKey(priv);
    CFDataRef pubdata = SecKeyCopyExternalRepresentation(pub, &err);
    if (!pubdata || CFDataGetLength(pubdata) != 65 || CFDataGetBytePtr(pubdata)[0] != 0x04) { fprintf(stderr, "auth: unexpected own EC pubkey encoding\n"); return -1; }
    memcpy(pkt + sizeof(hdr_full_t), CFDataGetBytePtr(pubdata) + 1, 64); CFRelease(pubdata); CFRelease(pub);
    uint8_t peer_oct[65]; peer_oct[0] = 0x04; memcpy(peer_oct + 1, a->pubkey_client2, 64);
    CFDataRef peerdata = CFDataCreate(NULL, peer_oct, 65);
    const void *pk_[] = { kSecAttrKeyType, kSecAttrKeyClass, kSecAttrKeySizeInBits }; const void *pv_[] = { kSecAttrKeyTypeECSECPrimeRandom, kSecAttrKeyClassPublic, nbits };
    CFDictionaryRef pattrs = CFDictionaryCreate(NULL, pk_, pv_, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SecKeyRef peer = SecKeyCreateWithData(peerdata, pattrs, &err); CFRelease(peerdata); CFRelease(pattrs); CFRelease(nbits);
    if (!peer) { fprintf(stderr, "auth: bad peer EC pubkey\n"); return -1; }
    CFDictionaryRef params = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef shared = SecKeyCopyKeyExchangeResult(priv, kSecKeyAlgorithmECDHKeyExchangeStandard, peer, params, &err);
    CFRelease(params); CFRelease(peer); CFRelease(priv);
    if (!shared || CFDataGetLength(shared) != 32) { fprintf(stderr, "auth: ECDH derive failed\n"); if (shared) CFRelease(shared); return -1; }
    memcpy(secret, CFDataGetBytePtr(shared), 32); CFRelease(shared);
    CC_SHA256(secret, 32, secret_hash);
    if (compute_prf("Master Secret", secret_hash, 32, randoms, sizeof randoms, a->master_secret, SECRET_LEN)) return -1;
    return send_pkt(a, CMD2_HOST_PUBKEY, pkt, sizeof pkt);
}

static int send_complete(gip_auth_t *a) {
    uint8_t ctrl[2] = { CTX_CONTROL, CTRL_COMPLETE };
    a->complete = 1;
    uint8_t randoms[RANDOM_LEN * 2], key[SESSION_KEY_LEN];
    memcpy(randoms, a->random_host, RANDOM_LEN); memcpy(randoms + RANDOM_LEN, a->random_client, RANDOM_LEN);
    compute_prf("EXPORTER DAWN data channel session key for controller", a->master_secret, SECRET_LEN, randoms, sizeof randoms, key, SESSION_KEY_LEN);
    fprintf(stderr, "auth: handshake complete (session key derived)\n");
    return a->send(a->user, ctrl, sizeof ctrl, 0);
}

static int handle_ack(gip_auth_t *a) {
    switch (a->last_sent) {
    case CMD2_HOST_HELLO:  return request_pkt(a, CMD2_CLIENT_HELLO, RANDOM_LEN + 108 + 32);
    case CMD2_HOST_PUBKEY: return send_finish(a, CMD2_HOST_FINISH);
    case CMD2_HOST_FINISH: return request_pkt(a, CMD2_CLIENT_FINISH, TRANSCRIPT_LEN + 32);
    case CMD_HOST_HELLO:   return request_pkt(a, CMD_CLIENT_HELLO, RANDOM_LEN + 48);
    case CMD_HOST_SECRET:  return send_finish(a, CMD_HOST_FINISH);
    case CMD_HOST_FINISH:  return request_pkt(a, CMD_CLIENT_FINISH, TRANSCRIPT_LEN + 32);
    default: fprintf(stderr, "auth: unexpected ack for 0x%02x\n", a->last_sent); return -1;
    }
}

static int handle_finish(gip_auth_t *a, const uint8_t *d, int len) {
    uint8_t tr[32], fin[32];
    if (len < TRANSCRIPT_LEN + 32) return -1;
    if (transcript_get(a, tr)) return -1;
    if (compute_prf("Device Finished", a->master_secret, SECRET_LEN, tr, 32, fin, 32)) return -1;
    if (memcmp(d, fin, 32)) { fprintf(stderr, "auth: transcript mismatch (device finished)\n"); a->failed = 1; return -1; }
    return send_complete(a);
}

static int dispatch(gip_auth_t *a, uint8_t cmd, const uint8_t *d, int len) {
    switch (cmd) {
    case CMD2_CLIENT_HELLO:
        if (len < RANDOM_LEN + 108 + 32) return -1;
        memcpy(a->random_client, d, RANDOM_LEN);
        return request_pkt(a, CMD2_CLIENT_CERT, 4 + 136 + 32 + 20 + 576);
    case CMD2_CLIENT_CERT:
        if (len >= 4 + 136 + 32 + 20) fprintf(stderr, "auth2: cert header=%.4s chip=%.32s rev=%.20s\n", (const char *)d, (const char *)d + 140, (const char *)d + 172);
        return request_pkt(a, CMD2_CLIENT_PUBKEY, AUTH2_PUBKEY_LEN + 64);
    case CMD2_CLIENT_PUBKEY:
        if (len < AUTH2_PUBKEY_LEN + 64) return -1;
        memcpy(a->pubkey_client2, d, AUTH2_PUBKEY_LEN);
        return exchange_ecdh(a);
    case CMD2_CLIENT_FINISH: return handle_finish(a, d, len);
    case CMD_CLIENT_HELLO:
        if (len < RANDOM_LEN + 48) return -1;
        memcpy(a->random_client, d, RANDOM_LEN);
        return request_pkt(a, CMD_CLIENT_CERT, CERT_MAX_LEN);
    case CMD_CLIENT_CERT: {
        static const uint8_t seq[] = { 0x30, 0x82, 0x01, 0x0a };
        if (len > CERT_MAX_LEN) return -1;
        for (int i = 0; i + 4 <= len; i++) {
            if (memcmp(d + i, seq, 4)) continue;
            if (i + PUBKEY_LEN > len) return -1;
            memcpy(a->pubkey_client, d + i, PUBKEY_LEN);
            return exchange_rsa(a);
        }
        fprintf(stderr, "auth: RSA pubkey not found in certificate\n"); return -1; }
    case CMD_CLIENT_FINISH: return handle_finish(a, d, len);
    default: fprintf(stderr, "auth: unknown command 0x%02x\n", cmd); return -1;
    }
}

int gip_auth_process(gip_auth_t *a, const uint8_t *data, int len) {
    if (len < (int)sizeof(hdr_hs_t)) return -1;
    const hdr_hs_t *hs = (const hdr_hs_t *)data;
    if (hs->error) { fprintf(stderr, "auth: device reported error 0x%02x (cmd 0x%02x)\n", hs->error, hs->command); a->failed = 1; return -1; }
    if (hs->options & OPT_ACKNOWLEDGE) {
        if (hs->command == 0x01) return handle_ack(a);
        fprintf(stderr, "auth: handshake failed: 0x%02x\n", hs->command); a->failed = 1; return -1;
    }
    if (len < (int)sizeof(hdr_full_t)) return -1;
    const hdr_full_t *full = (const hdr_full_t *)data;
    if (full->hs.command != full->data.command) {
        fprintf(stderr, "auth: protocol upgrade to v2 (hs cmd 0x%02x, data cmd 0x%02x)\n", full->hs.command, full->data.command);
        transcript_reset(a); a->v2 = 1;
        return send_hello2(a);
    }
    /* xone hashes a received message after dispatch, but dispatch defers sending (workqueue) for the
     * cert/pubkey steps, so the received message lands in the transcript before our reply does. The
     * client-finish check, however, must use the transcript *without* the finish message itself. */
    uint8_t cmd = full->data.command;
    int is_finish = (cmd == CMD_CLIENT_FINISH || cmd == CMD2_CLIENT_FINISH);
    if (!is_finish) transcript_update(a, data + sizeof(hdr_hs_t), len - sizeof(hdr_hs_t));
    int r = dispatch(a, cmd, data + sizeof(hdr_full_t), len - sizeof(hdr_full_t));
    if (r) return r;
    if (is_finish) transcript_update(a, data + sizeof(hdr_hs_t), len - sizeof(hdr_hs_t));
    (void)rd16;
    return 0;
}

gip_auth_t *gip_auth_new(gip_auth_send_fn send, void *user) {
    gip_auth_t *a = calloc(1, sizeof *a); a->send = send; a->user = user;
    transcript_reset(a);
    return a;
}
void gip_auth_free(gip_auth_t *a) { if (!a) return; free(a); }
int gip_auth_start(gip_auth_t *a) { return send_hello(a); }
int gip_auth_complete(const gip_auth_t *a) { return a->complete; }
const char *gip_auth_state_name(const gip_auth_t *a) { return a->complete ? "complete" : a->failed ? "failed" : "in progress"; }
