#include <linux/in6.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netlink.h>

#include <net/genetlink.h>

#include "hashtab.h"
#include "tutuicmptunnel.h"

static const struct nla_policy tutu_genl_policy[TUTU_ATTR_MAX + 1] = {
  [TUTU_ATTR_CONFIG] = {.type = NLA_BINARY, .len = sizeof(struct tutu_config)},
  [TUTU_ATTR_STATS]  = {.type = NLA_BINARY, .len = sizeof(struct tutu_stats)},

  [TUTU_ATTR_EGRESS]    = {.type = NLA_BINARY, .len = sizeof(struct tutu_egress)},
  [TUTU_ATTR_INGRESS]   = {.type = NLA_BINARY, .len = sizeof(struct tutu_ingress)},
  [TUTU_ATTR_SESSION]   = {.type = NLA_BINARY, .len = sizeof(struct tutu_session)},
  [TUTU_ATTR_USER_INFO] = {.type = NLA_BINARY, .len = sizeof(struct tutu_user_info)},
};

static struct genl_family tutu_genl_family;

/* 先声明 handler，下面要用到指针 */
static int tutu_genl_get_config(struct sk_buff *skb, struct genl_info *info);
static int tutu_genl_set_config(struct sk_buff *skb, struct genl_info *info);
static int tutu_genl_get_stats(struct sk_buff *skb, struct genl_info *info);
static int tutu_genl_clr_stats(struct sk_buff *skb, struct genl_info *info);

#define DEFINE_TUTU_GENL_FUNCS(_dir, _map, _value_type, _attr, _LOOKUP_CMD, _DEL_CMD, _UPDATE_CMD, _FIRST_CMD, _NEXT_CMD)      \
                                                                                                                               \
  static int tutu_genl_lookup_##_dir(struct sk_buff *skb, struct genl_info *info) {                                            \
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
    hdr = genlmsg_put_reply(msg, info, &tutu_genl_family, 0, _LOOKUP_CMD);                                                     \
    if (!hdr) {                                                                                                                \
      nlmsg_free(msg);                                                                                                         \
      return -ENOMEM;                                                                                                          \
    }                                                                                                                          \
                                                                                                                               \
    err = nla_put(msg, _attr, sizeof(entry), &entry);                                                                          \
    if (err) {                                                                                                                 \
      genlmsg_cancel(msg, hdr);                                                                                                \
      nlmsg_free(msg);                                                                                                         \
      return err;                                                                                                              \
    }                                                                                                                          \
                                                                                                                               \
    genlmsg_end(msg, hdr);                                                                                                     \
    return genlmsg_reply(msg, info);                                                                                           \
  }                                                                                                                            \
                                                                                                                               \
  static int tutu_genl_delete_##_dir(struct sk_buff *skb, struct genl_info *info) {                                            \
    struct tutu_##_dir entry;                                                                                                  \
    int                err;                                                                                                    \
                                                                                                                               \
    if (!info->attrs[_attr])                                                                                                   \
      return -EINVAL;                                                                                                          \
    if (nla_len(info->attrs[_attr]) != sizeof(entry))                                                                          \
      return -EINVAL;                                                                                                          \
                                                                                                                               \
    memcpy(&entry, nla_data(info->attrs[_attr]), sizeof(entry));                                                               \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    err = tutu_map_delete_elem(_map, &entry.key);                                                                              \
    rcu_read_unlock();                                                                                                         \
    return err;                                                                                                                \
  }                                                                                                                            \
                                                                                                                               \
  static int tutu_genl_update_##_dir(struct sk_buff *skb, struct genl_info *info) {                                            \
    struct tutu_##_dir entry;                                                                                                  \
    int                err;                                                                                                    \
                                                                                                                               \
    if (!info->attrs[_attr])                                                                                                   \
      return -EINVAL;                                                                                                          \
    if (nla_len(info->attrs[_attr]) != sizeof(entry))                                                                          \
      return -EINVAL;                                                                                                          \
                                                                                                                               \
    memcpy(&entry, nla_data(info->attrs[_attr]), sizeof(entry));                                                               \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    err = tutu_map_update_elem(_map, &entry.key, &entry.value, entry.map_flags);                                               \
    rcu_read_unlock();                                                                                                         \
                                                                                                                               \
    return err;                                                                                                                \
  }                                                                                                                            \
                                                                                                                               \
  static int tutu_genl_get_first_key_##_dir(struct sk_buff *skb, struct genl_info *info) {                                     \
    struct tutu_##_dir entry;                                                                                                  \
    struct sk_buff    *msg;                                                                                                    \
    void              *hdr;                                                                                                    \
    int                err;                                                                                                    \
                                                                                                                               \
    memset(&entry, 0, sizeof(entry));                                                                                          \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    err = tutu_map_get_next_key(_map, NULL, &entry.key);                                                                       \
    rcu_read_unlock();                                                                                                         \
                                                                                                                               \
    if (err)                                                                                                                   \
      return err;                                                                                                              \
                                                                                                                               \
    msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);                                                                         \
    if (!msg)                                                                                                                  \
      return -ENOMEM;                                                                                                          \
                                                                                                                               \
    hdr = genlmsg_put_reply(msg, info, &tutu_genl_family, 0, _FIRST_CMD);                                                      \
    if (!hdr) {                                                                                                                \
      nlmsg_free(msg);                                                                                                         \
      return -ENOMEM;                                                                                                          \
    }                                                                                                                          \
                                                                                                                               \
    err = nla_put(msg, _attr, sizeof(entry), &entry);                                                                          \
    if (err) {                                                                                                                 \
      genlmsg_cancel(msg, hdr);                                                                                                \
      nlmsg_free(msg);                                                                                                         \
      return err;                                                                                                              \
    }                                                                                                                          \
                                                                                                                               \
    genlmsg_end(msg, hdr);                                                                                                     \
    return genlmsg_reply(msg, info);                                                                                           \
  }                                                                                                                            \
                                                                                                                               \
  static int tutu_genl_get_next_key_##_dir(struct sk_buff *skb, struct genl_info *info) {                                      \
    struct tutu_##_dir entry;                                                                                                  \
    struct sk_buff    *msg;                                                                                                    \
    void              *hdr;                                                                                                    \
    int                err;                                                                                                    \
                                                                                                                               \
    if (!info->attrs[_attr])                                                                                                   \
      return -EINVAL;                                                                                                          \
    if (nla_len(info->attrs[_attr]) != sizeof(entry))                                                                          \
      return -EINVAL;                                                                                                          \
                                                                                                                               \
    memcpy(&entry, nla_data(info->attrs[_attr]), sizeof(entry));                                                               \
                                                                                                                               \
    rcu_read_lock();                                                                                                           \
    err = tutu_map_get_next_key(_map, &entry.key, &entry.key);                                                                 \
    rcu_read_unlock();                                                                                                         \
                                                                                                                               \
    if (err)                                                                                                                   \
      return err;                                                                                                              \
                                                                                                                               \
    msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);                                                                         \
    if (!msg)                                                                                                                  \
      return -ENOMEM;                                                                                                          \
                                                                                                                               \
    hdr = genlmsg_put_reply(msg, info, &tutu_genl_family, 0, _NEXT_CMD);                                                       \
    if (!hdr) {                                                                                                                \
      nlmsg_free(msg);                                                                                                         \
      return -ENOMEM;                                                                                                          \
    }                                                                                                                          \
                                                                                                                               \
    err = nla_put(msg, _attr, sizeof(entry), &entry);                                                                          \
    if (err) {                                                                                                                 \
      genlmsg_cancel(msg, hdr);                                                                                                \
      nlmsg_free(msg);                                                                                                         \
      return err;                                                                                                              \
    }                                                                                                                          \
                                                                                                                               \
    genlmsg_end(msg, hdr);                                                                                                     \
    return genlmsg_reply(msg, info);                                                                                           \
  }

/* egress */
DEFINE_TUTU_GENL_FUNCS(egress, egress_peer_map, struct egress_peer_value, TUTU_ATTR_EGRESS, TUTU_CMD_LOOKUP_EGRESS,
                       TUTU_CMD_DELETE_EGRESS, TUTU_CMD_UPDATE_EGRESS, TUTU_CMD_GET_FIRST_KEY_EGRESS,
                       TUTU_CMD_GET_NEXT_KEY_EGRESS);

/* ingress */
DEFINE_TUTU_GENL_FUNCS(ingress, ingress_peer_map, struct ingress_peer_value, TUTU_ATTR_INGRESS, TUTU_CMD_LOOKUP_INGRESS,
                       TUTU_CMD_DELETE_INGRESS, TUTU_CMD_UPDATE_INGRESS, TUTU_CMD_GET_FIRST_KEY_INGRESS,
                       TUTU_CMD_GET_NEXT_KEY_INGRESS);

/* session */
DEFINE_TUTU_GENL_FUNCS(session, session_map, struct session_value, TUTU_ATTR_SESSION, TUTU_CMD_LOOKUP_SESSION,
                       TUTU_CMD_DELETE_SESSION, TUTU_CMD_UPDATE_SESSION, TUTU_CMD_GET_FIRST_KEY_SESSION,
                       TUTU_CMD_GET_NEXT_KEY_SESSION);

/* user_info */
DEFINE_TUTU_GENL_FUNCS(user_info, user_map, struct user_info, TUTU_ATTR_USER_INFO, TUTU_CMD_LOOKUP_USER_INFO,
                       TUTU_CMD_DELETE_USER_INFO, TUTU_CMD_UPDATE_USER_INFO, TUTU_CMD_GET_FIRST_KEY_USER_INFO,
                       TUTU_CMD_GET_NEXT_KEY_USER_INFO);

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
  return tutu_clear_stats();
}

static const struct genl_ops tutu_genl_ops[] = {
  {.cmd = TUTU_CMD_GET_CONFIG, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_get_config},
  {.cmd = TUTU_CMD_SET_CONFIG, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_set_config},
  {.cmd = TUTU_CMD_GET_STATS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_get_stats},
  {.cmd = TUTU_CMD_CLR_STATS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_clr_stats},

  /* egress */
  {.cmd = TUTU_CMD_LOOKUP_EGRESS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_lookup_egress},
  {.cmd = TUTU_CMD_DELETE_EGRESS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_delete_egress},
  {.cmd = TUTU_CMD_UPDATE_EGRESS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_update_egress},
  {.cmd = TUTU_CMD_GET_FIRST_KEY_EGRESS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_get_first_key_egress},
  {.cmd = TUTU_CMD_GET_NEXT_KEY_EGRESS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_get_next_key_egress},

  /* ingress */
  {.cmd = TUTU_CMD_LOOKUP_INGRESS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_lookup_ingress},
  {.cmd = TUTU_CMD_DELETE_INGRESS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_delete_ingress},
  {.cmd = TUTU_CMD_UPDATE_INGRESS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_update_ingress},
  {.cmd = TUTU_CMD_GET_FIRST_KEY_INGRESS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_get_first_key_ingress},
  {.cmd = TUTU_CMD_GET_NEXT_KEY_INGRESS, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_get_next_key_ingress},

  /* session */
  {.cmd = TUTU_CMD_LOOKUP_SESSION, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_lookup_session},
  {.cmd = TUTU_CMD_DELETE_SESSION, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_delete_session},
  {.cmd = TUTU_CMD_UPDATE_SESSION, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_update_session},
  {.cmd = TUTU_CMD_GET_FIRST_KEY_SESSION, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_get_first_key_session},
  {.cmd = TUTU_CMD_GET_NEXT_KEY_SESSION, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_get_next_key_session},

  /* user_info */
  {.cmd = TUTU_CMD_LOOKUP_USER_INFO, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_lookup_user_info},
  {.cmd = TUTU_CMD_DELETE_USER_INFO, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_delete_user_info},
  {.cmd = TUTU_CMD_UPDATE_USER_INFO, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_update_user_info},
  {.cmd = TUTU_CMD_GET_FIRST_KEY_USER_INFO, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_get_first_key_user_info},
  {.cmd = TUTU_CMD_GET_NEXT_KEY_USER_INFO, .flags = 0, .policy = tutu_genl_policy, .doit = tutu_genl_get_next_key_user_info},
};

static struct genl_family tutu_genl_family = {
  .name    = TUTU_GENL_FAMILY_NAME,
  .version = TUTU_GENL_VERSION,
  .maxattr = TUTU_ATTR_MAX,
  .module  = THIS_MODULE,
  .netnsok = true,

  .ops   = tutu_genl_ops,
  .n_ops = ARRAY_SIZE(tutu_genl_ops),
};

int tutu_genl_init(void) {
  int err;

  err = genl_register_family(&tutu_genl_family);
  if (err) {
    pr_err("tutu: genl_register_family failed: %d\n", err);
    return err;
  }

  pr_info("tutu: genetlink family '%s' registered, id=%u\n", tutu_genl_family.name, tutu_genl_family.id);
  return 0;
}

void tutu_genl_exit(void) {
  int err;

  err = genl_unregister_family(&tutu_genl_family);
  if (err)
    pr_err("tutu: genl_unregister_family failed: %d\n", err);
}

// vim: set sw=2 ts=2 expandtab:
