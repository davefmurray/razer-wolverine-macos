/*
 * giptool - userspace Microsoft GIP (Xbox One protocol) reader for macOS via libusb.
 * Target: Razer Wolverine V3 Pro (1532:0a3f, Xbox mode), works for any class FF/47/D0 interface.
 *
 * References: MS-GIPUSB (Microsoft open spec, 2024), Linux xpad.c, SDL SDL_hidapi_xboxone.c, medusalix/xone.
 *
 * Host sequence implemented (spec 1.3.1): Hello <- device; Metadata Request -> ; fragmented Metadata
 * Response <- (ACKed); Set Device State: Start -> ; LED -> ; (auth-done ->) ; rumble init -> ; then
 * Gamepad Input Reports (0x20) <- and Guide button (0x07) <-, ACKing anything flagged ACME.
 *
 * Usage: giptool [--raw] [--json] [--seconds N] [--init meta,power,led,auth,rumble] [--vid 0x1532] [--pid 0x0a3f]
 */
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include "gipauth.h"

#define GIP_CMD_ACK        0x01
#define GIP_CMD_HELLO      0x02
#define GIP_CMD_STATUS     0x03
#define GIP_CMD_METADATA   0x04
#define GIP_CMD_STATE      0x05
#define GIP_CMD_AUTH       0x06
#define GIP_CMD_GUIDE      0x07
#define GIP_CMD_RUMBLE     0x09
#define GIP_CMD_LED        0x0A
#define GIP_CMD_INPUT      0x20

#define GIP_OPT_ACK        0x10   /* ACME: acknowledge me */
#define GIP_OPT_INTERNAL   0x20   /* system message */
#define GIP_OPT_CHUNK_START 0x40
#define GIP_OPT_CHUNK      0x80

static volatile sig_atomic_t stop = 0;
static void on_sigint(int s) { (void)s; stop = 1; }

static int opt_raw = 0, opt_json = 0;
static double opt_seconds = 0;
static const char *opt_init = "meta,power,led,auth,rumble";
static int opt_restart = 0, opt_serial = 0, opt_query = 0; static double t_started = 0; static int post_done = 0;
static int vid = 0x1532, pid = 0x0a3f;
static uint8_t ep_in = 0, ep_out = 0;
static uint8_t seq = 1;

typedef struct {
    int16_t lx, ly, rx, ry;
    uint16_t lt, rt;
    uint16_t buttons;   /* raw bytes 4..5 (little endian), keep-alive bit cleared */
    int guide;
} gip_state_t;

typedef struct {
    uint8_t cmd, opts, seq;
    uint32_t len;          /* payload length of this packet/fragment */
    uint32_t chunk_offset; /* fragment offset; for the first fragment = total message length */
    int hdr_len;
} gip_hdr_t;

/* metadata reassembly */
static uint8_t *meta_buf = NULL; static uint32_t meta_total = 0, meta_received = 0; static int meta_done = 0;
/* generic chunked receive (one message at a time) */
static uint8_t rx_cmd = 0; static uint8_t *rx_buf = NULL; static uint32_t rx_total = 0, rx_received = 0;
/* chunked send state (xone gip_send_pkt semantics) */
#define GIP_PKT_MAX 58
static struct { int active; uint8_t cmd, opts, seq; uint8_t *data; uint32_t total; } tx;
static gip_auth_t *auth = NULL;
static libusb_device_handle *g_h = NULL;

enum { ST_WAIT_HELLO, ST_META, ST_RUNNING } state = ST_WAIT_HELLO;

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static void hexdump(const char *tag, const uint8_t *b, int n) {
    printf("%s (%d):", tag, n);
    for (int i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n"); fflush(stdout);
}

static int decode_varint(const uint8_t *d, int len, uint32_t *val) {
    int i; *val = 0;
    for (i = 0; i < 4 && i < len; i++) { *val |= (uint32_t)(d[i] & 0x7f) << (7 * i); if (!(d[i] & 0x80)) break; }
    return i + 1;
}

static int decode_header(gip_hdr_t *h, const uint8_t *d, int len) {
    if (len < 4) return -1;
    h->cmd = d[0]; h->opts = d[1]; h->seq = d[2];
    int p = 3;
    p += decode_varint(d + p, len - p, &h->len);
    h->chunk_offset = 0;
    if (h->opts & GIP_OPT_CHUNK) p += decode_varint(d + p, len - p, &h->chunk_offset);
    h->hdr_len = p;
    return p;
}

static int gip_send(libusb_device_handle *h, uint8_t *pkt, int len, const char *what) {
    int xfer = 0;
    int r = libusb_interrupt_transfer(h, ep_out, pkt, len, &xfer, 250);
    if (opt_raw || r) { printf("TX %-14s r=%d%s", what, r, r ? libusb_error_name(r) : ""); hexdump("", pkt, len); }
    return r;
}

static int gip_send_cmd(libusb_device_handle *h, uint8_t cmd, uint8_t opts, const uint8_t *payload, int plen, const char *what) {
    uint8_t pkt[64];
    pkt[0] = cmd; pkt[1] = opts; pkt[2] = seq++; if (seq == 0) seq = 1; pkt[3] = (uint8_t)plen;
    if (plen) memcpy(pkt + 4, payload, plen);
    return gip_send(h, pkt, 4 + plen, what);
}

static int encode_varint(uint8_t *b, uint32_t v) { int i; for (i = 0; i < 4; i++) { b[i] = v & 0x7f; v >>= 7; if (v) b[i] |= 0x80; else break; } return i + 1; }
static int encode_header(uint8_t *b, uint8_t cmd, uint8_t opts, uint8_t sq, uint32_t len, uint32_t chunk_offset) {
    int n = 0; b[n++] = cmd; b[n++] = opts; b[n++] = sq;
    n += encode_varint(b + n, len);
    int actual = n + ((opts & GIP_OPT_CHUNK) ? encode_varint((uint8_t[4]){0}, chunk_offset) : 0);
    if (actual % 2) { b[n - 1] |= 0x80; b[n++] = 0; }        /* header length must be even */
    if (opts & GIP_OPT_CHUNK) n += encode_varint(b + n, chunk_offset);
    return n;
}
static int send_raw(uint8_t cmd, uint8_t opts, uint8_t sq, const uint8_t *data, uint32_t len, uint32_t chunk_offset, const char *what) {
    uint8_t pkt[64]; int hl = encode_header(pkt, cmd, opts, sq, len, chunk_offset);
    if (hl + len > 64) { fprintf(stderr, "send_raw: too long\n"); return -1; }
    if (len) memcpy(pkt + hl, data, len);
    return gip_send(g_h, pkt, hl + len, what);
}
/* Send a GIP message of arbitrary size: single packet if it fits, else xone-style chunked transfer driven by ACKs. */
static int gip_send_msg(uint8_t cmd, uint8_t opts, const uint8_t *data, uint32_t len, const char *what) {
    uint8_t sq = seq++; if (seq == 0) seq = 1;
    if (len <= GIP_PKT_MAX) return send_raw(cmd, opts, sq, data, len, 0, what);
    if (tx.active) fprintf(stderr, "warning: chunked send already active, overwriting\n");
    free(tx.data); tx.data = malloc(len); memcpy(tx.data, data, len);
    tx.active = 1; tx.cmd = cmd; tx.opts = opts & ~(GIP_OPT_ACK | GIP_OPT_CHUNK_START); tx.seq = sq; tx.total = len;
    return send_raw(cmd, opts | GIP_OPT_ACK | GIP_OPT_CHUNK_START | GIP_OPT_CHUNK, sq, data, GIP_PKT_MAX, len, what);
}
static void gip_handle_tx_ack(const uint8_t *pl, int plen) {
    if (!tx.active || plen < 9 || pl[1] != tx.cmd) return;
    uint32_t acked = pl[3] | (pl[4] << 8);
    if (acked < tx.total) {
        uint32_t remaining = tx.total - GIP_PKT_MAX;   /* xone: resend everything after the first chunk */
        while (remaining) {
            uint32_t n = remaining > GIP_PKT_MAX ? GIP_PKT_MAX : remaining;
            uint8_t o = tx.opts | GIP_OPT_CHUNK | (remaining <= GIP_PKT_MAX ? GIP_OPT_ACK : 0);
            send_raw(tx.cmd, o, tx.seq, tx.data + (tx.total - remaining), n, tx.total - remaining, "chunk");
            remaining -= n;
        }
    } else {
        send_raw(tx.cmd, tx.opts | GIP_OPT_CHUNK, tx.seq, NULL, 0, tx.total, "chunk_end");
        tx.active = 0;
    }
}
static int auth_send_cb(void *user, const uint8_t *payload, int len, int need_ack) {
    (void)user; return gip_send_msg(GIP_CMD_AUTH, GIP_OPT_INTERNAL | (need_ack ? GIP_OPT_ACK : 0), payload, len, "auth");
}

/* Protocol Control ACK (xone gip_acknowledge_pkt / spec 3.1.5.1):
 * 01 20 <seq of acked pkt> 09 | 00 <cmd> <opts=0x20> <received lo> <received hi> 00 00 <remaining lo> <remaining hi> */
static void gip_ack(libusb_device_handle *h, const gip_hdr_t *in, uint32_t received, uint32_t remaining) {
    uint8_t pkt[13] = { GIP_CMD_ACK, GIP_OPT_INTERNAL, in->seq, 0x09,
                        0x00, in->cmd, GIP_OPT_INTERNAL, (uint8_t)received, (uint8_t)(received >> 8), 0, 0,
                        (uint8_t)remaining, (uint8_t)(remaining >> 8) };
    gip_send(h, pkt, sizeof pkt, "ack");
}

static const char *btn_names_lo[8] = { "?", "KEEPALIVE", "MENU", "VIEW", "A", "B", "X", "Y" };
static const char *btn_names_hi[8] = { "DU", "DD", "DL", "DR", "LB", "RB", "LS", "RS" };

static void print_state(const gip_state_t *s) {
    uint8_t lo = s->buttons & 0xff, hi = s->buttons >> 8;
    if (opt_json) {
        printf("{\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,\"lt\":%u,\"rt\":%u,"
               "\"a\":%d,\"b\":%d,\"x\":%d,\"y\":%d,\"lb\":%d,\"rb\":%d,\"ls\":%d,\"rs\":%d,"
               "\"menu\":%d,\"view\":%d,\"guide\":%d,\"du\":%d,\"dd\":%d,\"dl\":%d,\"dr\":%d}\n",
               s->lx, s->ly, s->rx, s->ry, s->lt, s->rt,
               !!(lo&0x10), !!(lo&0x20), !!(lo&0x40), !!(lo&0x80),
               !!(hi&0x10), !!(hi&0x20), !!(hi&0x40), !!(hi&0x80),
               !!(lo&0x04), !!(lo&0x08), s->guide,
               !!(hi&0x01), !!(hi&0x02), !!(hi&0x04), !!(hi&0x08));
        fflush(stdout); return;
    }
    if (opt_raw) return;
    printf("\033[H\033[J");
    printf("Razer Wolverine V3 Pro  (GIP over libusb)   Ctrl-C to quit\n\n");
    printf("LX: %6d    LY: %6d\n", s->lx, s->ly);
    printf("RX: %6d    RY: %6d\n", s->rx, s->ry);
    printf("LT: %4u/1023  RT: %4u/1023\n\n", s->lt, s->rt);
    printf("A: %d  B: %d  X: %d  Y: %d\n", !!(lo&0x10), !!(lo&0x20), !!(lo&0x40), !!(lo&0x80));
    printf("LB: %d RB: %d LS: %d RS: %d\n", !!(hi&0x10), !!(hi&0x20), !!(hi&0x40), !!(hi&0x80));
    printf("D-pad U: %d D: %d L: %d R: %d\n", !!(hi&0x01), !!(hi&0x02), !!(hi&0x04), !!(hi&0x08));
    printf("Menu: %d View: %d Guide: %d\n", !!(lo&0x04), !!(lo&0x08), s->guide);
    printf("\nraw buttons: 0x%04x  [", s->buttons);
    for (int i = 0; i < 8; i++) if (lo & (1 << i)) printf(" %s", btn_names_lo[i]);
    for (int i = 0; i < 8; i++) if (hi & (1 << i)) printf(" %s", btn_names_hi[i]);
    printf(" ]\n"); fflush(stdout);
}

static int find_endpoints(libusb_device *dev) {
    struct libusb_config_descriptor *cfg;
    if (libusb_get_config_descriptor(dev, 0, &cfg) != 0) return -1;
    int found = -1;
    for (int i = 0; i < cfg->bNumInterfaces && found < 0; i++) {
        const struct libusb_interface_descriptor *alt = &cfg->interface[i].altsetting[0];
        if (alt->bInterfaceClass == 0xFF && alt->bInterfaceSubClass == 0x47 && alt->bInterfaceProtocol == 0xD0 && alt->bNumEndpoints == 2) {
            ep_in = ep_out = 0;
            for (int e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_INTERRUPT) continue;
                if (ep->bEndpointAddress & 0x80) ep_in = ep->bEndpointAddress; else ep_out = ep->bEndpointAddress;
            }
            if (ep_in && ep_out) found = alt->bInterfaceNumber;
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

static libusb_device_handle *open_dev(int *iface_out) {
    libusb_device_handle *h = libusb_open_device_with_vid_pid(NULL, vid, pid);
    if (!h) return NULL;
    int iface = find_endpoints(libusb_get_device(h));
    if (iface < 0) { fprintf(stderr, "no GIP interface (FF/47/D0) with 2 interrupt endpoints\n"); libusb_close(h); return NULL; }
    int cur = 0; libusb_get_configuration(h, &cur);
    if (cur != 1) { int r = libusb_set_configuration(h, 1); fprintf(stderr, "set_configuration(1): %s (was %d)\n", r ? libusb_error_name(r) : "ok", cur); }
    int r = libusb_claim_interface(h, iface);
    if (r) { fprintf(stderr, "claim_interface failed: %s\n", libusb_error_name(r)); libusb_close(h); return NULL; }
    /* Like Linux xpad: explicitly select alt-setting 0 of the GIP audio interface (#1); some
     * third-party pads need this before they report properly. */
    if (libusb_claim_interface(h, 1) == 0) {
        int ra = libusb_set_interface_alt_setting(h, 1, 0);
        fprintf(stderr, "audio interface #1 alt 0: %s\n", ra ? libusb_error_name(ra) : "ok");
    } else fprintf(stderr, "could not claim audio interface #1 (ignored)\n");
    fprintf(stderr, "opened %04x:%04x, GIP interface #%d, EP IN 0x%02x, EP OUT 0x%02x\n", vid, pid, iface, ep_in, ep_out);
    *iface_out = iface; g_h = h;
    seq = 1; state = ST_WAIT_HELLO; tx.active = 0; gip_auth_free(auth); auth = NULL; meta_done = 0; meta_total = meta_received = 0; free(meta_buf); meta_buf = NULL;
    return h;
}

static void send_metadata_request(libusb_device_handle *h) {
    gip_send_cmd(h, GIP_CMD_METADATA, GIP_OPT_INTERNAL, NULL, 0, "meta_request");
    state = ST_META;
}

static void send_start_led_rumble(libusb_device_handle *h) {
    const uint8_t start[] = { 0x00 };                 /* Set Device State: Start */
    const uint8_t led[]   = { 0x00, 0x01, 0x14 };     /* LED on, brightness 0x14 */
    const uint8_t rmb[]   = { 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xEB };
    if (strstr(opt_init, "power"))  gip_send_cmd(h, GIP_CMD_STATE,  GIP_OPT_INTERNAL, start, sizeof start, "state_start");
    if (strstr(opt_init, "led"))    gip_send_cmd(h, GIP_CMD_LED,    GIP_OPT_INTERNAL, led, sizeof led, "led_on");
    if (strstr(opt_init, "rumble")) gip_send_cmd(h, GIP_CMD_RUMBLE, 0x00, rmb, sizeof rmb, "rumble_init");
    state = ST_RUNNING; t_started = now_s(); post_done = 0;
}
static int auth_started = 0;
/* Spec order (MS-GIPUSB 1.3.1): Hello -> Metadata -> Security -> Set Device State: Start -> ... -> input */
static void start_device(libusb_device_handle *h) {
    const uint8_t auth_done[] = { 0x01, 0x00 };       /* fake "security passed" (xpad xboxone_auth_done) */
    if (strstr(opt_init, "sdl")) {  /* SDL-driver order: Start/LED/rumble first, then security, then Start/LED again */
        opt_init = "power,led,rumble"; send_start_led_rumble(h); opt_init = "power,led";
        gip_auth_free(auth); auth = gip_auth_new(auth_send_cb, NULL); auth_started = 1; fprintf(stderr, "auth: starting security handshake (after Start)\n"); gip_auth_start(auth); state = ST_RUNNING; t_started = now_s(); post_done = 1; return;
    }
    if (strstr(opt_init, "authdone")) { gip_send_cmd(h, GIP_CMD_AUTH, GIP_OPT_INTERNAL, auth_done, sizeof auth_done, "auth_done"); send_start_led_rumble(h); }
    else if (strstr(opt_init, "auth")) { gip_auth_free(auth); auth = gip_auth_new(auth_send_cb, NULL); auth_started = 1; fprintf(stderr, "auth: starting security handshake\n"); gip_auth_start(auth); state = ST_RUNNING; t_started = now_s(); post_done = 1; }
    else send_start_led_rumble(h);
}
static void begin_init(libusb_device_handle *h) {
    if (strstr(opt_init, "meta") && !strstr(opt_init, "sdl")) send_metadata_request(h); else start_device(h);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--raw")) opt_raw = 1;
        else if (!strcmp(argv[i], "--json")) opt_json = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) opt_seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--vid") && i + 1 < argc) vid = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc) pid = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--init") && i + 1 < argc) opt_init = argv[++i];
        else if (!strcmp(argv[i], "--restart")) opt_restart = 1;
        else if (!strcmp(argv[i], "--serial")) opt_serial = 1;
        else if (!strcmp(argv[i], "--query")) opt_query = 1;
        else { fprintf(stderr, "usage: %s [--raw] [--json] [--seconds N] [--init meta,power,led,auth,rumble|none] [--vid 0x1532] [--pid 0x0a3f]\n", argv[0]); return 2; }
    }
    signal(SIGINT, on_sigint);
    if (libusb_init(NULL)) { fprintf(stderr, "libusb_init failed\n"); return 1; }

    int iface = -1;
    libusb_device_handle *h = open_dev(&iface);
    if (!h) { fprintf(stderr, "device %04x:%04x not found / could not open\n", vid, pid); return 1; }

    gip_state_t st = {0};
    uint8_t buf[64];
    int r;
    double t0 = now_s(), t_open = t0, t_last_req = 0;
    int n_input = 0, n_other = 0, meta_retries = 0;
    if (!opt_json && !opt_raw) print_state(&st);

    while (!stop) {
        double now = now_s();
        if (opt_seconds > 0 && now - t0 > opt_seconds) break;

        /* If no Hello shows up (device already active from a previous host), kick off init after 0.6 s. */
        if (state == ST_WAIT_HELLO && now - t_open > 0.6) { fprintf(stderr, "no Hello yet; sending init anyway\n"); t_last_req = now; begin_init(h); }
        /* Spec: host resends Metadata Request every 500 ms, up to 4 times. */
        if (state == ST_META && !meta_received && now - t_last_req > 0.5) {
            if (++meta_retries >= 4) { fprintf(stderr, "no metadata response; continuing without it\n"); start_device(h); }
            else { t_last_req = now; send_metadata_request(h); }
        }

        if (state == ST_RUNNING && !post_done && now - t_started > 0.4) {
            post_done = 1;
            if (opt_restart) {
                const uint8_t stp[] = { 0x01 }, strt[] = { 0x00 };
                gip_send_cmd(h, GIP_CMD_STATE, GIP_OPT_INTERNAL, stp, 1, "state_stop");
                usleep(100000);
                gip_send_cmd(h, GIP_CMD_STATE, GIP_OPT_INTERNAL, strt, 1, "state_start2");
            }
            if (opt_serial) { uint8_t sp[] = { 0x1E, 0x30, 0x07, 0x01, 0x04 }; gip_send(h, sp, sizeof sp, "serial_req"); }
            if (opt_query) {  /* Razer vendor frame: diagnostic-mode state query (class 0x00, sub 0x84), read-only */
                uint8_t q[64] = {0}; q[0] = 0x0F; q[1] = 0x00; q[2] = 0x00; q[3] = 0x01; q[0x0A] = 0x00; q[0x0B] = 0x84;
                uint8_t x = 0; for (int k = 0x06; k <= 0x3D; k++) x ^= q[k]; q[0x3E] = x;
                gip_send(h, q, sizeof q, "razer_query");
            }
        }
        int xfer = 0;
        r = libusb_interrupt_transfer(h, ep_in, buf, sizeof buf, &xfer, 100);
        if (r == LIBUSB_ERROR_TIMEOUT) continue;
        if (r) {
            fprintf(stderr, "read error: %s -- device lost, waiting for reconnect...\n", libusb_error_name(r));
            libusb_release_interface(h, iface); libusb_close(h); h = NULL;
            while (!stop && !h && (opt_seconds <= 0 || now_s() - t0 < opt_seconds)) { usleep(500000); h = open_dev(&iface); }
            if (!h) break;
            t_open = now_s(); meta_retries = 0;
            continue;
        }
        gip_hdr_t hd;
        if (decode_header(&hd, buf, xfer) < 0) continue;
        const uint8_t *pl = buf + hd.hdr_len;
        int plen = xfer - hd.hdr_len; if ((uint32_t)plen > hd.len) plen = hd.len;

        /* Generic reassembly of chunked messages other than metadata (e.g. auth certificates). */
        if ((hd.opts & GIP_OPT_CHUNK) && hd.cmd != GIP_CMD_METADATA) {
            if (hd.opts & GIP_OPT_CHUNK_START) { rx_cmd = hd.cmd; rx_total = hd.chunk_offset; hd.chunk_offset = 0; rx_received = 0; free(rx_buf); rx_buf = calloc(1, rx_total + 64); }
            if (rx_buf && hd.cmd == rx_cmd) {
                if (hd.len) { if (hd.chunk_offset + plen <= rx_total + 64) { memcpy(rx_buf + hd.chunk_offset, pl, plen); rx_received = hd.chunk_offset + plen; } }
                if (hd.opts & GIP_OPT_ACK) gip_ack(h, &hd, rx_received, rx_total > rx_received ? rx_total - rx_received : 0);
                if (hd.len == 0) {   /* completion */
                    if (opt_raw) { char tag[40]; snprintf(tag, sizeof tag, "RX chunked cmd 0x%02x complete", hd.cmd); hexdump(tag, rx_buf, rx_received); }
                    if (hd.cmd == GIP_CMD_AUTH && auth) {
                        if (gip_auth_process(auth, rx_buf, rx_received)) fprintf(stderr, "auth: processing failed (%s)\n", gip_auth_state_name(auth));
                        if (gip_auth_complete(auth) && auth_started) { auth_started = 0; send_start_led_rumble(h); }
                    }
                    else n_other++;
                    free(rx_buf); rx_buf = NULL;
                }
            }
            continue;
        }
        if (hd.cmd == GIP_CMD_ACK) { if (opt_raw) hexdump("RX ack", buf, xfer); gip_handle_tx_ack(pl, plen); continue; }
        if (hd.cmd == GIP_CMD_AUTH) {
            if (opt_raw) hexdump("RX auth", buf, xfer);
            if (hd.opts & GIP_OPT_ACK) gip_ack(h, &hd, hd.len, 0);
            if (auth) {
                if (gip_auth_process(auth, pl, plen)) fprintf(stderr, "auth: processing failed (%s)\n", gip_auth_state_name(auth));
                if (gip_auth_complete(auth) && auth_started) { auth_started = 0; send_start_led_rumble(h); }
            }
            continue;
        }
        switch (hd.cmd) {
        case GIP_CMD_HELLO:
            if (opt_raw) hexdump("RX hello", buf, xfer);
            if (state == ST_WAIT_HELLO) {
                fprintf(stderr, "Hello: vid=%02x%02x pid=%02x%02x fw=%u.%u.%u.%u\n", pl[9], pl[8], pl[11], pl[10],
                        pl[12] | pl[13] << 8, pl[14] | pl[15] << 8, pl[16] | pl[17] << 8, pl[18] | pl[19] << 8);
                t_last_req = now; begin_init(h);
            }
            break;
        case GIP_CMD_METADATA:
            if (opt_raw) hexdump("RX metadata", buf, xfer);
            if (hd.opts & GIP_OPT_CHUNK) {
                if (hd.opts & GIP_OPT_CHUNK_START) {
                    meta_total = hd.chunk_offset; hd.chunk_offset = 0; meta_received = 0;
                    free(meta_buf); meta_buf = calloc(1, meta_total + 64);
                    fprintf(stderr, "metadata: total %u bytes\n", meta_total);
                }
                if (hd.len == 0 && meta_buf) {   /* Metadata Complete: empty fragment */
                    fprintf(stderr, "metadata complete (%u/%u bytes)\n", meta_received, meta_total);
                    if (!opt_json) hexdump("METADATA", meta_buf, meta_received);
                    meta_done = 1;
                    if (hd.opts & GIP_OPT_ACK) gip_ack(h, &hd, meta_received, 0);
                    start_device(h);
                    break;
                }
                if (meta_buf && hd.chunk_offset + plen <= meta_total + 64) { memcpy(meta_buf + hd.chunk_offset, pl, plen); meta_received = hd.chunk_offset + plen; }
                if (hd.opts & GIP_OPT_ACK) gip_ack(h, &hd, meta_received, meta_total > meta_received ? meta_total - meta_received : 0);
            } else if (hd.opts & GIP_OPT_ACK) gip_ack(h, &hd, hd.len, 0);
            break;
        case GIP_CMD_INPUT:
            if (hd.opts & GIP_OPT_ACK) gip_ack(h, &hd, hd.len, 0);
            if (plen >= 14) {
                n_input++;
                st.buttons = (uint16_t)(pl[0] | (pl[1] << 8)) & ~0x0002;
                st.lt = (uint16_t)(pl[2] | (pl[3] << 8)) & 0x3FF;
                st.rt = (uint16_t)(pl[4] | (pl[5] << 8)) & 0x3FF;
                st.lx = (int16_t)(pl[6] | (pl[7] << 8));
                st.ly = (int16_t)(pl[8] | (pl[9] << 8));
                st.rx = (int16_t)(pl[10] | (pl[11] << 8));
                st.ry = (int16_t)(pl[12] | (pl[13] << 8));
                if (opt_raw) hexdump("RX input", buf, xfer);
                print_state(&st);
                if (plen > 14 && !opt_json && !opt_raw) hexdump("extra payload bytes", pl + 14, plen - 14);
            }
            break;
        case GIP_CMD_GUIDE:
            if (hd.opts & GIP_OPT_ACK) gip_ack(h, &hd, hd.len, 0);
            st.guide = pl[0] & 0x03;
            if (opt_raw) hexdump("RX guide", buf, xfer); else print_state(&st);
            break;
        default:
            n_other++;
            if (hd.opts & GIP_OPT_ACK) gip_ack(h, &hd, hd.len, 0);
            if (opt_raw || !opt_json) { char tag[32]; snprintf(tag, sizeof tag, "RX cmd 0x%02x", hd.cmd); if (!opt_raw) printf("\n"); hexdump(tag, buf, xfer); }
            break;
        }
    }
    fprintf(stderr, "\nreceived %d input reports, %d other packets in %.1fs (metadata %s, auth %s)\n", n_input, n_other, now_s() - t0, meta_done ? "ok" : "not received", auth ? gip_auth_state_name(auth) : "not attempted");
    if (h) { libusb_release_interface(h, iface); libusb_close(h); }
    libusb_exit(NULL);
    return 0;
}
