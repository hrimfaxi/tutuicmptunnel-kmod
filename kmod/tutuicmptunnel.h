#pragma once

#include <linux/ipv6.h>
#include <linux/types.h>

enum {
  TUTU_CMD_UNSPEC,

  TUTU_CMD_GET_CONFIG,
  TUTU_CMD_SET_CONFIG,

  TUTU_CMD_GET_STATS,
  TUTU_CMD_CLR_STATS,

  /* Egress */
  TUTU_CMD_GET_EGRESS, /* Support DOIT (Lookup) & DUMPIT (List) */
  TUTU_CMD_DELETE_EGRESS,
  TUTU_CMD_UPDATE_EGRESS,

  /* Ingress */
  TUTU_CMD_GET_INGRESS, /* Support DOIT (Lookup) & DUMPIT (List) */
  TUTU_CMD_DELETE_INGRESS,
  TUTU_CMD_UPDATE_INGRESS,

  /* Session */
  TUTU_CMD_GET_SESSION, /* Support DOIT (Lookup) & DUMPIT (List) */
  TUTU_CMD_DELETE_SESSION,
  TUTU_CMD_UPDATE_SESSION,

  /* User Info */
  TUTU_CMD_GET_USER_INFO, /* Support DOIT (Lookup) & DUMPIT (List) */
  TUTU_CMD_DELETE_USER_INFO,
  TUTU_CMD_UPDATE_USER_INFO,

  TUTU_CMD_IFNAME_GET,
  TUTU_CMD_IFNAME_ADD,
  TUTU_CMD_IFNAME_DEL,

  TUTU_CMD_MAX,
};

#define TUTU_CMD_MAX (__TUTU_CMD_MAX - 1)

enum {
  TUTU_ATTR_UNSPEC,

  TUTU_ATTR_CONFIG, /* binary: struct tutu_config */
  TUTU_ATTR_STATS,  /* binary: struct tutu_stats */

  TUTU_ATTR_EGRESS,    /* binary: struct tutu_egress */
  TUTU_ATTR_INGRESS,   /* binary: struct tutu_ingress */
  TUTU_ATTR_SESSION,   /* binary: struct tutu_session */
  TUTU_ATTR_USER_INFO, /* binary: struct tutu_user_info */

  TUTU_ATTR_IFNAME_NAME, /* String */

  TUTU_ATTR_MAX,
};

#define TUTU_ATTR_MAX (TUTU_ATTR_MAX - 1)

#define TUTU_GENL_FAMILY_NAME "tutuicmptunnel"
#define TUTU_GENL_VERSION     0x1

enum {
  TUTU_ANY     = 0, /* create new element or update existing */
  TUTU_NOEXIST = 1, /* create new element if it didn't exist */
  TUTU_EXIST   = 2, /* update existing element */
  TUTU_F_LOCK  = 4, /* spin_lock-ed map_lookup/map_update */
};

struct tutu_ifname_node {
  struct list_head list;
  char             name[IFNAMSIZ];
};

struct tutu_config {
  __u32 session_max_age;
  __u8  reserved0;
  __u8  is_server;
  __u16 reserved1; /* padding to 8-byte boundary */
};

struct tutu_stats {
  __u64 packets_processed;
  __u64 packets_dropped;
  __u64 checksum_errors;
  __u64 fragmented;
  __u64 gso;
};

// User struct to store in the map
struct user_info {
  struct in6_addr address;     // IP address in network byte order
  __be16          icmp_id;     // ICMP ID
  __be16          dport;       // Destination port
  __u8            comment[22]; // Comment
};

struct tutu_user_info {
  __u8             key;
  struct user_info value;
  __u64            map_flags;
};

struct egress_peer_key {
  struct in6_addr address; // Server address in network byte order
  __be16          port;    // Server port
};

struct egress_peer_value {
  __u8 uid;         // UID for this client
  __u8 comment[22]; // Comment
};

struct tutu_egress {
  struct egress_peer_key   key;
  struct egress_peer_value value;
  __u64                    map_flags;
};

struct ingress_peer_key {
  struct in6_addr address; // Server address in network byte order
  __u8            uid;     // UID (icmp id)
};

struct ingress_peer_value {
  __be16 port; // Server UDP Port
};

struct tutu_ingress {
  struct ingress_peer_key   key;
  struct ingress_peer_value value;
  __u64                     map_flags;
};

// 会话记录：本地的udp连接，注意sport是本地的而非远程client的源端口。
struct session_key {
  struct in6_addr address;
  __be16          sport;
  __be16          dport;
};

struct session_value {
  __u64  age;
  __u8   uid;
  __be16 client_sport; // 远程client的源端口
};

struct tutu_session {
  struct session_key   key;
  struct session_value value;
  __u64                map_flags;
};

struct tutu_htab;
extern struct tutu_htab *egress_peer_map;
extern struct tutu_htab *ingress_peer_map;
extern struct tutu_htab *session_map;
extern struct tutu_htab *user_map;

int  tutu_genl_init(void);
void tutu_genl_exit(void);
int  tutu_export_config(struct tutu_config *out);
int  tutu_set_config(const struct tutu_config *in);
int  tutu_clear_stats(void);
int  tutu_export_stats(struct tutu_stats *out);
int  ifset_reload_config(void);
bool net_has_device(const char *dev_name);

// vim: set sw=2 ts=2 expandtab:
