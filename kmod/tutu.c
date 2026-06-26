#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <crypto/algapi.h>
#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/icmp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/tcp.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/udp.h>
#include <net/checksum.h>
#include <net/ip6_checksum.h>

#if __has_include(<asm/unaligned.h>)
#include <asm/unaligned.h>
#else
#include <linux/unaligned.h>
#endif

#include "compat.h"
#include "defs.h"
#include "hashtab.h"
#include "tutuicmptunnel.h"

#include "net_proto.h"

LIST_HEAD(tutu_ifname_list);
DEFINE_MUTEX(tutu_ifname_lock);

/* Config structure protected by RCU: dynamic bitmap of allowed ifindex */
struct ifset {
  struct rcu_head rcu;
  bool            allow_all;                 /* 允许所有接口 */
  unsigned int    max_ifindex;               /* number of bits in bitmap */
  DECLARE_FLEX_ARRAY(unsigned long, bitmap); /* flexible array */
};

static struct ifset __rcu *g_ifset;
static DEFINE_MUTEX(g_ifset_mutex); // 保护ifnames和g_ifset

static unsigned int get_max_ifindex_locked(void) {
  struct net_device *dev;
  unsigned int       max_idx = 0;

  /* Device enumeration must be done under rtnl_lock */
  for_each_netdev(&init_net, dev) {
    if (dev->ifindex > max_idx)
      max_idx = dev->ifindex;
  }

  /* if no devices, ensure at least 1 bit so bitmap_alloc works */
  return max_idx ? max_idx : 1;
}

static struct ifset *ifset_alloc(unsigned int max_ifindex) {
  size_t        nbits  = (size_t) max_ifindex + 1; /* include index value as bit position */
  size_t        nlongs = BITS_TO_LONGS(nbits);
  size_t        size   = sizeof(struct ifset) + nlongs * sizeof(unsigned long);
  struct ifset *w      = kzalloc(size, GFP_KERNEL);

  if (w) {
    w->max_ifindex = max_ifindex;
  }
  return w;
}

static int build_ifset_from_list(struct ifset **out_new) {
  struct ifset            *w;
  unsigned int             max_idx;
  struct tutu_ifname_node *node;
  int                      err;

  /*
   * 1. 获取 RTNL 锁
   * 保护网络设备列表，确保 max_ifindex 稳定，
   * 且允许我们使用 __dev_get_by_name (无引用计数版本)
   */
  rtnl_lock();
  max_idx = get_max_ifindex_locked();

  /* 2. 分配 ifset 结构体 */
  w = ifset_alloc(max_idx);
  if (!w) {
    err = -ENOMEM;
    goto out_rtnl;
  }

  mutex_lock(&tutu_ifname_lock);
  // 如果列表为空，表示“允许所有接口”
  if (list_empty(&tutu_ifname_list)) {
    w->allow_all = true;
  } else {
    w->allow_all = false;

    /* 遍历链表 */
    list_for_each_entry(node, &tutu_ifname_list, list) {
      struct net_device *dev;

      dev = __dev_get_by_name(&init_net, node->name);

      if (!dev) {
        /* 接口名在配置里，但系统里没这个网卡 */
        pr_warn_ratelimited("interface '%s' not found (ignored)\n", node->name);
        continue;
      }

      /* 设置位图 */
      if (dev->ifindex <= w->max_ifindex)
        __set_bit(dev->ifindex, w->bitmap);
    }
  }

  mutex_unlock(&tutu_ifname_lock);

  *out_new = w;
  err      = 0;

out_rtnl:
  rtnl_unlock();
  return err;
}

int ifset_reload_config(void) {
  struct ifset *newcfg;
  struct ifset *oldcfg;
  int           err;

  err = build_ifset_from_list(&newcfg);
  if (err)
    return err;

  mutex_lock(&g_ifset_mutex);
  oldcfg = rcu_replace_pointer(g_ifset, newcfg, lockdep_is_held(&g_ifset_mutex));
  if (oldcfg)
    kfree_rcu(oldcfg, rcu);
  mutex_unlock(&g_ifset_mutex);
  return 0;
}

static void free_ifset(void) {
  struct ifset            *oldcfg;
  struct tutu_ifname_node *node, *tmp;

  mutex_lock(&g_ifset_mutex);
  oldcfg = rcu_replace_pointer(g_ifset, NULL, lockdep_is_held(&g_ifset_mutex));
  mutex_unlock(&g_ifset_mutex);

  if (oldcfg)
    kfree_rcu(oldcfg, rcu);

  mutex_lock(&tutu_ifname_lock);
  list_for_each_entry_safe(node, tmp, &tutu_ifname_list, list) {
    list_del(&node->list);
    kfree(node);
  }
  mutex_unlock(&tutu_ifname_lock);
}

bool net_has_device(const char *dev_name) {
  struct net_device *dev;
  bool               found = false;

  rtnl_lock();
  dev = dev_get_by_name(&init_net, dev_name);
  if (dev) {
    found = true;
    dev_put(dev);
  }
  rtnl_unlock();

  return found;
}

/* Decide if interface is allowed:
 * - if ifnames empty/NULL: allow all
 * - else: allow only if bit set
 * No explicit rcu_read_lock: single deref + read-only access, safe with kfree_rcu lifecycle.
 */
static bool iface_allowed(int ifindex) {
  WARN_ON_ONCE(!rcu_read_lock_held());

  bool allowed = true;

  const struct ifset *cfg = rcu_dereference(g_ifset);

  if (cfg) {
    if (cfg->allow_all) {
      allowed = true;
    } else {
      allowed = (ifindex > 0 && ifindex <= cfg->max_ifindex && test_bit(ifindex, cfg->bitmap));
    }
  }

  return allowed;
}

struct tutu_config_rcu {
  struct rcu_head    rcu;
  struct tutu_config inner;
};

static struct tutu_config_rcu g_cfg_init = {
  .inner =
    {
      .session_max_age = 60,
      .is_server       = 0,
    },
};

struct tutu_stats_k {
  atomic64_t packets_processed;
  atomic64_t packets_dropped;
  atomic64_t checksum_errors;
  atomic64_t fragmented;
  atomic64_t gso;
};

struct tutu_htab *egress_peer_map;
struct tutu_htab *ingress_peer_map;
struct tutu_htab *session_map;
struct tutu_htab *user_map;

static struct tutu_config_rcu __rcu *g_cfg_ptr;

DEFINE_PER_CPU(struct tutu_stats_k, g_stats_percpu);

static __always_inline __wsum udp_pseudoheader_sum(struct iphdr *iph, struct udphdr *udp) {
  return csum_tcpudp_nofold(iph->saddr, iph->daddr, ntohs(udp->len), IPPROTO_UDP, 0);
}

// icmp_len: icmp头部+icmp负载长度，主机字序
static __always_inline __wsum icmpv6_pseudoheader_sum(struct ipv6hdr *ip6h, u32 icmp_len) {
  return csum_unfold(~csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr, icmp_len, IPPROTO_ICMPV6, 0));
}

// 结果为大端
static __always_inline __wsum udp_header_sum(struct udphdr *udp) {
  return udp->source + udp->dest + udp->len;
}

static void ipv6_copy(struct in6_addr *dst, const struct in6_addr *src) {
  *dst = *src;
}

/*
 * 解析 L2/L3/L4 头部
 *
 * 功能:
 *  - 解析 L2 (Ethernet, 可选) 和 L3 (IPv4/IPv6) 头部。
 *  - 确定 L4 协议类型和 L3/L4 头部的总长度。
 *  - 计算指向 L4 协议的字段 (protocol/nexthdr) 的精确偏移量。
 *
 * 参数:
 *  - skb: 指向 skb 上下文的指针。
 *  - ip_type: [输出] ip协议类型: 4或者6
 *  - l2_len: [输出] L2 头部长度。
 *  - ip_hdr_len: [输出] L3 头部长度 (IPv4 或 IPv6 + 扩展头)。
 *  - ip_proto: [输出] L4 协议的值 (例如 IPPROTO_UDP)。
 *  - ip_proto_offset: [输出] 指向L4协议的字段(ip.protocol或最后一个ipv6.nexthdr)的偏移量，从数据包起始计算。
 *  - hdr_len: [输出] 从数据包开始到 L4 头部起始位置的总偏移量 (l2_len + ip_hdr_len)。
 *
 * 返回值:
 *  - 0: 成功。
 *  - -1: 失败（包太短、未知协议等）。
 */
static int parse_headers(struct sk_buff *skb, u32 *ip_type, u32 *l2_len, u32 *ip_hdr_len, u8 *ip_proto, u32 *ip_proto_offset,
                         u32 *hdr_len) {
  u32 local_l2_len = skb_network_offset(skb);
  int err          = -EINVAL;

  /* 初始化输出参数 */
  *ip_type = *l2_len = *ip_hdr_len = *ip_proto_offset = *hdr_len = *ip_proto = 0;

  if (skb->protocol == htons(ETH_P_IP)) {
    if (!pskb_may_pull(skb, local_l2_len + sizeof(struct iphdr)))
      return err;
    const struct iphdr *iph              = ip_hdr(skb);
    u32                 local_ip_hdr_len = iph->ihl * 4;

    if (local_ip_hdr_len < sizeof(*iph))
      return err;

    if (!pskb_may_pull(skb, local_l2_len + local_ip_hdr_len))
      return err;

    iph              = ip_hdr(skb);
    *ip_type         = 4;
    *ip_proto        = iph->protocol;
    *ip_hdr_len      = local_ip_hdr_len;
    *l2_len          = local_l2_len;
    *hdr_len         = local_l2_len + local_ip_hdr_len;
    *ip_proto_offset = local_l2_len + offsetof(struct iphdr, protocol);

    err = 0;
  } else if (skb->protocol == htons(ETH_P_IPV6)) {
    if (!pskb_may_pull(skb, local_l2_len + sizeof(struct ipv6hdr)))
      return err;
    const struct ipv6hdr *ipv6 = ipv6_hdr(skb);

    u8  next_hdr           = ipv6->nexthdr;
    u32 local_proto_offset = local_l2_len + offsetof(struct ipv6hdr, nexthdr);
    u32 current_hdr_start  = local_l2_len + sizeof(*ipv6);
    int i;

    /* 遍历扩展头（最多 8 层，避免死循环） */
    for (i = 0; i < 8; i++) {
      struct ipv6_opt_hdr *opt_hdr;

      if (!ipv6_ext_hdr(next_hdr))
        break;

      /* 以下扩展头无法按 ipv6_opt_hdr 通用规则安全解析，直接拒绝：
       * - FRAGMENT: 长度规则不同，且分片包无法在不重组前提下做协议转换
       * - AUTH:     长度规则为 (hdrlen+2)*4，与通用规则 (hdrlen+1)*8 不同
       * - ESP:      不具有标准扩展头格式
       * - NONE:     无后续负载，不应作为扩展头继续解析
       */
      if (next_hdr == NEXTHDR_FRAGMENT || next_hdr == NEXTHDR_AUTH ||
          next_hdr == NEXTHDR_ESP || next_hdr == NEXTHDR_NONE)
        return err;

      if (!pskb_may_pull(skb, current_hdr_start + sizeof(struct ipv6_opt_hdr)))
        return err;

      opt_hdr = (typeof(opt_hdr)) (skb->data + current_hdr_start);

      // 更新协议字段偏移量为当前扩展头的 nexthdr 字段的偏移量
      local_proto_offset = current_hdr_start + offsetof(struct ipv6_opt_hdr, nexthdr);
      next_hdr           = opt_hdr->nexthdr;
      current_hdr_start += (opt_hdr->hdrlen + 1) << 3;
      // 守护
      if (current_hdr_start > skb->len)
        return -EINVAL;
    }

    *ip_type         = 6;
    *ip_proto        = next_hdr;
    *ip_hdr_len      = current_hdr_start - local_l2_len;
    *l2_len          = local_l2_len;
    *hdr_len         = current_hdr_start;
    *ip_proto_offset = local_proto_offset;
    err              = 0;
  }

  return err;
}

// 从icmp头部生成检验和，跳过了checksum本身（视为0)
static __wsum icmphdr_cksum(struct icmphdr *icmp) {
  // 计算ICMP头部校验和
  __wsum  sum;
  __be16 *p = (__be16 *) icmp;

  sum = p[0];
  // 跳过checksum
  sum += p[2];
  sum += p[3];

  return sum;
}

// 从icmp检验和恢复udp负载的检验和, 支持icmpv6
static __wsum recover_payload_csum_from_icmp(struct icmphdr *icmp, struct ipv6hdr *ipv6, u32 payload_len) {
  __wsum payload_sum = csum_unfold(~icmp->checksum);
  __wsum icmphdr_sum = icmphdr_cksum(icmp);

  // 将icmp检验和取反后，减去掉icmp头部检验和
  payload_sum = csum_sub(payload_sum, icmphdr_sum);

  if (ipv6) {
    // IPV6下，icmp也要计算icmp伪头部:
    // 由于icmp检验和 = ~(icmp伪头部 + icmp头部 + icmp负载)
    // 所以icmp负载 = ~icmp检验和 - icmp头部 - icmp伪头部
    // 于是还需要减去IPv6 icmp伪头部
    __wsum csum = icmpv6_pseudoheader_sum(ipv6, payload_len + sizeof(struct icmphdr));
    payload_sum = csum_sub(payload_sum, csum);
  }

  return payload_sum;
}

// 检查并删除过期会话
static int check_age(struct tutu_config *cfg, struct session_key *lookup_key, struct session_value *value_ptr) {
  // 检查下age
  __u64 age = value_ptr->age;
  __u64 now = ktime_get_seconds();

  if (!age || now < age || now - age >= cfg->session_max_age) {
    // 太老，需要跳过并删除这个key
    pr_debug("session_map entry: age %llu too old: now is %llu\n", age, now);
    tutu_map_delete_elem(session_map, lookup_key);
    return -1;
  }

  // 此时需要更新下会话的寿命，否则过了update_interval会话就消失
  // 可以过1秒才更新，避免大量包造成过大压力
  if (now - age >= 1) {
    struct session_value new_value = *value_ptr;
    new_value.age                  = now;
    int err                        = tutu_map_update_elem(session_map, lookup_key, &new_value, TUTU_EXIST);
    pr_debug("session updated: age: %llu: %d\n", now, err);
  }

  return 0;
}

static __always_inline bool tutu_xor_enabled(const __u8 *key, __u8 key_len) {
  return key && key_len > 0 && key_len <= TUTU_XOR_KEY_MAX;
}

enum tutu_xor_dir {
  TUTU_XOR_DIR_C2S = 0,
  TUTU_XOR_DIR_S2C = 1,
};

/*
 * client egress  == server ingress == C2S
 * client ingress == server egress  == S2C
 */
static __always_inline enum tutu_xor_dir tutu_xor_dir(bool is_ingress, bool is_server) {
  return is_ingress == is_server ? TUTU_XOR_DIR_C2S : TUTU_XOR_DIR_S2C;
}

static __always_inline u32 tutu_mix32(u32 x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

static __always_inline u32 tutu_xor_key_start(__be16 icmp_seq, u32 payload_len, bool is_ingress, bool is_server, const u8 *key,
                                              u32 key_len) {
  enum tutu_xor_dir dir  = tutu_xor_dir(is_ingress, is_server);
  u32               seq  = ntohs(icmp_seq);
  u32               base = 0;
  u32               salt;

  if (key_len >= 4)
    base = get_unaligned_le32(key);
  else if (key_len > 0)
    base = key[0] * 0x01010101U;

  salt = base ^ (dir == TUTU_XOR_DIR_C2S ? 0x9e3779b9U : 0x7f4a7c15U);

  u32 mix_len = tutu_mix32(payload_len);
  return tutu_mix32(seq ^ salt ^ mix_len);
}

static inline void xor_with_key(u8 *p, u32 len, const u8 *key, u32 key_len, u32 key_start) {
  u32 offset = 0;
  u32 key_off;

  if (!key_len)
    return;

  key_off = key_start % key_len;

  while (offset < len) {
    u32 chunk = min(key_len - key_off, len - offset);

    crypto_xor(p + offset, key + key_off, chunk);

    offset += chunk;
    key_off = 0;
  }
}

static int skb_xor_payload_linear(struct sk_buff *skb, u32 off, u32 len, const __u8 *key, __u8 key_len, u32 key_start) {
  __u8 *p;
  int   err;

  if (!len || !key_len)
    return 0;

  if (!pskb_may_pull(skb, off + len))
    return -ENOMEM;

  err = skb_ensure_writable(skb, off + len);
  if (err)
    return err;

  p = skb->data + off;
  xor_with_key(p, len, key, key_len, key_start);

  return 0;
}

static __always_inline int skb_store_bytes_linear(struct sk_buff *skb, unsigned int off, const void *from, unsigned int len) {
  /* 已经 pskb_may_pull() 线性化了相关区域，但写之前仍建议确保可写 */
  if (skb_ensure_writable(skb, off + len))
    return -ENOMEM;

  memcpy(skb->data + off, from, len);
  return 0;
}

// 有可能导致ipv4/ipv6指针失效
static __always_inline int skb_update_ipv4_checksum(struct sk_buff *skb, struct iphdr *ipv4, u32 l2_len, u32 old_proto,
                                                    u32 new_proto) {
  const __be16 old_word = htons((ipv4->ttl << 8) | old_proto);
  const __be16 new_word = htons((ipv4->ttl << 8) | new_proto);

  if (old_word != new_word) {
    if (skb_ensure_writable(skb, l2_len + sizeof(struct iphdr)))
      return -ENOMEM;
    ipv4 = ip_hdr(skb);
    csum_replace2(&ipv4->check, old_word, new_word);
  }

  return 0;
}

static int force_sw_checksum = 0;
module_param(force_sw_checksum, int, 0644);
MODULE_PARM_DESC(force_sw_checksum, "Force software checksum calculation for all ICMP packets");

static int skb_change_type(struct sk_buff *skb, u32 ip_type, u32 l2_len, u32 ip_hdr_len, u32 ip_proto_offset, u32 l4_offset) {
  u8   ip_proto;
  bool use_partial;
  int  err;

  if (ip_proto_offset >= skb->len || l4_offset >= skb->len)
    return -EINVAL;

  ip_proto = skb->data[ip_proto_offset];

  /*
   * CHECKSUM_PARTIAL 要求 csum_start 指向 L4 头起始。
   * 协议从 UDP 改为 ICMP/ICMPv6 后 L4 头位置不变（同 8 字节），
   * 但仍需校验 csum_start 是否确实指向 l4_offset；不匹配则退回软件计算。
   */
  use_partial = !force_sw_checksum && skb->ip_summed == CHECKSUM_PARTIAL && skb_checksum_start_offset(skb) == (int) l4_offset;

  if (ip_type == 4 && ip_proto == IPPROTO_ICMP) {
    struct iphdr   *iph = ip_hdr(skb);
    struct icmphdr *icmph;
    size_t          icmp_len;

    if (ntohs(iph->tot_len) < ip_hdr_len)
      return -EINVAL;
    icmp_len = ntohs(iph->tot_len) - ip_hdr_len;
    icmp_len = min_t(size_t, icmp_len, skb->len - l4_offset);
    if (icmp_len < sizeof(*icmph))
      return -EINVAL;

    if (!use_partial) {
      err = skb_ensure_writable(skb, l4_offset + icmp_len);
      if (unlikely(err))
        return err;

      icmph           = (struct icmphdr *) (skb->data + l4_offset);
      icmph->checksum = 0;
      icmph->checksum = csum_fold(csum_partial((char *) icmph, (int) icmp_len, 0));
      skb->ip_summed  = CHECKSUM_UNNECESSARY;
    } else {
      err = skb_ensure_writable(skb, l4_offset + sizeof(*icmph));
      if (unlikely(err))
        return err;

      icmph            = (struct icmphdr *) (skb->data + l4_offset);
      icmph->checksum  = 0;
      skb->csum_offset = offsetof(struct icmphdr, checksum);
    }
  } else if (ip_type == 6 && ip_proto == IPPROTO_ICMPV6) {
    struct ipv6hdr  *ip6h = ipv6_hdr(skb);
    struct icmp6hdr *icmp6h;
    unsigned int     ext_hdr_len;
    unsigned int     icmp_len;

    if (ip_hdr_len < sizeof(struct ipv6hdr))
      return -EINVAL;
    ext_hdr_len = ip_hdr_len - sizeof(struct ipv6hdr);
    if (ntohs(ip6h->payload_len) < ext_hdr_len)
      return -EINVAL;
    icmp_len = ntohs(ip6h->payload_len) - ext_hdr_len;
    icmp_len = min_t(size_t, icmp_len, skb->len - l4_offset);
    if (icmp_len < sizeof(*icmp6h))
      return -EINVAL;

    if (!use_partial) {
      __wsum csum;

      err = skb_ensure_writable(skb, l4_offset + icmp_len);
      if (unlikely(err))
        return err;

      ip6h                = ipv6_hdr(skb);
      icmp6h              = (struct icmp6hdr *) (skb->data + l4_offset);
      icmp6h->icmp6_cksum = 0;
      csum                = csum_partial((char *) icmp6h, (int) icmp_len, 0);
      icmp6h->icmp6_cksum = csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr, icmp_len, IPPROTO_ICMPV6, csum);
      skb->ip_summed      = CHECKSUM_UNNECESSARY;
    } else {
      err = skb_ensure_writable(skb, l4_offset + sizeof(*icmp6h));
      if (unlikely(err))
        return err;

      icmp6h              = (struct icmp6hdr *) (skb->data + l4_offset);
      icmp6h->icmp6_cksum = 0;
      skb->csum_offset    = offsetof(struct icmp6hdr, icmp6_cksum);
    }
  }

  return 0;
}

static int update_session_map(struct user_info *user, u8 uid, __be16 icmp_seq) {
  int                err;
  struct session_key key = {.dport = user->dport, .sport = user->icmp_id, .address = user->address};
  __u64              now = ktime_get_seconds();

  struct session_value *exist = tutu_map_lookup_elem(session_map, &key);

  if (exist) {
    bool client_sport_changed = exist->client_sport != icmp_seq;
    bool uid_changed          = exist->uid != uid;
    bool age_exceed_1s        = (now > exist->age) && (now - exist->age > 1);

    // 若没有变化且未超过 1 秒，不更新
    if (!client_sport_changed && !uid_changed && !age_exceed_1s) {
      return 0;
    }
  }

  struct session_value value = {
    .uid          = uid,
    .age          = now,
    .client_sport = icmp_seq,
  };
  err = tutu_map_update_elem(session_map, &key, &value, TUTU_ANY);
  pr_debug("update session_map: sport %5u, dport: %5u, age: %llu: ret: %d\n", ntohs(key.sport), ntohs(key.dport), value.age,
           err);

  return err;
}

/*
 * 在可能导致 skb->data 位置变化的操作之后，重新恢复缓存的头部指针。
 *
 * udp 和 icmp 是同一个 L4 头部位置的两种协议视图：
 * skb->data + ip_end。
 *
 * 只有和当前包真实协议匹配的那个视图，在语义上才是有效的。
 */
#define RESTORE_SKB_POINTERS()                                                                                                 \
  do {                                                                                                                         \
    if (ip_type == 4) {                                                                                                        \
      ipv4 = ip_hdr(skb);                                                                                                      \
      ipv6 = NULL;                                                                                                             \
    } else if (ip_type == 6) {                                                                                                 \
      ipv4 = NULL;                                                                                                             \
      ipv6 = ipv6_hdr(skb);                                                                                                    \
    } else {                                                                                                                   \
      err = NF_ACCEPT;                                                                                                         \
      goto err_cleanup;                                                                                                        \
    }                                                                                                                          \
                                                                                                                               \
    udp  = (struct udphdr *) (skb->data + ip_end);                                                                             \
    icmp = (struct icmphdr *) (skb->data + ip_end);                                                                            \
  } while (0)

/*
 * egress_hook_func: 出向 UDP → ICMP 转换
 *
 * 核心机制：
 * - UDP 与 ICMP 头部大小相同（8 字节），原地替换即可
 * - icmp_id = icmp_seq = udp->source，每个 UDP 源端口独占一个 ICMP ID，
 *   因此多源端口应用天然复用，无需额外连接表
 * - Server 模式：用 udp->dest（即 NAT 后的 icmp_id）查 session_map，
 *   匹配后回包时复用 client_sport（原始值）重建 ICMP seq
 * - Client 模式：用 egress_peer_map 查隧道服务器配置
 */
static unsigned int egress_hook_func(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
  int                  err;
  struct tutu_config   config, *cfg = &config;
  struct tutu_stats_k *stat = this_cpu_ptr(&g_stats_percpu);

  if (!skb || !ip_hdr(skb)) {
    return NF_ACCEPT;
  }

  rcu_read_lock();
  struct tutu_config_rcu *p = rcu_dereference(g_cfg_ptr);
  if (likely(p)) {
    config = p->inner;
  } else {
    pr_err_ratelimited("no config?\n");
    err = NF_ACCEPT;
    goto err_cleanup;
  }

  u32 ip_end = 0, ip_proto_offset = 0, l2_len, ip_hdr_len, ip_type;
  u8  ip_proto;

  {
    const struct net_device *out     = state->out;
    int                      ifindex = out ? out->ifindex : (skb ? skb->skb_iif : 0);

    if (!iface_allowed(ifindex)) {
      err = NF_ACCEPT;
      goto err_cleanup;
    }
  }

  try2_ok(parse_headers(skb, &ip_type, &l2_len, &ip_hdr_len, &ip_proto, &ip_proto_offset, &ip_end));
#if 0
  pr_debug("parse headers: ip_type: %d, l2_len: %d, ip_hdr_len: %d, ip_proto: %d, ip_proto_offset: %d, ip_end: %d\n", ip_type,
          l2_len, ip_hdr_len, ip_proto, ip_proto_offset, ip_end);
#endif
  try2_ok(ip_proto == IPPROTO_UDP ? 0 : -1);
  // 重新pull 整个以太网+ip头部+udp头部
  // 不需要整个udp包：因为udp负载没有被修改过，也不需要检查udp负载长度
  try2_ok(pskb_may_pull(skb, ip_end + sizeof(struct udphdr)) ? 0 : -1);

  struct udphdr  *udp  = NULL;
  struct icmphdr *icmp = NULL;
  struct iphdr   *ipv4 = NULL;
  struct ipv6hdr *ipv6 = NULL;

  RESTORE_SKB_POINTERS();

  (void) icmp;

  // 非法的udp长度
  if (ntohs(udp->len) < sizeof(*udp)) {
    err = NF_ACCEPT;
    goto err_cleanup;
  }

  struct user_info *user = NULL;
  __be16            icmp_id, icmp_seq;
  u8                icmp_type = 0, uid;

  const __u8 *xor_key     = NULL;
  __u8        xor_key_len = 0;

  if (ipv4) {
    // UDP payload length
    u16 udp_payload_len = ntohs(udp->len) - sizeof(*udp);
    pr_debug("Outgoing UDP: %pI4:%5u -> %pI4:%5u, length: %u\n", &ipv4->saddr, ntohs(udp->source), &ipv4->daddr,
             ntohs(udp->dest), udp_payload_len);
  } else if (ipv6) {
    u16 udp_payload_len = ntohs(udp->len) - sizeof(*udp);
    pr_debug("Outgoing UDP: %pI6:%5u -> %pI6:%5u, length: %u\n", &ipv6->saddr, ntohs(udp->source), &ipv6->daddr,
             ntohs(udp->dest), udp_payload_len);
  }

  if (cfg->is_server) {
    // Server mode: Find user by destination IP and source port
    struct session_key lookup_key = {
      .dport = udp->source,
      .sport = udp->dest,
    };

    if (ipv4) {
      struct in6_addr in6;

      ipv6_addr_set_v4mapped(get_unaligned(&ipv4->daddr), &in6);
      lookup_key.address = in6;
    } else if (ipv6) {
      ipv6_copy(&lookup_key.address, &ipv6->daddr);
    }

    pr_debug("search port: %5u, sport: %5u\n", ntohs(lookup_key.dport), ntohs(lookup_key.sport));
    struct session_value *value_ptr = tutu_map_lookup_elem(session_map, &lookup_key);

    if (!value_ptr) {
      if (ipv4) {
        pr_debug("cannot get uid: %pI4:%5u -> %pI4:%5u\n", &ipv4->saddr, ntohs(udp->source), &ipv4->daddr, ntohs(udp->dest));
      } else if (ipv6) {
        pr_debug("cannot get uid: %pI6:%5u -> %pI6:%5u\n", &ipv6->saddr, ntohs(udp->source), &ipv6->daddr, ntohs(udp->dest));
      }

      err = NF_ACCEPT;
      goto err_cleanup;
    }

    uid      = value_ptr->uid;
    icmp_seq = value_ptr->client_sport;
    try2_ok(check_age(cfg, &lookup_key, value_ptr), "check age: %ld\n", _ret);
    user = try2_p_ok(tutu_map_lookup_elem(user_map, &uid), "invalid uid: %u\n", uid);

    if (skb_is_gso(skb)) {
      pr_debug("cannot handle GSO packets: length %u\n", skb->len);
      atomic64_inc(&stat->gso);
      err = NF_DROP;
      goto err_cleanup;
    }

    if (ipv4) {
      icmp_type = ICMP_ECHO_REPLY;
    } else if (ipv6) {
      icmp_type = ICMP6_ECHO_REPLY;
    }

    icmp_id     = user->icmp_id;
    xor_key     = user->xor_key;
    xor_key_len = user->xor_key_len;
  } else {
    struct egress_peer_key peer_key = {
      .port = udp->dest,
    };

    if (ipv4) {
      pr_debug("egress: udp: %pI4:%5u\n", &ipv4->saddr, ntohs(udp->dest));
      ipv6_addr_set_v4mapped(get_unaligned(&ipv4->daddr), &peer_key.address);
    } else if (ipv6) {
      pr_debug("egress: udp: src %pI6:%5u\n", &ipv6->saddr, ntohs(udp->dest));
      ipv6_copy(&peer_key.address, &ipv6->daddr);
    }

    struct egress_peer_value *peer_value = try2_p_ok(tutu_map_lookup_elem(egress_peer_map, &peer_key),
                                                     "egress client: unrelated packet\n");
    {
      if (skb_is_gso(skb)) {
        pr_debug("cannot handle GSO packets: length %u\n", skb->len);
        atomic64_inc(&stat->gso);
        err = NF_DROP;
        goto err_cleanup;
      }
    }

    if (ipv4) {
      // 如果UDP包为分片（包括第一个包或后续包），无法重写后续包没有的UDP头部，直接丢包
      if ((ntohs(ipv4->frag_off) & 0x1FFF) != 0 || (ntohs(ipv4->frag_off) & 0x2000) != 0) {
        // 检查分片偏移和MF标志，只要有分片相关标志就丢弃
        pr_debug("drop fragmented UDP packet\n");
        atomic64_inc(&stat->fragmented);
        err = NF_DROP;
        goto err_cleanup;
      }
    }

    uid = peer_value->uid;

    if (ipv4) {
      icmp_type = ICMP_ECHO_REQUEST;
    } else {
      icmp_type = ICMP6_ECHO_REQUEST;
    }

    // icmp_id也使用源端口, 服务器有可能看到被nat修改后的新值
    icmp_id = icmp_seq = udp->source;
    xor_key            = peer_value->xor_key;
    xor_key_len        = peer_value->xor_key_len;
  }

  atomic64_inc(&stat->packets_processed);

  struct udphdr old_udp = *udp;

  _Static_assert(sizeof(struct icmphdr) == sizeof(struct udphdr), "ICMP and UDP header sizes must match");

  if (tutu_xor_enabled(xor_key, xor_key_len)) {
    u32 l4_len, payload_off, payload_len;

    if (ipv4) {
      if (ntohs(ipv4->tot_len) < ip_hdr_len + sizeof(struct udphdr)) {
        err = NF_ACCEPT;
        goto err_cleanup;
      }

      l4_len = ntohs(ipv4->tot_len) - ip_hdr_len;
    } else if (ipv6) {
      u32 ip_payload_len = ntohs(ipv6->payload_len);
      u32 ext_len;

      if (ip_hdr_len < sizeof(struct ipv6hdr)) {
        err = NF_ACCEPT;
        goto err_cleanup;
      }

      ext_len = ip_hdr_len - sizeof(struct ipv6hdr);

      if (ip_payload_len < ext_len + sizeof(struct udphdr)) {
        err = NF_ACCEPT;
        goto err_cleanup;
      }

      l4_len = ip_payload_len - ext_len;
    } else {
      err = NF_ACCEPT;
      goto err_cleanup;
    }

    payload_off = ip_end + sizeof(struct udphdr);
    payload_len = l4_len - sizeof(struct udphdr);

    u32 key_start = tutu_xor_key_start(icmp_seq, payload_len, false, !!cfg->is_server, xor_key, xor_key_len);

    err = skb_xor_payload_linear(skb, payload_off, payload_len, xor_key, xor_key_len, key_start);
    if (err) {
      atomic64_inc(&stat->packets_dropped);
      pr_debug("skb_xor_payload_linear failed: %d\n", err);
      err = NF_DROP;
      goto err_cleanup;
    }

    RESTORE_SKB_POINTERS();
  }

  // Create an ICMP header in place of the UDP header
  struct icmphdr icmp_hdr = {
    .type     = icmp_type,
    .code     = uid,
    .un       = {.echo = {.id = icmp_id, .sequence = icmp_seq}},
    .checksum = 0,
  };

  if (!old_udp.check) {
    pr_debug("udp must has checksum\n");
    atomic64_inc(&stat->packets_dropped);
    err = NF_DROP;
    goto err_cleanup;
  }

  pr_debug("icmp hdr checksum: 0x%04x\n", ntohs(icmp_hdr.checksum));

  // 将UDP头部替换为ICMP头部
  err = skb_store_bytes_linear(skb, ip_end, &icmp_hdr, sizeof(icmp_hdr));
  if (err) {
    atomic64_inc(&stat->packets_dropped);
    pr_debug("skb_store_bytes_linear failed: %d\n", err);
    err = NF_DROP;
    goto err_cleanup;
  }

  RESTORE_SKB_POINTERS();

  // 修改IP协议为ICMP
  // 只有ipv4才需要修复ip头部检验和
  u8 new_proto = IPPROTO_ICMPV6;
  if (ipv4) {
    new_proto = IPPROTO_ICMP;

    err = skb_update_ipv4_checksum(skb, ipv4, l2_len, IPPROTO_UDP, new_proto);
    if (err) {
      atomic64_inc(&stat->packets_dropped);
      pr_debug("skb_update_ipv4_checksum failed: %d\n", err);
      err = NF_DROP;
      goto err_cleanup;
    }

    RESTORE_SKB_POINTERS();
  }

  err = skb_store_bytes_linear(skb, ip_proto_offset, &new_proto, sizeof(new_proto));
  if (err) {
    atomic64_inc(&stat->packets_dropped);
    pr_debug("skb_store_bytes_linear failed: %d\n", err);
    err = NF_DROP;
    goto err_cleanup;
  }

  RESTORE_SKB_POINTERS();

  // ipv4: 如果是硬件offload：设置csum_offset为icmphdr->checksum位置。不需要修改csum_start，继续硬件offload
  // ipv4: 如果是软件计算: 重新算整个icmp的检验和，停止硬件计算
  // ipv6: 重新算整个icmpv6的检验和，停止硬件计算
  err = skb_change_type(skb, ip_type, l2_len, ip_hdr_len, ip_proto_offset, ip_end);
  if (err) {
    atomic64_inc(&stat->packets_dropped);
    pr_debug("skb_change_type failed: %d\n", err);
    err = NF_DROP;
    goto err_cleanup;
  }

  RESTORE_SKB_POINTERS();
  {
    u16 udp_payload_len = ntohs(old_udp.len) - sizeof(*udp);
    pr_debug("  Rebuilt ICMP: id: %u, seq: %u, type: %u, code: %u, length: %u\n", ntohs(icmp_hdr.un.echo.id),
             ntohs(icmp_hdr.un.echo.sequence), icmp_type, uid, udp_payload_len);
    // dump_skb(skb);
  }

  err = NF_ACCEPT;
err_cleanup:
  rcu_read_unlock();
  return err;
}

// 更新udp检验和
static void update_udp_cksum(struct iphdr *ipv4, struct ipv6hdr *ipv6, struct udphdr *udp, __wsum payload_sum) {
  __wsum total_sum;

  // 先把 UDP头部的检验和和负载的检验和加在一起
  total_sum = csum_add(udp_header_sum(udp), payload_sum);

  if (ipv4) {
    // 加上 IPv4 伪头部
    __wsum pseudo_sum = udp_pseudoheader_sum(ipv4, udp);
    // 折叠
    udp->check = csum_fold(csum_add(total_sum, pseudo_sum));
  } else if (ipv6) {
    // 直接使用csum_ipv6_magic()完成
    udp->check = csum_ipv6_magic(&ipv6->saddr, &ipv6->daddr, ntohs(udp->len), IPPROTO_UDP, total_sum);
  } else {
    WARN_ONCE(1, "neither ipv4 nor ipv6 packet");
    udp->check = 0;
  }

  // rfc768规定
  if (!udp->check)
    udp->check = CSUM_MANGLED_0;
}

/*
 * ingress_hook_func: 入向 ICMP → UDP 转换
 *
 * 核心机制：
 * - 提取 ICMP code 作为 uid，验证客户端地址
 * - 动态更新 user->icmp_id 为 NAT 改写后的最新值，保证后续 egress 查找有效
 * - 用 (icmp_id, dport) 作为 session_key 插入/更新 session_map；
 *   icmp_seq 存入 client_sport，作为原始 UDP 源端口的"备份通道"
 * - NAT 设备通常改写 icmp_id 但不动 icmp_seq，因此：
 *   · icmp_id 走公网标识（NAT 通道），用于服务器 egress 查找
 *   · icmp_seq 走原始标识（内网通道），客户端用其重建 UDP 目的端口
 * - 客户端重建时只看 icmp_seq 作为原始源端口，不依赖 icmp_id，
 *   即使 NAT 通过别的会话的 icmp_id 将包还原回来，seq 通道仍保证数据不串
 */
static unsigned int ingress_hook_func(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
  int                  err;
  struct tutu_config   config, *cfg = &config;
  struct tutu_stats_k *stat = this_cpu_ptr(&g_stats_percpu);

  if (!skb || !ip_hdr(skb)) {
    return NF_ACCEPT;
  }

  rcu_read_lock();
  struct tutu_config_rcu *p = rcu_dereference(g_cfg_ptr);
  if (likely(p)) {
    config = p->inner;
  } else {
    pr_err_ratelimited("no config?\n");
    err = NF_ACCEPT;
    goto err_cleanup;
  }

  u32 ip_end = 0, ip_proto_offset = 0, l2_len, ip_hdr_len, ip_type;
  u8  ip_proto;

  {
    const struct net_device *in      = state->in;
    int                      ifindex = in ? in->ifindex : (skb ? skb->skb_iif : 0);

    if (!iface_allowed(ifindex)) {
      err = NF_ACCEPT;
      goto err_cleanup;
    }
  }

  try2_ok(parse_headers(skb, &ip_type, &l2_len, &ip_hdr_len, &ip_proto, &ip_proto_offset, &ip_end));
#if 0
  pr_debug("parse headers: ip_type: %d, l2_len: %d, ip_hdr_len: %d, ip_proto: %d, ip_proto_offset: %d, ip_end: %d\n", ip_type,
          l2_len, ip_hdr_len, ip_proto, ip_proto_offset, ip_end);
#endif
  try2_ok((ip_proto == IPPROTO_ICMP || ip_proto == IPPROTO_ICMPV6) ? 0 : -1);
  // 重新pull 整个以太网+ip头部+icmp/icmp6头部
  try2_ok(pskb_may_pull(skb, ip_end + sizeof(struct icmphdr)) ? 0 : -1, "pull data failed: %ld\n", _ret);
  _Static_assert(sizeof(struct icmphdr) == sizeof(struct icmp6hdr), "ICMP and ICMPv6 header sizes must match");

  struct iphdr   *ipv4 = NULL;
  struct ipv6hdr *ipv6 = NULL;
  struct icmphdr *icmp = NULL;
  struct udphdr  *udp  = NULL;

  RESTORE_SKB_POINTERS();

  (void) udp;

  {
    // 至少应该有l3头部长度再加上icmp头部
    u32 least_size = ip_hdr_len + sizeof(struct icmphdr);

    // 检查ipv4/ipv6报文长度是否合理
    if (ipv4) {
      // IPv4: icmp报文全长必须足够
      if (ntohs(ipv4->tot_len) < least_size) {
        err = NF_ACCEPT;
        goto err_cleanup;
      }
    } else if (ipv6) {
      // 守护
      if (ip_hdr_len < sizeof(struct ipv6hdr)) {
        err = NF_ACCEPT;
        goto err_cleanup;
      }
      // IPV6的payload_len: 不包括基本头部的剩余数据包长度
      // 加上ipv6头部后必须足够
      if (sizeof(struct ipv6hdr) + ntohs(ipv6->payload_len) < least_size) {
        err = NF_ACCEPT;
        goto err_cleanup;
      }
    }
  }

  // 由于icmp和icmp6hdr大小相等，而且前4字节完全等价，我们使用icmphdr作为表示icmp头部的类型
  _Static_assert(sizeof(struct icmphdr) == sizeof(struct icmp6hdr), "ICMP and ICMPv6 header sizes must match");

  // Extract UID from ICMP code
  u8     uid      = icmp->code;
  __be16 icmp_seq = icmp->un.echo.sequence;
  __be16 icmp_id  = icmp->un.echo.id;

  // udp源端口, 为0说明不合法(可能为ping产生), 直接丢弃
  try2_ok(icmp_seq != 0 ? 0 : -1);

  struct user_info *user = NULL;

  const __u8 *xor_key     = NULL;
  __u8        xor_key_len = 0;

  if (cfg->is_server) {
    // Server: Check for ECHO_REQUEST and valid UID
    if (ipv4) {
      try2_ok(icmp->type == ICMP_ECHO_REQUEST ? 0 : -1);
    } else if (ipv6) {
      try2_ok(icmp->type == ICMP6_ECHO_REQUEST ? 0 : -1);
    }
    // Find user by UID
    user = try2_p_ok(tutu_map_lookup_elem(user_map, &uid), "cannot get user: %u\n", uid);

    // 验证客户端地址与用户配置地址相等
    if (ipv4) {
      struct in6_addr in6;
      ipv6_addr_set_v4mapped(get_unaligned(&ipv4->saddr), &in6);
      try2_ok(!ipv6_addr_cmp(&user->address, &in6) ? 0 : -1, "unrelated client ipv4 address\n");
    } else if (ipv6) {
      try2_ok(!ipv6_addr_cmp(&user->address, &ipv6->saddr) ? 0 : -1, "unrelated client ipv6 address\n");
    }

    // 优化：只有发生变化才需要更新user_map
    // icmp_id：客户端更新了icmp_id，此时需要更新
    if (user->icmp_id != icmp_id) {
      struct user_info new_user = *user;

      new_user.icmp_id = icmp_id;
      err              = tutu_map_update_elem(user_map, &uid, &new_user, TUTU_EXIST);
      pr_debug("user_map updated: uid: %u, icmp_id: %u, %d\n", uid, icmp_id, err);

      /* 重新 lookup 获取最新 user 信息，确保后续使用的字段都是更新后的 */
      user = try2_p_ok(tutu_map_lookup_elem(user_map, &uid), "cannot get user: %u\n", uid);
    }

    // 需要更新session_map
    try2_ok(update_session_map(user, uid, icmp_seq), "update session map: %ld\n", _ret);
    xor_key     = user->xor_key;
    xor_key_len = user->xor_key_len;
  } else {
    if (ipv4) {
      try2_ok(icmp->type == ICMP_ECHO_REPLY ? 0 : -1);
    } else if (ipv6) {
      try2_ok(icmp->type == ICMP6_ECHO_REPLY ? 0 : -1);
    }
  }

  __be16 udp_src, udp_dst;

  if (cfg->is_server) {
    try2_ok(user ? 0 : -1, "no user?\n");
    // Server mode
    // 使用icmp_id作为源端口: nat转换后的值,一定是唯一的
    udp_src = user->icmp_id;
    udp_dst = user->dport;
  } else {
    struct ingress_peer_key peer_key = {
      .uid = icmp->code,
    };

    if (ipv4) {
      pr_debug("ingress: icmp: %pI4:%u\n", &ipv4->saddr, icmp->code);
      ipv6_addr_set_v4mapped(get_unaligned(&ipv4->saddr), &peer_key.address);
    } else if (ipv6) {
      pr_debug("ingress: icmp: src %pI6 id %u\n", &ipv6->saddr, icmp->code);
      ipv6_copy(&peer_key.address, &ipv6->saddr);
    }

    struct ingress_peer_value *peer_value = try2_p_ok(tutu_map_lookup_elem(ingress_peer_map, &peer_key),
                                                      "ingress client: unrelated packet\n");
    udp_src                               = peer_value->port;
    udp_dst                               = icmp_seq; // Use ICMP sequence as destination port
    xor_key                               = peer_value->xor_key;
    xor_key_len                           = peer_value->xor_key_len;
  }

  atomic64_inc(&stat->packets_processed);

  if (skb_is_gso(skb)) {
    pr_debug("cannot handle GSO packets: length %u\n", skb->len);
    atomic64_inc(&stat->gso);
    err = NF_DROP;
    goto err_cleanup;
  }

  struct in6_addr saddr, daddr;

  if (ipv4) {
    ipv6_addr_set_v4mapped(get_unaligned(&ipv4->saddr), &saddr);
    ipv6_addr_set_v4mapped(get_unaligned(&ipv4->daddr), &daddr);
  } else if (ipv6) {
    ipv6_copy(&saddr, &ipv6->saddr);
    ipv6_copy(&daddr, &ipv6->daddr);
  }

  // ICMP payload length
  u16 payload_len = 0;

  if (ipv4) {
    payload_len = ntohs(ipv4->tot_len) - ip_hdr_len - sizeof(struct icmphdr);
  } else if (ipv6) {
    // 先还原出IPv6包的物理总长(包含基本头)，再减去已解析的L3头和ICMPv6头
    payload_len = sizeof(struct ipv6hdr) + ntohs(ipv6->payload_len) - ip_hdr_len - sizeof(struct icmp6hdr);
  }

  {
    unsigned int need_len = ip_end + sizeof(struct icmphdr);

    if (tutu_xor_enabled(xor_key, xor_key_len) && payload_len > 0)
      need_len += payload_len;

    if (!pskb_may_pull(skb, need_len)) {
      err = NF_ACCEPT;
      goto err_cleanup;
    }

    RESTORE_SKB_POINTERS();
  }

  // Create a UDP header in place of the ICMP header
  struct udphdr udp_hdr = {
    .source = udp_src,
    .dest   = udp_dst,
    .len    = htons(sizeof(struct udphdr) + payload_len),
  };

  struct icmphdr old_icmp;

  old_icmp = *icmp;

  pr_debug("Incoming ICMP: %pI6 -> %pI6\n", &saddr, &daddr);
  pr_debug("  id: %u, seq: %u, length: %u,\n", ntohs(icmp_id), ntohs(icmp_seq), payload_len);

  // 只有ipv4才需要修复ip头部检验和
  u8 new_proto = IPPROTO_UDP;

  err = skb_store_bytes_linear(skb, ip_proto_offset, &new_proto, sizeof(new_proto));
  if (err) {
    atomic64_inc(&stat->packets_dropped);
    pr_debug("skb_store_bytes_linear failed: %d\n", err);
    err = NF_DROP;
    goto err_cleanup;
  }

  RESTORE_SKB_POINTERS();

  if (ipv4) {
    err = skb_update_ipv4_checksum(skb, ipv4, l2_len, IPPROTO_ICMP, new_proto);
    if (err) {
      atomic64_inc(&stat->packets_dropped);
      pr_debug("skb_update_ipv4_checksum failed: %d\n", err);
      err = NF_DROP;
      goto err_cleanup;
    }

    RESTORE_SKB_POINTERS();
  }

  _Static_assert(sizeof(struct icmphdr) == sizeof(struct udphdr), "ICMP and UDP header sizes must match");
  _Static_assert(sizeof(udp_hdr) == 8, "ICMP and UDP header sizes must match");

  {
    __wsum       payload_sum;
    unsigned int payload_off = ip_end + sizeof(struct icmphdr);

    if (tutu_xor_enabled(xor_key, xor_key_len) && payload_len > 0) {
      u32 key_start = tutu_xor_key_start(icmp_seq, payload_len, true, !!cfg->is_server, xor_key, xor_key_len);

      err = skb_xor_payload_linear(skb, payload_off, payload_len, xor_key, xor_key_len, key_start);
      if (err) {
        atomic64_inc(&stat->packets_dropped);
        pr_debug("skb_xor_payload_linear failed: %d\n", err);
        err = NF_DROP;
        goto err_cleanup;
      }

      RESTORE_SKB_POINTERS();

      payload_sum = csum_partial(skb->data + payload_off, payload_len, 0);
    } else {
      payload_sum = recover_payload_csum_from_icmp(&old_icmp, ipv6, payload_len);
    }

    pr_debug("payload sum: %04x\n", payload_sum);
    update_udp_cksum(ipv4, ipv6, &udp_hdr, payload_sum);
  }

  // Replace ICMP header with UDP header
  err = skb_store_bytes_linear(skb, ip_end, &udp_hdr, sizeof(udp_hdr));
  if (err) {
    atomic64_inc(&stat->packets_dropped);
    pr_debug("skb_store_bytes_linear failed: %d\n", err);
    err = NF_DROP;
    goto err_cleanup;
  }

  RESTORE_SKB_POINTERS();

  {
    pr_debug("  Rebuilt UDP: %pI6:%5u -> %pI6:%5u, length: %u\n", &saddr, ntohs(udp_src), &daddr, ntohs(udp_dst), payload_len);
    // dump_skb(skb);
  }

  err = NF_ACCEPT;
err_cleanup:
  rcu_read_unlock();
  return err;
}

static bool local_only = false;
module_param(local_only, bool, 0444);
MODULE_PARM_DESC(local_only, "If true, only intercept locally generated UDP traffic (mode: client only). Cannot be changed "
                             "after module load. Default: false.");

static struct nf_hook_ops ingress_hook_ops = {
  .hook     = ingress_hook_func,
  .pf       = NFPROTO_INET,
  .hooknum  = NF_INET_PRE_ROUTING,
  .priority = NF_IP_PRI_FIRST,
};

static struct nf_hook_ops egress_hook_ops_post = {
  .hook     = egress_hook_func,
  .pf       = NFPROTO_INET,
  .hooknum  = NF_INET_POST_ROUTING,
  .priority = NF_IP_PRI_LAST,
};

static struct nf_hook_ops egress_hook_ops_local = {
  .hook     = egress_hook_func,
  .pf       = NFPROTO_INET,
  .hooknum  = NF_INET_LOCAL_OUT,
  .priority = NF_IP_PRI_LAST,
};

int tutu_export_config(struct tutu_config *out) {
  int                           err = -ENOENT;
  const struct tutu_config_rcu *cfg;

  rcu_read_lock();
  cfg = rcu_dereference(g_cfg_ptr);
  if (cfg) {
    *out = cfg->inner;
    err  = 0;
  }
  rcu_read_unlock();

  return err;
}

static DEFINE_MUTEX(cfg_mutex);

static struct tutu_config_rcu *set_new_config(struct tutu_config_rcu *newcfg) {
  struct tutu_config_rcu *oldcfg;

  mutex_lock(&cfg_mutex);
  oldcfg = rcu_replace_pointer(g_cfg_ptr, newcfg, lockdep_is_held(&cfg_mutex));
  mutex_unlock(&cfg_mutex);
  return oldcfg;
}

int tutu_set_config(const struct tutu_config *in) {
  struct tutu_config_rcu *new_cfg, *old_cfg;

  if (!in)
    return -EINVAL;

  if (in->session_max_age == 0)
    return -EINVAL;

  if (in->is_server != 0 && in->is_server != 1)
    return -EINVAL;

  new_cfg = kzalloc(sizeof(*new_cfg), GFP_KERNEL);
  if (!new_cfg)
    return -ENOMEM;

  new_cfg->inner = *in;

  old_cfg = set_new_config(new_cfg);
  if (old_cfg)
    kfree_rcu(old_cfg, rcu);

  return 0;
}

int tutu_export_stats(struct tutu_stats *out) {
  u64 packets_processed, packets_dropped, checksum_errors, fragmented, gso;
  int cpu;

  packets_processed = packets_dropped = checksum_errors = fragmented = gso = 0;
  for_each_possible_cpu(cpu) {
    struct tutu_stats_k *st = per_cpu_ptr(&g_stats_percpu, cpu);
    packets_processed += (u64) atomic64_read(&st->packets_processed);
    packets_dropped += (u64) atomic64_read(&st->packets_dropped);
    checksum_errors += (u64) atomic64_read(&st->checksum_errors);
    fragmented += (u64) atomic64_read(&st->fragmented);
    gso += (u64) atomic64_read(&st->gso);
  }

  out->packets_processed = packets_processed;
  out->packets_dropped   = packets_dropped;
  out->checksum_errors   = checksum_errors;
  out->fragmented        = fragmented;
  out->gso               = gso;

  return 0;
}

int tutu_clear_stats(void) {
  int cpu;

  for_each_possible_cpu(cpu) {
    struct tutu_stats_k *st = per_cpu_ptr(&g_stats_percpu, cpu);

    atomic64_set(&st->packets_processed, 0);
    atomic64_set(&st->packets_dropped, 0);
    atomic64_set(&st->checksum_errors, 0);
    atomic64_set(&st->fragmented, 0);
    atomic64_set(&st->gso, 0);
  }
  return 0;
}

typedef bool (*tutu_gc_predicate)(const struct session_key *key, const struct session_value *value, void *ctx);

static bool gc_session_check_age(const struct session_key *key, const struct session_value *value, void *ctx) {
  u32 session_max_age = 0;

  rcu_read_lock();
  struct tutu_config_rcu *p = rcu_dereference(g_cfg_ptr);
  if (p)
    session_max_age = READ_ONCE(p->inner.session_max_age);
  rcu_read_unlock();

  __u64 now = ktime_get_seconds();
  __u64 age = value ? READ_ONCE(value->age) : 0;

  if (!age || now < age || now - age >= session_max_age) {
    pr_debug("gc: age %llu too old: now is %llu\n", age, now);
    return true;
  }

  return false;
}

static int gc_session(tutu_gc_predicate need_delete, void *ctx) {
  struct session_key cur_key;
  struct session_key next_key;
  int                err;

  rcu_read_lock();
  err = tutu_map_get_next_key(session_map, NULL, &cur_key);
  if (err)
    goto out;

  while (1) {
    struct session_value *val = tutu_map_lookup_elem(session_map, &cur_key);

    err = tutu_map_get_next_key(session_map, &cur_key, &next_key);
    if (err && err != -ENOENT) {
      goto out;
    }

    bool del = need_delete(&cur_key, val, ctx);

    if (del) {
      tutu_map_delete_elem(session_map, &cur_key);
    }

    // 没有下一个了
    if (err == -ENOENT)
      break;

    // 前进
    cur_key = next_key;
  }

  err = 0;
out:
  rcu_read_unlock();
  if (err == -ENOENT)
    err = 0;

  return err;
}

struct tutu_gc_ctx {
  struct delayed_work dwork;
  unsigned long       period_jiffies;
};

static struct tutu_gc_ctx *gcctx;

static void tutu_gc_work(struct work_struct *work) {
  struct tutu_gc_ctx *ctx = container_of(to_delayed_work(work), struct tutu_gc_ctx, dwork);

  (void) gc_session(gc_session_check_age, NULL);
  queue_delayed_work(system_unbound_wq, &ctx->dwork, ctx->period_jiffies);
}

static int tutu_gc_start(unsigned int period_sec) {
  if (gcctx)
    return 0;

  gcctx = kzalloc(sizeof(*gcctx), GFP_KERNEL);
  if (!gcctx)
    return -ENOMEM;

  INIT_DELAYED_WORK(&gcctx->dwork, tutu_gc_work);
  gcctx->period_jiffies = msecs_to_jiffies(period_sec * 1000);

  queue_delayed_work(system_unbound_wq, &gcctx->dwork, gcctx->period_jiffies);
  return 0;
}

static void tutu_gc_stop(void) {
  if (!gcctx)
    return;
  cancel_delayed_work_sync(&gcctx->dwork);
  kfree(gcctx);
  gcctx = NULL;
}

static unsigned int egress_peer_map_size = 1024;
module_param(egress_peer_map_size, uint, 0400);
MODULE_PARM_DESC(egress_peer_map_size, "Size for the egress peer map, must be power of 2");

static unsigned int ingress_peer_map_size = 1024;
module_param(ingress_peer_map_size, uint, 0400);
MODULE_PARM_DESC(ingress_peer_map_size, "Size for the ingress peer map, must be power of 2");

static unsigned int session_map_size = 16384;
module_param(session_map_size, uint, 0400);
MODULE_PARM_DESC(session_map_size, "Size for the session map, must be power of 2");

static void reload_work_func(struct work_struct *work) {
  int err;

  err = ifset_reload_config();

  if (err)
    pr_err("reload_config_locked() failed: %d\n", err);
}

static DECLARE_DELAYED_WORK(g_reload_work, reload_work_func);

static int netdev_event_handler(struct notifier_block *nb, unsigned long event, void *ptr) {
  struct net_device *dev = netdev_notifier_info_to_dev(ptr);
  const char        *ev  = NULL;

  switch (event) {
  case NETDEV_REGISTER:
    ev = "REGISTER";
    break;
  case NETDEV_UNREGISTER:
    ev = "UNREGISTER";
    break;
  default:
    return NOTIFY_DONE;
  }

  pr_debug("reloading interface: event=%s dev=%s\n", ev, dev ? dev->name : "unknown");
  schedule_delayed_work(&g_reload_work, msecs_to_jiffies(100));
  return NOTIFY_DONE;
}

static struct notifier_block g_netdev_notifier = {
  .notifier_call = netdev_event_handler,
};

static struct nf_hook_ops *egress_hook_registered = NULL;

static int __init tutuicmptunnel_module_init(void) {
  int                     err;
  struct tutu_config_rcu *cfg_init;
  struct nf_hook_ops     *egress_hook_to_register = NULL;

  if (!is_power_of_2(egress_peer_map_size) || !is_power_of_2(ingress_peer_map_size) || !is_power_of_2(session_map_size) ||
      egress_peer_map_size < 256 || ingress_peer_map_size < 256 || session_map_size < 256) {
    pr_err("Invalid map size: all map sizes must be a power of 2 and >= 256.\n");
    return -EINVAL;
  }

  err = ifset_reload_config();
  if (err) {
    pr_err("ifset config failed: %d\n", err);
    return err;
  }

  egress_peer_map = tutu_map_alloc(sizeof(struct egress_peer_key), sizeof(struct egress_peer_value), egress_peer_map_size);
  if (IS_ERR(egress_peer_map)) {
    err = PTR_ERR(egress_peer_map);
    pr_err("failed to create egress peer map: %d\n", err);
    goto err_free_ifset;
  }

  ingress_peer_map = tutu_map_alloc(sizeof(struct ingress_peer_key), sizeof(struct ingress_peer_value), ingress_peer_map_size);
  if (IS_ERR(ingress_peer_map)) {
    err = PTR_ERR(ingress_peer_map);
    pr_err("failed to create ingress peer map: %d\n", err);
    goto err_free_egress_peer_map;
  }

  session_map = tutu_map_alloc(sizeof(struct session_key), sizeof(struct session_value), session_map_size);
  if (IS_ERR(session_map)) {
    err = PTR_ERR(session_map);
    pr_err("failed to create session map: %d\n", err);
    goto err_free_ingress_peer_map;
  }

  user_map = tutu_map_alloc(sizeof(u8), sizeof(struct user_info), 256);
  if (IS_ERR(user_map)) {
    err = PTR_ERR(user_map);
    pr_err("failed to create user map: %d\n", err);
    goto err_free_session_map;
  }

  /* 初始化 RCU 指针的初始对象 */
  cfg_init = kmemdup(&g_cfg_init, sizeof(g_cfg_init), GFP_KERNEL);
  if (!cfg_init) {
    err = -ENOMEM;
    goto err_free_user_map;
  }
  rcu_assign_pointer(g_cfg_ptr, cfg_init);

  err = nf_register_net_hook(&init_net, &ingress_hook_ops);
  if (err < 0) {
    pr_err("failed to register ingress hook\n");
    goto err_free_cfg;
  }

  pr_debug("ingress hook registered.\n");

  if (local_only) {
    egress_hook_to_register = &egress_hook_ops_local;
  } else {
    egress_hook_to_register = &egress_hook_ops_post;
  }

  err = nf_register_net_hook(&init_net, egress_hook_to_register);
  if (err < 0) {
    pr_err("failed to register egress hook\n");
    goto err_unreg_ingress;
  }
  egress_hook_registered = egress_hook_to_register;

  pr_debug("egress hook registered.\n");
  err = tutu_genl_init();
  if (err)
    goto err_unreg_egress;

  err = register_netdevice_notifier(&g_netdev_notifier);
  if (err)
    goto err_genl_exit;

  err = tutu_gc_start(1);
  if (err)
    goto err_unregister_netdevice_notifier;
  return 0;

err_unregister_netdevice_notifier:
  unregister_netdevice_notifier(&g_netdev_notifier);
err_genl_exit:
  tutu_genl_exit();
err_unreg_egress:
  if (egress_hook_registered)
    nf_unregister_net_hook(&init_net, egress_hook_registered);
  egress_hook_registered = NULL;
err_unreg_ingress:
  nf_unregister_net_hook(&init_net, &ingress_hook_ops);
err_free_cfg:
  cfg_init = set_new_config(NULL);
  if (cfg_init)
    kfree_rcu(cfg_init, rcu);
err_free_user_map:
  tutu_map_free(user_map);
err_free_session_map:
  tutu_map_free(session_map);
err_free_ingress_peer_map:
  tutu_map_free(ingress_peer_map);
err_free_egress_peer_map:
  tutu_map_free(egress_peer_map);
err_free_ifset:
  free_ifset();

  return err;
}

static void __exit tutuicmptunnel_module_exit(void) {
  struct tutu_config_rcu *old_cfg;

  tutu_gc_stop();
  unregister_netdevice_notifier(&g_netdev_notifier);
  cancel_delayed_work_sync(&g_reload_work);
  tutu_genl_exit();
  nf_unregister_net_hook(&init_net, egress_hook_registered);
  nf_unregister_net_hook(&init_net, &ingress_hook_ops);

  old_cfg = set_new_config(NULL);
  if (old_cfg)
    kfree_rcu(old_cfg, rcu);

  tutu_map_free(user_map);
  tutu_map_free(session_map);
  tutu_map_free(ingress_peer_map);
  tutu_map_free(egress_peer_map);

  free_ifset();
  pr_info("tutuicmptunnel: device removed\n");
}

module_init(tutuicmptunnel_module_init);
module_exit(tutuicmptunnel_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("outmatch@gmail.com");
MODULE_DESCRIPTION("tutuicmptunnel: UDP to ICMP tunnel based on nftable hooks");

// vim: set sw=2 ts=2 expandtab:
