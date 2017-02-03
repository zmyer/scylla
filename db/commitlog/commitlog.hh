/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Modified by ScyllaDB
 * Copyright (C) 2015 ScyllaDB
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

#pragma once

#include <memory>

#include "utils/data_output.hh"
#include "core/future.hh"
#include "core/shared_ptr.hh"
#include "core/stream.hh"
#include "utils/UUID.hh"
#include "replay_position.hh"
#include "commitlog_entry.hh"

class file;

namespace db {

class config;

using cf_id_type = utils::UUID;

/*
 * Commit Log tracks every write operation into the system. The aim of
 * the commit log is to be able to successfully recover data that was
 * not stored to disk via the Memtable.
 *
 * This impl is cassandra log format compatible (for what it is worth).
 * The behaviour is similar, but not 100% identical as "stock cl".
 *
 * Files are managed with "normal" file writes (as normal as seastar
 * gets) - no mmapping. Data is kept in internal buffers which, when
 * full, are written to disk (see below). Files are also flushed
 * periodically (or always), ensuring all data is written + writes are
 * complete.
 *
 * In BATCH mode, every write to the log will also send the data to disk
 * + issue a flush and wait for both to complete.
 *
 * In PERIODIC mode, most writes will only add to the internal memory
 * buffers. If the mem buffer is saturated, data is sent to disk, but we
 * don't wait for the write to complete. However, if periodic (timer)
 * flushing has not been done in X ms, we will write + flush to file. In
 * which case we wait for it.
 *
 * The commitlog does not guarantee any ordering between "add" callers
 * (due to the above). The actual order in the commitlog is however
 * identified by the replay_position returned.
 *
 * Like the stock cl, the log segments keep track of the highest dirty
 * (added) internal position for a given table id (cf_id_type / UUID).
 * Code should ensure to use discard_completed_segments with UUID +
 * highest rp once a memtable has been flushed. This will allow
 * discarding used segments. Failure to do so will keep stuff
 * indefinately.
 */
class commitlog {
public:
    using timeout_clock = lowres_clock;

    class segment_manager;
    class segment;

private:
    ::shared_ptr<segment_manager> _segment_manager;
public:
    enum class sync_mode {
        PERIODIC, BATCH
    };
    struct config {
        config() = default;
        config(const config&) = default;
        config(const db::config&);

        sstring commit_log_location;
        uint64_t commitlog_total_space_in_mb = 0;
        uint64_t commitlog_segment_size_in_mb = 32;
        uint64_t commitlog_sync_period_in_ms = 10 * 1000; //TODO: verify default!
        // Max number of segments to keep in pre-alloc reserve.
        // Not (yet) configurable from scylla.conf.
        uint64_t max_reserve_segments = 12;
        // Max active writes/flushes. Default value
        // zero means try to figure it out ourselves
        uint64_t max_active_writes = 0;
        uint64_t max_active_flushes = 0;

        sync_mode mode = sync_mode::PERIODIC;
    };

    struct descriptor {
    private:
        descriptor(std::pair<uint64_t, uint32_t> p);
    public:
        static const std::string SEPARATOR;
        static const std::string FILENAME_PREFIX;
        static const std::string FILENAME_EXTENSION;

        descriptor(descriptor&&) = default;
        descriptor(const descriptor&) = default;
        descriptor(segment_id_type i, uint32_t v = 1);
        descriptor(replay_position p);
        descriptor(sstring filename);

        sstring filename() const;
        operator replay_position() const;

        const segment_id_type id;
        const uint32_t ver;
    };

    commitlog(commitlog&&) noexcept;
    ~commitlog();

    /**
     * Commitlog is created via a factory func.
     * This of course because it needs to access disk to get up to speed.
     * Optionally, could have an "init" func and require calling this.
     */
    static future<commitlog> create_commitlog(config);

    /**
     * Note: To be able to keep impl out of header file,
     * actual data writing is done via a std::function.
     * If it is proven that this has unacceptable overhead, this can be replace
     * by iter an interface or move segments and stuff into the header. But
     * I hope not.
     *
     * A serializing func is provided along with a parameter indicating the size
     * of data to be written. (See add).
     * Don't write less, absolutely don't write more...
     */
    using output = data_output;
    using serializer_func = std::function<void(output&)>;

    /**
     * Add a "Mutation" to the commit log.
     *
     * Resolves with timed_out_error when timeout is reached.
     *
     * @param mutation_func a function that writes 'size' bytes to the log, representing the mutation.
     */
    future<replay_position> add(const cf_id_type& id, size_t size, timeout_clock::time_point timeout, serializer_func mutation_func);

    /**
     * Template version of add.
     * Resolves with timed_out_error when timeout is reached.
     * @param mu an invokable op that generates the serialized data. (Of size bytes)
     */
    template<typename _MutationOp>
    future<replay_position> add_mutation(const cf_id_type& id, size_t size, timeout_clock::time_point timeout, _MutationOp&& mu) {
        return add(id, size, timeout, [mu = std::forward<_MutationOp>(mu)](output& out) {
            mu(out);
        });
    }

    /**
     * Template version of add.
     * @param mu an invokable op that generates the serialized data. (Of size bytes)
     */
    template<typename _MutationOp>
    future<replay_position> add_mutation(const cf_id_type& id, size_t size, _MutationOp&& mu) {
        return add_mutation(id, size, timeout_clock::time_point::max(), std::forward<_MutationOp>(mu));
    }

    /**
     * Add an entry to the commit log.
     * Resolves with timed_out_error when timeout is reached.
     * @param entry_writer a writer responsible for writing the entry
     */
    future<replay_position> add_entry(const cf_id_type& id, const commitlog_entry_writer& entry_writer, timeout_clock::time_point timeout);

    /**
     * Modifies the per-CF dirty cursors of any commit log segments for the column family according to the position
     * given. Discards any commit log segments that are no longer used.
     *
     * @param cfId    the column family ID that was flushed
     * @param context the replay position of the flush
     */
    void discard_completed_segments(const cf_id_type&, const replay_position&);

    /**
     * A 'flush_handler' is invoked when the CL determines that size on disk has
     * exceeded allowable threshold. It is called once for every currently active
     * CF id with the highest replay_position which we would prefer to free "until".
     * I.e. a the highest potentially freeable position in the CL.
     *
     * Whatever the callback does to help (or not) this desire is up to him.
     * This is called synchronously, so callee might want to instigate async ops
     * in the background.
     *
     */
    typedef std::function<void(cf_id_type, replay_position)> flush_handler;
    typedef uint64_t flush_handler_id;

    class flush_handler_anchor {
    public:
        friend class commitlog;
        ~flush_handler_anchor();
        flush_handler_anchor(flush_handler_anchor&&);
        flush_handler_anchor(const flush_handler_anchor&) = delete;

        flush_handler_id release(); // disengage anchor - danger danger.
        void unregister();

    private:
        flush_handler_anchor(commitlog&, flush_handler_id);

        commitlog & _cl;
        flush_handler_id _id;
    };

    flush_handler_anchor add_flush_handler(flush_handler);
    void remove_flush_handler(flush_handler_id);

    /**
     * Returns a vector of the segment names
     */
    std::vector<sstring> get_active_segment_names() const;

    /**
     * Returns a vector of segment paths which were
     * preexisting when this instance of commitlog was created.
     *
     * The list will be empty when called for the second time.
     */
    std::vector<sstring> get_segments_to_replay();

    uint64_t get_total_size() const;
    uint64_t get_completed_tasks() const;
    uint64_t get_flush_count() const;
    uint64_t get_pending_tasks() const;
    uint64_t get_pending_flushes() const;
    uint64_t get_pending_allocations() const;
    uint64_t get_flush_limit_exceeded_count() const;
    uint64_t get_num_segments_created() const;
    uint64_t get_num_segments_destroyed() const;
    /**
     * Get number of inactive (finished), segments lingering
     * due to still being dirty
     */
    uint64_t get_num_dirty_segments() const;
    /**
     * Get number of active segments, i.e. still being allocated to
     */
    uint64_t get_num_active_segments() const;

    /**
     * Returns the largest amount of data that can be written in a single "mutation".
     */
    size_t max_record_size() const;

    /**
     * Return max allowed pending writes (per this shard)
     */
    uint64_t max_active_writes() const;
    /**
     * Return max allowed pending flushes (per this shard)
     */
    uint64_t max_active_flushes() const;

    future<> clear();

    const config& active_config() const;

    /**
     * Issues disk sync on all (allocating) segments. I.e. ensures that
     * all data written up until this call is indeed on disk.
     * _However_, if you issue new "add" ops while this is executing,
     * those can/will be missed.
     */
    future<> sync_all_segments();
    /**
     * Shuts everything down and causes any
     * incoming writes to throw exceptions
     */
    future<> shutdown();

    future<std::vector<descriptor>> list_existing_descriptors() const;
    future<std::vector<descriptor>> list_existing_descriptors(const sstring& dir) const;

    future<std::vector<sstring>> list_existing_segments() const;
    future<std::vector<sstring>> list_existing_segments(const sstring& dir) const;

    typedef std::function<future<>(temporary_buffer<char>, replay_position)> commit_load_reader_func;

    class segment_data_corruption_error: public std::runtime_error {
    public:
        segment_data_corruption_error(std::string msg, uint64_t s)
                : std::runtime_error(msg), _bytes(s) {
        }
        uint64_t bytes() const {
            return _bytes;
        }
    private:
        uint64_t _bytes;
    };

    static subscription<temporary_buffer<char>, replay_position> read_log_file(file, commit_load_reader_func, position_type = 0);
    static future<std::unique_ptr<subscription<temporary_buffer<char>, replay_position>>> read_log_file(
            const sstring&, commit_load_reader_func, position_type = 0);
private:
    commitlog(config);

    struct entry_writer {
        virtual size_t size(segment&) = 0;
        // Returns segment-independent size of the entry. Must be <= than segment-dependant size.
        virtual size_t size() = 0;
        virtual void write(segment&, output&) = 0;
    };
};

}
