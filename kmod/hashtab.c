#include <linux/filter.h>
#include <linux/jhash.h>
#include <linux/vmalloc.h>

#include "hashtab.h"
#include "tutuicmptunnel.h"

/* Called from syscall */
struct tutu_htab *tutu_map_alloc(u32 key_size, u32 value_size, u32 max_entries) {
  struct tutu_htab *htab;
  int               err, i;

  htab = kzalloc(sizeof(*htab), GFP_KERNEL);
  if (!htab)
    return ERR_PTR(-ENOMEM);

  /* mandatory map attributes */
  htab->key_size    = key_size;
  htab->value_size  = value_size;
  htab->max_entries = max_entries;

  /* check sanity of attributes.
   * value_size == 0 may be allowed in the future to use map as a set
   */
  err = -EINVAL;
  if (htab->max_entries == 0 || htab->key_size == 0 || htab->value_size == 0)
    goto free_htab;

  /* hash table size must be power of 2 */
  htab->n_buckets = roundup_pow_of_two(htab->max_entries);
  htab->elem_size = sizeof(struct htab_elem) + round_up(htab->key_size, 8) + htab->value_size;

  /* prevent zero size kmalloc and check for u32 overflow */
  if (htab->n_buckets == 0 || htab->n_buckets > U32_MAX / sizeof(struct hlist_head))
    goto free_htab;

  if ((u64) htab->n_buckets * sizeof(struct hlist_head) + (u64) htab->elem_size * htab->max_entries >= U32_MAX - PAGE_SIZE)
    /* make sure page count doesn't overflow */
    goto free_htab;

  err           = -ENOMEM;
  htab->buckets = kmalloc_array(htab->n_buckets, sizeof(struct hlist_head), GFP_KERNEL | __GFP_NOWARN);

  if (!htab->buckets) {
    htab->buckets = vmalloc(htab->n_buckets * sizeof(struct hlist_head));
    if (!htab->buckets)
      goto free_htab;
  }

  for (i = 0; i < htab->n_buckets; i++)
    INIT_HLIST_HEAD(&htab->buckets[i]);

  raw_spin_lock_init(&htab->lock);
  htab->count = 0;

  return htab;

free_htab:
  kfree(htab);
  return ERR_PTR(err);
}

static inline u32 htab_map_hash(const void *key, u32 key_len) {
  return jhash(key, key_len, 0);
}

static inline struct hlist_head *select_bucket(struct tutu_htab *htab, u32 hash) {
  return &htab->buckets[hash & (htab->n_buckets - 1)];
}

static struct htab_elem *lookup_elem_raw(struct hlist_head *head, u32 hash, void *key, u32 key_size) {
  struct htab_elem *l;

  hlist_for_each_entry_rcu(l, head, hash_node) if (l->hash == hash && !memcmp(&l->key, key, key_size)) return l;

  return NULL;
}

static void *htab_map_lookup_elem(struct tutu_htab *htab, void *key) {
  struct hlist_head *head;
  struct htab_elem  *l;
  u32                hash, key_size;

  /* Must be called with rcu_read_lock. */
  WARN_ON_ONCE(!rcu_read_lock_held());

  key_size = htab->key_size;

  hash = htab_map_hash(key, key_size);

  head = select_bucket(htab, hash);

  l = lookup_elem_raw(head, hash, key, key_size);

  if (l)
    return l->key + round_up(htab->key_size, 8);

  return NULL;
}

static int htab_map_get_next_key(struct tutu_htab *htab, void *key, void *next_key) {
  struct hlist_head *head;
  struct htab_elem  *l, *next_l;
  u32                hash, key_size;
  int                i = 0;

  WARN_ON_ONCE(!rcu_read_lock_held());

  key_size = htab->key_size;

  if (!key)
    goto find_first_elem;

  hash = htab_map_hash(key, key_size);

  head = select_bucket(htab, hash);

  /* lookup the key */
  l = lookup_elem_raw(head, hash, key, key_size);

  if (!l)
    goto find_first_elem;

  /* key was found, get next key in the same bucket */
  next_l = hlist_entry_safe(rcu_dereference_raw(hlist_next_rcu(&l->hash_node)), struct htab_elem, hash_node);

  if (next_l) {
    /* if next elem in this hash list is non-zero, just return it */
    memcpy(next_key, next_l->key, key_size);
    return 0;
  }

  /* no more elements in this hash list, go to the next bucket */
  i = hash & (htab->n_buckets - 1);
  i++;

find_first_elem:
  /* iterate over buckets */
  for (; i < htab->n_buckets; i++) {
    head = select_bucket(htab, i);

    /* pick first element in the bucket */
    next_l = hlist_entry_safe(rcu_dereference_raw(hlist_first_rcu(head)), struct htab_elem, hash_node);
    if (next_l) {
      /* if it's not empty, just return it */
      memcpy(next_key, next_l->key, key_size);
      return 0;
    }
  }

  /* itereated over all buckets and all elements */
  return -ENOENT;
}

static int htab_map_update_elem(struct tutu_htab *htab, void *key, void *value, u64 map_flags) {
  struct htab_elem  *l_new, *l_old;
  struct hlist_head *head;
  unsigned long      flags;
  u32                key_size;
  int                ret;

  if (map_flags > TUTU_EXIST)
    /* unknown flags */
    return -EINVAL;

  WARN_ON_ONCE(!rcu_read_lock_held());

  /* allocate new element outside of lock */
  l_new = kmalloc(htab->elem_size, GFP_ATOMIC | __GFP_NOWARN);
  if (!l_new)
    return -ENOMEM;

  key_size = htab->key_size;

  memcpy(l_new->key, key, key_size);
  memcpy(l_new->key + round_up(key_size, 8), value, htab->value_size);

  l_new->hash = htab_map_hash(l_new->key, key_size);

  /* htab_map_update_elem() can be called in_irq() */
  raw_spin_lock_irqsave(&htab->lock, flags);

  head = select_bucket(htab, l_new->hash);

  l_old = lookup_elem_raw(head, l_new->hash, key, key_size);

  if (!l_old && unlikely(htab->count >= htab->max_entries)) {
    /* if elem with this 'key' doesn't exist and we've reached
     * max_entries limit, fail insertion of new elem
     */
    ret = -E2BIG;
    goto err;
  }

  if (l_old && map_flags == TUTU_NOEXIST) {
    /* elem already exists */
    ret = -EEXIST;
    goto err;
  }

  if (!l_old && map_flags == TUTU_EXIST) {
    /* elem doesn't exist, cannot update it */
    ret = -ENOENT;
    goto err;
  }

  /* add new element to the head of the list, so that concurrent
   * search will find it before old elem
   */
  hlist_add_head_rcu(&l_new->hash_node, head);
  if (l_old) {
    hlist_del_rcu(&l_old->hash_node);
    kfree_rcu(l_old, rcu);
  } else {
    htab->count++;
  }
  raw_spin_unlock_irqrestore(&htab->lock, flags);

  return 0;
err:
  raw_spin_unlock_irqrestore(&htab->lock, flags);
  kfree(l_new);
  return ret;
}

/* Called from syscall or from eBPF program */
static int htab_map_delete_elem(struct tutu_htab *htab, void *key) {
  struct hlist_head *head;
  struct htab_elem  *l;
  unsigned long      flags;
  u32                hash, key_size;
  int                ret = -ENOENT;

  WARN_ON_ONCE(!rcu_read_lock_held());

  key_size = htab->key_size;

  hash = htab_map_hash(key, key_size);

  raw_spin_lock_irqsave(&htab->lock, flags);

  head = select_bucket(htab, hash);

  l = lookup_elem_raw(head, hash, key, key_size);

  if (l) {
    hlist_del_rcu(&l->hash_node);
    htab->count--;
    kfree_rcu(l, rcu);
    ret = 0;
  }

  raw_spin_unlock_irqrestore(&htab->lock, flags);
  return ret;
}

static void delete_all_elements(struct tutu_htab *htab) {
  int i;

  for (i = 0; i < htab->n_buckets; i++) {
    struct hlist_head *head = select_bucket(htab, i);
    struct hlist_node *n;
    struct htab_elem  *l;

    hlist_for_each_entry_safe(l, n, head, hash_node) {
      hlist_del_rcu(&l->hash_node);
      htab->count--;
      kfree(l);
    }
  }
}

/* Called when map->refcnt goes to zero, either from workqueue or from syscall */
void tutu_map_free(struct tutu_htab *htab) {
  /* at this point bpf_prog->aux->refcnt == 0 and this map->refcnt == 0,
   * so the programs (can be more than one that used this map) were
   * disconnected from events. Wait for outstanding critical sections in
   * these programs to complete
   */
  synchronize_rcu();

  /* some of kfree_rcu() callbacks for elements of this map may not have
   * executed. It's ok. Proceed to free residual elements and map itself
   */
  delete_all_elements(htab);
  kvfree(htab->buckets);
  kfree(htab);
}

void *tutu_map_lookup_elem(struct tutu_htab *htab, void *key) {
  void *val;

  val = htab_map_lookup_elem(htab, key);
  return val;
}

int tutu_map_delete_elem(struct tutu_htab *htab, void *key) {
  int err;

  err = htab_map_delete_elem(htab, key);
  return err;
}

int tutu_map_get_next_key(struct tutu_htab *htab, void *key, void *next_key) {
  int err;

  err = htab_map_get_next_key(htab, key, next_key);
  return err;
}

int tutu_map_update_elem(struct tutu_htab *htab, void *key, void *value, u64 map_flags) {
  int err;

  err = htab_map_update_elem(htab, key, value, map_flags);
  return err;
}

// vim: set sw=2 ts=2 expandtab:
