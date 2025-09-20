#pragma once

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

#define ICMP6_ECHO_REQUEST 128
#define ICMP6_ECHO_REPLY   129

// Jumps to `err_cleanup`, returning _ret while printing error.
//
// Requires `err_cleanup` label, `err` to be defined inside function scope, and `err` to be
// returned after cleanup.
#define err_cleanup(...)                                                                                                       \
  _get_macro(_0, ##__VA_ARGS__, _cleanup_fmt, _cleanup_fmt, _cleanup_fmt, _cleanup_fmt, _cleanup, )(__VA_ARGS__)
#define _cleanup(ret)                                                                                                          \
  ({                                                                                                                           \
    err = (ret);                                                                                                               \
    goto err_cleanup;                                                                                                          \
  })
#define _cleanup_fmt(ret, ...)                                                                                                 \
  ({                                                                                                                           \
    pr_debug(__VA_ARGS__);                                                                                                     \
    err = (ret);                                                                                                               \
    goto err_cleanup;                                                                                                          \
  })

#define _get_macro(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

#define try2_ret(expr, retval, ...)                                                                                            \
  ({                                                                                                                           \
    long _ret = (expr);                                                                                                        \
    if (unlikely(_ret < 0))                                                                                                    \
      err_cleanup(retval, ##__VA_ARGS__);                                                                                      \
    _ret;                                                                                                                      \
  })

#define try2_ok(x, ...) try2_ret(x, NF_ACCEPT, ##__VA_ARGS__)

#define try2_p_ret(expr, retval, ...)                                                                                          \
  ({                                                                                                                           \
    void *_ptr = (expr);                                                                                                       \
    if (unlikely(!_ptr))                                                                                                       \
      err_cleanup(retval, ##__VA_ARGS__);                                                                                      \
    _ptr;                                                                                                                      \
  })

#define try2_p_ok(x, ...) try2_p_ret(x, NF_ACCEPT, ##__VA_ARGS__)

// vim: set sw=2 tabstop=2 expandtab:
