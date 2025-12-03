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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "list.h"
#include "log.h"
#include "resolve.h"
#include "try.h"
#include "tuctl.h"
#include "tuparser.h"

#define HOMEPAGE_STR "https://github.com/hrimfaxi/" STR(PROJECT_NAME)

#include "tutuicmptunnel.h"

#define TUTU_MAP_FOREACH(fd, entry, GET_FIRST, GET_NEXT, LOOKUP, body)                                                         \
  do {                                                                                                                         \
    try2(ioctl(fd, GET_FIRST, &(entry)), #GET_FIRST ": %s", strerrno);                                                         \
    do {                                                                                                                       \
      try2(ioctl(fd, LOOKUP, &(entry)), #LOOKUP ": %s", strerrno);                                                             \
      body                                                                                                                     \
    } while (!ioctl(fd, GET_NEXT, &(entry)));                                                                                  \
  } while (0)

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

static int tutuicmptunnel_fd = -1;

static void deinit_tutuicmptunnel(void) {
  if (tutuicmptunnel_fd >= 0)
    close(tutuicmptunnel_fd);
  tutuicmptunnel_fd = -1;
}

static int init_tutuicmptunnel(void) {
  int fd;

  deinit_tutuicmptunnel();
  fd = open("/dev/tutuicmptunnel", O_RDWR, 0);
  if (fd < 0)
    return fd;

  tutuicmptunnel_fd = fd;
  return fd;
}

static int set_config_map(int fd, const struct tutu_config *cfg) {
  return ioctl(fd, TUTU_SET_CONFIG, cfg);
}

static int get_config_map(int fd, struct tutu_config *cfg) {
  return ioctl(fd, TUTU_GET_CONFIG, cfg);
}

static int set_egress_peer_map(int fd, const struct egress_peer_key *key, const struct egress_peer_value *value) {
  struct tutu_egress egress = {
    .key       = *key,
    .value     = *value,
    .map_flags = TUTU_ANY,
  };

  return ioctl(fd, TUTU_UPDATE_EGRESS, &egress);
}

static int set_ingress_peer_map(int fd, const struct ingress_peer_key *key, const struct ingress_peer_value *value) {
  struct tutu_ingress ingress = {
    .key       = *key,
    .value     = *value,
    .map_flags = TUTU_NOEXIST,
  };

  return ioctl(fd, TUTU_UPDATE_INGRESS, &ingress);
}

static int delete_egress_peer_map(int fd, const struct egress_peer_key *key) {
  struct tutu_egress egress = {
    .key = *key,
  };

  return ioctl(fd, TUTU_DELETE_EGRESS, &egress);
}

static int delete_ingress_peer_map(int fd, const struct ingress_peer_key *key) {
  struct tutu_ingress ingress = {
    .key = *key,
  };

  return ioctl(fd, TUTU_DELETE_INGRESS, &ingress);
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

struct iface_node {
  char             name[IFNAMSIZ];
  struct list_head head;
};

static int add_iface(struct list_head *iface_list_head, const char *iface) {
  struct iface_node *new_iface = (typeof(new_iface)) malloc(sizeof(*new_iface));

  if (!new_iface)
    return -ENOMEM;

  strncpy(new_iface->name, iface, sizeof(new_iface->name));
  new_iface->name[sizeof(new_iface->name) - 1] = '\0';
  list_add_tail(&new_iface->head, iface_list_head);
  return 0;
}

static int write_to_sysfs(const char *sysfs_name, const char *value) {
  int     fd;
  size_t  len;
  ssize_t written;

  fd = open(sysfs_name, O_WRONLY);
  if (fd < 0)
    return -errno;

  len     = strlen(value);
  written = write(fd, value, len);
  close(fd);

  if (written < 0)
    return -errno;

  if ((size_t) written != len)
    return -EIO; /* 未写全 */

  return 0;
}

static void print_iface_usage(const char *prog, const char *action_word) {
  fprintf(stderr,
          "Usage: %s %s [OPTIONS] iface IFACE [IFACE...]\n\n"
          "  %s a network interface %s " STR(PROJECT_NAME) ".\n\n"
                                                           "Arguments:\n"
                                                           "  %-22s One or more network interface names (e.g., eth0, ppp0)\n",
          prog, action_word, strcmp(action_word, "load") == 0 ? "Add" : "Remove",
          strcmp(action_word, "load") == 0 ? "to" : "from", "IFACE [IFACE...]");
}

static int handle_iface_op(int argc, char **argv, const char *sysfs_path, const char *action_word, const char *action_desc) {
  LIST_HEAD(iface_list_head);
  struct iface_node *node, *tmp_node;
  int                err = -EINVAL;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (strcasecmp(tok, "iface") == 0) {
      if (i + 1 >= argc) {
        log_error("\"iface\" keyword needs an argument.");
        goto usage;
      }
      const char *ifname = argv[++i];
      try2(add_iface(&iface_list_head, ifname), "add_iface: %s", strret);
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
      goto usage;
    }
  }

  if (list_empty(&iface_list_head)) {
    log_error("At least one interface name must be specified with \"iface\".");
    goto usage;
  }

  list_for_each_entry(node, &iface_list_head, head) {
    log_info("%s for interface: %s", action_desc, node->name);
    try2(write_to_sysfs(sysfs_path, node->name), _("failed to write to sysfs: %s"), strret);
  }

  err = 0;

err_cleanup:
  list_for_each_entry_safe(node, tmp_node, &iface_list_head, head) {
    list_del(&node->head);
    free(node);
  }
  return err;

usage:
  print_iface_usage(STR(PROG_NAME), action_word);
  err = -EINVAL;
  goto err_cleanup;
}

int cmd_load(int argc, char **argv) {
  return handle_iface_op(argc, argv, "/sys/module/tutuicmptunnel/parameters/ifnames_add", "load", "Configuring");
}

int cmd_unload(int argc, char **argv) {
  return handle_iface_op(argc, argv, "/sys/module/tutuicmptunnel/parameters/ifnames_remove", "unload", "Deconfiguring");
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
  try2(set_config_map(tutuicmptunnel_fd, &cfg), _("set_config_map: %s"), strerrno);

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
  try2(set_config_map(tutuicmptunnel_fd, &cfg));

  err = 0;
err_cleanup:
  return err;
}

static int get_server_mode(bool *server) {
  struct tutu_config cfg = {};
  int                err;

  try2(get_config_map(tutuicmptunnel_fd, &cfg), _("get_config_map: %s"), strerrno);

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

  err = set_ingress_peer_map(tutuicmptunnel_fd, &ingress.key, &ingress.value);
  if (err) {
    if (errno == EEXIST) {
      try2(ioctl(tutuicmptunnel_fd, TUTU_LOOKUP_INGRESS, &ingress), _("ioctl lookup ingress: %s"), strerrno);
      if (port == ntohs(ingress.value.port)) {
        err = 0;
      } else {
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
      log_error(_("set_ingress_peer_map failed: %s"), strerrno);
      goto err_cleanup;
    }
  }

  err = try2(set_egress_peer_map(tutuicmptunnel_fd, &egress.key, &egress.value), _("set_egress_peer_map: %s"), strerrno);

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

  struct tutu_egress egress;

  TUTU_MAP_FOREACH(tutuicmptunnel_fd, egress, TUTU_GET_FIRST_KEY_EGRESS, TUTU_GET_NEXT_KEY_EGRESS, TUTU_LOOKUP_EGRESS, {
    if (uid == egress.value.uid && !memcmp(&egress.key.address, &in6, sizeof(in6))) {
      try2(delete_egress_peer_map(tutuicmptunnel_fd, &egress.key), _("delete egress peer map: %s"), strerrno);
      deleted = true;
      break;
    }
  });

  struct tutu_ingress ingress;

  TUTU_MAP_FOREACH(tutuicmptunnel_fd, ingress, TUTU_GET_FIRST_KEY_INGRESS, TUTU_GET_NEXT_KEY_INGRESS, TUTU_LOOKUP_INGRESS, {
    if (uid == ingress.key.uid && !memcmp(&ingress.key.address, &in6, sizeof(in6))) {
      try2(delete_ingress_peer_map(tutuicmptunnel_fd, &ingress.key), _("delete ingress peer map: %s"), strerrno);
      deleted = true;
      break;
    }
  });

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
  try(resolve_ip_addr(family, address, &client_addr));

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

  err = try2(ioctl(tutuicmptunnel_fd, TUTU_UPDATE_USER_INFO, &user_info), _("ioctl update user info: %s"), strerrno);

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

  err = try2(ioctl(tutuicmptunnel_fd, TUTU_DELETE_USER_INFO, &uid), _("ioctl get user info failed: %s"), strerrno);

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

static int get_stats_map(int fd, struct tutu_stats *stats) {
  int err;

  err = ioctl(fd, TUTU_GET_STATS, stats);
  return err;
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

static void rstrip(char *s) {
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char) s[n - 1]))
    s[--n] = '\0';
}

static char *lskip(char *s) {
  while (*s && isspace((unsigned char) *s))
    s++;
  return s;
}

static int print_ifnames(const char *path) {
  int    err;
  FILE  *f   = NULL;
  char  *buf = NULL;
  size_t cap = 0;

  f = try2_p(fopen(path, "re"), "failed to open %s: %s", path, strerrno);

  {
    errno     = 0;
    ssize_t n = getdelim(&buf, &cap, '\0', f);
    try2(n >= 0 ? 0 : -1, "failed to read %s: %s", path, strerrno);
  }

  rstrip(buf);
  char *p = lskip(buf);

  printf("Managed interfaces:\n");

  if (!*p) {
    // 空字符串：表示所有接口被管理
    printf("  [all interfaces]\n\n");
    err = 0;
    goto err_cleanup;
  }

  // CSV 分割并逐项打印
  char *saveptr = NULL;
  for (char *tok = strtok_r(p, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
    tok = lskip(tok);
    rstrip(tok);
    if (*tok == '\0')
      continue;
    printf("  %s\n", tok);
  }

  printf("\n");
  err = 0;

err_cleanup:
  if (f)
    fclose(f);
  free(buf);
  return err;
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
  try2(get_config_map(tutuicmptunnel_fd, &cfg), _("get_config_map: %s"), strerrno);

  printf("%s: Role: %s\n\n", STR(PROJECT_NAME), cfg.is_server ? "Server" : "Client");

  print_ifnames("/sys/module/tutuicmptunnel/parameters/ifnames");

  if (cfg.is_server) {
    struct tutu_user_info user_info;

    printf("Peers:\n");
    // 打印所有peer

    err = ioctl(tutuicmptunnel_fd, TUTU_GET_FIRST_KEY_USER_INFO, &user_info);

    if (err == 0) {
      TUTU_MAP_FOREACH(tutuicmptunnel_fd, user_info, TUTU_GET_FIRST_KEY_USER_INFO, TUTU_GET_NEXT_KEY_USER_INFO,
                       TUTU_LOOKUP_USER_INFO, {
                         char            ipstr[INET6_ADDRSTRLEN];
                         struct in6_addr in6    = user_info.value.address;
                         char           *uidstr = NULL;

                         try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s %s", ipstr, strret);
                         try2(uid2string(user_info.key, &uidstr, 0), "uid2string: %s", strret);
                         printf("  %s, Address: %s, "
                                "Dport: %u, ICMP: %u",
                                uidstr, ipstr, ntohs(user_info.value.dport), ntohs(user_info.value.icmp_id));
                         free(uidstr);
                         print_comment((const char *) user_info.value.comment, sizeof(user_info.value.comment), 0);
                         printf("\n");
                       });
    } else {
      if (errno != ENOENT) {
        log_error("ioctl user_info first key failed: %s", strerrno);
      }
    }

    if (debug) {
      struct tutu_session session;
      __u64               boot = 0;

      try2(get_boot_seconds(&boot), _("failed to get boot seconds: %s"), strret);
      printf("\nSessions (max age: %u, current: %llu):\n", cfg.session_max_age, boot);
      err = ioctl(tutuicmptunnel_fd, TUTU_GET_FIRST_KEY_SESSION, &session);

      if (err == 0) {
        TUTU_MAP_FOREACH(tutuicmptunnel_fd, session, TUTU_GET_FIRST_KEY_SESSION, TUTU_GET_NEXT_KEY_SESSION, TUTU_LOOKUP_SESSION,
                         {
                           char            ipstr[INET6_ADDRSTRLEN];
                           struct in6_addr in6;
                           char           *uidstr = NULL;

                           in6 = session.key.address;
                           try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
                           try2(uid2string(session.value.uid, &uidstr, 0), "uid2string: %s", strret);
                           printf("  Address: %s, SPort: %u, DPort: %u => "
                                  "%s, Age: %llu, Client Sport: %u\n",
                                  ipstr, ntohs(session.key.sport), ntohs(session.key.dport), uidstr, session.value.age,
                                  ntohs(session.value.client_sport));
                           free(uidstr);
                         });
      } else {
        if (errno != ENOENT) {
          log_error("ioctl session first key failed: %s", strerrno);
        }
      }
    }
  } else { /* client 角色 */
    int                cnt = 0;
    struct tutu_egress egress;

    printf("Client Peers: \n");
    err = ioctl(tutuicmptunnel_fd, TUTU_GET_FIRST_KEY_EGRESS, &egress);

    if (err == 0) {
      TUTU_MAP_FOREACH(tutuicmptunnel_fd, egress, TUTU_GET_FIRST_KEY_EGRESS, TUTU_GET_NEXT_KEY_EGRESS, TUTU_LOOKUP_EGRESS, {
        if (ntohs(egress.key.port)) {
          char            ipstr[INET6_ADDRSTRLEN];
          struct in6_addr in6    = egress.key.address;
          char           *uidstr = NULL;

          try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
          try2(uid2string(egress.value.uid, &uidstr, 0), "uid2string: %s", strret);
          printf("  %s, Address: %s, Port: %u", uidstr, ipstr, ntohs(egress.key.port));
          free(uidstr);
          print_comment((const char *) egress.value.comment, sizeof(egress.value.comment), 0);
          printf("\n");
          cnt++;
        }
      });
    } else {
      if (errno != ENOENT) {
        log_error("ioctl egress first key failed: %s", strerrno);
      }
    }

    if (!cnt) {
      printf("No peer configure\n");
    }

    if (debug) {
      printf("\nIngress peers:\n");
      struct tutu_ingress ingress;

      err = ioctl(tutuicmptunnel_fd, TUTU_GET_FIRST_KEY_INGRESS, &ingress);

      if (err == 0) {
        TUTU_MAP_FOREACH(tutuicmptunnel_fd, ingress, TUTU_GET_FIRST_KEY_INGRESS, TUTU_GET_NEXT_KEY_INGRESS, TUTU_LOOKUP_INGRESS,
                         {
                           char  ipstr[INET6_ADDRSTRLEN];
                           char *uidstr = NULL;

                           try2(ipv6_ntop(ipstr, &ingress.key.address), "ipv6_ntop: %s", strret);
                           try2(uid2string(ingress.key.uid, &uidstr, 0), "uid2string: %s", strret);
                           printf("  %s, Address: %s => Sport: %u\n", uidstr, ipstr, htons(ingress.value.port));
                           free(uidstr);
                         });
      } else {
        if (errno != ENOENT) {
          log_error("ioctl ingress first key failed: %s", strerrno);
        }
      }
    }
  }

  if (debug) {
    struct tutu_stats stats;

    printf("\nPackets:\n");

    try2(get_stats_map(tutuicmptunnel_fd, &stats), _("get_stats_map: %s"), strerrno);
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
  try2(get_config_map(tutuicmptunnel_fd, &cfg), _("get_config_map: %s"), strerrno);

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
    struct tutu_user_info user_info;

    printf("server max-age %u\n\n", cfg.session_max_age);
    err = ioctl(tutuicmptunnel_fd, TUTU_GET_FIRST_KEY_USER_INFO, &user_info);

    if (err == 0) {
      TUTU_MAP_FOREACH(tutuicmptunnel_fd, user_info, TUTU_GET_FIRST_KEY_USER_INFO, TUTU_GET_NEXT_KEY_USER_INFO,
                       TUTU_LOOKUP_USER_INFO, {
                         char            ipstr[INET6_ADDRSTRLEN];
                         struct in6_addr in6;
                         char           *uidstr = NULL;

                         in6 = user_info.value.address;
                         try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
                         try2(uid2string(user_info.key, &uidstr, 1), "uid2string: %s", strret);
                         printf("server-add "
                                "%s "
                                "addr %s "
                                "icmp-id %u "
                                "port %u",
                                uidstr, ipstr, ntohs(user_info.value.icmp_id), ntohs(user_info.value.dport));
                         free(uidstr);

                         print_comment((const char *) user_info.value.comment, sizeof(user_info.value.comment), 1);
                         printf("\n");
                       });
    } else {
      if (errno != ENOENT) {
        log_error("ioctl user_info first key failed: %s", strerrno);
      }
    }
  } else {
    struct tutu_egress egress;

    printf("client\n");
    err = ioctl(tutuicmptunnel_fd, TUTU_GET_FIRST_KEY_EGRESS, &egress);

    if (err == 0) {
      TUTU_MAP_FOREACH(tutuicmptunnel_fd, egress, TUTU_GET_FIRST_KEY_EGRESS, TUTU_GET_NEXT_KEY_EGRESS, TUTU_LOOKUP_EGRESS, {
        if (ntohs(egress.key.port)) {
          char            ipstr[INET6_ADDRSTRLEN];
          struct in6_addr in6    = egress.key.address;
          char           *uidstr = NULL;

          try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
          try2(uid2string(egress.value.uid, &uidstr, 1), "uid2string: %s", strret);
          printf("client-add "
                 "%s "
                 "addr %s "
                 "port %u",
                 uidstr, ipstr, ntohs(egress.key.port));
          free(uidstr);
          print_comment((const char *) egress.value.comment, sizeof(egress.value.comment), 1);
          printf("\n");
        }
      });
    } else {
      if (errno != ENOENT) {
        log_error("ioctl egress first key failed: %s", strerrno);
      }
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

  try2(init_tutuicmptunnel(), _("open tutuicmptunnel device: %s"), strerrno);
  try2(ioctl(tutuicmptunnel_fd, TUTU_GET_CONFIG, &cfg), _("ioctl get tutu config: %s"), strerrno);
  log_info("max allowed age: %u", cfg.session_max_age);

  struct tutu_session session;

  err = ioctl(tutuicmptunnel_fd, TUTU_GET_FIRST_KEY_SESSION, &session);

  if (err == 0) {
    TUTU_MAP_FOREACH(tutuicmptunnel_fd, session, TUTU_GET_FIRST_KEY_SESSION, TUTU_GET_NEXT_KEY_SESSION, TUTU_LOOKUP_SESSION, {
      char            ipstr[INET6_ADDRSTRLEN];
      struct in6_addr in6;
      __u64           age;

      in6 = session.key.address;
      try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
      age = session.value.age;
      if (!age || now - age > cfg.session_max_age) {
        char *uidstr = NULL;
        try2(uid2string(session.value.uid, &uidstr, 0), "uid2string: %s", strret);
        printf("Reaping old entry: Address: %s, DPort: %u, SPort: %u, %s, Age: %llu\n", ipstr, ntohs(session.key.dport),
               ntohs(session.key.sport), uidstr, age);
        free(uidstr);
        ioctl(tutuicmptunnel_fd, TUTU_DELETE_USER_INFO, &session.key);
      }
    });
  } else {
    if (errno != ENOENT) {
      log_error("ioctl session first key failed: %s", strerrno);
    }
  }

  err = 0;
err_cleanup:
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
