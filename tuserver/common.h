#pragma once

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "list.h"
#include "sodium.h"

#define _STR(x) #x
#define STR(x)  _STR(x)

#define SALT_LEN    16
#define TS_LEN      8
#define KEYB        crypto_aead_xchacha20poly1305_ietf_KEYBYTES
#define NONCE_LEN   crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
#define TAG         crypto_aead_xchacha20poly1305_ietf_ABYTES
#define MIN_LEN     (SALT_LEN + TS_LEN + NONCE_LEN + TAG) // salt+ts+nonce+tag
#define MAX_CT_SIZE 1444
#define MAX_PT_SIZE (MAX_CT_SIZE - MIN_LEN)

#define DEFAULT_SERVER      "127.0.0.1"
#define DEFAULT_SERVER_PORT 14801
#define DEFAULT_WINDOW      30
#define DEFAULT_REPLAY_MAX  32768

// --- 重放窗口链表 ---
struct replay_entry {
  struct list_head list;
  time_t           ts;
  uint8_t          nonce[NONCE_LEN];
};

struct replay_window {
  int              window; // 秒
  int              max_size;
  int              count;
  struct list_head head;
};

struct sockaddr_storage;
int addr_to_str(const struct sockaddr_storage *addr, char *out, size_t len);
int psk2key(const char *psk, const uint8_t *salt, uint8_t *key);
int remove_padding(uint8_t *pt, unsigned long long *pt_len);
int encrypt_and_send_packet(int sock, const struct sockaddr *cli, socklen_t clen, struct replay_window *rwin, const char *psk,
                            const char *payload, size_t payload_len, size_t *out_packet_len);
int decrypt_and_validate_packet(uint8_t *pt_out, unsigned long long *pt_len_out, const uint8_t *pkt_in, ssize_t pkt_len,
                                struct replay_window *rwin, const char *psk, const struct sockaddr_storage *cli);

// 重放相关
void replay_window_init(struct replay_window *rw, int window, int max_size);
void replay_window_free(struct replay_window *rw);
int  replay_check(struct replay_window *rw, time_t ts, const uint8_t nonce[NONCE_LEN]);
int  replay_add(struct replay_window *rw, time_t ts, const uint8_t nonce[NONCE_LEN]);

// vim: set sw=2 expandtab :
