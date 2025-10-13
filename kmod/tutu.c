#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/icmp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/tcp.h>
#include <linux/uaccess.h>
#include <linux/udp.h>
#include <net/checksum.h>
#include <net/ip6_checksum.h>

#if __has_include(<asm/unaligned.h>)
#include <asm/unaligned.h>
#else
#include <linux/unaligned.h>
#endif

#include "defs.h"
#include "hashtab.h"
#include "tutuicmptunnel.h"

/* Module parameter: comma-separated interface names, e.g., "wan,enp4s0" */
static char *ifnames;
MODULE_PARM_DESC(ifnames, "Comma-separated list of interface names to allow (empty means no filtering)");

static char *ifnames_add;
static char *ifnames_remove;
static bool  ifnames_clear;

MODULE_PARM_DESC(ifnames_add, "Comma-separated list of interface names to add");
MODULE_PARM_DESC(ifnames_remove, "Comma-separated list of interface names to remove");
MODULE_PARM_DESC(ifnames_clear, "Set to 'true' or '1' to clear the interface list");

static u32 dev_mode = 0400;
module_param(dev_mode, int, 0);
MODULE_PARM_DESC(dev_mode, "default device right, default: 0700");

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

static int build_ifset_from_names(const char *csv, struct ifset **out_new) {
  struct ifset *w;
  unsigned int  max_idx;
  char         *dup = NULL, *p, *tok;
  int           err;

  rtnl_lock();
  max_idx = get_max_ifindex_locked();

  w = ifset_alloc(max_idx);
  if (!w) {
    err = -ENOMEM;
    goto out;
  }

  if (!csv || !*csv) {
    /* Empty list => allow all (bitmap left as zero, special-cased later) */
    w->allow_all = true;
    *out_new     = w;
    err          = 0;
    goto out;
  }

  w->allow_all = false;
  dup          = kstrdup(csv, GFP_KERNEL);
  if (!dup) {
    kfree(w);
    err = -ENOMEM;
    goto out;
  }
  p = dup;

  while ((tok = strsep(&p, ",;: \t\n")) != NULL) {
    struct net_device *dev;
    if (!*tok)
      continue;
    dev = dev_get_by_name(&init_net, tok);
    if (!dev) {
      pr_warn("ifset: interface '%s' not found (ignored)\n", tok);
      continue;
    }
    if (dev->ifindex <= w->max_ifindex)
      __set_bit(dev->ifindex, w->bitmap);
    dev_put(dev);
  }

  kfree(dup);
  *out_new = w;
  err      = 0;
out:
  rtnl_unlock();
  return err;
}

static int reload_config_locked(void) {
  struct ifset *newcfg;
  struct ifset *oldcfg;
  int           err = build_ifset_from_names(ifnames, &newcfg);
  if (err)
    return err;

  oldcfg = rcu_replace_pointer(g_ifset, newcfg, lockdep_is_held(&g_ifset_mutex));
  kfree_rcu(oldcfg, rcu);

  pr_debug("ifset: config reloaded (ifnames=\"%s\")\n", ifnames ? ifnames : "");
  return 0;
}

static void free_ifset(void) {
  struct ifset *oldcfg;

  mutex_lock(&g_ifset_mutex);
  oldcfg = rcu_replace_pointer(g_ifset, NULL, lockdep_is_held(&g_ifset_mutex));
  kfree(ifnames);
  ifnames = NULL;
  mutex_unlock(&g_ifset_mutex);

  kfree_rcu(oldcfg, rcu);
}

static int param_set_ifnames(const char *val, const struct kernel_param *kp) {
  int   err;
  char *val_alloc = NULL, *clean_val = NULL;

  val_alloc = kstrdup(val, GFP_KERNEL);
  if (!val_alloc)
    return -ENOMEM;

  clean_val = strstrip(val_alloc);
  mutex_lock(&g_ifset_mutex);
  err = param_set_charp(clean_val, kp); /* updates 'ifnames' */
  if (err)
    goto out;

  err = reload_config_locked();
out:
  mutex_unlock(&g_ifset_mutex);
  kfree(val_alloc);
  return err;
}

static int param_get_ifnames(char *buffer, const struct kernel_param *kp) {
  int err;

  mutex_lock(&g_ifset_mutex);
  err = param_get_charp(buffer, kp);
  mutex_unlock(&g_ifset_mutex);

  return err;
}

static int param_set_ifnames_add(const char *val, const struct kernel_param *kp) {
  char  *new_ifnames;
  char  *val_alloc = NULL, *clean_val = NULL, *tmp_ifnames = NULL;
  size_t old_len, add_len;
  int    err = 0;

  if (!val || !*val)
    return 0;

  /* 规则：不允许输入中包含逗号 */
  if (strchr(val, ',')) {
    pr_warn("ifset: 'add' parameter only accepts a single interface name.\n");
    return -EINVAL;
  }

  val_alloc = kstrdup(val, GFP_KERNEL);
  if (!val_alloc)
    return -ENOMEM;

  clean_val = strstrip(val_alloc);
  if (!*clean_val) {
    kfree(val_alloc);
    return 0;
  }

  mutex_lock(&g_ifset_mutex);

  if (ifnames && *ifnames) {
    char *p, *tok;
    bool  found = false;

    tmp_ifnames = kstrdup(ifnames, GFP_KERNEL);
    if (!tmp_ifnames) {
      err = -ENOMEM;
      goto out;
    }

    p = tmp_ifnames;
    while ((tok = strsep(&p, ",")) != NULL) {
      if (!strcmp(tok, clean_val)) {
        found = true;
        break;
      }
    }

    if (found) {
      /* 介面已存在，视为空操作成功，直接退出 */
      err = 0;
      goto out;
    }
  }

  old_len = ifnames ? strlen(ifnames) : 0;

  if (old_len) {
    /* 拼接 "old,new" */
    add_len     = strlen(clean_val);
    new_ifnames = kmalloc(old_len + 1 + add_len + 1, GFP_KERNEL);
    if (new_ifnames)
      snprintf(new_ifnames, old_len + 1 + add_len + 1, "%s,%s", ifnames, clean_val);
  } else {
    /* 如果列表为空，直接复制输入即可 */
    new_ifnames = kstrdup(clean_val, GFP_KERNEL);
  }

  if (!new_ifnames) {
    err = -ENOMEM;
    goto out;
  }

  kfree(ifnames);
  ifnames = new_ifnames;
  err     = reload_config_locked();

out:
  kfree(tmp_ifnames);
  mutex_unlock(&g_ifset_mutex);
  kfree(val_alloc);
  return err;
}

static int param_set_ifnames_remove(const char *val, const struct kernel_param *kp) {
  char  *new_ifnames = NULL;
  char  *p_ifnames   = NULL;
  char  *current_p   = NULL;
  char  *tok         = NULL;
  char  *val_alloc   = NULL;
  char  *clean_val   = NULL;
  int    err;
  size_t alloc_size;

  if (!val || !*val)
    return 0;

  /* 规则：不允许输入中包含逗号 */
  if (strchr(val, ',')) {
    pr_warn("ifset: 'remove' parameter only accepts a single interface name.\n");
    return -EINVAL;
  }

  val_alloc = kstrdup(val, GFP_KERNEL);
  if (!val_alloc)
    return -ENOMEM;

  clean_val = strstrip(val_alloc);
  if (!*clean_val) {
    kfree(val_alloc);
    return 0;
  }

  mutex_lock(&g_ifset_mutex);
  err = 0;
  if (!ifnames || !*ifnames)
    goto out;

  alloc_size  = strlen(ifnames) + 1;
  new_ifnames = kzalloc(alloc_size, GFP_KERNEL);
  if (!new_ifnames) {
    err = -ENOMEM;
    goto out;
  }

  p_ifnames = kstrdup(ifnames, GFP_KERNEL);
  if (!p_ifnames) {
    err = -ENOMEM;
    goto out;
  }

  /* 核心逻辑：遍历列表，跳过匹配的元素，复制其他所有元素 */
  current_p = new_ifnames;
  while ((tok = strsep(&p_ifnames, ",")) != NULL) {
    if (!*tok)
      continue;

    /* 如果当前 token 不是要删除的那个，就把它加到新字串中 */
    if (strcmp(tok, clean_val)) {
      if (current_p != new_ifnames)
        *current_p++ = ',';
      size_t remaining_size = (new_ifnames + alloc_size) - current_p;
      current_p += snprintf(current_p, remaining_size, "%s", tok);
    }
  }

  /* 更新全域状态 */
  kfree(ifnames);
  ifnames     = new_ifnames;
  new_ifnames = NULL; /* 所有权已转移 */
  err         = reload_config_locked();

out:
  kfree(p_ifnames);
  kfree(new_ifnames);
  mutex_unlock(&g_ifset_mutex);
  kfree(val_alloc);
  return err;
}

static int param_set_ifnames_clear(const char *val, const struct kernel_param *kp) {
  bool clear;
  int  err = 0;
  if (kstrtobool(val, &clear) || !clear)
    return 0;

  mutex_lock(&g_ifset_mutex);
  kfree(ifnames);
  ifnames = NULL;
  err     = reload_config_locked();
  mutex_unlock(&g_ifset_mutex);
  return err;
}

static const struct kernel_param_ops ifnames_ops = {
  .set = param_set_ifnames,
  .get = param_get_ifnames,
};

/* Bind our custom ops to the 'ifnames' parameter */
module_param_cb(ifnames, &ifnames_ops, &ifnames, 0644);

static const struct kernel_param_ops ifnames_add_ops = {
  .set = param_set_ifnames_add,
  .get = NULL,
};
module_param_cb(ifnames_add, &ifnames_add_ops, &ifnames_add, 0200); /* write-only */

static const struct kernel_param_ops ifnames_remove_ops = {
  .set = param_set_ifnames_remove,
  .get = NULL,
};
module_param_cb(ifnames_remove, &ifnames_remove_ops, &ifnames_remove, 0200);

static const struct kernel_param_ops ifnames_clear_ops = {
  .set = param_set_ifnames_clear,
  .get = NULL,
};
module_param_cb(ifnames_clear, &ifnames_clear_ops, &ifnames_clear, 0200);

/* Decide if interface is allowed:
 * - if ifnames empty/NULL: allow all
 * - else: allow only if bit set
 * No explicit rcu_read_lock: single deref + read-only access, safe with kfree_rcu lifecycle.
 */
static bool iface_allowed(int ifindex) {
  bool allowed = true;

  const struct ifset *cfg = rcu_dereference(g_ifset);

  WARN_ON_ONCE(!rcu_read_lock_held());

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

static struct tutu_htab *egress_peer_map;
static struct tutu_htab *ingress_peer_map;
static struct tutu_htab *session_map;
static struct tutu_htab *user_map;

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
 *  - ip_len: [输出] L3 头部长度 (IPv4 或 IPv6 + 扩展头)。
 *  - ip_proto: [输出] L4 协议的值 (例如 IPPROTO_UDP)。
 *  - ip_proto_offset: [输出] 指向L4协议的字段(ip.protocol或最后一个ipv6.nexthdr)的偏移量，从数据包起始计算。
 *  - hdr_len: [输出] 从数据包开始到 L4 头部起始位置的总偏移量 (l2_len + ip_len)。
 *
 * 返回值:
 *  - 0: 成功。
 *  - -1: 失败（包太短、未知协议等）。
 */
static int parse_headers(struct sk_buff *skb, u32 *ip_type, u32 *l2_len, u32 *ip_len, u8 *ip_proto, u32 *ip_proto_offset,
                         u32 *hdr_len) {
  u32 local_l2_len = skb_network_offset(skb);

  /* 初始化输出参数 */
  *ip_type = *l2_len = *ip_len = *ip_proto_offset = *hdr_len = *ip_proto = 0;

  if (skb->protocol == htons(ETH_P_IP)) {
    if (!pskb_may_pull(skb, local_l2_len + sizeof(struct iphdr)))
      return -EINVAL;
    const struct iphdr *iph          = ip_hdr(skb);
    u32                 local_ip_len = iph->ihl * 4;

    if (local_ip_len < sizeof(*iph))
      return -EINVAL;

    if (!pskb_may_pull(skb, local_l2_len + local_ip_len))
      return -EINVAL;

    *ip_type         = 4;
    *ip_proto        = iph->protocol;
    *ip_len          = local_ip_len;
    *l2_len          = local_l2_len;
    *hdr_len         = local_l2_len + local_ip_len;
    *ip_proto_offset = local_l2_len + offsetof(struct iphdr, protocol);
    return 0;
  } else if (skb->protocol == htons(ETH_P_IPV6)) {
    if (!pskb_may_pull(skb, local_l2_len + sizeof(struct ipv6hdr)))
      return -EINVAL;
    const struct ipv6hdr *ipv6 = ipv6_hdr(skb);

    u8  next_hdr           = ipv6->nexthdr;
    u32 local_proto_offset = local_l2_len + offsetof(struct ipv6hdr, nexthdr);
    u32 current_hdr_start  = local_l2_len + sizeof(*ipv6);

    /* 遍历扩展头（最多 8 层，避免死循环） */
    for (int i = 0; i < 8; i++) {
      struct ipv6_opt_hdr *opt_hdr;

      if (!ipv6_ext_hdr(next_hdr))
        break;

      if (!pskb_may_pull(skb, current_hdr_start + sizeof(struct ipv6_opt_hdr)))
        return -EINVAL;

      opt_hdr = (typeof(opt_hdr)) (skb->data + current_hdr_start);

      // 更新协议字段偏移量为当前扩展头的 nexthdr 字段的偏移量
      local_proto_offset = current_hdr_start + offsetof(struct ipv6_opt_hdr, nexthdr);
      next_hdr           = opt_hdr->nexthdr;
      current_hdr_start += (opt_hdr->hdrlen + 1) << 3;
    }

    *ip_type         = 6;
    *ip_proto        = next_hdr;
    *ip_len          = current_hdr_start - local_l2_len;
    *l2_len          = local_l2_len;
    *hdr_len         = current_hdr_start;
    *ip_proto_offset = local_proto_offset;
    return 0;
  }

  return -EINVAL; /* 其他协议 */
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

  if (!age || now - age >= cfg->session_max_age) {
    // 太老，需要跳过并删除这个key
    pr_debug("session_map entry: age %llu too old: now is %llu\n", age, now);
    tutu_map_delete_elem(session_map, lookup_key);
    return -1;
  }

  // 此时需要更新下会话的寿命，否则过了update_interval会话就消失
  // 可以过1秒才更新，避免大量包造成过大压力
  if (now - age >= 1) {
    int err;

    value_ptr->age = now;
    err            = tutu_map_update_elem(session_map, lookup_key, value_ptr, TUTU_EXIST);
    (void) err;
    pr_debug("session updated: age: %llu: %d\n", now, err);
  }
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
static __always_inline int update_ipv4_checksum(struct sk_buff *skb, struct iphdr *ipv4, u32 l2_len, u32 old_proto,
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

static int skb_change_type(struct sk_buff *skb, u32 ip_type, u32 l2_len, u32 ip_len, u32 ip_proto_offset, u32 l4_offset) {
  const u8 ip_proto = skb->data[ip_proto_offset];
  int      err;

  if (ip_type == 4 && ip_proto == IPPROTO_ICMP) {
    if (force_sw_checksum || skb->ip_summed == CHECKSUM_NONE) {
      struct iphdr   *iph      = ip_hdr(skb);
      struct icmphdr *icmph    = (typeof(icmph)) (skb->data + l4_offset);
      size_t          icmp_len = ntohs(iph->tot_len) - ip_len;

      icmp_len = min_t(size_t, icmp_len, skb->len - l4_offset);

      err = skb_ensure_writable(skb, l4_offset + icmp_len); // 整个icmp包
      if (unlikely(err))
        return err;

      iph             = ip_hdr(skb);
      icmph           = (typeof(icmph)) (skb->data + l4_offset);
      icmph->checksum = 0;
      icmph->checksum = csum_fold(csum_partial((char *) icmph, icmp_len, 0));
      skb->ip_summed  = CHECKSUM_UNNECESSARY;
    } else if (skb->ip_summed == CHECKSUM_PARTIAL) {
      // skb->csum_start: 不变，因为udp->icmp并没有修改包长度
      // skb->csum_offset: 应该指向icmp头部检验和位置
      skb->csum_offset = offsetof(struct icmphdr, checksum);
    }
  } else if (ip_type == 6 && ip_proto == IPPROTO_ICMPV6) {
    struct ipv6hdr  *ip6h   = ipv6_hdr(skb);
    struct icmp6hdr *icmp6h = (typeof(icmp6h)) (skb->data + l4_offset);
    // skb_network_offset(skb) == l2_len
    unsigned int ext_hdr_len = l4_offset - l2_len - sizeof(struct ipv6hdr);
    unsigned int icmp_len    = ntohs(ip6h->payload_len) - ext_hdr_len;

    icmp_len = min_t(size_t, icmp_len, skb->len - l4_offset);

    if (force_sw_checksum || skb->ip_summed == CHECKSUM_NONE) {
      err = skb_ensure_writable(skb, l4_offset + icmp_len);
      if (unlikely(err))
        return err;

      ip6h   = ipv6_hdr(skb);
      icmp6h = (typeof(icmp6h)) (skb->data + l4_offset);
      // 同样为了兼容性修复icmpv6检验和
      icmp6h->icmp6_cksum = 0;
      // 计算 ICMPv6 校验和(带伪头部)
      __wsum csum         = csum_partial((char *) icmp6h, icmp_len, 0);
      icmp6h->icmp6_cksum = csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr, icmp_len, IPPROTO_ICMPV6, csum);
      skb->ip_summed      = CHECKSUM_UNNECESSARY;
    } else if (skb->ip_summed == CHECKSUM_PARTIAL) {
      err = skb_ensure_writable(skb, l4_offset + sizeof(struct icmp6hdr));
      if (unlikely(err))
        return err;

      ip6h   = ipv6_hdr(skb);
      icmp6h = (typeof(icmp6h)) (skb->data + l4_offset);
      // 计算 ICMPv6 校验和: 只算icmpv6伪头部，让硬件完成整个检验和计算
      // 由于csum_ipv6_magic()结果是最终检验和，需要反转才得到icmpv6伪头部
      icmp6h->icmp6_cksum = ~csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr, icmp_len, IPPROTO_ICMPV6, 0);
      // 而skb->csum_offset应该指向icmpv6头部检验和位置
      skb->csum_offset = offsetof(struct icmp6hdr, icmp6_cksum);
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

  u32 ip_end = 0, ip_proto_offset = 0, l2_len, ip_len, ip_type;
  u8  ip_proto;

  {
    const struct net_device *out     = state->out;
    int                      ifindex = out ? out->ifindex : (skb ? skb->skb_iif : 0);

    if (!iface_allowed(ifindex)) {
      err = NF_ACCEPT;
      goto err_cleanup;
    }
  }

  try2_ok(parse_headers(skb, &ip_type, &l2_len, &ip_len, &ip_proto, &ip_proto_offset, &ip_end));
#if 0
  pr_debug("parse headers: ip_type: %d, l2_len: %d, ip_len: %d, ip_proto: %d, ip_proto_offset: %d, ip_end: %d\n", ip_type,
          l2_len, ip_len, ip_proto, ip_proto_offset, ip_end);
#endif
  try2_ok(ip_proto == IPPROTO_UDP ? 0 : -1);
  // 重新pull 整个以太网+ip头部+udp头部
  // 不需要整个udp包：因为udp负载没有被修改过，也不需要检查udp负载长度
  try2_ok(pskb_may_pull(skb, ip_end + sizeof(struct udphdr)) ? 0 : -1);

  struct udphdr *udp = udp_hdr(skb);
  if (!udp) {
    err = NF_ACCEPT;
    goto err_cleanup;
  }

  struct iphdr   *ipv4 = NULL;
  struct ipv6hdr *ipv6 = NULL;

  if (ip_type == 4) {
    ipv4 = ip_hdr(skb);
  } else if (ip_type == 6) {
    ipv6 = ipv6_hdr(skb);
  } else {
    // 不可能
    err = NF_ACCEPT;
    goto err_cleanup;
  }

  struct user_info *user = NULL;
  __be16            icmp_id, icmp_seq;
  u8                icmp_type = 0, uid;

  if (ipv4) {
    // UDP payload length
    u16 udp_payload_len = ntohs(udp->len) - sizeof(*udp);
    pr_debug("Outgoing UDP: %pI4:%5d -> %pI4:%5d, length: %u\n", &ipv4->saddr, htons(udp->source), &ipv4->daddr,
             htons(udp->dest), udp_payload_len);
  } else if (ipv6) {
    u16 udp_payload_len = ntohs(udp->len) - sizeof(*udp);
    pr_debug("Outgoing UDP: %pI6:%5u -> %pI6:%5u, length: %u\n", &ipv6->saddr, htons(udp->source), &ipv6->daddr,
             htons(udp->dest), udp_payload_len);
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

    uid = value_ptr->uid;
    try2_ok(check_age(cfg, &lookup_key, value_ptr), "check age: %ld\n", _ret);
    user = try2_p_ok(tutu_map_lookup_elem(user_map, &uid), "invalid uid: %u\n", uid);

    if (ipv4) {
      icmp_type = ICMP_ECHO_REPLY;
    } else if (ipv6) {
      icmp_type = ICMP6_ECHO_REPLY;
    }

    icmp_id  = user->icmp_id;
    icmp_seq = value_ptr->client_sport;
  } else {
    struct egress_peer_key peer_key = {
      .port = udp->dest,
    };

    if (ipv4) {
      pr_debug("egress: udp: %pI4:%5u\n", &ipv4->saddr, udp->dest);
      ipv6_addr_set_v4mapped(get_unaligned(&ipv4->daddr), &peer_key.address);
    } else if (ipv6) {
      pr_debug("egress: udp: src %pI6:%5u\n", &ipv6->saddr, udp->dest);
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
  }

  atomic64_inc(&stat->packets_processed);

  struct udphdr old_udp = *udp;

  _Static_assert(sizeof(struct icmphdr) == sizeof(struct udphdr), "ICMP and UDP header sizes must match");

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

  // 写入字节之后， ipv4 & ipv6不再能访问
  // 需要重设指针
  if (ipv4) {
    ipv4 = ip_hdr(skb);
  } else if (ipv6) {
    ipv6 = ipv6_hdr(skb);
  }

  // 修改IP协议为ICMP
  // 只有ipv4才需要修复ip头部检验和
  u8 new_proto = IPPROTO_ICMPV6;
  if (ipv4) {
    new_proto = IPPROTO_ICMP;

    err = update_ipv4_checksum(skb, ipv4, l2_len, IPPROTO_UDP, new_proto);
    if (err) {
      atomic64_inc(&stat->packets_dropped);
      pr_debug("update_ipv4_checksum failed: %d\n", err);
      err = NF_DROP;
      goto err_cleanup;
    }

    ipv4 = ip_hdr(skb);
  }

  err = skb_store_bytes_linear(skb, ip_proto_offset, &new_proto, sizeof(new_proto));
  if (err) {
    atomic64_inc(&stat->packets_dropped);
    pr_debug("skb_store_bytes_linear failed: %d\n", err);
    err = NF_DROP;
    goto err_cleanup;
  }

  // ipv4: 如果是硬件offload：设置csum_offset为icmphdr->checksum位置。不需要修改csum_start，继续硬件offload
  // ipv4: 如果是软件计算: 重新算整个icmp的检验和，停止硬件计算
  // ipv6: 重新算整个icmpv6的检验和，停止硬件计算
  err = skb_change_type(skb, ip_type, l2_len, ip_len, ip_proto_offset, ip_end);
  (void) err;

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

  u32 ip_end = 0, ip_proto_offset = 0, l2_len, ip_len, ip_type;
  u8  ip_proto;

  {
    const struct net_device *in      = state->in;
    int                      ifindex = in ? in->ifindex : (skb ? skb->skb_iif : 0);

    if (!iface_allowed(ifindex)) {
      err = NF_ACCEPT;
      goto err_cleanup;
    }
  }

  try2_ok(parse_headers(skb, &ip_type, &l2_len, &ip_len, &ip_proto, &ip_proto_offset, &ip_end));
#if 0
  pr_debug("parse headers: ip_type: %d, l2_len: %d, ip_len: %d, ip_proto: %d, ip_proto_offset: %d, ip_end: %d\n", ip_type,
          l2_len, ip_len, ip_proto, ip_proto_offset, ip_end);
#endif
  try2_ok((ip_proto == IPPROTO_ICMP || ip_proto == IPPROTO_ICMPV6) ? 0 : -1);
  // 重新pull 整个以太网+ip头部+icmp/icmp6头部
  try2_ok(pskb_may_pull(skb, ip_end + sizeof(struct icmphdr)) ? 0 : -1, "pull data failed: %ld\n", _ret);
  _Static_assert(sizeof(struct icmphdr) == sizeof(struct icmp6hdr), "ICMP and ICMPv6 header sizes must match");

  struct iphdr   *ipv4 = NULL;
  struct ipv6hdr *ipv6 = NULL;

  if (ip_type == 4) {
    ipv4 = ip_hdr(skb);
  } else if (ip_type == 6) {
    ipv6 = ipv6_hdr(skb);
  } else {
    // 不可能
    err = NF_ACCEPT;
    goto err_cleanup;
  }

  // 由于icmp和icmp6hdr大小相等，而且前4字节完全等价，我们使用icmphdr作为表示icmp头部的类型
  struct icmphdr *icmp = (typeof(icmp)) (skb->data + ip_end);
  _Static_assert(sizeof(struct icmphdr) == sizeof(struct icmp6hdr), "ICMP and ICMPv6 header sizes must match");

  // Extract UID from ICMP code
  u8     uid      = icmp->code;
  __be16 icmp_seq = icmp->un.echo.sequence;
  __be16 icmp_id  = icmp->un.echo.id;

  struct user_info *user = NULL;

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
      user->icmp_id = icmp_id;
      // Update user info
      tutu_map_update_elem(user_map, &uid, user, TUTU_EXIST);
    }

    // 需要更新session_map
    try2_ok(update_session_map(user, uid, icmp_seq), "update session map: %ld\n", _ret);
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
    payload_len = ntohs(ipv4->tot_len) - ip_len - sizeof(struct icmphdr);
  } else if (ipv6) {
    // ip_len需要减去ipv6头部,因为ipv6的长度是负载长度(不包括ipv6头)
    payload_len = ntohs(ipv6->payload_len) - (ip_len - sizeof(struct ipv6hdr)) - sizeof(struct icmp6hdr);
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
  pr_debug("  id: %u, seq: %u, length: %u,\n", htons(icmp_id), htons(icmp_seq), payload_len);

  // 只有ipv4才需要修复ip头部检验和
  u8 new_proto = IPPROTO_UDP;

  err = skb_store_bytes_linear(skb, ip_proto_offset, &new_proto, sizeof(new_proto));
  if (err) {
    atomic64_inc(&stat->packets_dropped);
    pr_debug("skb_store_bytes_linear failed: %d\n", err);
    err = NF_DROP;
    goto err_cleanup;
  }

  // 写入字节之后， ipv4 & ipv6不再能访问
  // 需要重设指针
  if (ipv4) {
    ipv4 = ip_hdr(skb);
  } else if (ipv6) {
    ipv6 = ipv6_hdr(skb);
  }

  if (ipv4) {
    err = update_ipv4_checksum(skb, ipv4, l2_len, IPPROTO_ICMP, new_proto);
    if (err) {
      atomic64_inc(&stat->packets_dropped);
      pr_debug("update_ipv4_checksum failed: %d\n", err);
      err = NF_DROP;
      goto err_cleanup;
    }

    ipv4 = ip_hdr(skb);
  }

  _Static_assert(sizeof(struct icmphdr) == sizeof(struct udphdr), "ICMP and UDP header sizes must match");
  _Static_assert(sizeof(udp_hdr) == 8, "ICMP and UDP header sizes must match");

  {
    __wsum payload_sum = recover_payload_csum_from_icmp(&old_icmp, ipv6, payload_len);

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

  {
    pr_debug("  Rebuilt UDP: %pI6:%5u -> %pI6:%5u, length: %u\n", &saddr, ntohs(udp_src), &daddr, ntohs(udp_dst), payload_len);
    // dump_skb(skb);
  }

  err = NF_ACCEPT;
err_cleanup:
  rcu_read_unlock();
  return err;
}

static struct nf_hook_ops ingress_hook_ops = {
  .hook     = ingress_hook_func,
  .pf       = NFPROTO_INET,
  .hooknum  = NF_INET_PRE_ROUTING,
  .priority = NF_IP_PRI_FIRST,
};

static struct nf_hook_ops egress_hook_ops = {
  .hook     = egress_hook_func,
  .pf       = NFPROTO_INET,
  .hooknum  = NF_INET_POST_ROUTING,
  .priority = NF_IP_PRI_LAST,
};

static dev_t         tutu_devno;
static struct cdev   tutu_cdev;
static struct class *tutu_class;

#define DEV_NAME   KBUILD_MODNAME
#define CLASS_NAME KBUILD_MODNAME

static int tutu_export_config(struct tutu_config *out) {
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

static int tutu_set_config(const struct tutu_config *in) {
  struct tutu_config_rcu *new_cfg, *old_cfg;

  new_cfg = kzalloc(sizeof(*new_cfg), GFP_KERNEL);
  if (!new_cfg)
    return -ENOMEM;

  new_cfg->inner = *in;

  old_cfg = set_new_config(new_cfg);
  kfree_rcu(old_cfg, rcu);

  return 0;
}

static int tutu_export_stats(struct tutu_stats *out) {
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

static int tutu_clear_stats(void) {
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

static long ioctl_cmd_get_config(void __user *argp, struct tutu_config *cfg) {
  int err;

  err = tutu_export_config(cfg);
  if (err)
    return err;
  if (copy_to_user(argp, cfg, sizeof(*cfg))) {
    return -EFAULT;
  }
  return 0;
}

static long ioctl_cmd_set_config(void __user *argp, struct tutu_config *cfg) {
  if (copy_from_user(cfg, argp, sizeof(*cfg)))
    return -EFAULT;
  return tutu_set_config(cfg);
}

static long ioctl_cmd_get_stats(void __user *argp, struct tutu_stats *st) {
  int err;

  err = tutu_export_stats(st);
  if (err)
    return err;
  if (copy_to_user(argp, st, sizeof(*st)))
    return -EFAULT;
  return 0;
}

#define DEFINE_TUTU_IOCTL_FUNCS(_dir, _map, _value_type)                                                                       \
  static long ioctl_cmd_lookup_##_dir(void __user *argp, struct tutu_##_dir *entry) {                                          \
    _value_type *value;                                                                                                        \
    int          err = 0;                                                                                                      \
                                                                                                                               \
    if (copy_from_user(entry, argp, sizeof(*entry)))                                                                           \
      return -EFAULT;                                                                                                          \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    value = tutu_map_lookup_elem(_map, &entry->key);                                                                           \
    if (value)                                                                                                                 \
      memcpy(&entry->value, value, sizeof(entry->value));                                                                      \
    else                                                                                                                       \
      err = -ENOENT;                                                                                                           \
    rcu_read_unlock();                                                                                                         \
                                                                                                                               \
    if (err)                                                                                                                   \
      return err;                                                                                                              \
                                                                                                                               \
    if (copy_to_user(argp, entry, sizeof(*entry)))                                                                             \
      return -EFAULT;                                                                                                          \
    return 0;                                                                                                                  \
  }                                                                                                                            \
                                                                                                                               \
  static long ioctl_cmd_delete_##_dir(void __user *argp, struct tutu_##_dir *entry) {                                          \
    int err;                                                                                                                   \
                                                                                                                               \
    if (copy_from_user(entry, argp, sizeof(*entry)))                                                                           \
      return -EFAULT;                                                                                                          \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    err = tutu_map_delete_elem(_map, &entry->key);                                                                             \
    rcu_read_unlock();                                                                                                         \
    return err;                                                                                                                \
  }                                                                                                                            \
                                                                                                                               \
  static long ioctl_cmd_update_##_dir(void __user *argp, struct tutu_##_dir *entry) {                                          \
    int err;                                                                                                                   \
                                                                                                                               \
    if (copy_from_user(entry, argp, sizeof(*entry)))                                                                           \
      return -EFAULT;                                                                                                          \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    err = tutu_map_update_elem(_map, &entry->key, &entry->value, entry->map_flags);                                            \
    rcu_read_unlock();                                                                                                         \
                                                                                                                               \
    return err;                                                                                                                \
  }                                                                                                                            \
                                                                                                                               \
  static long ioctl_cmd_get_first_key_##_dir(void __user *argp, struct tutu_##_dir *entry) {                                   \
    int err;                                                                                                                   \
                                                                                                                               \
    if (copy_from_user(entry, argp, sizeof(*entry)))                                                                           \
      return -EFAULT;                                                                                                          \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    err = tutu_map_get_next_key(_map, NULL, &entry->key);                                                                      \
    rcu_read_unlock();                                                                                                         \
                                                                                                                               \
    if (err)                                                                                                                   \
      return err;                                                                                                              \
                                                                                                                               \
    if (copy_to_user(argp, entry, sizeof(*entry)))                                                                             \
      return -EFAULT;                                                                                                          \
                                                                                                                               \
    return 0;                                                                                                                  \
  }                                                                                                                            \
                                                                                                                               \
  static long ioctl_cmd_get_next_key_##_dir(void __user *argp, struct tutu_##_dir *entry) {                                    \
    int err;                                                                                                                   \
                                                                                                                               \
    if (copy_from_user(entry, argp, sizeof(*entry)))                                                                           \
      return -EFAULT;                                                                                                          \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    err = tutu_map_get_next_key(_map, &entry->key, &entry->key);                                                               \
    rcu_read_unlock();                                                                                                         \
                                                                                                                               \
    if (err)                                                                                                                   \
      return err;                                                                                                              \
                                                                                                                               \
    if (copy_to_user(argp, entry, sizeof(*entry)))                                                                             \
      return -EFAULT;                                                                                                          \
                                                                                                                               \
    return 0;                                                                                                                  \
  }

DEFINE_TUTU_IOCTL_FUNCS(egress, egress_peer_map, struct egress_peer_value)
DEFINE_TUTU_IOCTL_FUNCS(ingress, ingress_peer_map, struct ingress_peer_value)
DEFINE_TUTU_IOCTL_FUNCS(session, session_map, struct session_peer_value)
DEFINE_TUTU_IOCTL_FUNCS(user_info, user_map, struct user_info_peer_value)

static long tutu_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
  struct tutu_config    cfg       = {};
  struct tutu_stats     st        = {};
  struct tutu_egress    egress    = {};
  struct tutu_ingress   ingress   = {};
  struct tutu_session   session   = {};
  struct tutu_user_info user_info = {};

  pr_debug("ioctl stub called, cmd=0x%x, arg=%lu\n", cmd, arg);

  void __user *argp = (void __user *) arg;
  (void) argp;

  switch (cmd) {
  case TUTU_GET_CONFIG: {
    return ioctl_cmd_get_config(argp, &cfg);
  }
  case TUTU_SET_CONFIG: {
    return ioctl_cmd_set_config(argp, &cfg);
  }
  case TUTU_GET_STATS: {
    return ioctl_cmd_get_stats(argp, &st);
  }
  case TUTU_CLR_STATS: {
    return tutu_clear_stats();
  }
  case TUTU_LOOKUP_EGRESS: {
    return ioctl_cmd_lookup_egress(argp, &egress);
  }
  case TUTU_DELETE_EGRESS: {
    return ioctl_cmd_delete_egress(argp, &egress);
  }
  case TUTU_UPDATE_EGRESS: {
    return ioctl_cmd_update_egress(argp, &egress);
  }
  case TUTU_GET_FIRST_KEY_EGRESS: {
    return ioctl_cmd_get_first_key_egress(argp, &egress);
  }
  case TUTU_GET_NEXT_KEY_EGRESS: {
    return ioctl_cmd_get_next_key_egress(argp, &egress);
  }
  case TUTU_LOOKUP_INGRESS: {
    return ioctl_cmd_lookup_ingress(argp, &ingress);
  }
  case TUTU_DELETE_INGRESS: {
    return ioctl_cmd_delete_ingress(argp, &ingress);
  }
  case TUTU_UPDATE_INGRESS: {
    return ioctl_cmd_update_ingress(argp, &ingress);
  }
  case TUTU_GET_FIRST_KEY_INGRESS: {
    return ioctl_cmd_get_first_key_ingress(argp, &ingress);
  }
  case TUTU_GET_NEXT_KEY_INGRESS: {
    return ioctl_cmd_get_next_key_ingress(argp, &ingress);
  }
  case TUTU_LOOKUP_SESSION: {
    return ioctl_cmd_lookup_session(argp, &session);
  }
  case TUTU_DELETE_SESSION: {
    return ioctl_cmd_delete_session(argp, &session);
  }
  case TUTU_UPDATE_SESSION: {
    return ioctl_cmd_update_session(argp, &session);
  }
  case TUTU_GET_FIRST_KEY_SESSION: {
    return ioctl_cmd_get_first_key_session(argp, &session);
  }
  case TUTU_GET_NEXT_KEY_SESSION: {
    return ioctl_cmd_get_next_key_session(argp, &session);
  }
  case TUTU_LOOKUP_USER_INFO: {
    return ioctl_cmd_lookup_user_info(argp, &user_info);
  }
  case TUTU_DELETE_USER_INFO: {
    return ioctl_cmd_delete_user_info(argp, &user_info);
  }
  case TUTU_UPDATE_USER_INFO: {
    return ioctl_cmd_update_user_info(argp, &user_info);
  }
  case TUTU_GET_FIRST_KEY_USER_INFO: {
    return ioctl_cmd_get_first_key_user_info(argp, &user_info);
  }
  case TUTU_GET_NEXT_KEY_USER_INFO: {
    return ioctl_cmd_get_next_key_user_info(argp, &user_info);
  }
  }

  return -ENOTTY;
}

static int tutu_open(struct inode *inode, struct file *file) {
  pr_debug("device opened\n");
  return 0;
}

static int tutu_release(struct inode *inode, struct file *file) {
  pr_debug("device released\n");
  return 0;
}

static const struct file_operations tutu_fops = {
  .owner          = THIS_MODULE,
  .open           = tutu_open,
  .release        = tutu_release,
  .unlocked_ioctl = tutu_unlocked_ioctl,
};

typedef bool (*tutu_gc_predicate)(const struct session_key *key, const struct session_value *value, void *ctx);

/* called under rcu_read_lock(); no sleep or blocking ops allowed */
static bool gc_session_check_age(const struct session_key *key, const struct session_value *value, void *ctx) {
  u32 session_max_age = 0;

  rcu_read_lock();
  struct tutu_config_rcu *p = rcu_dereference(g_cfg_ptr);
  if (p)
    session_max_age = READ_ONCE(p->inner.session_max_age);
  rcu_read_unlock();

  __u64 now = ktime_get_seconds();
  __u64 age = value ? READ_ONCE(value->age) : 0;

  if (!age || now - age >= session_max_age) {
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

static char *devnode(const struct device *dev, umode_t *mode) {
  if (mode)
    *mode = (umode_t) dev_mode;
  return NULL;
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

static int __init tutuicmptunnel_module_init(void) {
  int                     err;
  struct tutu_config_rcu *cfg_init;
  struct device          *dev;

  if (!is_power_of_2(egress_peer_map_size) || !is_power_of_2(ingress_peer_map_size) || !is_power_of_2(session_map_size) ||
      egress_peer_map_size < 256 || ingress_peer_map_size < 256 || session_map_size < 256) {
    pr_err("Invalid map size: all map sizes must be a power of 2 and >= 256.\n");
    return -EINVAL;
  }

  mutex_lock(&g_ifset_mutex);
  err = reload_config_locked();
  mutex_unlock(&g_ifset_mutex);
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

  err = nf_register_net_hook(&init_net, &egress_hook_ops);
  if (err < 0) {
    pr_err("failed to register egress hook\n");
    goto err_unreg_ingress;
  }

  pr_debug("egress hook registered.\n");

  /* 分配主次设备号 */
  err = alloc_chrdev_region(&tutu_devno, 0, 1, DEV_NAME);
  if (err) {
    pr_err("alloc_chrdev_region failed: %d\n", err);
    goto err_unreg_egress;
  }

  /* 初始化并添加 cdev */
  cdev_init(&tutu_cdev, &tutu_fops);
  tutu_cdev.owner = THIS_MODULE;

  err = cdev_add(&tutu_cdev, tutu_devno, 1);
  if (err) {
    pr_err("cdev_add failed: %d\n", err);
    goto err_unreg_chrdev;
  }

  /* 创建 class 与 device，生成 /dev 节点 */
  tutu_class = class_create(CLASS_NAME);
  if (IS_ERR(tutu_class)) {
    err = PTR_ERR(tutu_class);
    pr_err("class_create failed: %d\n", err);
    tutu_class = NULL;
    goto err_cdev_del;
  }

  tutu_class->devnode = &devnode;

  dev = device_create(tutu_class, NULL, tutu_devno, NULL, DEV_NAME);
  if (IS_ERR(dev)) {
    err = PTR_ERR(dev);
    pr_err("device_create failed: %d\n", err);
    goto err_class_destroy;
  }

  tutu_gc_start(1);
  pr_info("device ready at /dev/%s (major=%d minor=%d)\n", DEV_NAME, MAJOR(tutu_devno), MINOR(tutu_devno));
  return 0;

err_class_destroy:
  class_destroy(tutu_class);
  tutu_class = NULL;
err_cdev_del:
  cdev_del(&tutu_cdev);
err_unreg_chrdev:
  unregister_chrdev_region(tutu_devno, 1);
err_unreg_egress:
  nf_unregister_net_hook(&init_net, &egress_hook_ops);
err_unreg_ingress:
  nf_unregister_net_hook(&init_net, &ingress_hook_ops);
err_free_cfg:
  cfg_init = set_new_config(NULL);
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
  device_destroy(tutu_class, tutu_devno);
  class_destroy(tutu_class);
  cdev_del(&tutu_cdev);
  unregister_chrdev_region(tutu_devno, 1);
  nf_unregister_net_hook(&init_net, &egress_hook_ops);
  nf_unregister_net_hook(&init_net, &ingress_hook_ops);

  old_cfg = set_new_config(NULL);
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

// vim: set sw=2 tabstop=2 expandtab:
