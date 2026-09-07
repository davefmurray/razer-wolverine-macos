/* GIP security handshake (host side), ported from medusalix/xone auth/auth.c + auth/crypto.c to userspace/OpenSSL. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef int (*gip_auth_send_fn)(void *user, const uint8_t *payload, int len, int need_ack);

typedef struct gip_auth gip_auth_t;

gip_auth_t *gip_auth_new(gip_auth_send_fn send, void *user);
void gip_auth_free(gip_auth_t *a);
int gip_auth_start(gip_auth_t *a);                          /* sends host hello (v1) */
int gip_auth_process(gip_auth_t *a, const uint8_t *data, int len); /* feed payload of an incoming 0x06 message */
int gip_auth_complete(const gip_auth_t *a);                 /* 1 once the handshake succeeded */
const char *gip_auth_state_name(const gip_auth_t *a);
