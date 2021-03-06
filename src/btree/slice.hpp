// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef BTREE_SLICE_HPP_
#define BTREE_SLICE_HPP_

#include <vector>

#include "buffer_cache/types.hpp"
#include "concurrency/fifo_checker.hpp"
#include "containers/scoped.hpp"
#include "perfmon/perfmon.hpp"

const unsigned int STARTING_ROOT_EVICTION_PRIORITY = 2 << 16;

class backfill_callback_t;
class key_tester_t;

class btree_stats_t {
public:
    explicit btree_stats_t(perfmon_collection_t *parent, const std::string &identifier)
        : btree_collection(),
          btree_collection_membership(parent, &btree_collection, "btree-" + identifier),
          pm_keys_read(secs_to_ticks(1)),
          pm_keys_set(secs_to_ticks(1)),
          pm_keys_expired(secs_to_ticks(1)),
          pm_keys_membership(&btree_collection,
              &pm_keys_read, "keys_read",
              &pm_keys_set, "keys_set",
              &pm_keys_expired, "keys_expired",
              NULLPTR)
    { }

    perfmon_collection_t btree_collection;
    perfmon_membership_t btree_collection_membership;
    perfmon_rate_monitor_t
        pm_keys_read,
        pm_keys_set,
        pm_keys_expired;
    perfmon_multi_membership_t pm_keys_membership;
};

/* btree_slice_t is a thin wrapper around cache_t that handles initializing the buffer
cache for the purpose of storing a btree. There are many btree_slice_ts per
btree_key_value_store_t. */

class btree_slice_t : public home_thread_mixin_debug_only_t {
public:
    // Initializes a cache for use with btrees (by creating the superblock in block SUPERBLOCK_ID),
    // setting the initial value of the metainfo (with a single key/value pair).
    static void create(cache_t *cache, const std::vector<char> &metainfo_key, const std::vector<char> &metainfo_value);

    // Blocks
    // Creates a btree_slice_t on a cache with data in it putting the
    // superblock at the specified location
    static void create(cache_t *cache, block_id_t superblock_id, transaction_t *txn,
            const std::vector<char> &metainfo_key, const std::vector<char> &metainfo_value);

    // Blocks
    btree_slice_t(cache_t *cache, perfmon_collection_t *parent, const std::string &identifier, block_id_t superblock_id = SUPERBLOCK_ID);

    // Blocks
    ~btree_slice_t();

    cache_t *cache() { return cache_; }
    cache_account_t *get_backfill_account() { return backfill_account.get(); }

    order_checkpoint_t pre_begin_txn_checkpoint_;

    btree_stats_t stats;

    block_id_t get_superblock_id();
private:
    cache_t *cache_;

    block_id_t superblock_id_;

    // Cache account to be used when backfilling.
    scoped_ptr_t<cache_account_t> backfill_account;

    DISABLE_COPYING(btree_slice_t);

    //Information for cache eviction
public:
    eviction_priority_t root_eviction_priority;
};

#endif /* BTREE_SLICE_HPP_ */
