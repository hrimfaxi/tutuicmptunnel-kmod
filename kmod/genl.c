#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/slab.h>
#include <linux/version.h>

#include <net/genetlink.h>

#include "hashtab.h"
#include "tutuicmptunnel.h"

/* -1 表示不启用该检查 */
static int allowed_uid = -1;
static int allowed_gid = -1;

module_param(allowed_uid, int, 0644);
MODULE_PARM_DESC(allowed_uid, "Extra allowed euid (int, -1 to disable)");

module_param(allowed_gid, int, 0644);
MODULE_PARM_DESC(allowed_gid, "Extra allowed egid (int, -1 to disable)");

extern struct list_head tutu_ifname_list;
extern struct mutex     tutu_ifname_lock;

static bool tutu_user_allowed(const struct sk_buff *skb, const struct genl_info *info) {
  const struct scm_creds *creds;
  kuid_t             uid;
  kgid_t             gid;

  /* 1. 只要有 CAP_NET_ADMIN 就放行（通常是 root 或带该 capability 的服务） */
  if (ns_capable(current_user_ns(), CAP_NET_ADMIN))
    return true;

  creds = &NETLINK_CB(skb).creds;
  uid   = creds->uid;
  gid   = creds->gid;

  pr_debug("from pid %u uid %u gid %u\n", creds->pid, __kuid_val(uid), __kgid_val(gid));

  /* 3. 检查 allowed_uid（-1 表示不启用） */
  if (allowed_uid >= 0) {
    kuid_t auid = KUIDT_INIT(allowed_uid);
    if (uid_valid(auid) && uid_eq(uid, auid))
      return true;
  }

  /* 4. 检查 allowed_gid（-1 表示不启用） */
  if (allowed_gid >= 0) {
    kgid_t agid = KGIDT_INIT(allowed_gid);
    if (gid_valid(agid) && gid_eq(gid, agid))
      return true;
  }

  /* 都不满足，拒绝 */
  return false;
}

/*
 * 兼容性处理：
 * Linux 5.2 引入了 family 级别的 policy (global policy)。
 *在此之前，policy 必须挂载在每个 ops 上。
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 2, 0)
/* 老内核：Family 没有 policy，Ops 需要 policy */
#define TUTU_OPS_POLICY .policy = tutu_genl_policy,
#define TUTU_FAM_POLICY
#else
/* 新内核：Family 有 policy，Ops 不需要（也不建议设置） */
#define TUTU_OPS_POLICY
#define TUTU_FAM_POLICY .policy = tutu_genl_policy,
#endif

static const struct nla_policy tutu_genl_policy[TUTU_ATTR_MAX + 1] = {
  [TUTU_ATTR_CONFIG] = {.type = NLA_BINARY, .len = sizeof(struct tutu_config)},
  [TUTU_ATTR_STATS]  = {.type = NLA_BINARY, .len = sizeof(struct tutu_stats)},

  [TUTU_ATTR_EGRESS]      = {.type = NLA_BINARY, .len = sizeof(struct tutu_egress)},
  [TUTU_ATTR_INGRESS]     = {.type = NLA_BINARY, .len = sizeof(struct tutu_ingress)},
  [TUTU_ATTR_SESSION]     = {.type = NLA_BINARY, .len = sizeof(struct tutu_session)},
  [TUTU_ATTR_USER_INFO]   = {.type = NLA_BINARY, .len = sizeof(struct tutu_user_info)},
  [TUTU_ATTR_IFNAME_NAME] = {.type = NLA_NUL_STRING, .len = IFNAMSIZ - 1},
};

static struct genl_family tutu_genl_family;

static int tutu_genl_get_config(struct sk_buff *skb, struct genl_info *info);
static int tutu_genl_set_config(struct sk_buff *skb, struct genl_info *info);
static int tutu_genl_get_stats(struct sk_buff *skb, struct genl_info *info);
static int tutu_genl_clr_stats(struct sk_buff *skb, struct genl_info *info);

#define DEFINE_TUTU_GENL_FUNCS(_dir, _map, _value_type, _attr, _CMD_GET)                                                       \
                                                                                                                               \
  /* --- 1. Single Lookup (GET DOIT) --- */                                                                                    \
  static int tutu_genl_get_##_dir(struct sk_buff *skb, struct genl_info *info) {                                               \
    struct tutu_##_dir entry;                                                                                                  \
    _value_type       *value;                                                                                                  \
    struct sk_buff    *msg;                                                                                                    \
    void              *hdr;                                                                                                    \
    int                err = 0;                                                                                                \
                                                                                                                               \
    if (!info->attrs[_attr])                                                                                                   \
      return -EINVAL;                                                                                                          \
    if (nla_len(info->attrs[_attr]) != sizeof(entry))                                                                          \
      return -EINVAL;                                                                                                          \
                                                                                                                               \
    /* 仅复制 key 部分即可，但为了简单这里复制整体 */                                                                          \
    memcpy(&entry, nla_data(info->attrs[_attr]), sizeof(entry));                                                               \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    value = tutu_map_lookup_elem(_map, &entry.key);                                                                            \
    if (value)                                                                                                                 \
      memcpy(&entry.value, value, sizeof(entry.value));                                                                        \
    else                                                                                                                       \
      err = -ENOENT;                                                                                                           \
    rcu_read_unlock();                                                                                                         \
                                                                                                                               \
    if (err)                                                                                                                   \
      return err;                                                                                                              \
                                                                                                                               \
    msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);                                                                         \
    if (!msg)                                                                                                                  \
      return -ENOMEM;                                                                                                          \
                                                                                                                               \
    hdr = genlmsg_put_reply(msg, info, &tutu_genl_family, 0, _CMD_GET);                                                        \
    if (!hdr) {                                                                                                                \
      nlmsg_free(msg);                                                                                                         \
      return -ENOMEM;                                                                                                          \
    }                                                                                                                          \
                                                                                                                               \
    if (nla_put(msg, _attr, sizeof(entry), &entry)) {                                                                          \
      genlmsg_cancel(msg, hdr);                                                                                                \
      nlmsg_free(msg);                                                                                                         \
      return -EMSGSIZE;                                                                                                        \
    }                                                                                                                          \
                                                                                                                               \
    genlmsg_end(msg, hdr);                                                                                                     \
    return genlmsg_reply(msg, info);                                                                                           \
  }                                                                                                                            \
                                                                                                                               \
  /* --- 2. Dump Operations (GET DUMPIT) --- */                                                                                \
  /* 上下文结构，用于在多次 recv 之间保存遍历状态 (Key) */                                                                     \
  struct tutu_dump_ctx_##_dir {                                                                                                \
    bool               started;                                                                                                \
    bool               done;                                                                                                   \
    struct tutu_##_dir cursor_entry; /* 保存当前的 Key */                                                                      \
  };                                                                                                                           \
                                                                                                                               \
  static int tutu_genl_dump_##_dir(struct sk_buff *skb, struct netlink_callback *cb) {                                         \
    struct tutu_dump_ctx_##_dir *ctx = (void *) cb->args[0];                                                                   \
    struct tutu_##_dir           temp_entry;                                                                                   \
    _value_type                 *val;                                                                                          \
    void                        *hdr;                                                                                          \
    int                          err;                                                                                          \
                                                                                                                               \
    /* 第一次调用分配上下文 */                                                                                                 \
    if (!ctx) {                                                                                                                \
      ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);                                                                                 \
      if (!ctx)                                                                                                                \
        return -ENOMEM;                                                                                                        \
      cb->args[0] = (long) ctx;                                                                                                \
    }                                                                                                                          \
    /* 如果上次已经标记完成了，直接返回 0，告诉 Netlink 结束了 */                                                              \
    if (ctx->done)                                                                                                             \
      return 0;                                                                                                                \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    if (!ctx->started) {                                                                                                       \
      /* 只有没开始的时候，才需要去拿第一个 */                                                                                 \
      err = tutu_map_get_next_key(_map, NULL, &ctx->cursor_entry.key);                                                         \
      if (err) {                                                                                                               \
        /* Map 为空，直接结束 */                                                                                               \
        rcu_read_unlock();                                                                                                     \
        ctx->done = true; /* 标记完成 */                                                                                       \
        return 0;                                                                                                              \
      }                                                                                                                        \
      ctx->started = true;                                                                                                     \
    }                                                                                                                          \
                                                                                                                               \
    /* 开始遍历循环 */                                                                                                         \
    while (true) {                                                                                                             \
      /* 查找 Value */                                                                                                         \
      val = tutu_map_lookup_elem(_map, &ctx->cursor_entry.key);                                                                \
      if (val) {                                                                                                               \
        /* 组装数据 */                                                                                                         \
        memcpy(&temp_entry.key, &ctx->cursor_entry.key, sizeof(temp_entry.key));                                               \
        memcpy(&temp_entry.value, val, sizeof(temp_entry.value));                                                              \
        temp_entry.map_flags = 0;                                                                                              \
                                                                                                                               \
        hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq, &tutu_genl_family, NLM_F_MULTI, _CMD_GET);      \
        if (!hdr)                                                                                                              \
          break; /* Buffer full */                                                                                             \
                                                                                                                               \
        if (nla_put(skb, _attr, sizeof(temp_entry), &temp_entry)) {                                                            \
          genlmsg_cancel(skb, hdr);                                                                                            \
          break; /* Buffer full */                                                                                             \
        }                                                                                                                      \
        genlmsg_end(skb, hdr);                                                                                                 \
      }                                                                                                                        \
                                                                                                                               \
      /* 准备下一次迭代 */                                                                                                     \
      /* 使用 temp 暂存 next key，成功后更新 cursor */                                                                         \
      {                                                                                                                        \
        struct tutu_##_dir next_node;                                                                                          \
        err = tutu_map_get_next_key(_map, &ctx->cursor_entry.key, &next_node.key);                                             \
        if (err) {                                                                                                             \
          ctx->done = true;                                                                                                    \
          break; /* 遍历完成 */                                                                                                \
        }                                                                                                                      \
        ctx->cursor_entry = next_node; /* 更新 cursor 指向下一个待处理 Key */                                                  \
      }                                                                                                                        \
    }                                                                                                                          \
    rcu_read_unlock();                                                                                                         \
                                                                                                                               \
    return skb->len;                                                                                                           \
  }                                                                                                                            \
                                                                                                                               \
  static int tutu_genl_done_##_dir(struct netlink_callback *cb) {                                                              \
    struct tutu_dump_ctx_##_dir *ctx = (void *) cb->args[0];                                                                   \
    kfree(ctx);                                                                                                                \
    cb->args[0] = 0;                                                                                                           \
    return 0;                                                                                                                  \
  }                                                                                                                            \
                                                                                                                               \
  /* --- 3. Delete --- */                                                                                                      \
  static int tutu_genl_delete_##_dir(struct sk_buff *skb, struct genl_info *info) {                                            \
    struct tutu_##_dir entry;                                                                                                  \
    int                err;                                                                                                    \
                                                                                                                               \
    if (!tutu_user_allowed(skb, info)) {                                                                                       \
      NL_SET_ERR_MSG(info->extack, "permission denied for this command");                                                      \
      return -EPERM;                                                                                                           \
    }                                                                                                                          \
    if (!info->attrs[_attr])                                                                                                   \
      return -EINVAL;                                                                                                          \
    if (nla_len(info->attrs[_attr]) != sizeof(entry))                                                                          \
      return -EINVAL;                                                                                                          \
    memcpy(&entry, nla_data(info->attrs[_attr]), sizeof(entry));                                                               \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    err = tutu_map_delete_elem(_map, &entry.key);                                                                              \
    rcu_read_unlock();                                                                                                         \
    return err;                                                                                                                \
  }                                                                                                                            \
                                                                                                                               \
  /* --- 4. Update/Set --- */                                                                                                  \
  static int tutu_genl_update_##_dir(struct sk_buff *skb, struct genl_info *info) {                                            \
    struct tutu_##_dir entry;                                                                                                  \
    int                err;                                                                                                    \
    if (!tutu_user_allowed(skb, info)) {                                                                                       \
      NL_SET_ERR_MSG(info->extack, "permission denied for this command");                                                      \
      return -EPERM;                                                                                                           \
    }                                                                                                                          \
    if (!info->attrs[_attr])                                                                                                   \
      return -EINVAL;                                                                                                          \
    if (nla_len(info->attrs[_attr]) != sizeof(entry))                                                                          \
      return -EINVAL;                                                                                                          \
    memcpy(&entry, nla_data(info->attrs[_attr]), sizeof(entry));                                                               \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    err = tutu_map_update_elem(_map, &entry.key, &entry.value, entry.map_flags);                                               \
    rcu_read_unlock();                                                                                                         \
                                                                                                                               \
    return err;                                                                                                                \
  }

/* 生成 Egress 函数 */
DEFINE_TUTU_GENL_FUNCS(egress, egress_peer_map, struct egress_peer_value, TUTU_ATTR_EGRESS, TUTU_CMD_GET_EGRESS);

/* 生成 Ingress 函数 */
DEFINE_TUTU_GENL_FUNCS(ingress, ingress_peer_map, struct ingress_peer_value, TUTU_ATTR_INGRESS, TUTU_CMD_GET_INGRESS);

/* 生成 Session 函数 */
DEFINE_TUTU_GENL_FUNCS(session, session_map, struct session_value, TUTU_ATTR_SESSION, TUTU_CMD_GET_SESSION);

/* 生成 User Info 函数 */
DEFINE_TUTU_GENL_FUNCS(user_info, user_map, struct user_info, TUTU_ATTR_USER_INFO, TUTU_CMD_GET_USER_INFO);

static int tutu_genl_get_config(struct sk_buff *skb, struct genl_info *info) {
  struct sk_buff    *msg;
  void              *hdr;
  struct tutu_config cfg = {};
  int                err;

  err = tutu_export_config(&cfg);
  if (err)
    return err;

  msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
  if (!msg)
    return -ENOMEM;

  hdr = genlmsg_put_reply(msg, info, &tutu_genl_family, 0, TUTU_CMD_GET_CONFIG);
  if (!hdr) {
    nlmsg_free(msg);
    return -ENOMEM;
  }

  err = nla_put(msg, TUTU_ATTR_CONFIG, sizeof(cfg), &cfg);
  if (err) {
    genlmsg_cancel(msg, hdr);
    nlmsg_free(msg);
    return err;
  }

  genlmsg_end(msg, hdr);
  return genlmsg_reply(msg, info);
}

static int tutu_genl_set_config(struct sk_buff *skb, struct genl_info *info) {
  struct tutu_config cfg;

  if (!tutu_user_allowed(skb, info)) {
    NL_SET_ERR_MSG(info->extack, "permission denied for this command");
    return -EPERM;
  }

  if (!info->attrs[TUTU_ATTR_CONFIG])
    return -EINVAL;

  if (nla_len(info->attrs[TUTU_ATTR_CONFIG]) != sizeof(cfg))
    return -EINVAL;

  memcpy(&cfg, nla_data(info->attrs[TUTU_ATTR_CONFIG]), sizeof(cfg));

  return tutu_set_config(&cfg);
}

static int tutu_genl_get_stats(struct sk_buff *skb, struct genl_info *info) {
  struct sk_buff   *msg;
  void             *hdr;
  struct tutu_stats st = {};
  int               err;

  err = tutu_export_stats(&st);
  if (err)
    return err;

  msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
  if (!msg)
    return -ENOMEM;

  hdr = genlmsg_put_reply(msg, info, &tutu_genl_family, 0, TUTU_CMD_GET_STATS);
  if (!hdr) {
    nlmsg_free(msg);
    return -ENOMEM;
  }

  err = nla_put(msg, TUTU_ATTR_STATS, sizeof(st), &st);
  if (err) {
    genlmsg_cancel(msg, hdr);
    nlmsg_free(msg);
    return err;
  }

  genlmsg_end(msg, hdr);
  return genlmsg_reply(msg, info);
}

static int tutu_genl_clr_stats(struct sk_buff *skb, struct genl_info *info) {
  if (!tutu_user_allowed(skb, info)) {
    NL_SET_ERR_MSG(info->extack, "permission denied for this command");
    return -EPERM;
  }

  return tutu_clear_stats();
}

/* 辅助函数：查找是否存在 */
static bool __ifname_exists(const char *name) {
  struct tutu_ifname_node *node;
  list_for_each_entry(node, &tutu_ifname_list, list) {
    if (!strcmp(node->name, name))
      return true;
  }
  return false;
}

static int tutu_genl_ifname_add(struct sk_buff *skb, struct genl_info *info) {
  struct tutu_ifname_node *node;
  char                    *name;
  int                      err;

  if (!tutu_user_allowed(skb, info)) {
    NL_SET_ERR_MSG(info->extack, "permission denied for this command");
    return -EPERM;
  }

  if (!info->attrs[TUTU_ATTR_IFNAME_NAME])
    return -EINVAL;

  name = nla_data(info->attrs[TUTU_ATTR_IFNAME_NAME]);

  if (!net_has_device(name))
    return -ENODEV;

  mutex_lock(&tutu_ifname_lock);

  /* 查重 */
  if (__ifname_exists(name)) {
    err = -EEXIST;
    goto out_unlock;
  }

  /* 分配新节点 */
  node = kmalloc(sizeof(*node), GFP_KERNEL);
  if (!node) {
    err = -ENOMEM;
    goto out_unlock;
  }

  strscpy(node->name, name, IFNAMSIZ);
  list_add_tail(&node->list, &tutu_ifname_list);
  mutex_unlock(&tutu_ifname_lock);

  /* 触发配置重载（此处不能锁） */
  return ifset_reload_config();

out_unlock:
  mutex_unlock(&tutu_ifname_lock);
  return err;
}

static int tutu_genl_ifname_del(struct sk_buff *skb, struct genl_info *info) {
  struct tutu_ifname_node *node, *tmp;
  char                    *name;
  bool                     found = false;

  if (!tutu_user_allowed(skb, info)) {
    NL_SET_ERR_MSG(info->extack, "permission denied for this command");
    return -EPERM;
  }

  if (!info->attrs[TUTU_ATTR_IFNAME_NAME])
    return -EINVAL;

  name = nla_data(info->attrs[TUTU_ATTR_IFNAME_NAME]);

  mutex_lock(&tutu_ifname_lock);
  list_for_each_entry_safe(node, tmp, &tutu_ifname_list, list) {
    if (!strcmp(node->name, name)) {
      list_del(&node->list);
      kfree(node);
      found = true;
      break; /* 找到了就退出 */
    }
  }

  mutex_unlock(&tutu_ifname_lock);
  return found ? ifset_reload_config() : -ENOENT;
}

static int tutu_genl_ifname_dump(struct sk_buff *skb, struct netlink_callback *cb) {
  struct tutu_ifname_node *node;
  int                      idx = 0, start_idx = (int) cb->args[0];
  void                    *hdr;

  mutex_lock(&tutu_ifname_lock);
  list_for_each_entry(node, &tutu_ifname_list, list) {
    /* 跳过已经发送过的 */
    if (idx < start_idx) {
      idx++;
      continue;
    }

    /* 构造消息 */
    hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq, &tutu_genl_family, NLM_F_MULTI, TUTU_CMD_IFNAME_GET);
    if (!hdr)
      break; // Buffer 满了

    if (nla_put_string(skb, TUTU_ATTR_IFNAME_NAME, node->name)) {
      genlmsg_cancel(skb, hdr);
      break; // Buffer 满了
    }

    genlmsg_end(skb, hdr);
    idx++;
  }

  mutex_unlock(&tutu_ifname_lock);

  cb->args[0] = idx; // 记录进度
  return (int) skb->len;
}

static const struct genl_ops tutu_genl_ops[] = {
  /* Config & Stats */
  {.cmd = TUTU_CMD_GET_CONFIG, .doit = tutu_genl_get_config, TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_SET_CONFIG, .doit = tutu_genl_set_config, TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_GET_STATS, .doit = tutu_genl_get_stats, TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_CLR_STATS, .doit = tutu_genl_clr_stats, TUTU_OPS_POLICY},

  /* Egress */
  {.cmd    = TUTU_CMD_GET_EGRESS,
   .doit   = tutu_genl_get_egress,
   .dumpit = tutu_genl_dump_egress,
   .done   = tutu_genl_done_egress,
   TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_DELETE_EGRESS, .doit = tutu_genl_delete_egress, TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_UPDATE_EGRESS, .doit = tutu_genl_update_egress, TUTU_OPS_POLICY},

  /* Ingress */
  {.cmd    = TUTU_CMD_GET_INGRESS,
   .doit   = tutu_genl_get_ingress,
   .dumpit = tutu_genl_dump_ingress,
   .done   = tutu_genl_done_ingress,
   TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_DELETE_INGRESS, .doit = tutu_genl_delete_ingress, TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_UPDATE_INGRESS, .doit = tutu_genl_update_ingress, TUTU_OPS_POLICY},

  /* Session */
  {.cmd    = TUTU_CMD_GET_SESSION,
   .doit   = tutu_genl_get_session,
   .dumpit = tutu_genl_dump_session,
   .done   = tutu_genl_done_session,
   TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_DELETE_SESSION, .doit = tutu_genl_delete_session, TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_UPDATE_SESSION, .doit = tutu_genl_update_session, TUTU_OPS_POLICY},

  /* User Info */
  {.cmd    = TUTU_CMD_GET_USER_INFO,
   .doit   = tutu_genl_get_user_info,
   .dumpit = tutu_genl_dump_user_info,
   .done   = tutu_genl_done_user_info,
   TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_DELETE_USER_INFO, .doit = tutu_genl_delete_user_info, TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_UPDATE_USER_INFO, .doit = tutu_genl_update_user_info, TUTU_OPS_POLICY},

  /* Get/Dump */
  {.cmd = TUTU_CMD_IFNAME_GET, .dumpit = tutu_genl_ifname_dump, TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_IFNAME_ADD, .doit = tutu_genl_ifname_add, TUTU_OPS_POLICY},
  {.cmd = TUTU_CMD_IFNAME_DEL, .doit = tutu_genl_ifname_del, TUTU_OPS_POLICY},
};

static struct genl_family tutu_genl_family = {.name    = TUTU_GENL_FAMILY_NAME,
                                              .version = TUTU_GENL_VERSION,
                                              .maxattr = TUTU_ATTR_MAX,
                                              .module  = THIS_MODULE,
                                              .netnsok = true,
                                              .policy  = tutu_genl_policy, /* 全局 Policy */
                                              .ops     = tutu_genl_ops,
                                              .n_ops   = ARRAY_SIZE(tutu_genl_ops),
                                              TUTU_FAM_POLICY};

int tutu_genl_init(void) {
  int err;

  err = genl_register_family(&tutu_genl_family);
  if (err) {
    pr_err("genl_register_family failed: %d\n", err);
    return err;
  }

  pr_info("genetlink family '%s' registered, id=%u\n", tutu_genl_family.name, tutu_genl_family.id);
  return 0;
}

void tutu_genl_exit(void) {
  int err;

  err = genl_unregister_family(&tutu_genl_family);
  if (err)
    pr_err("genl_unregister_family failed: %d\n", err);
}

// vim: set sw=2 ts=2 expandtab:
