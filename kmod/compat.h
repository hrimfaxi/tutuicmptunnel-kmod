#pragma once

#include <linux/version.h>

#ifndef DECLARE_FLEX_ARRAY
#define DECLARE_FLEX_ARRAY(TYPE, NAME) TYPE NAME[0]
#endif

/*
 * 兼容性处理：
 * Linux 7.2 起 system_unbound_wq 被标记为 __WQ_DEPRECATED，入队时会打印
 * "work func ... enqueued on deprecated workqueue. Use system_{percpu|dfl}_wq
 * instead."（commit 64d8eae3f895），替代品 system_dfl_wq 自 6.17 起可用
 * (commit 128ea9f6ccfb "workqueue: Add system_percpu_wq and system_dfl_wq")。
 * 故 7.2+ 改用 system_dfl_wq，老内核继续用 system_unbound_wq。
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
#define TUTU_SYSTEM_UNBOUND_WQ system_dfl_wq
#else
#define TUTU_SYSTEM_UNBOUND_WQ system_unbound_wq
#endif

// vim: set sw=2 ts=2 expandtab:
