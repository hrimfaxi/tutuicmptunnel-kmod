#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libmnl/libmnl.h>
#include <linux/genetlink.h>

#include "list.h"
#include "log.h"
#include "resolve.h"
#include "try.h"
#include "tuctl.h"
#include "tuparser.h"

#define HOMEPAGE_STR "https://github.com/hrimfaxi/" STR(PROJECT_NAME)

#include "tutuicmptunnel.h"

typedef struct {
  const char *name;
  int (*handler)(int argc, char **argv);
  const char *desc;
} subcommand_t;

// 子命令处理函数声明
int cmd_load(int argc, char **argv);
int cmd_unload(int argc, char **argv);
int cmd_server(int argc, char **argv);
int cmd_client(int argc, char **argv);
int cmd_client_add(int argc, char **argv);
int cmd_client_del(int argc, char **argv);
int cmd_server_add(int argc, char **argv);
int cmd_server_del(int argc, char **argv);
int cmd_reaper(int argc, char **argv);
int cmd_status(int argc, char **argv);
int cmd_dump(int argc, char **argv);
int cmd_script(int argc, char **argv);
int cmd_help(int argc, char **argv);
int cmd_version(int argc, char **argv);

/* 全局状态 */
static struct mnl_socket *g_nl        = NULL;
static int                g_family_id = 0;

static int ctrl_attr_cb(const struct nlattr *attr, void *data) {
  const struct nlattr **tb   = data;
  int                   type = mnl_attr_get_type(attr);
  if (mnl_attr_type_valid(attr, CTRL_ATTR_MAX) < 0)
    return MNL_CB_OK;
  tb[type] = attr;
  return MNL_CB_OK;
}

static int ctrl_cb(const struct nlmsghdr *nlh, void *data) {
  struct nlattr     *tb[CTRL_ATTR_MAX + 1] = {};
  struct genlmsghdr *genl                  = mnl_nlmsg_get_payload(nlh);
  int                err;

  err = mnl_attr_parse(nlh, sizeof(*genl), ctrl_attr_cb, tb);
  if (err < 0)
    return MNL_CB_ERROR;

  if (tb[CTRL_ATTR_FAMILY_ID]) {
    *(uint16_t *) data = mnl_attr_get_u16(tb[CTRL_ATTR_FAMILY_ID]);
  }

  return MNL_CB_OK;
}

static int get_family_id_internal(struct mnl_socket *nl, const char *family_name, int *family_id) {
  char               buf[MNL_SOCKET_BUFFER_SIZE] = {};
  struct nlmsghdr   *nlh;
  struct genlmsghdr *genl;
  uint16_t           id = 0;
  int                err;

  nlh              = mnl_nlmsg_put_header(buf);
  nlh->nlmsg_type  = GENL_ID_CTRL;
  nlh->nlmsg_flags = NLM_F_REQUEST;
  nlh->nlmsg_seq   = 0;

  genl          = mnl_nlmsg_put_extra_header(nlh, sizeof(struct genlmsghdr));
  genl->cmd     = CTRL_CMD_GETFAMILY;
  genl->version = 1;

  mnl_attr_put_strz(nlh, CTRL_ATTR_FAMILY_NAME, family_name);

  try2(mnl_socket_sendto(nl, nlh, nlh->nlmsg_len));
  err = try2(mnl_socket_recvfrom(nl, buf, sizeof(buf)));
  try2(mnl_cb_run(buf, err, 0, mnl_socket_get_portid(nl), ctrl_cb, &id));
  *family_id = id;
  err        = 0;

err_cleanup:
  return err;
}

static void deinit_tutuicmptunnel(void) {
  if (g_nl) {
    mnl_socket_close(g_nl);
    g_nl = NULL;
  }
  g_family_id = 0;
}

static int init_tutuicmptunnel(void) {
  int err;

  deinit_tutuicmptunnel();
  g_nl = try2_p(mnl_socket_open(NETLINK_GENERIC));
  try2(mnl_socket_bind(g_nl, 0, MNL_SOCKET_AUTOPID));
  try2(get_family_id_internal(g_nl, TUTU_GENL_FAMILY_NAME, &g_family_id));
  if (!g_family_id) {
    err = -EINVAL;
    goto err_cleanup;
  }

  err = 0;
err_cleanup:
  if (err) {
    deinit_tutuicmptunnel();
  }

  return err;
}

/* --- 内部辅助：发送简单的 Request (用于 Update/Delete/Set) --- */
static int send_simple_cmd(int cmd, int attr_type, const void *data, size_t len, int flags) {
  char               buf[MNL_SOCKET_BUFFER_SIZE] = {};
  struct nlmsghdr   *nlh;
  struct genlmsghdr *genl;
  int                err;

  if (!g_nl) {
    return -EBADF;
  }

  nlh              = mnl_nlmsg_put_header(buf);
  nlh->nlmsg_type  = g_family_id;
  nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;
  nlh->nlmsg_seq   = time(NULL);

  genl          = mnl_nlmsg_put_extra_header(nlh, sizeof(struct genlmsghdr));
  genl->cmd     = cmd;
  genl->version = TUTU_GENL_VERSION;

  if (data && len > 0) {
    mnl_attr_put(nlh, attr_type, len, data);
  }

  try2(mnl_socket_sendto(g_nl, nlh, nlh->nlmsg_len));
  err = try2(mnl_socket_recvfrom(g_nl, buf, sizeof(buf)));
  try2(mnl_cb_run(buf, err, nlh->nlmsg_seq, mnl_socket_get_portid(g_nl), NULL, NULL));
  err = 0;

err_cleanup:
  return err;
}

/* --- 内部辅助：Lookup 回调 --- */
static int single_data_cb(const struct nlmsghdr *nlh, void *data) {
  struct nlattr *attr;

  /*
   * 修正点：
   * 使用 mnl_attr_for_each 宏来遍历属性。
   * 参数3 (offset): 必须跳过 Generic Netlink 头部 (sizeof(struct genlmsghdr))
   */
  mnl_attr_for_each(attr, nlh, sizeof(struct genlmsghdr)) {
    /*
     * 这里我们假设内核只回传了一个属性（例如 TUTU_ATTR_CONFIG），
     * 或者我们只关心第一个属性。
     */
    if (data) {
      memcpy(data, mnl_attr_get_payload(attr), mnl_attr_get_payload_len(attr));
    }

    /* 拿到第一个属性后，直接返回，不再继续遍历 */
    return MNL_CB_OK;
  }

  return MNL_CB_OK;
}

static int set_config_map(const struct tutu_config *cfg) {
  return send_simple_cmd(TUTU_CMD_SET_CONFIG, TUTU_ATTR_CONFIG, cfg, sizeof(*cfg), 0);
}

static int set_user_info_map(const struct tutu_user_info *info) {
  /*
   * 发送 UPDATE 命令
   * map_flags 已经在 info 结构体里了，直接传整个结构体即可
   */
  return send_simple_cmd(TUTU_CMD_UPDATE_USER_INFO, TUTU_ATTR_USER_INFO, info, sizeof(*info), 0);
}

static int delete_user_info_map(__u8 uid) {
  struct tutu_user_info info = {
    .key = uid,
  };

  /* 发送 DELETE 命令，带上完整的结构体 */
  return send_simple_cmd(TUTU_CMD_DELETE_USER_INFO, TUTU_ATTR_USER_INFO, &info, sizeof(info), 0);
}

/*
 * 通用函数：发送 GET 请求并接收同步响应
 * cmd:       命令字 (TUTU_CMD_...)
 * attr_type: 属性类型 (TUTU_ATTR_...)，仅在 in_data 存在时使用
 * in_data:   发送给内核的数据 (Key)，可为 NULL
 * in_len:    发送数据的长度
 * out_data:  用于接收内核回传数据的缓冲区指针
 */
static int send_and_recv_data(int cmd, int attr_type, const void *in_data, size_t in_len, void *out_data) {
  char               buf[MNL_SOCKET_BUFFER_SIZE];
  struct nlmsghdr   *nlh;
  struct genlmsghdr *genl;
  int                err;

  if (!g_nl) {
    errno = EBADF;
    return -1;
  }

  nlh              = mnl_nlmsg_put_header(buf);
  nlh->nlmsg_type  = g_family_id;
  nlh->nlmsg_flags = NLM_F_REQUEST;
  nlh->nlmsg_seq   = time(NULL);

  genl          = mnl_nlmsg_put_extra_header(nlh, sizeof(struct genlmsghdr));
  genl->cmd     = cmd;
  genl->version = TUTU_GENL_VERSION;

  /* 如果有输入数据 (比如 Lookup 时的 Key)，则放入属性 */
  if (in_data && in_len > 0) {
    mnl_attr_put(nlh, attr_type, in_len, in_data);
  }

  try2(mnl_socket_sendto(g_nl, nlh, nlh->nlmsg_len));
  err = try2(mnl_socket_recvfrom(g_nl, buf, sizeof(buf)));
  /* 使用 single_data_cb 解析回包，将结果填入 out_data */
  try2(mnl_cb_run(buf, err, nlh->nlmsg_seq, mnl_socket_get_portid(g_nl), single_data_cb, out_data));
  err = 0;

err_cleanup:
  return err;
}

/*
 * Lookup: 既发送数据 (Key)，也接收数据 (Value 回填到 data)
 */
static int lookup_map(int cmd, int attr_type, void *data, size_t len) {
  /* 输入和输出都使用同一个 data 指针 */
  return send_and_recv_data(cmd, attr_type, data, len, data);
}

static int get_config_map(struct tutu_config *cfg) {
  // 只接收数据，不发参数
  return send_and_recv_data(TUTU_CMD_GET_CONFIG, 0, NULL, 0, cfg);
}

static int get_stats_map(struct tutu_stats *stats) {
  // 只接收数据，不发参数
  return send_and_recv_data(TUTU_CMD_GET_STATS, 0, NULL, 0, stats);
}

static int set_egress_peer_map(const struct egress_peer_key *key, const struct egress_peer_value *value) {
  struct tutu_egress egress = {.key = *key, .value = *value, .map_flags = TUTU_ANY};
  return send_simple_cmd(TUTU_CMD_UPDATE_EGRESS, TUTU_ATTR_EGRESS, &egress, sizeof(egress), 0);
}

static int set_ingress_peer_map(const struct ingress_peer_key *key, const struct ingress_peer_value *value) {
  struct tutu_ingress ingress = {.key = *key, .value = *value, .map_flags = TUTU_NOEXIST};
  return send_simple_cmd(TUTU_CMD_UPDATE_INGRESS, TUTU_ATTR_INGRESS, &ingress, sizeof(ingress), 0);
}

static int delete_egress_peer_map(const struct egress_peer_key *key) {
  struct tutu_egress egress = {.key = *key};
  return send_simple_cmd(TUTU_CMD_DELETE_EGRESS, TUTU_ATTR_EGRESS, &egress, sizeof(egress), 0);
}

static int delete_ingress_peer_map(const struct ingress_peer_key *key) {
  struct tutu_ingress ingress = {.key = *key};
  return send_simple_cmd(TUTU_CMD_DELETE_INGRESS, TUTU_ATTR_INGRESS, &ingress, sizeof(ingress), 0);
}

/* 新增：Lookup 函数，用于替换逻辑中的 "EEXIST 后查询" */
static int lookup_ingress_peer_map(struct tutu_ingress *ingress) {
  return lookup_map(TUTU_CMD_GET_INGRESS, TUTU_ATTR_INGRESS, ingress, sizeof(*ingress));
}

/* 定义回调函数原型 */
typedef int (*tutu_iter_cb_t)(void *entry, void *user_data);

/* 通用 Dump 回调内部实现 */
struct iter_ctx {
  int            attr_type;
  int            struct_size;
  tutu_iter_cb_t user_cb;
  void          *user_data;
};

static int dump_cb_internal(const struct nlmsghdr *nlh, void *data) {
  struct iter_ctx *ctx = data;
  struct nlattr   *attr;

  mnl_attr_for_each(attr, nlh, sizeof(struct genlmsghdr)) {
    /* 校验属性类型 */
    if (mnl_attr_get_type(attr) == ctx->attr_type) {
      /*
       * 如果 struct_size 为 0，跳过长度检查（用于字符串或变长数据）
       * 否则严格检查长度（用于固定结构体）
       */
      if (ctx->struct_size > 0 && mnl_attr_get_payload_len(attr) != ctx->struct_size) {
        continue; // 长度不匹配，跳过
      }

      /*
       * 针对字符串的安全处理：
       * 如果 struct_size == 0，我们假设它是 NLA_NUL_STRING。
       * 虽然 Generic Netlink 通常保证 \0，但为了安全可以使用 mnl_attr_get_str (它会做检查)。
       * 这里为了保持通用性，我们直接传 payload。
       */
      if (ctx->user_cb(mnl_attr_get_payload(attr), ctx->user_data))
        return MNL_CB_STOP;
    }
  }
  return MNL_CB_OK;
}

static int tutu_foreach(int cmd, int attr_type, int size, tutu_iter_cb_t cb, void *user_data) {
  char               buf[MNL_SOCKET_BUFFER_SIZE] = {};
  struct nlmsghdr   *nlh;
  struct genlmsghdr *genl;
  int                err;
  struct iter_ctx    ctx = {.attr_type = attr_type, .struct_size = size, .user_cb = cb, .user_data = user_data};

  nlh              = mnl_nlmsg_put_header(buf);
  nlh->nlmsg_type  = g_family_id;
  nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  nlh->nlmsg_seq   = time(NULL);
  genl             = mnl_nlmsg_put_extra_header(nlh, sizeof(struct genlmsghdr));
  genl->cmd        = cmd;
  genl->version    = TUTU_GENL_VERSION;

  try2(mnl_socket_sendto(g_nl, nlh, nlh->nlmsg_len));

  while ((err = try2(mnl_socket_recvfrom(g_nl, buf, sizeof(buf)))) > 0) {
    err = try2(mnl_cb_run(buf, err, nlh->nlmsg_seq, mnl_socket_get_portid(g_nl), dump_cb_internal, &ctx));
    if (err <= 0) /* EOF or error */
      break;
  }

err_cleanup:
  return err;
}

/* 具体的遍历封装 */
static int foreach_egress(tutu_iter_cb_t cb, void *data) {
  return tutu_foreach(TUTU_CMD_GET_EGRESS, TUTU_ATTR_EGRESS, sizeof(struct tutu_egress), cb, data);
}

static int foreach_ingress(tutu_iter_cb_t cb, void *data) {
  return tutu_foreach(TUTU_CMD_GET_INGRESS, TUTU_ATTR_INGRESS, sizeof(struct tutu_ingress), cb, data);
}

static int foreach_user_info(tutu_iter_cb_t cb, void *data) {
  return tutu_foreach(TUTU_CMD_GET_USER_INFO, TUTU_ATTR_USER_INFO, sizeof(struct tutu_user_info), cb, data);
}

static int foreach_session(tutu_iter_cb_t cb, void *data) {
  return tutu_foreach(TUTU_CMD_GET_SESSION, TUTU_ATTR_SESSION, sizeof(struct tutu_session), cb, data);
}

/* 专门针对 ifname 的 foreach 包装，传入 size=0 */
static int foreach_ifname(tutu_iter_cb_t cb, void *data) {
  // 注意：第三个参数 size 传 0，表示不进行定长检查，因为字符串长度是可变的
  return tutu_foreach(TUTU_CMD_IFNAME_GET, TUTU_ATTR_IFNAME_NAME, 0, cb, data);
}

static int nl_mod_iface(int cmd, const char *ifname) {
  size_t len = strlen(ifname) + 1; /* 包含 \0 */
  /* 0 表示 flags */
  return send_simple_cmd(cmd, TUTU_ATTR_IFNAME_NAME, ifname, len, 0);
}

// 供外部调用的 Add
int nl_add_iface(const char *ifname) {
  return nl_mod_iface(TUTU_CMD_IFNAME_ADD, ifname);
}

// 供外部调用的 Del
int nl_del_iface(const char *ifname) {
  return nl_mod_iface(TUTU_CMD_IFNAME_DEL, ifname);
}

static uid_map_t uids;
static int       numeric = 0;
static int       debug   = 0;
static int       family  = AF_UNSPEC;
static int       help    = 0;

// 使用完后需要释放string
static int uid2string(uint8_t uid, char **string, int dump) {
  const char *host = NULL;
  int         err;
  char       *uid_fmt = dump ? "uid %u" : "UID: %u";

  if (numeric || uid_map_get_host(&uids, uid, &host))
    return asprintf(string, uid_fmt, uid) != -1 ? 0 : -ENOMEM;

  const char *user_fmt = dump ? "user %s" : "User: %s";
  err                  = asprintf(string, user_fmt, host) != -1 ? 0 : -ENOMEM;

  return err;
}

static int string2uid(const char *string, uint8_t *uid) {
  int err;
  int tmp_uid = 0;

  err = uid_map_get_uid(&uids, string, &tmp_uid);
  if (!err) {
    *uid = (uint8_t) tmp_uid;
    return 0;
  }

  return parse_uid(string, uid);
}

static void print_iface_usage(const char *prog, bool add) {
  fprintf(stderr,
          "Usage: %s %s [OPTIONS] iface IFACE [IFACE...]\n\n"
          "  %s a network interface %s " STR(PROJECT_NAME) ".\n\n"
                                                           "Arguments:\n"
                                                           "  %-22s One or more network interface names (e.g., eth0, ppp0)\n",
          prog, add ? "load" : "unload", add ? "Add" : "Remove", add ? "to" : "from", "IFACE [IFACE...]");
}

/* 回调函数：打印每一个收到的接口名 */
static int ifname_print_cb(void *entry, void *user_data) {
  const char *name  = (const char *) entry;
  bool       *found = (bool *) user_data;

  /* 打印接口名 */
  printf("  %s\n", name);

  /* 标记至少找到了一个 */
  if (found)
    *found = true;

  return 0; /* 返回 0 继续遍历 */
}

static int print_ifnames(void) {
  bool found = false;
  int  err;

  printf("Managed interfaces:\n");

  /* 调用 Generic Netlink Dump */
  try2(foreach_ifname(ifname_print_cb, &found), "Error dumping interfaces: %s", strerrno);

  if (!found) {
    printf("  [all interfaces]\n");
  }

  printf("\n");
  err = 0;

err_cleanup:
  return err;
}

static int handle_iface_op_nl(int argc, char **argv, bool is_add, const char *action_desc) {
  int  err              = 0;
  bool action_performed = false;

  try2(init_tutuicmptunnel(), _("open tutuicmptunnel device: %s"), strerrno);

  /* 1. 解析参数并执行 Add/Del */
  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (strcasecmp(tok, "iface") == 0) {
      if (i + 1 >= argc) {
        log_error("\"iface\" keyword needs an argument.");
        goto usage;
      }
      const char *ifname = argv[++i];

      err              = (is_add ? nl_add_iface : nl_del_iface)(ifname);
      action_performed = true;
      if (err < 0) {
        // 如果是删除且返回 ENOENT，通常可以忽略或报 Warning
        if (!is_add && errno == ENOENT) {
          log_warn("Interface %s not found in list.", ifname);
          err = 0;
        } else {
          log_error("Failed to %s interface %s: %s", is_add ? "add" : "remove", ifname, strerrno);
          goto err_cleanup;
        }
      } else {
        log_info("%s interface: %s", action_desc, ifname);
      }
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
      goto usage;
    }
  }

  if (!action_performed) {
    log_error("At least one interface name must be specified with \"iface\".");
    return -EINVAL;
  }

  /* 2. 操作完成后，打印当前的列表状态 */
  print_ifnames();

  err = 0;
err_cleanup:
  return err;

usage:
  print_iface_usage(STR(PROG_NAME), is_add);
  err = -EINVAL;
  goto err_cleanup;
}

/* 对外暴露的命令函数 */
int cmd_load(int argc, char **argv) {
  return handle_iface_op_nl(argc, argv, true, "Adding");
}

int cmd_unload(int argc, char **argv) {
  return handle_iface_op_nl(argc, argv, false, "Removing");
}

#define CMD_SERVER_SUMMARY "Set up " STR(PROJECT_NAME) " in server mode"

static int print_server_usage(int argc, char *argv[]) {
  (void) argc;

  fprintf(stderr,
          "Usage: %s %s [OPTIONS]\n\n"

          "  " CMD_SERVER_SUMMARY ".\n\n"

          "Options:\n"
          "  %-15s Set the session aging time in seconds.\n",
          STR(PROG_NAME), argv[0], "max-age AGE");
  return 0;
}

int cmd_server(int argc, char **argv) {
  uint32_t session_max_age = 60;
  int      err             = 0;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (matches(tok, "no-fixup")) {
      log_warn("ignore obsolete no-fixup keyword");
    } else if (matches(tok, "max-age")) {
      if (++i >= argc)
        goto usage;
      try(parse_age(argv[i], &session_max_age));
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_server_usage(argc, argv), -EINVAL;
    }
  }

  printf("server mode, session max age=%u\n", session_max_age);

  struct tutu_config cfg = {
    .is_server       = 1,
    .session_max_age = session_max_age,
  };

  try2(init_tutuicmptunnel(), _("open tutuicmptunnel device: %s"), strerrno);
  try2(set_config_map(&cfg), _("set_config_map: %s"), strerrno);

  err = 0;
err_cleanup:
  return err;
}

#define CMD_CLIENT_SUMMARY "Set up " STR(PROJECT_NAME) " in client mode"

static int print_client_usage(int argc, char **argv) {
  (void) argc;
  fprintf(stderr,
          "Usage: %s %s [OPTIONS]\n\n"

          "  " CMD_CLIENT_SUMMARY ".\n\n",
          STR(PROG_NAME), argv[0]);
  return 0;
}

int cmd_client(int argc, char **argv) {
  int err = 0;

  struct tutu_config cfg;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (matches(tok, "no-fixup")) {
      log_warn("ignore obsolete no-fixup keyword");
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_client_usage(argc, argv), -EINVAL;
    }
  }

  printf("client mode\n");

  cfg = (typeof(cfg)) {
    .is_server = 0,
  };
  try2(init_tutuicmptunnel(), _("open tutuicmptunnel device: %s"), strerrno);
  try2(set_config_map(&cfg));

  err = 0;
err_cleanup:
  return err;
}

static int get_server_mode(bool *server) {
  struct tutu_config cfg = {};
  int                err;

  try2(get_config_map(&cfg), _("get_config_map: %s"), strerrno);

  if (server)
    *server = cfg.is_server;

  err = 0;
err_cleanup:
  return err;
}

#define CMD_CLIENT_ADD_SUMMARY "Add a peer to the client configuration"

static int print_client_add_usage(int argc, char **argv) {
  (void) argc;
  fprintf(stderr,
          "Usage: %s %s [OPTIONS] address ADDR port PORT {uid UID | user USERNAME} [comment COMMENT]\n\n"

          "  " CMD_CLIENT_ADD_SUMMARY ".\n\n"

          "Arguments:\n"
          "  %-22s The peer address (domain name or IP).\n"
          "  %-22s The peer destination port.\n"
          "  %-22s Specify the client by numeric user ID.\n"
          "  %-22s Specify the client by username.\n"
          "  %-22s An optional descriptive comment.\n\n"

          "Options:\n"
          "  %-22s Force domain name resolution to IPv4.\n"
          "  %-22s Force domain name resolution to IPv6.\n"
          "  %-22s Display UID as a number instead of resolving it to a username"
          " in command output. \n",

          STR(PROG_NAME), argv[0], "address ADDR", "port PORT", "uid UID", "user USERNAME", "comment COMMENT", "-4", "-6",
          "-n");
  return 0;
}

int cmd_client_add(int argc, char **argv) {
  const char *address   = NULL;
  const char *comment   = NULL;
  uint16_t    port      = 0;
  uint8_t     uid       = 0;
  bool        uid_set   = false;
  int         err       = 0;
  bool        is_server = false;

  struct tutu_egress  egress;
  struct tutu_ingress ingress;
  struct in6_addr     in6;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (is_address_kw(tok)) {
      if (++i >= argc)
        goto usage;
      address = argv[i];
    } else if (matches(tok, "comment")) {
      if (++i >= argc)
        goto usage;
      comment = argv[i];
    } else if (matches(tok, "port")) {
      if (++i >= argc)
        goto usage;
      try(parse_port(argv[i], &port));
    } else if (is_user_kw(tok)) {
      if (++i >= argc)
        goto usage;
      try(string2uid(argv[i], &uid));
      uid_set = true;
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_client_add_usage(argc, argv), -EINVAL;
    }
  }

  try2(init_tutuicmptunnel(), _("open tutuicmptunnel device: %s"), strerrno);
  try2(get_server_mode(&is_server));
  if (is_server) {
    log_error("You must be in client mode to add peers.");
    err = -EINVAL;
    goto err_cleanup;
  }

  if (!address || !port || !uid_set) {
    log_error("UID, address, and port must be specified.");
    goto usage;
  }

  egress = (typeof(egress)) {
    .key =
      {
        .port = htons(port),
      },
    .value =
      {
        .uid = uid,
      },
  };

  try2(resolve_ip_addr(family, address, &in6));
  egress.key.address = in6;

  if (comment) {
    if (strpbrk(comment, "\r\n")) {
      log_error("Multi-line comment is not allowed.");
      err = -EINVAL;
      goto err_cleanup;
    }

    strncpy((char *) egress.value.comment, comment, sizeof(egress.value.comment));
    egress.value.comment[sizeof(egress.value.comment) - 1] = '\0';
  }

  ingress = (typeof(ingress)) {
    .key =
      {
        .uid     = uid,
        .address = in6,
      },
    .value =
      {
        .port = htons(port),
      },
  };

  /* 1. 尝试设置 (TUTU_NOEXIST) */
  err = set_ingress_peer_map(&ingress.key, &ingress.value);

  if (err < 0) { /* Netlink 封装函数出错返回 -1 */
    if (errno == EEXIST) {
      /* Lookup 也失败了 (这种情况比较少见，可能是权限或并发删除) */
      try2(lookup_ingress_peer_map(&ingress), _("lookup ingress failed: %s"), strerrno);

      /* 3. 比较端口 (保持原逻辑) */
      if (port == ntohs(ingress.value.port)) {
        err = 0; /* 端口一致，视为成功 */
      } else {
        /* 端口冲突，报错 (保持原逻辑) */
        char ipstr[INET6_ADDRSTRLEN], *uidstr = NULL;
        try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
        try2(uid2string(uid, &uidstr, 1), "uid2string: %s", strret);

        log_error("Unable to configure %s for address %s port %u because another port is already in use on this address",
                  uidstr, ipstr, port);
        free(uidstr);
        err = -EEXIST;
        goto err_cleanup;
      }
    } else {
      /* 其他错误 (保持原逻辑) */
      log_error(_("set_ingress_peer_map failed: %s"), strerror(errno));
      goto err_cleanup;
    }
  }

  try2(set_egress_peer_map(&egress.key, &egress.value), _("set_egress_peer_map: %s"), strerrno);

  {
    char ipstr[INET6_ADDRSTRLEN], *uidstr = NULL;
    try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
    try2(uid2string(uid, &uidstr, 0), "uid2string: %s", strret);
    log_info("client set: %s, address: %s, port: %u, comment: %.*s", uidstr, ipstr, port, (int) sizeof(egress.value.comment),
             egress.value.comment);
    free(uidstr);
  }

  err = 0;
err_cleanup:
  return err;
}

#define CMD_CLIENT_DEL_SUMMARY "Remove a peer from the client configuration"

static int print_client_del_usage(int argc, char **argv) {
  (void) argc;
  fprintf(stderr,
          "Usage: %s %s [OPTIONS] {uid UID | user USERNAME} address ADDRESS\n\n"

          "  " CMD_CLIENT_DEL_SUMMARY ".\n\n"

          "Arguments:\n"
          "  You must specify either a uid or a username, as well as the address, to delete the peer.\n\n"

          "  %-24s The numeric user ID of the peer to delete.\n"
          "  %-24s The username of the peer to delete.\n"
          "  %-24s The server address associated with the peer.\n"
          "  %-24s This uniquely identifies the configuration to delete.\n\n"

          "Options:\n"
          "  %-22s Display UID as a number instead of resolving it to a username"
          " in command output. \n",

          STR(PROG_NAME), argv[0], "uid UID", "user USERNAME", "address ADDRESS", "", "-n");
  return 0;
}

struct delete_search_ctx {
  uint8_t         target_uid;
  struct in6_addr target_ip;

  /* Egress 搜索结果 */
  bool                   found_egress;
  struct egress_peer_key pending_egress_key;

  /* Ingress 搜索结果 */
  bool                    found_ingress;
  struct ingress_peer_key pending_ingress_key;
};

/* Egress 的回调：匹配 UID 和 IP，匹配则删除并停止遍历 */
static int check_and_del_egress_cb(void *entry_ptr, void *user_data) {
  struct tutu_egress       *egress = entry_ptr;
  struct delete_search_ctx *ctx    = user_data;

  if (!ctx->found_egress && ctx->target_uid == egress->value.uid &&
      !memcmp(&egress->key.address, &ctx->target_ip, sizeof(struct in6_addr))) {
    ctx->pending_egress_key = egress->key;
    ctx->found_egress       = true;
    return 0; /* 不能提前停止，否则会因为遍历消息残留在socket里导致下一命令不执行 */
  }

  return 0; /* 继续遍历 */
}

/* Ingress 的回调：逻辑类似，但注意 uid 在 Key 中 */
static int check_and_del_ingress_cb(void *entry_ptr, void *user_data) {
  struct tutu_ingress      *ingress = entry_ptr;
  struct delete_search_ctx *ctx     = user_data;

  if (!ctx->found_ingress && ctx->target_uid == ingress->key.uid &&
      !memcmp(&ingress->key.address, &ctx->target_ip, sizeof(struct in6_addr))) {
    ctx->pending_ingress_key = ingress->key;
    ctx->found_ingress       = true;
    return 0;
  }
  return 0;
}

int cmd_client_del(int argc, char **argv) {
  const char     *address = NULL;
  uint8_t         uid     = 0;
  bool            uid_set = false;
  struct in6_addr in6;
  int             err       = -EINVAL;
  bool            is_server = false;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (is_address_kw(tok)) {
      if (++i >= argc)
        goto usage;

      address = argv[i];
    } else if (is_user_kw(tok)) {
      if (++i >= argc)
        goto usage;

      try(string2uid(argv[i], &uid));
      uid_set = true;
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      /* 未知关键字 */
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_client_del_usage(argc, argv), -EINVAL;
    }
  }

  try2(init_tutuicmptunnel(), _("open tutuicmptunnel device: %s"), strerrno);
  try2(get_server_mode(&is_server));
  if (is_server) {
    log_error("You must be in client mode to delete peers.");
    err = -EINVAL;
    goto err_cleanup;
  }

  // 检查必要参数
  if (!address || !uid_set) {
    log_error("UID and address must be specified.");
    goto usage;
  }

  try2(resolve_ip_addr(family, address, &in6));
  bool deleted = false;

  /* 初始化搜索上下文，传入局部变量 uid 和 in6 */
  struct delete_search_ctx ctx = {.target_uid = uid, .target_ip = in6};

  /* 1. 遍历并处理 Egress */
  foreach_egress(check_and_del_egress_cb, &ctx);

  /* 2. 遍历并处理 Ingress */
  /* 继续使用同一个 ctx */
  foreach_ingress(check_and_del_ingress_cb, &ctx);

  if (ctx.found_egress) {
    try2(delete_egress_peer_map(&ctx.pending_egress_key), "delete egress failed: %s", strerrno);
  }

  if (ctx.found_ingress) {
    try2(delete_ingress_peer_map(&ctx.pending_ingress_key), "delete ingress failed: %s", strerrno);
  }

  if (ctx.found_egress && ctx.found_ingress) {
    deleted = true;
  }

  char *uidstr = NULL;
  try2(uid2string(uid, &uidstr, 0), "uid2string: %s", strret);
  if (deleted)
    log_info("client deleted: %s", uidstr);
  else
    log_info("client peer: %s address %s not found", uidstr, address);
  free(uidstr);

  err = 0;
err_cleanup:
  return err;
}

#define CMD_SERVER_ADD_SUMMARY "Add a peer to the server configuration"

static int print_server_add_usage(int argc, char **argv) {
  (void) argc;
  fprintf(
    stderr,
    "Usage: %s %s [OPTIONS] {uid UID | user USERNAME} address ADDR port PORT [icmp-id ID] [sport PORT] [comment COMMENT]\n\n"

    "  " CMD_SERVER_ADD_SUMMARY ".\n\n"

    "Arguments:\n"
    "  You must specify the client to add by providing EITHER a uid OR a username.\n\n"

    "  %-22s The numeric user ID to authorize.\n"
    "  %-22s The username to authorize.\n"
    "  %-22s The client's source address.\n"
    "  %-22s The destination port on the server for the tunnel.\n"
    "  %-22s Optional: The specific ICMP ID for the client.\n"
    "  %-22s Optional: The specific source port for the client.\n"
    "  %-22s Optional: A descriptive comment for this client entry.\n\n"

    "Options:\n"
    "  %-22s When resolving a hostname in 'address', force IPv4.\n"
    "  %-22s When resolving a hostname in 'address', force IPv6.\n"
    "  %-22s Display UID as a number instead of resolving it to a username"
    " in command output. \n",

    STR(PROG_NAME), argv[0], "uid UID", "user USERNAME", "address ADDR", "port PORT", "icmp-id ICMP_ID", "sport PORT",
    "comment COMMENT", "-4", "-6", "-n");
  return 0;
}

int cmd_server_add(int argc, char **argv) {
  uint8_t     uid       = 0;
  const char *address   = NULL;
  uint16_t    port      = 0;
  uint16_t    icmp_id   = 0;
  bool        uid_set   = false;
  char       *comment   = NULL;
  int         err       = -EINVAL;
  bool        is_server = false;

  struct user_info user;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (is_user_kw(tok)) {
      if (++i >= argc)
        goto usage;

      try(string2uid(argv[i], &uid));
      uid_set = true;
    } else if (is_address_kw(tok)) {
      if (++i >= argc)
        goto usage;

      address = argv[i];
    } else if (matches(tok, "sport")) {
      if (++i >= argc)
        goto usage;

      log_warn("ignore obsolete sport keyword");
    } else if (matches(tok, "icmp-id")) {
      if (++i >= argc)
        goto usage;

      try(parse_icmp_id(argv[i], &icmp_id));
    } else if (matches(tok, "port")) {
      if (++i >= argc)
        goto usage;

      try(parse_port(argv[i], &port));
    } else if (matches(tok, "comment")) {
      if (++i >= argc)
        goto usage;

      comment = argv[i];
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      /* 未知关键字 */
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_server_add_usage(argc, argv), -EINVAL;
    }
  }

  /* 必要字段检查 */
  if (!uid_set || !address || !port) {
    log_error("uid, address and port must be specified");
    goto usage;
  }

  try2(init_tutuicmptunnel(), _("open tutuicmptunnel device: %s"), strerrno);
  try2(get_server_mode(&is_server));
  if (!is_server) {
    log_error("You must be in server mode to add peers.");
    err = -EINVAL;
    goto err_cleanup;
  }

  struct in6_addr client_addr = {};
  try2(resolve_ip_addr(family, address, &client_addr));

  user = (typeof(user)) {
    .address = client_addr,
    .icmp_id = htons(icmp_id),
    .dport   = htons(port),
  };

  if (comment) {
    if (strpbrk(comment, "\r\n")) {
      log_error("Multi-line comment is not allowed.");
      err = -EINVAL;
      goto err_cleanup;
    }

    strncpy((char *) user.comment, comment, sizeof(user.comment));
    user.comment[sizeof(user.comment) - 1] = '\0';
  }

  struct tutu_user_info user_info = {
    .key       = uid,
    .value     = user,
    .map_flags = TUTU_ANY,
  };

  try2(set_user_info_map(&user_info), _("netlink update user info: %s"), strerrno);

  {
    char  ipstr[INET6_ADDRSTRLEN];
    char *uidstr = NULL;
    try2(ipv6_ntop(ipstr, &user.address), "ipv6_ntop: %s %s", ipstr, strret);
    try2(uid2string(uid, &uidstr, 0), "uid2string: %s", strret);
    log_info("server updated: %s, address: %s, dport: %u, comment: %.*s", uidstr, ipstr, ntohs(user.dport),
             (int) sizeof(user.comment), user.comment);
    free(uidstr);
  }

  err = 0;
err_cleanup:
  return err;
}

#define CMD_SERVER_DEL_SUMMARY "Remove a peer from the server configuration"

static int print_server_del_usage(int argc, char **argv) {
  (void) argc;

  fprintf(stderr,
          "Usage: %s %s [OPTIONS] {uid <id> | user <name>}\n\n"

          "  " CMD_SERVER_DEL_SUMMARY ".\n\n"

          "Arguments:\n"
          "  You must specify a user to delete using exactly one of the following identifiers:\n\n"
          "  %-22s Specifies the target user by their numerical User ID (UID).\n"
          "  %-22s Specifies the target user by their username.\n\n"

          "Options:\n"
          "  %-22s Display UID as a number instead of resolving it to a username"
          " in command output. \n",

          STR(PROG_NAME), argv[0], "uid <id>", "user <name>", "-n");
  return 0;
}

int cmd_server_del(int argc, char **argv) {
  uint8_t uid       = 0;
  bool    uid_set   = false;
  int     err       = -EINVAL;
  bool    is_server = false;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (is_user_kw(tok)) {
      if (++i >= argc)
        goto usage;

      try(string2uid(argv[i], &uid));
      uid_set = true;
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      /* 未知关键字 */
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_server_del_usage(argc, argv), -EINVAL;
    }
  }

  // 检查必要参数
  if (!uid_set) {
    log_error("UID must be specified.");
    goto usage;
  }

  try2(init_tutuicmptunnel(), _("open tutuicmptunnel device: %s"), strerrno);
  try2(get_server_mode(&is_server));
  if (!is_server) {
    log_error("You must be in server mode to delete peers.");
    err = -EINVAL;
    goto err_cleanup;
  }

  try2(delete_user_info_map(uid), _("netlink delete user info failed: %s"), strerrno);

  char *uidstr = NULL;
  try2(uid2string(uid, &uidstr, 0), "uid2string: %s", strret);
  log_info("server deleted: %s", uidstr);
  free(uidstr);

  err = 0;
err_cleanup:
  return err;
}

#define CMD_STATUS_SUMMARY "Display the current status and active sessions"

static int print_status_usage(int argc, char *argv[]) {
  (void) argc;

  fprintf(stderr,
          "Usage: %s %s [OPTIONS] [debug]\n\n"

          "  " CMD_STATUS_SUMMARY ".\n\n"

          "Arguments:\n"
          "  %-22s Optional: If specified, displays detailed debugging information.\n\n"

          "Options:\n"
          "  %-22s Display UID as a number instead of resolving it to a username"
          " in command output. \n",

          STR(PROG_NAME), argv[0], "debug", "-n");
  return 0;
}

static void print_comment(const char *comment, size_t comment_len, int dsl) {
  if (!comment[0])
    return;

  printf(dsl ? " comment " : ", Comment: ");
  printf("%.*s", (int) comment_len, comment);
}

static int get_boot_seconds(__u64 *seconds) {
  if (!seconds)
    return -EINVAL;

  struct timespec ts;

#ifdef CLOCK_BOOTTIME
  if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
    if (ts.tv_sec < 0)
      return -EFAULT;
    *seconds = (__u64) ts.tv_sec;
    return 0;
  }

  if (errno != EINVAL && errno != ENOTSUP) {
    return -errno;
  }
#endif

  // 2) 回退 CLOCK_MONOTONIC（不包含挂起）
#ifdef CLOCK_MONOTONIC
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    if (ts.tv_sec < 0)
      return -EFAULT;
    *seconds = (__u64) ts.tv_sec;
    return 0;
  }
#endif

  // 3) 最后回退到 /proc/uptime
  FILE *f = fopen("/proc/uptime", "re");
  if (f) {
    double up      = 0.0;
    int    scanned = fscanf(f, "%lf", &up);
    fclose(f);
    if (scanned == 1 && up >= 0.0) {
      *seconds = (__u64) up;
      return 0;
    }

    return -EIO;
  }

  return -errno;
}

static int print_user_info_cb(void *entry_ptr, void *user_data) {
  (void) user_data;
  struct tutu_user_info *u_info = entry_ptr; // 强转类型

  char            ipstr[INET6_ADDRSTRLEN];
  struct in6_addr in6    = u_info->value.address;
  char           *uidstr = NULL;

  /* 转换 IPv6 地址 */
  if (ipv6_ntop(ipstr, &in6) < 0) {
    // 如果你的环境里有 log_error，可以用它，否则用 fprintf
    fprintf(stderr, "ipv6_ntop failed: %s\n", strerror(errno));
    return 0; // Skip this one
  }

  /* 转换 UID */
  // 注意：原代码 u_info.key 是传值，uid2string 原型可能是 (uint8_t, char**, ...)
  if (uid2string(u_info->key, &uidstr, 0) < 0) {
    fprintf(stderr, "uid2string failed: %s\n", strerror(errno));
    return 0;
  }

  /* 打印主要信息 */
  printf("  %s, Address: %s, Dport: %u, ICMP: %u", uidstr, ipstr, ntohs(u_info->value.dport), ntohs(u_info->value.icmp_id));

  /* 释放由 uid2string 分配的内存 */
  free(uidstr);

  /* 打印注释 (假设 print_comment 只是打印不换行) */
  print_comment((const char *) u_info->value.comment, sizeof(u_info->value.comment), 0);
  printf("\n");

  return 0; /* 返回 0 表示继续遍历下一个 */
}

static int print_session_cb(void *entry_ptr, void *user_data) {
  (void) user_data;

  /* 强转指针类型 */
  struct tutu_session *sess = entry_ptr;

  char            ipstr[INET6_ADDRSTRLEN];
  struct in6_addr in6    = sess->key.address;
  char           *uidstr = NULL;

  if (ipv6_ntop(ipstr, &in6) < 0) {
    log_error("ipv6_ntop failed: %s", strerrno);
    return 0;
  }

  if (uid2string(sess->value.uid, &uidstr, 0) < 0) {
    log_error("uid2string failed: %s", strerrno);
    return 0;
  }

  /* 打印逻辑 */
  printf("  Address: %s, SPort: %u, DPort: %u => %s, Age: %llu, Client Sport: %u\n", ipstr, ntohs(sess->key.sport),
         ntohs(sess->key.dport), uidstr, (unsigned long long) sess->value.age, /* 强转以匹配 %llu */
         ntohs(sess->value.client_sport));

  free(uidstr);

  return 0; /* 返回 0 表示继续遍历 */
}

static int print_egress_peer_cb(void *entry_ptr, void *user_data) {
  struct tutu_egress *egress = entry_ptr;
  int                *cnt    = user_data; /* 将 user_data 还原为 int 指针 */

  /* 原逻辑的过滤条件: if (ntohs(egress.key.port)) */
  if (ntohs(egress->key.port)) {
    char            ipstr[INET6_ADDRSTRLEN];
    struct in6_addr in6    = egress->key.address;
    char           *uidstr = NULL;

    /* 替换 try2 宏为标准错误打印，出错不中断遍历 */
    if (ipv6_ntop(ipstr, &in6) < 0) {
      fprintf(stderr, "ipv6_ntop failed: %s\n", strerror(errno));
      return 0;
    }

    if (uid2string(egress->value.uid, &uidstr, 0) < 0) {
      fprintf(stderr, "uid2string failed: %s\n", strerror(errno));
      return 0;
    }

    printf("  %s, Address: %s, Port: %u", uidstr, ipstr, ntohs(egress->key.port));

    free(uidstr);

    print_comment((const char *) egress->value.comment, sizeof(egress->value.comment), 0);
    printf("\n");

    /* 核心逻辑：计数器加一 */
    (*cnt)++;
  }

  return 0; /* 继续遍历 */
}

static int print_ingress_peer_cb(void *entry_ptr, void *user_data) {
  (void) user_data;

  struct tutu_ingress *ingress = entry_ptr;

  char  ipstr[INET6_ADDRSTRLEN];
  char *uidstr = NULL;

  if (ipv6_ntop(ipstr, &ingress->key.address) < 0) {
    fprintf(stderr, "ipv6_ntop failed: %s\n", strerror(errno));
    return 0;
  }

  if (uid2string(ingress->key.uid, &uidstr, 0) < 0) {
    fprintf(stderr, "uid2string failed: %s\n", strerror(errno));
    return 0;
  }

  printf("  %s, Address: %s => Sport: %u\n", uidstr, ipstr, ntohs(ingress->value.port));

  free(uidstr);
  return 0;
}

int cmd_status(int argc, char **argv) {
  int err = 0;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (matches(tok, "debug")) {
      debug = 1;
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_status_usage(argc, argv), -EINVAL;
    }
  }

  struct tutu_config cfg = {};
  try2(init_tutuicmptunnel(), _("open tutuicmptunnel device: %s"), strerrno);
  try2(get_config_map(&cfg), _("get_config_map: %s"), strerrno);

  printf("%s: Role: %s\n\n", STR(PROJECT_NAME), cfg.is_server ? "Server" : "Client");

  print_ifnames();

  if (cfg.is_server) {
    printf("Peers:\n");
    // 打印所有peer

    if (foreach_user_info(print_user_info_cb, NULL) < 0) {
      log_error("netlink user_info first key failed: %s", strerrno);
    }

    if (debug) {
      __u64 boot = 0;

      try2(get_boot_seconds(&boot), _("failed to get boot seconds: %s"), strret);
      printf("\nSessions (max age: %u, current: %llu):\n", cfg.session_max_age, boot);
      if (foreach_session(print_session_cb, NULL) < 0) {
        log_error("netlink session first key failed: %s", strerrno);
      }
    }
  } else { /* client 角色 */
    int cnt = 0;

    printf("Client Peers: \n");

    if (foreach_egress(print_egress_peer_cb, &cnt) < 0) {
      /* 对应原来的 log_error */
      log_error(_("dump egress peers failed: %s"), strerror(errno));
    }

    /* 如果回调一次没跑，或者跑了但没符合条件的，cnt 依然是 0 */
    if (!cnt) {
      printf("No peer configure\n");
    }

    if (debug) {
      printf("\nIngress peers:\n");

      if (foreach_ingress(print_ingress_peer_cb, NULL) < 0) {
        log_error(_("dump ingress peers failed: %s"), strerror(errno));
      }
    }
  }

  if (debug) {
    struct tutu_stats stats;

    printf("\nPackets:\n");

    try2(get_stats_map(&stats), _("get_stats_map: %s"), strerrno);
    printf("  processed:   %8llu\n", stats.packets_processed);
    printf("  dropped:     %8llu\n", stats.packets_dropped);
    printf("  cksum error: %8llu\n", stats.checksum_errors);
    printf("  fragmented:  %8llu\n", stats.fragmented);
    printf("  GSO:         %8llu\n", stats.gso);
  }

  err = 0;
err_cleanup:
  return err;
}

#define CMD_DUMP_SUMMARY "Dump the current running configuration in a raw format"

static int print_dump_usage(int argc, char *argv[]) {
  (void) argc;

  fprintf(stderr,
          "Usage: %s %s [OPTIONS]\n\n"

          "  " CMD_DUMP_SUMMARY ".\n\n"

          "Options:\n"
          "  %-22s Display UID as a number instead of resolving it to a username"
          " in command output. \n",

          STR(PROG_NAME), argv[0], "-n");
  return 0;
}

/* 回调：导出 Server 模式下的 User Info */
static int dump_user_info_cb(void *entry_ptr, void *user_data) {
  (void) user_data;

  struct tutu_user_info *u_info = entry_ptr;

  char            ipstr[INET6_ADDRSTRLEN];
  struct in6_addr in6    = u_info->value.address;
  char           *uidstr = NULL;

  /* 辅助函数出错不中断遍历，只打印错误 */
  if (ipv6_ntop(ipstr, &in6) < 0) {
    fprintf(stderr, "ipv6_ntop failed: %s\n", strerror(errno));
    return 0;
  }

  /* 注意：uid2string 第三个参数 1 表示 hex/decimal 格式控制？保持原代码逻辑 */
  if (uid2string(u_info->key, &uidstr, 1) < 0) {
    fprintf(stderr, "uid2string failed: %s\n", strerror(errno));
    return 0;
  }

  printf("server-add "
         "%s "
         "addr %s "
         "icmp-id %u "
         "port %u",
         uidstr, ipstr, ntohs(u_info->value.icmp_id), ntohs(u_info->value.dport));

  free(uidstr);

  /* print_comment 第三个参数 1 保持原逻辑 (可能表示自动换行或加#号) */
  print_comment((const char *) u_info->value.comment, sizeof(u_info->value.comment), 1);
  printf("\n");

  return 0;
}

/* 回调：导出 Client 模式下的 Egress Peers */
static int dump_egress_cb(void *entry_ptr, void *user_data) {
  (void) user_data;

  struct tutu_egress *egress = entry_ptr;

  /* 过滤逻辑：只导出端口不为 0 的项 */
  if (ntohs(egress->key.port)) {
    char            ipstr[INET6_ADDRSTRLEN];
    struct in6_addr in6    = egress->key.address;
    char           *uidstr = NULL;

    if (ipv6_ntop(ipstr, &in6) < 0) {
      fprintf(stderr, "ipv6_ntop failed: %s\n", strerror(errno));
      return 0;
    }

    if (uid2string(egress->value.uid, &uidstr, 1) < 0) {
      fprintf(stderr, "uid2string failed: %s\n", strerror(errno));
      return 0;
    }

    printf("client-add "
           "%s "
           "addr %s "
           "port %u",
           uidstr, ipstr, ntohs(egress->key.port));

    free(uidstr);

    print_comment((const char *) egress->value.comment, sizeof(egress->value.comment), 1);
    printf("\n");
  }
  return 0;
}

int cmd_dump(int argc, char **argv) {
  int                err = 0;
  struct tutu_config cfg = {};

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_dump_usage(argc, argv), -EINVAL;
    }
  }

  try2(init_tutuicmptunnel(), _("open tutuicmptunnel device: %s"), strerrno);
  try2(get_config_map(&cfg), _("get_config_map: %s"), strerrno);

  {
    time_t    now = time(NULL);
    char      buf[64];
    struct tm tm = *localtime(&now);
    strftime(buf, sizeof(buf), "%F %T %Z", &tm);
    buf[sizeof(buf) - 1] = '\0';

    printf("#!%s/sbin/ktuctl script -\n", STR(INSTALL_PREFIX_DIR));
    printf("# Auto-generated by \"ktuctl dump\" on %s\n\n", buf);
  }

  if (cfg.is_server) {
    printf("server max-age %u\n\n", cfg.session_max_age);

    if (foreach_user_info(dump_user_info_cb, NULL) < 0) {
      log_error("dump user info failed: %s", strerrno);
    }

  } else {
    printf("client\n");

    if (foreach_egress(dump_egress_cb, NULL) < 0) {
      log_error("dump egress failed: %s", strerrno);
    }
  }

  err = 0;
err_cleanup:
  return err;
}

#define CMD_REAPER_SUMMARY "Cleanup stale NAT sessions"
#define CMD_SCRIPT_SUMMARY "Execute a batch of commands from a file or standard input"

// clang-format off
static subcommand_t subcommands[] = {
  { "load", cmd_load, "Add network interface to " STR(PROJECT_NAME), },
  { "unload", cmd_unload, "Remove network interface from " STR(PROJECT_NAME), },
  { "server", cmd_server, CMD_SERVER_SUMMARY, },
  { "client", cmd_client, CMD_CLIENT_SUMMARY, },
  { "client-add", cmd_client_add, CMD_CLIENT_ADD_SUMMARY, },
  { "client-del", cmd_client_del, CMD_CLIENT_DEL_SUMMARY, },
  { "server-add", cmd_server_add, CMD_SERVER_ADD_SUMMARY, },
  { "server-del", cmd_server_del, CMD_SERVER_DEL_SUMMARY, },
  { "reaper", cmd_reaper, CMD_REAPER_SUMMARY, },
  { "status", cmd_status, CMD_STATUS_SUMMARY, },
  { "version", cmd_version, "Show program version", },
  { "dump", cmd_dump, CMD_DUMP_SUMMARY, },
  { "script", cmd_script, CMD_SCRIPT_SUMMARY, },
  { "help", cmd_help, "Show this help message"},
  {NULL, NULL, NULL},
};
// clang-format on

static void print_help_usage(void) {
  printf("Usage: %s <subcommand> [options]\n", STR(PROG_NAME));
  printf("Subcommands:\n");
  for (int i = 0; subcommands[i].name; ++i) {
    printf("  %-10s  %s\n", subcommands[i].name, subcommands[i].desc);
  }
}

int dispatch(int argc, char **argv) {
  if (argc <= 0 || !argv || !argv[0])
    return -EINVAL;

  for (subcommand_t *sc = subcommands; sc->name; ++sc) {
    if (!strcmp(argv[0], sc->name)) {
      return sc->handler(argc, argv);
    }
  }

  return log_error("error: unknown command '%s'", argv[0]), print_help_usage(), -EINVAL;
}

static int print_script_usage(int argc, char **argv) {
  (void) argc;
  fprintf(stderr,
          "Usage: %s %s [<file>|-]\n\n"

          "  " CMD_SCRIPT_SUMMARY ".\n\n"

          "Arguments:\n"
          "  %-22s The path to a script file containing commands to be executed.\n"
          "  %-22s If specified, commands will be read from standard input (stdin).\n\n",

          STR(PROG_NAME), argv[0], "<file>", "-");
  return 0;
}

extern int   script_parse(void);
extern FILE *script_in;
extern void  script_lex_destroy(void);
extern int   script_exit_code;

int cmd_script(int argc, char **argv) {
  FILE       *fp   = NULL;
  const char *path = "-";
  int         err  = 0;

  if (help || argc > 2) {
    return print_script_usage(argc, argv), -EINVAL;
  }

  if (argc == 2) {
    path = argv[1];
  }

  // 打开文件
  if (strcmp(path, "-") == 0) {
    fp = stdin;
  } else {
    fp = try2_p(fopen(path, "r"), "fopen: %s", strret);
  }

  script_in = fp;

  script_exit_code = 0;
  err              = script_parse();

  if (err) {
    log_error("Script execution aborted due to %s.", err == 1 ? "syntax error" : "memory exhaustion");
    err = -1;
    goto err_cleanup;
  }

  err = script_exit_code;
err_cleanup:
  script_lex_destroy();

  if (fp && fp != stdin) {
    fclose(fp);
  }

  return err;
}

static __u64 get_monotonic_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (__u64) ts.tv_sec;
}

static int print_reaper_usage(int argc, char *argv[]) {
  (void) argc;

  fprintf(stderr,
          "Usage: %s %s [OPTIONS]\n\n"

          "  " CMD_REAPER_SUMMARY ".\n\n"
          "%s\n",
          STR(PROG_NAME), argv[0],
#ifdef DISABLE_BPF_TIMER
          ""
#else
          "  This command is OBSOLETE. Server no longer needs it."
#endif
  );
  return 0;
}

static int delete_session_map(const struct session_key *key) {
  /* 构造一个包含 Key 的完整结构体 */
  struct tutu_session sess = {
    .key = *key,
  };
  /* 发送 DELETE_SESSION 命令 */
  return send_simple_cmd(TUTU_CMD_DELETE_SESSION, TUTU_ATTR_SESSION, &sess, sizeof(sess), 0);
}

/* 用于暂存待删除 Session Key 的节点 */
struct reap_node {
  struct session_key key;
  struct list_head   list; /* 内核风格链表锚点 */
};

/* 上下文结构体 */
struct reap_ctx {
  __u64            now;
  __u32            max_age;
  struct list_head reap_list; /* 链表头 */
  int              count;
};

/* 回调函数：检查并清理过期会话 */
static int reap_session_cb(void *entry_ptr, void *user_data) {
  struct tutu_session *sess = entry_ptr;
  struct reap_ctx     *ctx  = user_data;
  __u64                age  = sess->value.age;

  /* 检查是否过期 */
  if (!age || (ctx->now - age > ctx->max_age)) {
    char              ipstr[INET6_ADDRSTRLEN];
    struct in6_addr   in6    = sess->key.address;
    char             *uidstr = NULL;
    struct reap_node *node;

    /* 打印日志 */
    if (ipv6_ntop(ipstr, &in6) < 0)
      return 0;
    uid2string(sess->value.uid, &uidstr, 0);

    printf("Reaping old entry: Address: %s, DPort: %u, SPort: %u, %s, Age: %llu\n", ipstr, ntohs(sess->key.dport),
           ntohs(sess->key.sport), uidstr ? uidstr : "?", (unsigned long long) age);

    free(uidstr);

    /* 分配节点并加入链表 */
    node = malloc(sizeof(*node));
    if (node) {
      memcpy(&node->key, &sess->key, sizeof(node->key));
      INIT_LIST_HEAD(&node->list);
      /* 加入 ctx 链表尾部 */
      list_add_tail(&node->list, &ctx->reap_list);
      ctx->count++;
    } else {
      log_error("malloc failed during reap");
    }
  }

  return 0; /* 继续遍历，确保 Socket 排空 */
}

int cmd_reaper(int argc, char **argv) {
  struct tutu_config cfg = {};

  if (help) {
    return print_reaper_usage(argc, argv), -EINVAL;
  }

#ifndef DISABLE_BPF_TIMER
  log_warn("This command is OBSOLETE. Server no longer needs it.");
#endif

  int   err = 0;
  __u64 now = get_monotonic_seconds();

  struct reap_ctx ctx = {
    .now = now,
  };

  /* 立即初始化链表头 */
  INIT_LIST_HEAD(&ctx.reap_list);

  try2(init_tutuicmptunnel(), _("open tutuicmptunnel device: %s"), strerrno);
  try2(get_config_map(&cfg), _("netlink get tutu config: %s"), strerrno);
  log_info("max allowed age: %u", cfg.session_max_age);

  /* 获取配置成功后，更新 max_age */
  ctx.max_age = cfg.session_max_age;

  /* 1. 第一阶段：Dump 并收集 (Mark) */
  if (foreach_session(reap_session_cb, &ctx) < 0) {
    log_error("dump sessions failed: %s", strerrno);
  }

  /* 2. 第二阶段：遍历链表并删除 (Sweep) */
  if (ctx.count > 0) {
    log_info("Found %d expired sessions, deleting...", ctx.count);

    struct reap_node *node, *tmp;

    /* 使用 safe 版本，因为我们在循环里 free(node) */
    list_for_each_entry_safe(node, tmp, &ctx.reap_list, list) {
      /* 发起删除请求 */
      if (delete_session_map(&node->key) < 0) {
        log_warn("failed to delete session: %s", strerrno);
      }

      /* 从链表中摘除并释放内存 */
      list_del(&node->list);
      free(node);
    }
  }

  err = 0;
err_cleanup:
  /* 如果中间出错 goto 这里的处理：防止内存泄漏 */
  if (!list_empty(&ctx.reap_list)) {
    struct reap_node *node, *tmp;
    list_for_each_entry_safe(node, tmp, &ctx.reap_list, list) {
      list_del(&node->list);
      free(node);
    }
  }
  return err;
}

int cmd_help(int argc, char **argv) {
  (void) argc;
  (void) argv;
  print_help_usage();
  return 0;
}

int cmd_version(int argc, char **argv) {
  (void) argc;
  (void) argv;
  printf("%s: %s (%s)\n", STR(PROJECT_NAME), VERSION_STR, HOMEPAGE_STR);
  return 0;
}

static int parse_argument(int argc, char **argv, int *subcmd_pos) {
  int                  err            = 0;
  static struct option long_options[] = {
    {"numeric", no_argument, 0, 'n'}, {"debug", no_argument, 0, 'd'}, {"ipv4", no_argument, 0, '4'},
    {"ipv6", no_argument, 0, '6'},    {"help", no_argument, 0, 'h'},  {0, 0, 0, 0},
  };

  int opt;

  if (subcmd_pos)
    *subcmd_pos = -1;

  while ((opt = getopt_long(argc, argv, "nd46h", long_options, NULL)) != -1) {
    switch (opt) {
    case 'n':
      log_info("using numeric output for UIDs");
      numeric = 1;
      break;
    case 'd':
      log_info("using debug mode");
      debug = 1;
      break;
    case '4':
      log_info("hostnames will be resolved to IPv4 addresses only");
      family = AF_INET;
      break;
    case '6':
      log_info("hostnames will be resolved to IPv6 addresses only");
      family = AF_INET6;
      break;
    case 'h':
      help = 1;
      break;
    default:
      break;
    }
  }

  if (subcmd_pos && optind < argc)
    *subcmd_pos = optind;

  if (help && *subcmd_pos < 0) {
    cmd_help(argc, argv);
    err = -EINVAL;
  }

  return err;
}

int main(int argc, char *argv[]) {
  int err, subcmd_pos;

  uid_map_init(&uids);
  // 忽略错误
  err = uid_map_load(&uids, UID_CONFIG_PATH);

  try(parse_argument(argc, argv, &subcmd_pos));

  if (subcmd_pos < 0) {
    char *newargv[] = {"status", NULL};

    err = cmd_status(ARRAY_SIZE(newargv) - 1, newargv);
    goto err_cleanup;
  }

  err = dispatch(argc - subcmd_pos, argv + subcmd_pos);
err_cleanup:
  uid_map_free(&uids);
  deinit_tutuicmptunnel();
  return err;
}

// vim: set sw=2 ts=2 expandtab:
