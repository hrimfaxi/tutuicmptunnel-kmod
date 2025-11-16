#pragma once

#include <linux/ipv6.h>
#include <linux/types.h>

#define TUTU_IOC_MAGIC 'T'

enum {
  TUTU_ANY     = 0, /* create new element or update existing */
  TUTU_NOEXIST = 1, /* create new element if it didn't exist */
  TUTU_EXIST   = 2, /* update existing element */
  TUTU_F_LOCK  = 4, /* spin_lock-ed map_lookup/map_update */
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

#define TUTU_GET_CONFIG _IOR(TUTU_IOC_MAGIC, 0x01, struct tutu_config)
#define TUTU_SET_CONFIG _IOW(TUTU_IOC_MAGIC, 0x02, struct tutu_config)
#define TUTU_GET_STATS  _IOR(TUTU_IOC_MAGIC, 0x03, struct tutu_stats)
#define TUTU_CLR_STATS  _IO(TUTU_IOC_MAGIC, 0x04)

#define TUTU_LOOKUP_EGRESS        _IOR(TUTU_IOC_MAGIC, 0x05, struct tutu_egress)
#define TUTU_DELETE_EGRESS        _IOW(TUTU_IOC_MAGIC, 0x06, struct tutu_egress)
#define TUTU_UPDATE_EGRESS        _IOW(TUTU_IOC_MAGIC, 0x07, struct tutu_egress)
#define TUTU_GET_FIRST_KEY_EGRESS _IOR(TUTU_IOC_MAGIC, 0x08, struct tutu_egress)
#define TUTU_GET_NEXT_KEY_EGRESS  _IOR(TUTU_IOC_MAGIC, 0x09, struct tutu_egress)

#define TUTU_LOOKUP_INGRESS        _IOR(TUTU_IOC_MAGIC, 0x0a, struct tutu_ingress)
#define TUTU_DELETE_INGRESS        _IOW(TUTU_IOC_MAGIC, 0x0b, struct tutu_ingress)
#define TUTU_UPDATE_INGRESS        _IOW(TUTU_IOC_MAGIC, 0x0c, struct tutu_ingress)
#define TUTU_GET_FIRST_KEY_INGRESS _IOR(TUTU_IOC_MAGIC, 0x0d, struct tutu_ingress)
#define TUTU_GET_NEXT_KEY_INGRESS  _IOR(TUTU_IOC_MAGIC, 0x0e, struct tutu_ingress)

#define TUTU_LOOKUP_USER_INFO        _IOR(TUTU_IOC_MAGIC, 0x0f, struct tutu_user_info)
#define TUTU_DELETE_USER_INFO        _IOW(TUTU_IOC_MAGIC, 0x10, struct tutu_user_info)
#define TUTU_UPDATE_USER_INFO        _IOW(TUTU_IOC_MAGIC, 0x11, struct tutu_user_info)
#define TUTU_GET_FIRST_KEY_USER_INFO _IOR(TUTU_IOC_MAGIC, 0x12, struct tutu_user_info)
#define TUTU_GET_NEXT_KEY_USER_INFO  _IOR(TUTU_IOC_MAGIC, 0x13, struct tutu_user_info)

#define TUTU_LOOKUP_SESSION        _IOR(TUTU_IOC_MAGIC, 0x14, struct tutu_session)
#define TUTU_DELETE_SESSION        _IOW(TUTU_IOC_MAGIC, 0x15, struct tutu_session)
#define TUTU_UPDATE_SESSION        _IOW(TUTU_IOC_MAGIC, 0x16, struct tutu_session)
#define TUTU_GET_FIRST_KEY_SESSION _IOR(TUTU_IOC_MAGIC, 0x17, struct tutu_session)
#define TUTU_GET_NEXT_KEY_SESSION  _IOR(TUTU_IOC_MAGIC, 0x18, struct tutu_session)

// vim: set sw=2 ts=2 expandtab:
