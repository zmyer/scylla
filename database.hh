/*
 * Copyright (C) 2014 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DATABASE_HH_
#define DATABASE_HH_

#include "dht/i_partitioner.hh"
#include "locator/abstract_replication_strategy.hh"
#include "core/sstring.hh"
#include "core/shared_ptr.hh"
#include "net/byteorder.hh"
#include "utils/UUID_gen.hh"
#include "utils/UUID.hh"
#include "utils/hash.hh"
#include "db_clock.hh"
#include "gc_clock.hh"
#include <chrono>
#include "core/distributed.hh"
#include <functional>
#include <cstdint>
#include <unordered_map>
#include <map>
#include <set>
#include <iosfwd>
#include <boost/functional/hash.hpp>
#include <boost/range/algorithm/find.hpp>
#include <experimental/optional>
#include <string.h>
#include "types.hh"
#include "compound.hh"
#include "core/future.hh"
#include "core/gate.hh"
#include "cql3/column_specification.hh"
#include "db/commitlog/replay_position.hh"
#include <limits>
#include <cstddef>
#include "schema.hh"
#include "timestamp.hh"
#include "tombstone.hh"
#include "atomic_cell.hh"
#include "query-request.hh"
#include "keys.hh"
#include "mutation.hh"
#include "memtable.hh"
#include <list>
#include "mutation_reader.hh"
#include "row_cache.hh"
#include "compaction_strategy.hh"
#include "sstables/compaction_manager.hh"
#include "utils/exponential_backoff_retry.hh"
#include "utils/histogram.hh"
#include "utils/estimated_histogram.hh"
#include "sstables/compaction.hh"
#include "sstables/sstable_set.hh"
#include <seastar/core/rwlock.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/metrics_registration.hh>
#include "tracing/trace_state.hh"
#include <boost/intrusive/parent_from_member.hpp>
#include "db/view/view.hh"

class cell_locker;
class locked_cell;

class frozen_mutation;
class reconcilable_result;

namespace service {
class storage_proxy;
}

namespace sstables {

class sstable;
class entry_descriptor;
}

namespace db {
template<typename T>
class serializer;

class commitlog;
class config;

namespace system_keyspace {
void make(database& db, bool durable, bool volatile_testing_only);
}
}

class replay_position_reordered_exception : public std::exception {};

using shared_memtable = lw_shared_ptr<memtable>;
class memtable_list;

class dirty_memory_manager: public logalloc::region_group_reclaimer {
    // We need a separate boolean, because from the LSA point of view, pressure may still be
    // mounting, in which case the pressure flag could be set back on if we force it off.
    bool _db_shutdown_requested = false;

    database* _db;
    logalloc::region_group _region_group;

    // We would like to serialize the flushing of memtables. While flushing many memtables
    // simultaneously can sustain high levels of throughput, the memory is not freed until the
    // memtable is totally gone. That means that if we have throttled requests, they will stay
    // throttled for a long time. Even when we have virtual dirty, that only provides a rough
    // estimate, and we can't release requests that early.
    semaphore _flush_serializer;
    // We will accept a new flush before another one ends, once it is done with the data write.
    // That is so we can keep the disk always busy. But there is still some background work that is
    // left to be done. Mostly, update the caches and seal the auxiliary components of the SSTable.
    // This semaphore will cap the amount of background work that we have. Note that we're not
    // overly concerned about memtable memory, because dirty memory will put a limit to that. This
    // is mostly about dangling continuations. So that doesn't have to be a small number.
    static constexpr unsigned _max_background_work = 20;
    semaphore _background_work_flush_serializer = { _max_background_work };
    condition_variable _should_flush;
    int64_t _dirty_bytes_released_pre_accounted = 0;

    future<> flush_when_needed();
    struct flush_permit {
        semaphore_units<> permit;

        flush_permit(semaphore_units<>&& permit) : permit(std::move(permit)) {}
    };

    // We need to start a flush before the current one finishes, otherwise
    // we'll have a period without significant disk activity when the current
    // SSTable is being sealed, the caches are being updated, etc. To do that
    // we need to keep track of who is it that we are flushing this memory from.
    std::unordered_map<const logalloc::region*, flush_permit> _flush_manager;

    future<> _waiting_flush;
    virtual void start_reclaiming() noexcept override;

    bool has_pressure() const {
        return over_soft_limit();
    }

    seastar::metrics::metric_groups _metrics;
public:
    void setup_collectd(sstring namestr);

    future<> shutdown();

    // Limits and pressure conditions:
    // ===============================
    //
    // Virtual Dirty
    // -------------
    // We can't free memory until the whole memtable is flushed because we need to keep it in memory
    // until the end, but we can fake freeing memory. When we are done with an element of the
    // memtable, we will update the region group pretending memory just went down by that amount.
    //
    // Because the amount of memory that we pretend to free should be close enough to the actual
    // memory used by the memtables, that effectively creates two sub-regions inside the dirty
    // region group, of equal size. In the worst case, we will have <memtable_total_space> dirty
    // bytes used, and half of that already virtually freed.
    //
    // Hard Limit
    // ----------
    // The total space that can be used by memtables in each group is defined by the threshold, but
    // we will only allow the region_group to grow to half of that. This is because of virtual_dirty
    // as explained above. Because virtual dirty is implemented by reducing the usage in the
    // region_group directly on partition written, we want to throttle every time half of the memory
    // as seen by the region_group. To achieve that we need to set the hard limit (first parameter
    // of the region_group_reclaimer) to 1/2 of the user-supplied threshold
    //
    // Soft Limit
    // ----------
    // When the soft limit is hit, no throttle happens. The soft limit exists because we don't want
    // to start flushing only when the limit is hit, but a bit earlier instead. If we were to start
    // flushing only when the hard limit is hit, workloads in which the disk is fast enough to cope
    // would see latency added to some requests unnecessarily.
    //
    // We then set the soft limit to 80 % of the virtual dirty hard limit, which is equal to 40 % of
    // the user-supplied threshold.
    dirty_memory_manager(database& db, size_t threshold)
        : logalloc::region_group_reclaimer(threshold / 2, threshold * 0.40)
        , _db(&db)
        , _region_group(*this)
        , _flush_serializer(1)
        , _waiting_flush(flush_when_needed()) {}

    dirty_memory_manager() : logalloc::region_group_reclaimer()
        , _db(nullptr)
        , _region_group(*this)
        , _flush_serializer(1)
        , _waiting_flush(make_ready_future<>()) {}

    static dirty_memory_manager& from_region_group(logalloc::region_group *rg) {
        return *(boost::intrusive::get_parent_from_member(rg, &dirty_memory_manager::_region_group));
    }

    logalloc::region_group& region_group() {
        return _region_group;
    }

    const logalloc::region_group& region_group() const {
        return _region_group;
    }

    void revert_potentially_cleaned_up_memory(logalloc::region* from, int64_t delta) {
        _region_group.update(delta);
        _dirty_bytes_released_pre_accounted -= delta;
    }

    void account_potentially_cleaned_up_memory(logalloc::region* from, int64_t delta) {
        _region_group.update(-delta);
        _dirty_bytes_released_pre_accounted += delta;
    }

    // This can be called multiple times during the lifetime of the region, and should always
    // ultimately be called after the flush ends. However, some flushers may decide to call it
    // earlier. For instance, the normal memtables sealing function will call this before updating
    // the cache.
    //
    // Also, for sealing methods like the normal memtable sealing method - that may retry after a
    // failed write, calling this method after the attempt is completed with success or failure is
    // mandatory. That's because the new attempt will create a new flush reader for the same
    // SSTable, so we need to make sure that we revert the old charges.
    void remove_from_flush_manager(const logalloc::region *region) {
        auto it = _flush_manager.find(region);
        if (it != _flush_manager.end()) {
            _flush_manager.erase(it);
        }
    }

    void add_to_flush_manager(const logalloc::region *region, flush_permit&& permit) {
        _flush_manager.emplace(region, std::move(permit));
    }

    size_t real_dirty_memory() const {
        return _region_group.memory_used() + _dirty_bytes_released_pre_accounted;
    }

    size_t virtual_dirty_memory() const {
        return _region_group.memory_used();
    }

    future<> flush_one(memtable_list& cf, semaphore_units<> permit);

    future<semaphore_units<>> get_flush_permit() {
        return get_units(_flush_serializer, 1);
    }
};

extern thread_local dirty_memory_manager default_dirty_memory_manager;

// We could just add all memtables, regardless of types, to a single list, and
// then filter them out when we read them. Here's why I have chosen not to do
// it:
//
// First, some of the methods in which a memtable is involved (like seal) are
// assume a commitlog, and go through great care of updating the replay
// position, flushing the log, etc.  We want to bypass those, and that has to
// be done either by sprikling the seal code with conditionals, or having a
// separate method for each seal.
//
// Also, if we ever want to put some of the memtables in as separate allocator
// region group to provide for extra QoS, having the classes properly wrapped
// will make that trivial: just pass a version of new_memtable() that puts it
// in a different region, while the list approach would require a lot of
// conditionals as well.
//
// If we are going to have different methods, better have different instances
// of a common class.
class memtable_list {
public:
    enum class flush_behavior { delayed, immediate };
private:
    std::vector<shared_memtable> _memtables;
    std::function<future<> (flush_behavior)> _seal_fn;
    std::function<schema_ptr()> _current_schema;
    dirty_memory_manager* _dirty_memory_manager;
    std::experimental::optional<shared_promise<>> _flush_coalescing;
public:
    memtable_list(std::function<future<> (flush_behavior)> seal_fn, std::function<schema_ptr()> cs, dirty_memory_manager* dirty_memory_manager)
        : _memtables({})
        , _seal_fn(seal_fn)
        , _current_schema(cs)
        , _dirty_memory_manager(dirty_memory_manager) {
        add_memtable();
    }

    memtable_list(std::function<schema_ptr()> cs, dirty_memory_manager* dirty_memory_manager)
        : _memtables({})
        , _seal_fn()
        , _current_schema(cs)
        , _dirty_memory_manager(dirty_memory_manager) {
        add_memtable();
    }

    bool may_flush() const {
        return bool(_seal_fn);
    }

    shared_memtable back() {
        return _memtables.back();
    }

    // The caller has to make sure the element exist before calling this.
    void erase(const shared_memtable& element) {
        _memtables.erase(boost::range::find(_memtables, element));
    }
    void clear() {
        _memtables.clear();
    }

    size_t size() const {
        return _memtables.size();
    }

    future<> seal_active_memtable(flush_behavior behavior) {
        return _seal_fn(behavior);
    }

    auto begin() noexcept {
        return _memtables.begin();
    }

    auto begin() const noexcept {
        return _memtables.begin();
    }

    auto end() noexcept {
        return _memtables.end();
    }

    auto end() const noexcept {
        return _memtables.end();
    }

    memtable& active_memtable() {
        return *_memtables.back();
    }

    void add_memtable() {
        _memtables.emplace_back(new_memtable());
    }

    logalloc::region_group& region_group() {
        return _dirty_memory_manager->region_group();
    }
    // This is used for explicit flushes. Will queue the memtable for flushing and proceed when the
    // dirty_memory_manager allows us to. We will not seal at this time since the flush itself
    // wouldn't happen anyway. Keeping the memtable in memory will potentially increase the time it
    // spends in memory allowing for more coalescing opportunities.
    future<> request_flush();
private:
    lw_shared_ptr<memtable> new_memtable();
};

using sstable_list = sstables::sstable_list;

// The CF has a "stats" structure. But we don't want all fields here,
// since some of them are fairly complex for exporting to collectd. Also,
// that structure matches what we export via the API, so better leave it
// untouched. And we need more fields. We will summarize it in here what
// we need.
struct cf_stats {
    int64_t pending_memtables_flushes_count = 0;
    int64_t pending_memtables_flushes_bytes = 0;

    // number of time the clustering filter was executed
    int64_t clustering_filter_count = 0;
    // sstables considered by the filter (so dividing this by the previous one we get average sstables per read)
    int64_t sstables_checked_by_clustering_filter = 0;
    // number of times the filter passed the fast-path checks
    int64_t clustering_filter_fast_path_count = 0;
    // how many sstables survived the clustering key checks
    int64_t surviving_sstables_after_clustering_filter = 0;
};

class column_family {
public:
    struct config {
        sstring datadir;
        bool enable_disk_writes = true;
        bool enable_disk_reads = true;
        bool enable_cache = true;
        bool enable_commitlog = true;
        bool enable_incremental_backups = false;
        ::dirty_memory_manager* dirty_memory_manager = &default_dirty_memory_manager;
        ::dirty_memory_manager* streaming_dirty_memory_manager = &default_dirty_memory_manager;
        restricted_mutation_reader_config read_concurrency_config;
        restricted_mutation_reader_config streaming_read_concurrency_config;
        ::cf_stats* cf_stats = nullptr;
        uint64_t max_cached_partition_size_in_bytes;
    };
    struct no_commitlog {};
    struct stats {
        /** Number of times flush has resulted in the memtable being switched out. */
        int64_t memtable_switch_count = 0;
        /** Estimated number of tasks pending for this column family */
        int64_t pending_flushes = 0;
        int64_t live_disk_space_used = 0;
        int64_t total_disk_space_used = 0;
        int64_t live_sstable_count = 0;
        /** Estimated number of compactions pending for this column family */
        int64_t pending_compactions = 0;
        utils::timed_rate_moving_average_and_histogram reads{256};
        utils::timed_rate_moving_average_and_histogram writes{256};
        utils::estimated_histogram estimated_read;
        utils::estimated_histogram estimated_write;
        utils::estimated_histogram estimated_sstable_per_read{35};
        utils::timed_rate_moving_average_and_histogram tombstone_scanned;
        utils::timed_rate_moving_average_and_histogram live_scanned;
    };

    struct snapshot_details {
        int64_t total;
        int64_t live;
    };
private:
    schema_ptr _schema;
    config _config;
    mutable stats _stats;

    uint64_t _failed_counter_applies_to_memtable = 0;

    template<typename... Args>
    void do_apply(Args&&... args);

    lw_shared_ptr<memtable_list> _memtables;

    // In older incarnations, we simply commited the mutations to memtables.
    // However, doing that makes it harder for us to provide QoS within the
    // disk subsystem. Keeping them in separate memtables allow us to properly
    // classify those streams into its own I/O class
    //
    // We could write those directly to disk, but we still want the mutations
    // coming through the wire to go to a memtable staging area.  This has two
    // major advantages:
    //
    // first, it will allow us to properly order the partitions. They are
    // hopefuly sent in order but we can't really guarantee that without
    // sacrificing sender-side parallelism.
    //
    // second, we will be able to coalesce writes from multiple plan_id's and
    // even multiple senders, as well as automatically tapping into the dirty
    // memory throttling mechanism, guaranteeing we will not overload the
    // server.
    lw_shared_ptr<memtable_list> _streaming_memtables;
    utils::phased_barrier _streaming_flush_phaser;

    // If mutations are fragmented during streaming the sstables cannot be made
    // visible immediately after memtable flush, because that could cause
    // readers to see only a part of a partition thus violating isolation
    // guarantees.
    // Mutations that are sent in fragments are kept separately in per-streaming
    // plan memtables and the resulting sstables are not made visible until
    // the streaming is complete.
    struct streaming_memtable_big {
        lw_shared_ptr<memtable_list> memtables;
        std::vector<sstables::shared_sstable> sstables;
        seastar::gate flush_in_progress;
    };
    std::unordered_map<utils::UUID, lw_shared_ptr<streaming_memtable_big>> _streaming_memtables_big;

    future<> flush_streaming_big_mutations(utils::UUID plan_id);
    void apply_streaming_big_mutation(schema_ptr m_schema, utils::UUID plan_id, const frozen_mutation& m);
    future<> seal_active_streaming_memtable_big(streaming_memtable_big& smb);

    lw_shared_ptr<memtable_list> make_memory_only_memtable_list();
    lw_shared_ptr<memtable_list> make_memtable_list();
    lw_shared_ptr<memtable_list> make_streaming_memtable_list();
    lw_shared_ptr<memtable_list> make_streaming_memtable_big_list(streaming_memtable_big& smb);

    sstables::compaction_strategy _compaction_strategy;
    // generation -> sstable. Ordered by key so we can easily get the most recent.
    lw_shared_ptr<sstables::sstable_set> _sstables;
    // sstables that have been compacted (so don't look up in query) but
    // have not been deleted yet, so must not GC any tombstones in other sstables
    // that may delete data in these sstables:
    std::vector<sstables::shared_sstable> _sstables_compacted_but_not_deleted;
    // sstables that have been opened but not loaded yet, that's because refresh
    // needs to load all opened sstables atomically, and now, we open a sstable
    // in all shards at the same time, which makes it hard to store all sstables
    // we need to load later on for all shards.
    std::vector<sstables::shared_sstable> _sstables_opened_but_not_loaded;
    // sstables that are shared between several shards so we want to rewrite
    // them (split the data belonging to this shard to a separate sstable),
    // but for correct compaction we need to start the compaction only after
    // reading all sstables.
    std::vector<sstables::shared_sstable> _sstables_need_rewrite;
    // Control background fibers waiting for sstables to be deleted
    seastar::gate _sstable_deletion_gate;
    // There are situations in which we need to stop writing sstables. Flushers will take
    // the read lock, and the ones that wish to stop that process will take the write lock.
    rwlock _sstables_lock;
    mutable row_cache _cache; // Cache covers only sstables.
    std::experimental::optional<int64_t> _sstable_generation = {};

    db::replay_position _highest_flushed_rp;
    // Provided by the database that owns this commitlog
    db::commitlog* _commitlog;
    compaction_manager& _compaction_manager;
    int _compaction_disabled = 0;
    class memtable_flush_queue;
    std::unique_ptr<memtable_flush_queue> _flush_queue;
    // Because streaming mutations bypass the commitlog, there is
    // no need for the complications of the flush queue. Besides, it
    // is easier to just use a common gate than it is to modify the flush_queue
    // to work both with and without a replay position.
    //
    // Last but not least, we seldom need to guarantee any ordering here: as long
    // as all data is waited for, we're good.
    seastar::gate _streaming_flush_gate;
    std::unordered_map<sstring, db::view::view> _views;
    std::vector<view_ptr> _view_schemas;
    semaphore _cache_update_sem{1};

    std::unique_ptr<cell_locker> _counter_cell_locks;
private:
    void update_stats_for_new_sstable(uint64_t disk_space_used_by_sstable, std::vector<unsigned>&& shards_for_the_sstable);
    // Adds new sstable to the set of sstables
    // Doesn't update the cache.
    // Doesn't trigger compaction.
    void add_sstable(lw_shared_ptr<sstables::sstable> sstable, std::vector<unsigned>&& shards_for_the_sstable);
    // returns an empty pointer if sstable doesn't belong to current shard.
    future<lw_shared_ptr<sstables::sstable>> open_sstable(sstables::foreign_sstable_open_info info, sstring dir,
        int64_t generation, sstables::sstable::version_types v, sstables::sstable::format_types f);
    void load_sstable(lw_shared_ptr<sstables::sstable>& sstable, bool reset_level = false);
    lw_shared_ptr<memtable> new_memtable();
    lw_shared_ptr<memtable> new_streaming_memtable();
    future<stop_iteration> try_flush_memtable_to_sstable(lw_shared_ptr<memtable> memt);
    future<> update_cache(memtable&, lw_shared_ptr<sstables::sstable_set> old_sstables);
    struct merge_comparator;

    // update the sstable generation, making sure that new new sstables don't overwrite this one.
    void update_sstables_known_generation(unsigned generation) {
        if (!_sstable_generation) {
            _sstable_generation = 1;
        }
        _sstable_generation = std::max<uint64_t>(*_sstable_generation, generation /  smp::count + 1);
    }

    uint64_t calculate_generation_for_new_table() {
        assert(_sstable_generation);
        // FIXME: better way of ensuring we don't attempt to
        // overwrite an existing table.
        return (*_sstable_generation)++ * smp::count + engine().cpu_id();
    }

    // inverse of calculate_generation_for_new_table(), used to determine which
    // shard a sstable should be opened at.
    static int64_t calculate_shard_from_sstable_generation(int64_t sstable_generation) {
        return sstable_generation % smp::count;
    }

    // Rebuild existing _sstables with new_sstables added to it and sstables_to_remove removed from it.
    void rebuild_sstable_list(const std::vector<sstables::shared_sstable>& new_sstables,
                              const std::vector<sstables::shared_sstable>& sstables_to_remove);
    void rebuild_statistics();
private:
    using virtual_reader_type = std::function<mutation_reader(schema_ptr, const dht::partition_range&, const query::partition_slice&, const io_priority_class&, tracing::trace_state_ptr)>;
    virtual_reader_type _virtual_reader;
    // Creates a mutation reader which covers sstables.
    // Caller needs to ensure that column_family remains live (FIXME: relax this).
    // The 'range' parameter must be live as long as the reader is used.
    // Mutations returned by the reader will all have given schema.
    mutation_reader make_sstable_reader(schema_ptr schema,
                                        const dht::partition_range& range,
                                        const query::partition_slice& slice,
                                        const io_priority_class& pc,
                                        tracing::trace_state_ptr trace_state) const;

    mutation_source sstables_as_mutation_source();
    partition_presence_checker make_partition_presence_checker(lw_shared_ptr<sstables::sstable_set>);
    std::chrono::steady_clock::time_point _sstable_writes_disabled_at;
    void do_trigger_compaction();
public:
    uint64_t failed_counter_applies_to_memtable() const {
        return _failed_counter_applies_to_memtable;
    }

    // This function should be called when this column family is ready for writes, IOW,
    // to produce SSTables. Extensive details about why this is important can be found
    // in Scylla's Github Issue #1014
    //
    // Nothing should be writing to SSTables before we have the chance to populate the
    // existing SSTables and calculate what should the next generation number be.
    //
    // However, if that happens, we want to protect against it in a way that does not
    // involve overwriting existing tables. This is one of the ways to do it: every
    // column family starts in an unwriteable state, and when it can finally be written
    // to, we mark it as writeable.
    //
    // Note that this *cannot* be a part of add_column_family. That adds a column family
    // to a db in memory only, and if anybody is about to write to a CF, that was most
    // likely already called. We need to call this explicitly when we are sure we're ready
    // to issue disk operations safely.
    void mark_ready_for_writes() {
        update_sstables_known_generation(0);
    }

    // Creates a mutation reader which covers all data sources for this column family.
    // Caller needs to ensure that column_family remains live (FIXME: relax this).
    // Note: for data queries use query() instead.
    // The 'range' parameter must be live as long as the reader is used.
    // Mutations returned by the reader will all have given schema.
    // If I/O needs to be issued to read anything in the specified range, the operations
    // will be scheduled under the priority class given by pc.
    mutation_reader make_reader(schema_ptr schema,
            const dht::partition_range& range = query::full_partition_range,
            const query::partition_slice& slice = query::full_slice,
            const io_priority_class& pc = default_priority_class(),
            tracing::trace_state_ptr trace_state = nullptr) const;

    // The streaming mutation reader differs from the regular mutation reader in that:
    //  - Reflects all writes accepted by replica prior to creation of the
    //    reader and a _bounded_ amount of writes which arrive later.
    //  - Does not populate the cache
    mutation_reader make_streaming_reader(schema_ptr schema,
            const dht::partition_range& range = query::full_partition_range) const;

    // Requires ranges to be sorted and disjoint.
    mutation_reader make_streaming_reader(schema_ptr schema,
            const dht::partition_range_vector& ranges) const;

    mutation_source as_mutation_source(tracing::trace_state_ptr trace_state) const;

    void set_virtual_reader(virtual_reader_type virtual_reader) {
        _virtual_reader = std::move(virtual_reader);
    }

    // Queries can be satisfied from multiple data sources, so they are returned
    // as temporaries.
    //
    // FIXME: in case a query is satisfied from a single memtable, avoid a copy
    using const_mutation_partition_ptr = std::unique_ptr<const mutation_partition>;
    using const_row_ptr = std::unique_ptr<const row>;
    memtable& active_memtable() { return _memtables->active_memtable(); }
    const row_cache& get_row_cache() const {
        return _cache;
    }

    row_cache& get_row_cache() {
        return _cache;
    }

    future<std::vector<locked_cell>> lock_counter_cells(const mutation& m);

    logalloc::occupancy_stats occupancy() const;
private:
    column_family(schema_ptr schema, config cfg, db::commitlog* cl, compaction_manager&);
public:
    column_family(schema_ptr schema, config cfg, db::commitlog& cl, compaction_manager& cm)
        : column_family(schema, std::move(cfg), &cl, cm) {}
    column_family(schema_ptr schema, config cfg, no_commitlog, compaction_manager& cm)
        : column_family(schema, std::move(cfg), nullptr, cm) {}
    column_family(column_family&&) = delete; // 'this' is being captured during construction
    ~column_family();
    const schema_ptr& schema() const { return _schema; }
    void set_schema(schema_ptr);
    db::commitlog* commitlog() { return _commitlog; }
    future<const_mutation_partition_ptr> find_partition(schema_ptr, const dht::decorated_key& key) const;
    future<const_mutation_partition_ptr> find_partition_slow(schema_ptr, const partition_key& key) const;
    future<const_row_ptr> find_row(schema_ptr, const dht::decorated_key& partition_key, clustering_key clustering_key) const;
    // Applies given mutation to this column family
    // The mutation is always upgraded to current schema.
    void apply(const frozen_mutation& m, const schema_ptr& m_schema, const db::replay_position& = db::replay_position());
    void apply(const mutation& m, const db::replay_position& = db::replay_position());
    void apply_streaming_mutation(schema_ptr, utils::UUID plan_id, const frozen_mutation&, bool fragmented);

    // Returns at most "cmd.limit" rows
    future<lw_shared_ptr<query::result>> query(schema_ptr,
        const query::read_command& cmd, query::result_request request,
        const dht::partition_range_vector& ranges,
        tracing::trace_state_ptr trace_state,
        query::result_memory_limiter& memory_limiter,
        uint64_t max_result_size);

    void start();
    future<> stop();
    future<> flush();
    future<> flush(const db::replay_position&);
    future<> flush_streaming_mutations(utils::UUID plan_id, dht::partition_range_vector ranges = dht::partition_range_vector{});
    future<> fail_streaming_mutations(utils::UUID plan_id);
    future<> clear(); // discards memtable(s) without flushing them to disk.
    future<db::replay_position> discard_sstables(db_clock::time_point);

    // Important warning: disabling writes will only have an effect in the current shard.
    // The other shards will keep writing tables at will. Therefore, you very likely need
    // to call this separately in all shards first, to guarantee that none of them are writing
    // new data before you can safely assume that the whole node is disabled.
    future<int64_t> disable_sstable_write() {
        _sstable_writes_disabled_at = std::chrono::steady_clock::now();
        return _sstables_lock.write_lock().then([this] {
            if (_sstables->all()->empty()) {
                return make_ready_future<int64_t>(0);
            }
            int64_t max = 0;
            for (auto&& s : *_sstables->all()) {
                max = std::max(max, s->generation());
            }
            return make_ready_future<int64_t>(max);
        });
    }

    // SSTable writes are now allowed again, and generation is updated to new_generation if != -1
    // returns the amount of microseconds elapsed since we disabled writes.
    std::chrono::steady_clock::duration enable_sstable_write(int64_t new_generation) {
        if (new_generation != -1) {
            update_sstables_known_generation(new_generation);
        }
        _sstables_lock.write_unlock();
        return std::chrono::steady_clock::now() - _sstable_writes_disabled_at;
    }

    // Make sure the generation numbers are sequential, starting from "start".
    // Generations before "start" are left untouched.
    //
    // Return the highest generation number seen so far
    //
    // Word of warning: although this function will reshuffle anything over "start", it is
    // very dangerous to do that with live SSTables. This is meant to be used with SSTables
    // that are not yet managed by the system.
    //
    // Parameter all_generations stores the generation of all SSTables in the system, so it
    // will be easy to determine which SSTable is new.
    // An example usage would query all shards asking what is the highest SSTable number known
    // to them, and then pass that + 1 as "start".
    future<std::vector<sstables::entry_descriptor>> reshuffle_sstables(std::set<int64_t> all_generations, int64_t start);

    // FIXME: this is just an example, should be changed to something more
    // general. compact_all_sstables() starts a compaction of all sstables.
    // It doesn't flush the current memtable first. It's just a ad-hoc method,
    // not a real compaction policy.
    future<> compact_all_sstables();
    // Compact all sstables provided in the vector.
    // If cleanup is set to true, compaction_sstables will run on behalf of a cleanup job,
    // meaning that irrelevant keys will be discarded.
    future<> compact_sstables(sstables::compaction_descriptor descriptor, bool cleanup = false);
    // Performs a cleanup on each sstable of this column family, excluding
    // those ones that are irrelevant to this node or being compacted.
    // Cleanup is about discarding keys that are no longer relevant for a
    // given sstable, e.g. after node loses part of its token range because
    // of a newly added node.
    future<> cleanup_sstables(sstables::compaction_descriptor descriptor);

    future<bool> snapshot_exists(sstring name);

    future<> snapshot(sstring name);
    future<> clear_snapshot(sstring name);
    future<std::unordered_map<sstring, snapshot_details>> get_snapshot_details();

    const bool incremental_backups_enabled() const {
        return _config.enable_incremental_backups;
    }

    void set_incremental_backups(bool val) {
        _config.enable_incremental_backups = val;
    }

    const sstables::sstable_set& get_sstable_set() const;
    lw_shared_ptr<sstable_list> get_sstables() const;
    lw_shared_ptr<sstable_list> get_sstables_including_compacted_undeleted() const;
    const std::vector<sstables::shared_sstable>& compacted_undeleted_sstables() const;
    std::vector<sstables::shared_sstable> select_sstables(const dht::partition_range& range) const;
    size_t sstables_count() const;
    std::vector<uint64_t> sstable_count_per_level() const;
    int64_t get_unleveled_sstables() const;

    void start_compaction();
    void trigger_compaction();
    future<> run_compaction(sstables::compaction_descriptor descriptor);
    void set_compaction_strategy(sstables::compaction_strategy_type strategy);
    const sstables::compaction_strategy& get_compaction_strategy() const {
        return _compaction_strategy;
    }

    sstables::compaction_strategy& get_compaction_strategy() {
        return _compaction_strategy;
    }

    const stats& get_stats() const {
        return _stats;
    }

    ::cf_stats* cf_stats() {
        return _config.cf_stats;
    }

    compaction_manager& get_compaction_manager() const {
        return _compaction_manager;
    }

    template<typename Func, typename Result = futurize_t<std::result_of_t<Func()>>>
    Result run_with_compaction_disabled(Func && func) {
        ++_compaction_disabled;
        return _compaction_manager.remove(this).then(std::forward<Func>(func)).finally([this] {
            if (--_compaction_disabled == 0) {
                // we're turning if on again, use function that does not increment
                // the counter further.
                do_trigger_compaction();
            }
        });
    }

    void add_or_update_view(view_ptr v);
    void remove_view(view_ptr v);
    const std::vector<view_ptr>& views() const;
private:
    void update_view_schemas();

    // One does not need to wait on this future if all we are interested in, is
    // initiating the write.  The writes initiated here will eventually
    // complete, and the seastar::gate below will make sure they are all
    // completed before we stop() this column_family.
    //
    // But it is possible to synchronously wait for the seal to complete by
    // waiting on this future. This is useful in situations where we want to
    // synchronously flush data to disk.
    future<> seal_active_memtable(memtable_list::flush_behavior behavior = memtable_list::flush_behavior::delayed);

    // I am assuming here that the repair process will potentially send ranges containing
    // few mutations, definitely not enough to fill a memtable. It wants to know whether or
    // not each of those ranges individually succeeded or failed, so we need a future for
    // each.
    //
    // One of the ways to fix that, is changing the repair itself to send more mutations at
    // a single batch. But relying on that is a bad idea for two reasons:
    //
    // First, the goals of the SSTable writer and the repair sender are at odds. The SSTable
    // writer wants to write as few SSTables as possible, while the repair sender wants to
    // break down the range in pieces as small as it can and checksum them individually, so
    // it doesn't have to send a lot of mutations for no reason.
    //
    // Second, even if the repair process wants to process larger ranges at once, some ranges
    // themselves may be small. So while most ranges would be large, we would still have
    // potentially some fairly small SSTables lying around.
    //
    // The best course of action in this case is to coalesce the incoming streams write-side.
    // repair can now choose whatever strategy - small or big ranges - it wants, resting assure
    // that the incoming memtables will be coalesced together.
    shared_promise<> _waiting_streaming_flushes;
    timer<> _delayed_streaming_flush{[this] { _streaming_memtables->request_flush(); }};
    future<> seal_active_streaming_memtable_delayed();
    future<> seal_active_streaming_memtable_immediate();
    future<> seal_active_streaming_memtable(memtable_list::flush_behavior behavior) {
        if (behavior == memtable_list::flush_behavior::delayed) {
            return seal_active_streaming_memtable_delayed();
        } else if (behavior == memtable_list::flush_behavior::immediate) {
            return seal_active_streaming_memtable_immediate();
        } else {
            // Impossible
            assert(0);
        }
    }

    // filter manifest.json files out
    static bool manifest_json_filter(const sstring& fname);

    // Iterate over all partitions.  Protocol is the same as std::all_of(),
    // so that iteration can be stopped by returning false.
    // Func signature: bool (const decorated_key& dk, const mutation_partition& mp)
    template <typename Func>
    future<bool> for_all_partitions(schema_ptr, Func&& func) const;
    void check_valid_rp(const db::replay_position&) const;
public:
    void start_rewrite();
    // Iterate over all partitions.  Protocol is the same as std::all_of(),
    // so that iteration can be stopped by returning false.
    future<bool> for_all_partitions_slow(schema_ptr, std::function<bool (const dht::decorated_key&, const mutation_partition&)> func) const;

    friend std::ostream& operator<<(std::ostream& out, const column_family& cf);
    // Testing purposes.
    friend class column_family_test;

    friend class distributed_loader;
};

class user_types_metadata {
    std::unordered_map<bytes, user_type> _user_types;
public:
    user_type get_type(const bytes& name) const {
        return _user_types.at(name);
    }
    const std::unordered_map<bytes, user_type>& get_all_types() const {
        return _user_types;
    }
    void add_type(user_type type) {
        auto i = _user_types.find(type->_name);
        assert(i == _user_types.end() || type->is_compatible_with(*i->second));
        _user_types[type->_name] = std::move(type);
    }
    void remove_type(user_type type) {
        _user_types.erase(type->_name);
    }
    friend std::ostream& operator<<(std::ostream& os, const user_types_metadata& m);
};

class keyspace_metadata final {
    sstring _name;
    sstring _strategy_name;
    std::map<sstring, sstring> _strategy_options;
    std::unordered_map<sstring, schema_ptr> _cf_meta_data;
    bool _durable_writes;
    lw_shared_ptr<user_types_metadata> _user_types;
public:
    keyspace_metadata(sstring name,
                 sstring strategy_name,
                 std::map<sstring, sstring> strategy_options,
                 bool durable_writes,
                 std::vector<schema_ptr> cf_defs = std::vector<schema_ptr>{},
                 lw_shared_ptr<user_types_metadata> user_types = make_lw_shared<user_types_metadata>())
        : _name{std::move(name)}
        , _strategy_name{strategy_name.empty() ? "NetworkTopologyStrategy" : strategy_name}
        , _strategy_options{std::move(strategy_options)}
        , _durable_writes{durable_writes}
        , _user_types{std::move(user_types)}
    {
        for (auto&& s : cf_defs) {
            _cf_meta_data.emplace(s->cf_name(), s);
        }
    }
    static lw_shared_ptr<keyspace_metadata>
    new_keyspace(sstring name,
                 sstring strategy_name,
                 std::map<sstring, sstring> options,
                 bool durables_writes,
                 std::vector<schema_ptr> cf_defs = std::vector<schema_ptr>{})
    {
        return ::make_lw_shared<keyspace_metadata>(name, strategy_name, options, durables_writes, cf_defs);
    }
    void validate() const;
    const sstring& name() const {
        return _name;
    }
    const sstring& strategy_name() const {
        return _strategy_name;
    }
    const std::map<sstring, sstring>& strategy_options() const {
        return _strategy_options;
    }
    const std::unordered_map<sstring, schema_ptr>& cf_meta_data() const {
        return _cf_meta_data;
    }
    bool durable_writes() const {
        return _durable_writes;
    }
    const lw_shared_ptr<user_types_metadata>& user_types() const {
        return _user_types;
    }
    void add_or_update_column_family(const schema_ptr& s) {
        _cf_meta_data[s->cf_name()] = s;
    }
    void remove_column_family(const schema_ptr& s) {
        _cf_meta_data.erase(s->cf_name());
    }
    void add_user_type(const user_type ut) {
        _user_types->add_type(ut);
    }
    void remove_user_type(const user_type ut) {
        _user_types->remove_type(ut);
    }
    std::vector<schema_ptr> tables() const;
    std::vector<view_ptr> views() const;
    friend std::ostream& operator<<(std::ostream& os, const keyspace_metadata& m);
};

class keyspace {
public:
    struct config {
        sstring datadir;
        bool enable_commitlog = true;
        bool enable_disk_reads = true;
        bool enable_disk_writes = true;
        bool enable_cache = true;
        bool enable_incremental_backups = false;
        ::dirty_memory_manager* dirty_memory_manager = &default_dirty_memory_manager;
        ::dirty_memory_manager* streaming_dirty_memory_manager = &default_dirty_memory_manager;
        restricted_mutation_reader_config read_concurrency_config;
        restricted_mutation_reader_config streaming_read_concurrency_config;
        ::cf_stats* cf_stats = nullptr;
    };
private:
    std::unique_ptr<locator::abstract_replication_strategy> _replication_strategy;
    lw_shared_ptr<keyspace_metadata> _metadata;
    config _config;
public:
    explicit keyspace(lw_shared_ptr<keyspace_metadata> metadata, config cfg)
        : _metadata(std::move(metadata))
        , _config(std::move(cfg))
    {}

    void update_from(lw_shared_ptr<keyspace_metadata>);

    /** Note: return by shared pointer value, since the meta data is
     * semi-volatile. I.e. we could do alter keyspace at any time, and
     * boom, it is replaced.
     */
    lw_shared_ptr<keyspace_metadata> metadata() const {
        return _metadata;
    }
    void create_replication_strategy(const std::map<sstring, sstring>& options);
    /**
     * This should not really be return by reference, since replication
     * strategy is also volatile in that it could be replaced at "any" time.
     * However, all current uses at least are "instantateous", i.e. does not
     * carry it across a continuation. So it is sort of same for now, but
     * should eventually be refactored.
     */
    locator::abstract_replication_strategy& get_replication_strategy();
    const locator::abstract_replication_strategy& get_replication_strategy() const;
    column_family::config make_column_family_config(const schema& s, const db::config& db_config) const;
    future<> make_directory_for_column_family(const sstring& name, utils::UUID uuid);
    void add_or_update_column_family(const schema_ptr& s) {
        _metadata->add_or_update_column_family(s);
    }
    void add_user_type(const user_type ut) {
        _metadata->add_user_type(ut);
    }
    void remove_user_type(const user_type ut) {
        _metadata->remove_user_type(ut);
    }

    // FIXME to allow simple registration at boostrap
    void set_replication_strategy(std::unique_ptr<locator::abstract_replication_strategy> replication_strategy);

    const bool incremental_backups_enabled() const {
        return _config.enable_incremental_backups;
    }

    void set_incremental_backups(bool val) {
        _config.enable_incremental_backups = val;
    }

    const sstring& datadir() const {
        return _config.datadir;
    }

    sstring column_family_directory(const sstring& name, utils::UUID uuid) const;
};

class no_such_keyspace : public std::runtime_error {
public:
    no_such_keyspace(const sstring& ks_name);
};

class no_such_column_family : public std::runtime_error {
public:
    no_such_column_family(const utils::UUID& uuid);
    no_such_column_family(const sstring& ks_name, const sstring& cf_name);
};

// Policy for distributed<database>:
//   broadcast metadata writes
//   local metadata reads
//   use shard_of() for data

class database {
public:
    using timeout_clock = lowres_clock;
private:
    ::cf_stats _cf_stats;
    static constexpr size_t max_concurrent_reads() { return 100; }
    static constexpr size_t max_system_concurrent_reads() { return 10; }
    struct db_stats {
        uint64_t total_writes = 0;
        uint64_t total_writes_failed = 0;
        uint64_t total_writes_timedout = 0;
        uint64_t total_reads = 0;
        uint64_t total_reads_failed = 0;
        uint64_t sstable_read_queue_overloaded = 0;

        uint64_t short_data_queries = 0;
        uint64_t short_mutation_queries = 0;
    };

    lw_shared_ptr<db_stats> _stats;

    std::unique_ptr<db::config> _cfg;

    dirty_memory_manager _system_dirty_memory_manager;
    dirty_memory_manager _dirty_memory_manager;
    dirty_memory_manager _streaming_dirty_memory_manager;

    semaphore _read_concurrency_sem{max_concurrent_reads()};
    restricted_mutation_reader_config _read_concurrency_config;
    semaphore _system_read_concurrency_sem{max_system_concurrent_reads()};
    restricted_mutation_reader_config _system_read_concurrency_config;

    std::unordered_map<sstring, keyspace> _keyspaces;
    std::unordered_map<utils::UUID, lw_shared_ptr<column_family>> _column_families;
    std::unordered_map<std::pair<sstring, sstring>, utils::UUID, utils::tuple_hash> _ks_cf_to_uuid;
    std::unique_ptr<db::commitlog> _commitlog;
    utils::UUID _version;
    // compaction_manager object is referenced by all column families of a database.
    compaction_manager _compaction_manager;
    seastar::metrics::metric_groups _metrics;
    bool _enable_incremental_backups = false;

    future<> init_commitlog();
    future<> apply_in_memory(const frozen_mutation& m, schema_ptr m_schema, db::replay_position, timeout_clock::time_point timeout);

private:
    // Unless you are an earlier boostraper or the database itself, you should
    // not be using this directly.  Go for the public create_keyspace instead.
    void add_keyspace(sstring name, keyspace k);
    void create_in_memory_keyspace(const lw_shared_ptr<keyspace_metadata>& ksm);
    friend void db::system_keyspace::make(database& db, bool durable, bool volatile_testing_only);
    void setup_metrics();

    future<> do_apply(schema_ptr, const frozen_mutation&, timeout_clock::time_point timeout);

    query::result_memory_limiter _result_memory_limiter;

    future<frozen_mutation> do_apply_counter_update(column_family& cf, const frozen_mutation& fm, schema_ptr m_schema);
public:
    static utils::UUID empty_version;

    query::result_memory_limiter& get_result_memory_limiter() {
        return _result_memory_limiter;
    }

    void set_enable_incremental_backups(bool val) { _enable_incremental_backups = val; }

    future<> parse_system_tables(distributed<service::storage_proxy>&);
    database();
    database(const db::config&);
    database(database&&) = delete;
    ~database();

    void update_version(const utils::UUID& version);

    const utils::UUID& get_version() const;

    db::commitlog* commitlog() const {
        return _commitlog.get();
    }

    compaction_manager& get_compaction_manager() {
        return _compaction_manager;
    }
    const compaction_manager& get_compaction_manager() const {
        return _compaction_manager;
    }

    void add_column_family(keyspace& ks, schema_ptr schema, column_family::config cfg);
    future<> add_column_family_and_make_directory(schema_ptr schema);

    /* throws std::out_of_range if missing */
    const utils::UUID& find_uuid(const sstring& ks, const sstring& cf) const;
    const utils::UUID& find_uuid(const schema_ptr&) const;

    /**
     * Creates a keyspace for a given metadata if it still doesn't exist.
     *
     * @return ready future when the operation is complete
     */
    future<> create_keyspace(const lw_shared_ptr<keyspace_metadata>&);
    /* below, find_keyspace throws no_such_<type> on fail */
    keyspace& find_keyspace(const sstring& name);
    const keyspace& find_keyspace(const sstring& name) const;
    bool has_keyspace(const sstring& name) const;
    future<> update_keyspace(const sstring& name);
    void drop_keyspace(const sstring& name);
    const auto& keyspaces() const { return _keyspaces; }
    std::vector<sstring> get_non_system_keyspaces() const;
    column_family& find_column_family(const sstring& ks, const sstring& name);
    const column_family& find_column_family(const sstring& ks, const sstring& name) const;
    column_family& find_column_family(const utils::UUID&);
    const column_family& find_column_family(const utils::UUID&) const;
    column_family& find_column_family(const schema_ptr&);
    const column_family& find_column_family(const schema_ptr&) const;
    bool column_family_exists(const utils::UUID& uuid) const;
    schema_ptr find_schema(const sstring& ks_name, const sstring& cf_name) const;
    schema_ptr find_schema(const utils::UUID&) const;
    bool has_schema(const sstring& ks_name, const sstring& cf_name) const;
    std::set<sstring> existing_index_names(const sstring& cf_to_exclude = sstring()) const;
    future<> stop();
    unsigned shard_of(const dht::token& t);
    unsigned shard_of(const mutation& m);
    unsigned shard_of(const frozen_mutation& m);
    future<lw_shared_ptr<query::result>> query(schema_ptr, const query::read_command& cmd, query::result_request request, const dht::partition_range_vector& ranges,
                                               tracing::trace_state_ptr trace_state, uint64_t max_result_size);
    future<reconcilable_result> query_mutations(schema_ptr, const query::read_command& cmd, const dht::partition_range& range,
                                                query::result_memory_accounter&& accounter, tracing::trace_state_ptr trace_state);
    // Apply the mutation atomically.
    // Throws timed_out_error when timeout is reached.
    future<> apply(schema_ptr, const frozen_mutation&, timeout_clock::time_point timeout = timeout_clock::time_point::max());
    future<> apply_streaming_mutation(schema_ptr, utils::UUID plan_id, const frozen_mutation&, bool fragmented);
    future<frozen_mutation> apply_counter_update(schema_ptr, const frozen_mutation& m, timeout_clock::time_point timeout = timeout_clock::time_point::max());
    keyspace::config make_keyspace_config(const keyspace_metadata& ksm);
    const sstring& get_snitch_name() const;
    future<> clear_snapshot(sstring tag, std::vector<sstring> keyspace_names);

    friend std::ostream& operator<<(std::ostream& out, const database& db);
    const std::unordered_map<sstring, keyspace>& get_keyspaces() const {
        return _keyspaces;
    }

    std::unordered_map<sstring, keyspace>& get_keyspaces() {
        return _keyspaces;
    }

    const std::unordered_map<utils::UUID, lw_shared_ptr<column_family>>& get_column_families() const {
        return _column_families;
    }

    std::unordered_map<utils::UUID, lw_shared_ptr<column_family>>& get_column_families() {
        return _column_families;
    }

    std::vector<lw_shared_ptr<column_family>> get_non_system_column_families() const;

    const std::unordered_map<std::pair<sstring, sstring>, utils::UUID, utils::tuple_hash>&
    get_column_families_mapping() const {
        return _ks_cf_to_uuid;
    }

    const db::config& get_config() const {
        return *_cfg;
    }

    future<> flush_all_memtables();

    // See #937. Truncation now requires a callback to get a time stamp
    // that must be guaranteed to be the same for all shards.
    typedef std::function<future<db_clock::time_point>()> timestamp_func;

    /** Truncates the given column family */
    future<> truncate(sstring ksname, sstring cfname, timestamp_func);
    future<> truncate(const keyspace& ks, column_family& cf, timestamp_func);

    bool update_column_family(schema_ptr s);
    future<> drop_column_family(const sstring& ks_name, const sstring& cf_name, timestamp_func);

    const logalloc::region_group& dirty_memory_region_group() const {
        return _dirty_memory_manager.region_group();
    }

    std::unordered_set<sstring> get_initial_tokens();
    std::experimental::optional<gms::inet_address> get_replace_address();
    bool is_replacing();
    semaphore& system_keyspace_read_concurrency_sem() {
        return _system_read_concurrency_sem;
    }

    friend class distributed_loader;
};

// FIXME: stub
class secondary_index_manager {};

future<> update_schema_version_and_announce(distributed<service::storage_proxy>& proxy);

class distributed_loader {
public:
    static future<> open_sstable(distributed<database>& db, sstables::entry_descriptor comps,
        std::function<future<> (column_family&, sstables::foreign_sstable_open_info)> func);
    static future<> load_new_sstables(distributed<database>& db, sstring ks, sstring cf, std::vector<sstables::entry_descriptor> new_tables);
    static future<std::vector<sstables::entry_descriptor>> flush_upload_dir(distributed<database>& db, sstring ks_name, sstring cf_name);
    static future<sstables::entry_descriptor> probe_file(distributed<database>& db, sstring sstdir, sstring fname);
    static future<> populate_column_family(distributed<database>& db, sstring sstdir, sstring ks, sstring cf);
    static future<> populate_keyspace(distributed<database>& db, sstring datadir, sstring ks_name);
    static future<> init_system_keyspace(distributed<database>& db);
    static future<> init_non_system_keyspaces(distributed<database>& db, distributed<service::storage_proxy>& proxy);
};

#endif /* DATABASE_HH_ */
