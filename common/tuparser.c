#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "list.h"
#include "log.h"
#include "try.h"
#include "tuparser.h"

int parse_u16(const char *input, uint16_t *out_u16) {
  char         *endptr = NULL;
  unsigned long value;

  errno = 0;
  value = strtoul(input, &endptr, 0);
  if (endptr == input || *endptr || errno == ERANGE || value > UINT16_MAX) {
    log_error("Invalid u16: %s", input);
    return -EINVAL;
  }

  *out_u16 = (typeof(*out_u16)) value;
  return 0;
}

// 源端口允许为0
int parse_sport(const char *input, uint16_t *out_sport) {
  return parse_u16(input, out_sport);
}

int parse_port(const char *input, uint16_t *out_port) {
  int err;

  err = parse_sport(input, out_port);
  if (!err && !*out_port) {
    log_error("Invalid port: %s", input);
    err = -EINVAL;
  }

  return err;
}

int parse_icmp_id(const char *input, uint16_t *out_icmp_id) {
  return parse_u16(input, out_icmp_id);
}

int parse_uid(const char *input, uint8_t *out_uid) {
  int      err;
  uint16_t tmp = 0;

  err = parse_u16(input, &tmp);
  if (err)
    return err;

  if (tmp > 255) {
    log_error("Invalid UID: %s", input);
    return -EINVAL;
  }

  *out_uid = (typeof(*out_uid)) tmp;
  return err;
}

int parse_u32(const char *input, uint32_t *out_u32) {
  char         *endptr = NULL;
  unsigned long ulong_val;

  errno     = 0; // 复位errno： strtoul函数本身不保证成功时设置errno=0
  ulong_val = strtoul(input, &endptr, 0);
  if (endptr == input || *endptr || errno == ERANGE || ulong_val > UINT32_MAX) {
    log_error("Invalid u32: %s", input);
    return -EINVAL;
  }

  *out_u32 = (typeof(*out_u32)) ulong_val;
  return 0;
}

int parse_age(const char *input, uint32_t *out_age) {
  int err = parse_u32(input, out_age);

  if (err)
    return err;

  if (*out_age == 0) {
    log_error("Invalid age: %s", input);
    return -EINVAL;
  }

  return 0;
}

int parse_window(const char *input, uint32_t *out_window) {
  int err = parse_u32(input, out_window);

  if (err)
    return err;

  if (*out_window == 0) {
    log_error("Invalid window: %s", input);
    return -EINVAL;
  }

  return 0;
}

void strip_inline_comment(char *s) {
  int in_squote = 0, in_dquote = 0;

  for (char *p = s; *p; ++p) {
    if (*p == '\\') { // 跳过反斜杠后的一个字符
      if (p[1])
        ++p;
      continue;
    }
    if (!in_dquote && *p == '\'') {
      in_squote = !in_squote;
      continue;
    }
    if (!in_squote && *p == '"') {
      in_dquote = !in_dquote;
      continue;
    }

    if (!in_squote && !in_dquote && *p == '#') { // 真·注释
      *p = '\0';
      break;
    }
  }
}

int matches(const char *arg, const char *keyword) {
  return strcasecmp(arg, keyword);
}

/* 可选：允许“addr”当“address” */
bool is_address_kw(const char *arg) {
  return matches(arg, "address") == 0 || matches(arg, "addr") == 0;
}

bool is_help_kw(const char *arg) {
  return matches(arg, "-h") == 0 || matches(arg, "--help") == 0 || matches(arg, "help") == 0;
}

bool is_user_kw(const char *arg) {
  return matches(arg, "uid") == 0 || matches(arg, "user") == 0;
}

int strdup_safe(const char *src, char **dst) {
  int   err;
  char *dup = strdup(src);

  if (!dup) {
    err = -ENOMEM;
    goto out;
  }

  *dst = dup;
  err  = 0;
out:
  return err;
}

// 参数链表节点
struct arg_node {
  struct list_head list;
  char            *arg;
};

static int unescape_copy(char *dst, const char *src, const char *end) {
  int         i = 0;
  const char *q = src;

  while (q < end) {
    if (*q == '\\' && (q + 1) < end)
      ++q; // 跳过转义符
    dst[i++] = *q++;
  }
  dst[i] = '\0';
  return i;
}

// 释放参数链表及其内存
void free_args_list(struct list_head *head) {
  struct arg_node *node, *tmp;
  list_for_each_entry_safe(node, tmp, head, list) {
    list_del(&node->list);
    free(node->arg);
    free(node);
  }
}

// 解析一行文本为参数链表，支持单双引号和转义
int split_args_list(const char *line, struct list_head *head) {
  int   err;
  char *arg = NULL;

  const char *p = line;
  while (*p) {
    while (isspace(*p))
      ++p; // 跳过前导空白
    if (!*p)
      break; // 行尾

    char        quote = 0;
    const char *start;

    if (*p == '"' || *p == '\'') {
      // 双/单引号参数
      quote = *p++;
      start = p;
      while (*p && *p != quote) {
        // 支持引号内转义（如 "a\"b"）
        if (*p == '\\' && p[1])
          ++p;
        ++p;
      }
    } else {
      // 普通参数
      start = p;
      while (*p && !isspace(*p) && *p != '"' && *p != '\'') {
        if (*p == '\\' && p[1])
          ++p;
        ++p;
      }
    }

    arg = try2_p(malloc(p - start + 1), "malloc");
    unescape_copy(arg, start, p);

    if (quote && *p == quote)
      ++p; // 跳过配对引号

    // 创建链表节点并插入链表
    struct arg_node *node = (typeof(node)) try2_p(malloc(sizeof(*node)), "malloc");
    node->arg             = arg;
    arg                   = NULL; // 已被链表管理
    list_add_tail(&node->list, head);
  }

  err = 0;
err_cleanup:
  if (err) {
    free(arg);
    free_args_list(head);
  }
  return err;
}

int args_list_to_argv(struct list_head *head, int *argc_out, char ***argv_out) {
  int              argc = 0;
  struct arg_node *node;
  char           **argv = NULL;

  // 统计参数个数
  list_for_each_entry(node, head, list) {
    ++argc;
  }

  // 分配数组空间
  argv = malloc(sizeof(char *) * (argc + 1));
  if (!argv)
    return -ENOMEM;

  int i = 0;
  list_for_each_entry(node, head, list) {
    argv[i++] = node->arg;
  }

  argv[i] = NULL;

  if (argc_out)
    *argc_out = argc;
  if (argv_out)
    *argv_out = argv;
  return 0;
}

int escapestr(const char *str, char **escaped) {
  if (!str || !escaped)
    return -EINVAL;

  size_t in_len  = strlen(str);
  size_t out_max = in_len * 2 + 1;

  char *buf = malloc(out_max);
  if (!buf)
    return -ENOMEM;

  size_t j = 0;
  for (size_t i = 0; i < in_len; ++i) {
    char c = str[i];
    switch (c) {
    case '"':
    case '\'':
    case '\\':
    case '$':
      buf[j++] = '\\';
    // fallthrough
    default:
      buf[j++] = c;
    }
  }

  buf[j]   = '\0';
  *escaped = buf;
  return 0;
}

// vim: set sw=2 ts=2 expandtab:
