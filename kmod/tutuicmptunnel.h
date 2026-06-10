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

  __TUTU_CMD_MAX,
};

#define TUTU_CMD_MAX     (__TUTU_CMD_MAX - 1)
#define TUTU_XOR_KEY_MAX 64

enum {
  TUTU_ATTR_UNSPEC,

  TUTU_ATTR_CONFIG, /* binary: struct tutu_config */
  TUTU_ATTR_STATS,  /* binary: struct tutu_stats */

  TUTU_ATTR_EGRESS,    /* binary: struct tutu_egress */
  TUTU_ATTR_INGRESS,   /* binary: struct tutu_ingress */
  TUTU_ATTR_SESSION,   /* binary: struct tutu_session */
  TUTU_ATTR_USER_INFO, /* binary: struct tutu_user_info */

  TUTU_ATTR_IFNAME_NAME, /* String */

  __TUTU_ATTR_MAX,
};

#define TUTU_ATTR_MAX (__TUTU_ATTR_MAX - 1)

#define TUTU_GENL_FAMILY_NAME "tutuicmptunnel"
#define TUTU_GENL_VERSION     0x1

enum {
  TUTU_ANY     = 0, /* create new element or update existing */
  TUTU_NOEXIST = 1, /* create new element if it didn't exist */
  TUTU_EXIST   = 2, /* update existing element */
  TUTU_F_LOCK  = 4, /* spin_lock-ed map_lookup/map_update */
};

/*
 * tutu_ifname_node: 接口名称链表节点
 * 用于记录需要启用tutuicmptunnel的接口
 */
struct tutu_ifname_node {
  struct list_head list;
  char             name[IFNAMSIZ];
};

/*
 * tutu_config: 运行时配置（单元素 map，key 恒为 0）
 * - session_max_age: 会话最大存活时间（秒）
 * - is_server: 1 为 server 模式，0 为 client 模式
 * - reserved*: 填充至 8 字节边界
 */
struct tutu_config {
  __u32 session_max_age;
  __u8  reserved0;
  __u8  is_server;
  __u16 reserved1; /* padding to 8-byte boundary */
};

/*
 * tutu_stats: 全局统计计数器
 */
struct tutu_stats {
  __u64 packets_processed;
  __u64 packets_dropped;
  __u64 checksum_errors;
  __u64 fragmented;
  __u64 gso;
};

/*
 * user_info: server 模式下，每个授权客户端的静态配置
 * - address: 客户端源地址，统一 IPv6（v4-mapped IPv4）
 * - icmp_id: 客户端最近一次使用的 ICMP Echo ID（网络字节序）。
 *            动态更新：ingress 时若发现新值，立即覆盖旧值。
 *            注意：多源端口并发时，该字段会被最后一次通信的 icmp_id
 *            覆盖，但这不影响正确性——真正区分多源端口的是 session_map
 *            的独立条目，user->icmp_id 仅用于 NAT 改写后的快速跟踪。
 * - dport: 服务器上 tutuicmptunnel 使用的 UDP 端口（网络字节序）
 * - comment: 纯文本备注
 * - xor_key: 简单 XOR 混淆密钥（可选，长度为 xor_key_len）。
 *            若 xor_key_len == 0 则禁用 XOR 混淆。
 *            注意：XOR 仅提供基础混淆，不替代 AEAD 加密。
 * - xor_key_len: 有效密钥长度，范围 0..TUTU_XOR_KEY_MAX
 * - reserved2: 填充至 8 字节边界
 */
struct user_info {
  struct in6_addr address;
  __be16          icmp_id;
  __be16          dport;
  __u8            comment[22];
  __u8            xor_key[TUTU_XOR_KEY_MAX];
  __u8            xor_key_len;
  __u8            reserved2[7];
};

/*
 * tutu_user_info: user_info 的 netlink 传输包装
 * 用于内核模块与用户态工具（ktuctl）之间的 netlink 消息传递。
 */
struct tutu_user_info {
  __u8             key;
  struct user_info value;
  __u64            map_flags;
};

/*
 * egress_peer_key: client 模式下，出向报文（客户端→服务器）的查找键
 * - address: 服务器地址，统一 IPv6（v4-mapped IPv4）
 * - port: 服务器 UDP 端口，网络字节序
 */
struct egress_peer_key {
  struct in6_addr address;
  __be16          port;
};

/*
 * egress_peer_value: client 模式下，出向 peer 的附加属性
 * - xor_key: 简单 XOR 混淆密钥（可选，长度为 xor_key_len）
 * - comment: 纯文本备注
 * - xor_key_len: 有效密钥长度，范围 0..TUTU_XOR_KEY_MAX. 0表示禁用
 * - uid: 服务器为该客户端分配的 UID
 */
struct egress_peer_value {
  __u8 xor_key[TUTU_XOR_KEY_MAX];
  __u8 comment[22];
  __u8 xor_key_len;
  __u8 uid;
};

/*
 * tutu_egress: egress_peer 的 netlink 传输包装
 */
struct tutu_egress {
  struct egress_peer_key   key;
  struct egress_peer_value value;
  __u64                    map_flags;
};

/*
 * ingress_peer_key: client 模式下，入向报文（服务器→客户端）的查找键
 * - address: 服务器地址，统一 IPv6（v4-mapped IPv4）
 * - uid: 服务器返回 ICMP 报文 code 字段携带的 UID
 */
struct ingress_peer_key {
  struct in6_addr address;
  __u8            uid;
};

/*
 * ingress_peer_value: client 模式下，入向 peer 的附加属性
 * - xor_key: 简单 XOR 混淆密钥（可选，长度为 xor_key_len）
 * - xor_key_len: 有效密钥长度，范围 0..TUTU_XOR_KEY_MAX
 * - reserved0: 填充
 * - port: 客户端本地期望的 UDP 源端口（网络字节序），
 *         server 回包时以此重建 UDP 头部
 * - reserved: 填充至 8 字节边界
 */
struct ingress_peer_value {
  __u8   xor_key[TUTU_XOR_KEY_MAX];
  __u8   xor_key_len;
  __u8   reserved0;
  __be16 port;
  __u8   reserved[4];
};

/*
 * tutu_ingress: ingress_peer 的 netlink 传输包装
 */
struct tutu_ingress {
  struct ingress_peer_key   key;
  struct ingress_peer_value value;
  __u64                     map_flags;
};

/*
 * session_key: server 模式下，用于从 UDP 回包定位所属 ICMP 会话
 * - address: 客户端地址，统一 IPv6；IPv4 以 v4-mapped 形式存储
 * - sport: 客户端 ICMP ID（可能被 NAT 改写后的值），网络字节序。
 *          NAT 设备通常改写 icmp_id 但不动 icmp_seq，因此服务器
 *          用 NAT 后的 icmp_id 作为查找键，才能匹配 UDP 回包的目的端口。
 * - dport: 服务器上 tutuicmptunnel 使用的 UDP 端口，网络字节序
 *
 * 注：sport/dport 的命名源于键值构造时的位置约定，并非严格对应
 *     报文方向的源/目的。构造详见 egress/ingress 处理路径。
 */
struct session_key {
  struct in6_addr address;
  __be16          sport;
  __be16          dport;
};

/*
 * session_value: server 模式下会话的元数据
 * - age: 上次活跃时间戳（秒）
 * - uid: 该会话所属用户的 UID
 * - client_sport: 客户端原始 UDP 源端口（网络字节序）。
 *                 客户端初始设置 icmp_id = icmp_seq = 源端口；NAT 设备
 *                 改写 icmp_id 后，服务器在 ingress 侧把原始值（icmp_seq）
 *                 存入本字段。回包时复用为 ICMP seq，NAT 不碰 seq，因此
 *                 客户端收到后可完美还原为原始 UDP 源端口。
 *                 同时 user->icmp_id 在 ingress 时动态更新为 NAT 改写后的
 *                 新值，该值被用作 UDP 源端口（见 ingress 处理），因此
 *                 UDP 回包的目的端口即为此值，保证 egress 侧 session_map
 *                 查找始终有效。
 */
struct session_value {
  __u64  age;
  __u8   uid;
  __be16 client_sport; // 远程client的源端口
};

/*
 * tutu_session: session 的 netlink 传输包装
 */
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
