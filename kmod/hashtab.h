#pragma once

#include <linux/spinlock.h>
#include <linux/types.h>

#include "compat.h"

struct tutu_htab {
  struct hlist_head *buckets;
  raw_spinlock_t     lock;
  u32                count;     /* number of elements in this hashtable */
  u32                n_buckets; /* number of hash buckets */
  u32                elem_size; /* size of each element in bytes */
  u32                key_size;
  u32                value_size;
  u32                max_entries;
};

/* each htab element is struct htab_elem + key + value */
struct htab_elem {
  struct hlist_node hash_node;
  struct rcu_head   rcu;
  u32               hash;
  DECLARE_FLEX_ARRAY(char, key);
};

struct tutu_htab *tutu_map_alloc(u32 key_size, u32 value_size, u32 max_entries);
void             *tutu_map_lookup_elem(struct tutu_htab *htab, void *key);
int               tutu_map_get_next_key(struct tutu_htab *htab, void *key, void *next_key);
int               tutu_map_update_elem(struct tutu_htab *htab, void *key, void *value, u64 map_flags);
int               tutu_map_delete_elem(struct tutu_htab *htab, void *key);
void              tutu_map_free(struct tutu_htab *htab);

// vim: set sw=2 ts=2 expandtab:
