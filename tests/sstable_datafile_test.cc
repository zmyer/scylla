/*
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

#include "core/sstring.hh"
#include "core/future-util.hh"
#include "core/align.hh"
#include "sstables/sstables.hh"
#include "sstables/key.hh"
#include "sstables/compress.hh"
#include "sstables/compaction.hh"
#include "tests/test-utils.hh"
#include "schema.hh"
#include "schema_builder.hh"
#include "database.hh"
#include "sstables/leveled_manifest.hh"
#include <memory>
#include "sstable_test.hh"
#include "core/seastar.hh"
#include "core/do_with.hh"
#include "sstables/compaction_manager.hh"
#include "tmpdir.hh"
#include "dht/i_partitioner.hh"
#include "range.hh"
#include "partition_slice_builder.hh"
#include "sstables/date_tiered_compaction_strategy.hh"
#include "mutation_assertions.hh"
#include "mutation_reader_assertions.hh"
#include "counters.hh"

#include <stdio.h>
#include <ftw.h>
#include <unistd.h>
#include <boost/range/algorithm/find_if.hpp>

using namespace sstables;

static sstring some_keyspace("ks");
static sstring some_column_family("cf");

atomic_cell make_atomic_cell(bytes_view value, uint32_t ttl = 0, uint32_t expiration = 0) {
    if (ttl) {
        return atomic_cell::make_live(0, value,
            gc_clock::time_point(gc_clock::duration(expiration)), gc_clock::duration(ttl));
    } else {
        return atomic_cell::make_live(0, value);
    }
}

SEASTAR_TEST_CASE(datafile_generation_01) {
    // Data file with clustering key
    //
    // Respective CQL table and CQL insert:
    // CREATE TABLE test (
    //    p1 text,
    //    c1 text,
    //    r1 int,
    //    r2 int,
    //    PRIMARY KEY (p1, c1)
    //  ) WITH compression = {};
    // INSERT INTO test (p1, c1, r1) VALUES ('key1', 'abc', 1);

    return test_setup::do_with_test_directory([] {
        schema_builder builder(make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}, {"r2", int32_type}}, {}, utf8_type)));
        builder.set_compressor_params(compression_parameters({ }));
        auto s = builder.build(schema_builder::compact_storage::no);

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 1, la, big);

        auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 1, big, sstable::component_type::Data);
        return sst->write_components(*mt).then([mt, sst, s, fname] {
            return open_file_dma(fname, open_flags::ro).then([] (file f) {
                auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                auto fut = f.dma_read(0, bufptr.get(), 4096);
                return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr)] (size_t size) mutable {
                    auto buf = bufptr.get();
                    size_t offset = 0;
                    std::vector<uint8_t> key = { 0, 4, 'k', 'e', 'y', '1' };
                    BOOST_REQUIRE(::memcmp(key.data(), &buf[offset], key.size()) == 0);
                    offset += key.size();
                    std::vector<uint8_t> deletion_time = { 0x7f, 0xff, 0xff, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0 };
                    BOOST_REQUIRE(::memcmp(deletion_time.data(), &buf[offset], deletion_time.size()) == 0);
                    offset += deletion_time.size();
                    std::vector<uint8_t> row_mark = { /* name */ 0, 9, 0, 3, 'a', 'b', 'c', 0, 0, 0, 0 };
                    // check if there is a row mark.
                    if (::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0) {
                        BOOST_REQUIRE(::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0);
                        offset += row_mark.size();
                        offset += 13; // skip mask, timestamp and value = 13 bytes.
                    }
                    std::vector<uint8_t> regular_row = { /* name */ 0, 0xb, 0, 3, 'a', 'b', 'c', 0, 0, 2, 'r', '1', 0,
                        /* mask */ 0, /* timestamp */ 0, 0, 0, 0, 0, 0, 0, 0, /* value */ 0, 0, 0, 4, 0, 0, 0, 1 };
                    BOOST_REQUIRE(::memcmp(regular_row.data(), &buf[offset], regular_row.size()) == 0);
                    offset += regular_row.size();
                    std::vector<uint8_t> end_of_row = { 0, 0 };
                    BOOST_REQUIRE(::memcmp(end_of_row.data(), &buf[offset], end_of_row.size()) == 0);
                    offset += end_of_row.size();
                    BOOST_REQUIRE(size == offset);
                    return f.close().finally([f] {});
                });
            });
        });
    });
}
SEASTAR_TEST_CASE(datafile_generation_02) {
    return test_setup::do_with_test_directory([] {
        // Data file with compound partition key and clustering key
        //
        // Respective CQL table and CQL insert:
        // CREATE TABLE table (
        //    p1 text,
        //    p2 text,
        //    c1 text,
        //    r1 int,
        //    PRIMARY KEY ((p1, p2), c1)
        // ) WITH compression = {};
        // INSERT INTO table (p1, p2, c1, r1) VALUES ('key1', 'key2', 'abc', 1);

        schema_builder builder(make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}, {"p2", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}}, {}, utf8_type)));
        builder.set_compressor_params(compression_parameters({ }));
        auto s = builder.build(schema_builder::compact_storage::no);

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        auto key = partition_key::from_exploded(*s, {to_bytes("key1"), to_bytes("key2")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 2, la, big);

        auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 2, big, sstable::component_type::Data);
        return sst->write_components(*mt).then([mt, sst, s, fname] {
            return open_file_dma(fname, open_flags::ro).then([] (file f) {
                auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                auto fut = f.dma_read(0, bufptr.get(), 4096);
                return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr)] (size_t size) mutable {
                    auto buf = bufptr.get();
                    size_t offset = 0;
                    // compound partition key
                    std::vector<uint8_t> compound_key = { /* first key */ 0, 0xe, 0, 4, 'k', 'e', 'y', '1', 0,
                        0, 4, 'k', 'e', 'y', '2', 0};
                    BOOST_REQUIRE(::memcmp(compound_key.data(), &buf[offset], compound_key.size()) == 0);
                    offset += compound_key.size();
                    std::vector<uint8_t> deletion_time = { 0x7f, 0xff, 0xff, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0 };
                    BOOST_REQUIRE(::memcmp(deletion_time.data(), &buf[offset], deletion_time.size()) == 0);
                    offset += deletion_time.size();
                    std::vector<uint8_t> row_mark = { /* name */ 0, 9, 0, 3, 'a', 'b', 'c', 0, 0, 0, 0 };
                    // check if there is a row mark.
                    if (::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0) {
                        BOOST_REQUIRE(::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0);
                        offset += row_mark.size();
                        offset += 13; // skip mask, timestamp and value = 13 bytes.
                    }
                    std::vector<uint8_t> regular_row = { /* name */ 0, 0xb, 0, 3, 'a', 'b', 'c', 0, 0, 2, 'r', '1', 0,
                        /* mask */ 0, /* timestamp */ 0, 0, 0, 0, 0, 0, 0, 0, /* value */ 0, 0, 0, 4, 0, 0, 0, 1 };
                    BOOST_REQUIRE(::memcmp(regular_row.data(), &buf[offset], regular_row.size()) == 0);
                    offset += regular_row.size();
                    std::vector<uint8_t> end_of_row = { 0, 0 };
                    BOOST_REQUIRE(::memcmp(end_of_row.data(), &buf[offset], end_of_row.size()) == 0);
                    offset += end_of_row.size();
                    BOOST_REQUIRE(size == offset);
                    return f.close().finally([f] {});
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(datafile_generation_03) {
    // Data file with compound clustering key
    //
    // Respective CQL table and CQL insert:
    // CREATE TABLE table (
    //    p1 text,
    //    c1 text,
    //    c2 text,
    //    r1 int,
    //    PRIMARY KEY (p1, c1, c2)
    // ) WITH compression = {};
    // INSERT INTO table (p1, c1, c2, r1) VALUES ('key1', 'abc', 'cde', 1);
    return test_setup::do_with_test_directory([] {
        schema_builder builder(make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}, {"c2", utf8_type}}, {{"r1", int32_type}}, {}, utf8_type)));
        builder.set_compressor_params(compression_parameters({ }));
        auto s = builder.build(schema_builder::compact_storage::no);

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc"), to_bytes("cde")});

        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 3, la, big);

        auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 3, big, sstable::component_type::Data);
        return sst->write_components(*mt).then([mt, sst, s, fname] {
            return open_file_dma(fname, open_flags::ro).then([] (file f) {
                auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                auto fut = f.dma_read(0, bufptr.get(), 4096);
                return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr)] (size_t size) mutable {
                    auto buf = bufptr.get();
                    size_t offset = 0;
                    std::vector<uint8_t> key = { 0, 4, 'k', 'e', 'y', '1' };
                    BOOST_REQUIRE(::memcmp(key.data(), &buf[offset], key.size()) == 0);
                    offset += key.size();
                    std::vector<uint8_t> deletion_time = { 0x7f, 0xff, 0xff, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0 };
                    BOOST_REQUIRE(::memcmp(deletion_time.data(), &buf[offset], deletion_time.size()) == 0);
                    offset += deletion_time.size();
                    std::vector<uint8_t> row_mark = { /* NOTE: with compound clustering key */
                        /* name */ 0, 0xf, 0, 3, 'a', 'b', 'c', 0, 0, 3, 'c', 'd', 'e', 0, 0, 0, 0 };
                    // check if there is a row mark.
                    if (::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0) {
                        BOOST_REQUIRE(::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0);
                        offset += row_mark.size();
                        offset += 13; // skip mask, timestamp and value = 13 bytes.
                    }
                    std::vector<uint8_t> regular_row = { /* NOTE: with compound clustering key */
                        /* name */ 0, 0x11, 0, 3, 'a', 'b', 'c', 0, 0, 3, 'c', 'd', 'e', 0, 0, 2, 'r', '1', 0,
                        /* mask */ 0, /* timestamp */ 0, 0, 0, 0, 0, 0, 0, 0, /* value */ 0, 0, 0, 4, 0, 0, 0, 1 };
                    BOOST_REQUIRE(::memcmp(regular_row.data(), &buf[offset], regular_row.size()) == 0);
                    offset += regular_row.size();
                    std::vector<uint8_t> end_of_row = { 0, 0 };
                    BOOST_REQUIRE(::memcmp(end_of_row.data(), &buf[offset], end_of_row.size()) == 0);
                    offset += end_of_row.size();
                    BOOST_REQUIRE(size == offset);
                    return f.close().finally([f]{});
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(datafile_generation_04) {
    // Data file with clustering key and static row
    //
    // Respective CQL table and CQL insert:
    // CREATE TABLE test (
    //    p1 text,
    //    c1 text,
    //    s1 int static,
    //    r1 int,
    //    PRIMARY KEY (p1, c1)
    //  ) WITH compression = {};
    // INSERT INTO test (p1, s1) VALUES ('key1', 10);
    // INSERT INTO test (p1, c1, r1) VALUES ('key1', 'abc', 1);
    return test_setup::do_with_test_directory([] {
        schema_builder builder(make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}}, {{"s1", int32_type}}, utf8_type)));
        builder.set_compressor_params(compression_parameters({ }));
        auto s = builder.build(schema_builder::compact_storage::no);

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");
        const column_definition& s1_col = *s->get_column_definition("s1");

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

        mutation m(key, s);
        m.set_static_cell(s1_col, make_atomic_cell(int32_type->decompose(10)));
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 4, la, big);

        auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 4, big, sstable::component_type::Data);
        return sst->write_components(*mt).then([mt, sst, s, fname] {
            return open_file_dma(fname, open_flags::ro).then([] (file f) {
                auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                auto fut = f.dma_read(0, bufptr.get(), 4096);
                return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr)] (size_t size) mutable {
                    auto buf = bufptr.get();
                    size_t offset = 0;
                    std::vector<uint8_t> key = { 0, 4, 'k', 'e', 'y', '1' };
                    BOOST_REQUIRE(::memcmp(key.data(), &buf[offset], key.size()) == 0);
                    offset += key.size();
                    std::vector<uint8_t> deletion_time = { 0x7f, 0xff, 0xff, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0 };
                    BOOST_REQUIRE(::memcmp(deletion_time.data(), &buf[offset], deletion_time.size()) == 0);
                    offset += deletion_time.size();
                    // static row representation
                    std::vector<uint8_t> static_row = { /* name */ 0, 0xa, 0xff, 0xff, 0, 0, 0, 0, 2, 's', '1', 0,
                        /* mask */ 0, /* timestamp */ 0, 0, 0, 0, 0, 0, 0, 0, /* value */ 0, 0, 0, 4, 0, 0, 0, 0xa };
                    BOOST_REQUIRE(::memcmp(static_row.data(), &buf[offset], static_row.size()) == 0);
                    offset += static_row.size();
                    std::vector<uint8_t> row_mark = { /* name */ 0, 9, 0, 3, 'a', 'b', 'c', 0, 0, 0, 0 };
                    // check if there is a row mark.
                    if (::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0) {
                        BOOST_REQUIRE(::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0);
                        offset += row_mark.size();
                        offset += 13; // skip mask, timestamp and value = 13 bytes.
                    }
                    std::vector<uint8_t> regular_row = { /* name */ 0, 0xb, 0, 3, 'a', 'b', 'c', 0, 0, 2, 'r', '1', 0,
                        /* mask */ 0, /* timestamp */ 0, 0, 0, 0, 0, 0, 0, 0, /* value */ 0, 0, 0, 4, 0, 0, 0, 1 };
                    BOOST_REQUIRE(::memcmp(regular_row.data(), &buf[offset], regular_row.size()) == 0);
                    offset += regular_row.size();
                    std::vector<uint8_t> end_of_row = { 0, 0 };
                    BOOST_REQUIRE(::memcmp(end_of_row.data(), &buf[offset], end_of_row.size()) == 0);
                    offset += end_of_row.size();
                    BOOST_REQUIRE(size == offset);
                    return f.close().finally([f]{});
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(datafile_generation_05) {
    // Data file with clustering key and expiring cells.
    //
    // Respective CQL table and CQL insert:
    // CREATE TABLE test (
    //    p1 text,
    //    c1 text,
    //    r1 int,
    //    PRIMARY KEY (p1, c1)
    //  ) WITH compression = {};
    // INSERT INTO test (p1, c1, r1) VALUES ('key1', 'abc', 1) USING TTL 3600;
    return test_setup::do_with_test_directory([] {
        schema_builder builder(make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}}, {}, utf8_type)));
        builder.set_compressor_params(compression_parameters({ }));
        auto s = builder.build(schema_builder::compact_storage::no);

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1), 3600, 3600));
        mt->apply(std::move(m));

        auto now = to_gc_clock(db_clock::from_time_t(0));
        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 5, la, big, now);

        return sst->write_components(*mt).then([mt, sst, s] {
            auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 5, big, sstable::component_type::Data);
            return open_file_dma(fname, open_flags::ro).then([] (file f) {
                auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                auto fut = f.dma_read(0, bufptr.get(), 4096);
                return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr)] (size_t size) mutable {
                    auto buf = bufptr.get();
                    size_t offset = 0;
                    std::vector<uint8_t> key = { 0, 4, 'k', 'e', 'y', '1' };
                    BOOST_REQUIRE(::memcmp(key.data(), &buf[offset], key.size()) == 0);
                    offset += key.size();
                    std::vector<uint8_t> deletion_time = { 0x7f, 0xff, 0xff, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0 };
                    BOOST_REQUIRE(::memcmp(deletion_time.data(), &buf[offset], deletion_time.size()) == 0);
                    offset += deletion_time.size();
                    std::vector<uint8_t> row_mark = { /* name */ 0, 9, 0, 3, 'a', 'b', 'c', 0, 0, 0, 0 };
                    // check if there is a row mark.
                    if (::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0) {
                        BOOST_REQUIRE(::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0);
                        offset += row_mark.size();
                        offset += 21; // skip mask, ttl, expiration, timestamp and value = 21 bytes.
                    }
                    std::vector<uint8_t> expiring_row = { /* name */ 0, 0xb, 0, 3, 'a', 'b', 'c', 0, 0, 2, 'r', '1', 0,
                        /* mask */ 2, /* ttl = 3600 */ 0, 0, 0xe, 0x10, /* expiration = ttl + 0 */ 0, 0, 0xe, 0x10,
                        /* timestamp */ 0, 0, 0, 0, 0, 0, 0, 0, /* value */ 0, 0, 0, 4, 0, 0, 0, 1 };
                    BOOST_REQUIRE(::memcmp(expiring_row.data(), &buf[offset], expiring_row.size()) == 0);
                    offset += expiring_row.size();
                    std::vector<uint8_t> end_of_row = { 0, 0 };
                    BOOST_REQUIRE(::memcmp(end_of_row.data(), &buf[offset], end_of_row.size()) == 0);
                    offset += end_of_row.size();
                    BOOST_REQUIRE(size == offset);
                    return f.close().finally([f]{});
                });
            });
        });
    });
}

atomic_cell make_dead_atomic_cell(uint32_t deletion_time) {
    return atomic_cell::make_dead(0, gc_clock::time_point(gc_clock::duration(deletion_time)));
}

SEASTAR_TEST_CASE(datafile_generation_06) {
    // Data file with clustering key and tombstone cells.
    //
    // Respective CQL table and CQL insert:
    // CREATE TABLE test (
    //    p1 text,
    //    c1 text,
    //    r1 int,
    //    PRIMARY KEY (p1, c1)
    //  ) WITH compression = {};
    // INSERT INTO test (p1, c1, r1) VALUES ('key1', 'abc', 1);
    // after flushed:
    // DELETE r1 FROM test WHERE p1 = 'key1' AND c1 = 'abc';
    return test_setup::do_with_test_directory([] {
        schema_builder builder(make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}}, {}, utf8_type)));
        builder.set_compressor_params(compression_parameters({ }));
        auto s = builder.build(schema_builder::compact_storage::no);

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_dead_atomic_cell(3600));
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 6, la, big);

        return sst->write_components(*mt).then([mt, sst, s] {
            auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 6, big, sstable::component_type::Data);
            return open_file_dma(fname, open_flags::ro).then([] (file f) {
                auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                auto fut = f.dma_read(0, bufptr.get(), 4096);
                return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr)] (size_t size) mutable {
                    auto buf = bufptr.get();
                    size_t offset = 0;
                    std::vector<uint8_t> key = { 0, 4, 'k', 'e', 'y', '1' };
                    BOOST_REQUIRE(::memcmp(key.data(), &buf[offset], key.size()) == 0);
                    offset += key.size();
                    std::vector<uint8_t> deletion_time = { 0x7f, 0xff, 0xff, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0 };
                    BOOST_REQUIRE(::memcmp(deletion_time.data(), &buf[offset], deletion_time.size()) == 0);
                    offset += deletion_time.size();
                    std::vector<uint8_t> row_mark = { /* name */ 0, 9, 0, 3, 'a', 'b', 'c', 0, 0, 0, 0 };
                    // check if there is a row mark.
                    if (::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0) {
                        BOOST_REQUIRE(::memcmp(row_mark.data(), &buf[offset], row_mark.size()) == 0);
                        offset += row_mark.size();
                        offset += 13; // skip mask, timestamp and expiration (value) = 13 bytes.
                    }
                    // tombstone cell
                    std::vector<uint8_t> row = { /* name */ 0, 0xb, 0, 3, 'a', 'b', 'c', 0, 0, 2, 'r', '1', 0,
                        /* mask */ 1, /* timestamp */ 0, 0, 0, 0, 0, 0, 0, 0,
                        /* expiration (value) */ 0, 0, 0, 4, 0, 0, 0xe, 0x10 };
                    BOOST_REQUIRE(::memcmp(row.data(), &buf[offset], row.size()) == 0);
                    offset += row.size();
                    std::vector<uint8_t> end_of_row = { 0, 0 };
                    BOOST_REQUIRE(::memcmp(end_of_row.data(), &buf[offset], end_of_row.size()) == 0);
                    offset += end_of_row.size();
                    BOOST_REQUIRE(size == offset);
                    return f.close().finally([f]{});
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(datafile_generation_07) {
    // Data file with clustering key and two sstable rows.
    // Only index file is validated in this test case.
    //
    // Respective CQL table and CQL insert:
    // CREATE TABLE test (
    //    p1 text,
    //    c1 text,
    //    r1 int,
    //    PRIMARY KEY (p1, c1)
    //  ) WITH compression = {};
    // INSERT INTO test (p1, c1, r1) VALUES ('key1', 'abc', 1);
    // INSERT INTO test (p1, c1, r1) VALUES ('key2', 'cde', 1);
    return test_setup::do_with_test_directory([] {
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}}, {}, utf8_type));

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m));

        auto key2 = partition_key::from_exploded(*s, {to_bytes("key2")});
        auto c_key2 = clustering_key::from_exploded(*s, {to_bytes("cde")});

        mutation m2(key2, s);
        m2.set_clustered_cell(c_key2, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m2));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 7, la, big);

        return sst->write_components(*mt).then([mt, sst, s] {
            auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 7, big, sstable::component_type::Index);
            return open_file_dma(fname, open_flags::ro).then([] (file f) {
                auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                auto fut = f.dma_read(0, bufptr.get(), 4096);
                return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr)] (size_t size) mutable {
                    auto buf = bufptr.get();
                    size_t offset = 0;
                    std::vector<uint8_t> key1 = { 0, 4, 'k', 'e', 'y', '1',
                        /* pos */ 0, 0, 0, 0, 0, 0, 0, 0, /* promoted index */ 0, 0, 0, 0};
                    BOOST_REQUIRE(::memcmp(key1.data(), &buf[offset], key1.size()) == 0);
                    offset += key1.size();
                    std::vector<uint8_t> key2 = { 0, 4, 'k', 'e', 'y', '2',
                        /* pos */ 0, 0, 0, 0, 0, 0, 0, 0x32, /* promoted index */ 0, 0, 0, 0};
                    BOOST_REQUIRE(::memcmp(key2.data(), &buf[offset], key2.size()) == 0);
                    offset += key2.size();
                    BOOST_REQUIRE(size == offset);
                    return f.close().finally([f]{});
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(datafile_generation_08) {
    // Data file with multiple rows.
    // Only summary file is validated in this test case.
    //
    // Respective CQL table and CQL insert:
    // CREATE TABLE test (
    //    p1 int,
    //    c1 text,
    //    r1 int,
    //    PRIMARY KEY (p1, c1)
    //  ) WITH compression = {};
    return test_setup::do_with_test_directory([] {
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", int32_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}}, {}, utf8_type));

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        // Create 150 partitions so that summary file store 2 entries, assuming min index
        // interval is 128.
        for (int32_t i = 0; i < 150; i++) {
            auto key = partition_key::from_exploded(*s, {int32_type->decompose(i)});
            auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

            mutation m(key, s);
            m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
            mt->apply(std::move(m));
        }

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 8, la, big);

        return sst->write_components(*mt).then([mt, sst, s] {
            auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 8, big, sstable::component_type::Summary);
            return open_file_dma(fname, open_flags::ro).then([] (file f) {
                auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                auto fut = f.dma_read(0, bufptr.get(), 4096);
                return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr)] (size_t size) mutable {
                    auto buf = bufptr.get();
                    size_t offset = 0;

                    std::vector<uint8_t> header = { /* min_index_interval */ 0, 0, 0, 0x80, /* size */ 0, 0, 0, 2,
                        /* memory_size */ 0, 0, 0, 0, 0, 0, 0, 0x20, /* sampling_level */ 0, 0, 0, 0x80,
                        /* size_at_full_sampling */  0, 0, 0, 2 };
                    BOOST_REQUIRE(::memcmp(header.data(), &buf[offset], header.size()) == 0);
                    offset += header.size();

                    std::vector<uint8_t> positions = { 0x8, 0, 0, 0, 0x14, 0, 0, 0 };
                    BOOST_REQUIRE(::memcmp(positions.data(), &buf[offset], positions.size()) == 0);
                    offset += positions.size();

                    std::vector<uint8_t> first_entry = { /* key */ 0, 0, 0, 0x17, /* position */ 0, 0, 0, 0, 0, 0, 0, 0 };
                    BOOST_REQUIRE(::memcmp(first_entry.data(), &buf[offset], first_entry.size()) == 0);
                    offset += first_entry.size();

                    std::vector<uint8_t> second_entry = { /* key */ 0, 0, 0, 0x65, /* position */ 0, 0x9, 0, 0, 0, 0, 0, 0 };
                    BOOST_REQUIRE(::memcmp(second_entry.data(), &buf[offset], second_entry.size()) == 0);
                    offset += second_entry.size();

                    std::vector<uint8_t> first_key = { 0, 0, 0, 0x4, 0, 0, 0, 0x17 };
                    BOOST_REQUIRE(::memcmp(first_key.data(), &buf[offset], first_key.size()) == 0);
                    offset += first_key.size();

                    std::vector<uint8_t> last_key = { 0, 0, 0, 0x4, 0, 0, 0, 0x67 };
                    BOOST_REQUIRE(::memcmp(last_key.data(), &buf[offset], last_key.size()) == 0);
                    offset += last_key.size();

                    BOOST_REQUIRE(size == offset);
                    return f.close().finally([f]{});
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(datafile_generation_09) {
    // Test that generated sstable components can be successfully loaded.
    return test_setup::do_with_test_directory([] {
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}}, {}, utf8_type));

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 9, la, big);

        return sst->write_components(*mt).then([mt, sst, s] {
            auto sst2 = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 9, la, big);

            return sstables::test(sst2).read_summary().then([sst, sst2] {
                summary& sst1_s = sstables::test(sst).get_summary();
                summary& sst2_s = sstables::test(sst2).get_summary();

                BOOST_REQUIRE(::memcmp(&sst1_s.header, &sst2_s.header, sizeof(summary::header)) == 0);
                BOOST_REQUIRE(sst1_s.positions == sst2_s.positions);
                BOOST_REQUIRE(sst1_s.entries == sst2_s.entries);
                BOOST_REQUIRE(sst1_s.first_key.value == sst2_s.first_key.value);
                BOOST_REQUIRE(sst1_s.last_key.value == sst2_s.last_key.value);
            }).then([sst, sst2] {
                return sstables::test(sst2).read_toc().then([sst, sst2] {
                    auto& sst1_c = sstables::test(sst).get_components();
                    auto& sst2_c = sstables::test(sst2).get_components();

                    BOOST_REQUIRE(sst1_c == sst2_c);
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(datafile_generation_10) {
    // Check that the component CRC was properly generated by re-computing the
    // checksum of data file and comparing it to the one stored.
    // Check that the component Digest was properly generated by using the
    // approach described above.
    return test_setup::do_with_test_directory([] {
        schema_builder builder(make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}}, {}, utf8_type)));
        builder.set_compressor_params(compression_parameters({ }));
        auto s = builder.build(schema_builder::compact_storage::no);

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 10, la, big);

        return sst->write_components(*mt).then([mt, sst, s] {
            auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 10, big, sstable::component_type::Data);
            return open_file_dma(fname, open_flags::ro).then([] (file f) {
                auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                auto fut = f.dma_read(0, bufptr.get(), 4096);
                return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr)] (size_t size) mutable {
                    assert(size > 0 && size < 4096);
                    const char* buf = bufptr.get();
                    uint32_t adler = checksum_adler32(buf, size);
                    f.close().finally([f]{});

                    auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 10, big, sstable::component_type::CRC);
                    return open_file_dma(fname, open_flags::ro).then([adler] (file f) {
                        auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                        auto fut = f.dma_read(0, bufptr.get(), 4096);
                        return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr), adler] (size_t size) mutable {
                            size_t offset = 0;
                            auto buf = bufptr.get();

                            std::vector<uint8_t> chunk_size = { 0, 1, 0, 0 };
                            BOOST_REQUIRE(::memcmp(chunk_size.data(), &buf[offset], chunk_size.size()) == 0);
                            offset += chunk_size.size();

                            auto *nr = reinterpret_cast<const net::packed<uint32_t> *>(&buf[offset]);
                            uint32_t stored_adler = net::ntoh(*nr);
                            offset += sizeof(uint32_t);
                            BOOST_REQUIRE(adler == stored_adler);

                            BOOST_REQUIRE(size == offset);
                            return f.close().finally([f]{});
                        });
                    }).then([adler] {
                        auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 10, big, sstable::component_type::Digest);
                        return open_file_dma(fname, open_flags::ro).then([adler] (file f) {
                            auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                            auto fut = f.dma_read(0, bufptr.get(), 4096);
                            return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr), adler] (size_t size) mutable {
                                auto buf = bufptr.get();

                                bytes stored_digest(reinterpret_cast<const signed char*>(buf), size);
                                bytes expected_digest = to_sstring<bytes>(adler);

                                BOOST_REQUIRE(size == expected_digest.size());
                                BOOST_REQUIRE(stored_digest == to_sstring<bytes>(adler));
                                return f.close().finally([f]{});
                            });
                        });
                    });
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(datafile_generation_11) {
    return test_setup::do_with_test_directory([] {
        auto s = complex_schema();

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& set_col = *s->get_column_definition("reg_set");
        const column_definition& static_set_col = *s->get_column_definition("static_collection");

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("c1"), to_bytes("c2")});

        mutation m(key, s);

        tombstone tomb(api::new_timestamp(), gc_clock::now());
        set_type_impl::mutation set_mut{{ tomb }, {
            { to_bytes("1"), make_atomic_cell({}) },
            { to_bytes("2"), make_atomic_cell({}) },
            { to_bytes("3"), make_atomic_cell({}) }
        }};

        auto set_type = static_pointer_cast<const set_type_impl>(set_col.type);
        m.set_clustered_cell(c_key, set_col, set_type->serialize_mutation_form(set_mut));

        auto static_set_type = static_pointer_cast<const set_type_impl>(static_set_col.type);
        m.set_static_cell(static_set_col, static_set_type->serialize_mutation_form(set_mut));

        auto key2 = partition_key::from_exploded(*s, {to_bytes("key2")});
        mutation m2(key2, s);
        set_type_impl::mutation set_mut_single{{}, {{ to_bytes("4"), make_atomic_cell({}) }}};

        m2.set_clustered_cell(c_key, set_col, set_type->serialize_mutation_form(set_mut_single));

        mt->apply(std::move(m));
        mt->apply(std::move(m2));

        auto verifier = [s, set_col, c_key] (auto& mutation) {

            auto& mp = mutation->partition();
            BOOST_REQUIRE(mp.clustered_rows().calculate_size() == 1);
            auto r = mp.find_row(*s, c_key);
            BOOST_REQUIRE(r);
            BOOST_REQUIRE(r->size() == 1);
            auto cell = r->find_cell(set_col.id);
            BOOST_REQUIRE(cell);
            auto t = static_pointer_cast<const collection_type_impl>(set_col.type);
            return t->deserialize_mutation_form(cell->as_collection_mutation());
        };

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 11, la, big);
        return sst->write_components(*mt).then([s, sst, mt, verifier, tomb, &static_set_col] {
            return reusable_sst(s, "tests/sstables/tests-temporary", 11).then([s, verifier, tomb, &static_set_col] (auto sstp) mutable {
                return do_with(sstables::key("key1"), [sstp, s, verifier, tomb, &static_set_col] (auto& key) {
                    return sstp->read_row(s, key).then([] (auto sm) {
                        return mutation_from_streamed_mutation(std::move(sm));
                    }).then([sstp, s, verifier, tomb, &static_set_col] (auto mutation) {
                        auto verify_set = [&tomb] (auto m) {
                            BOOST_REQUIRE(bool(m.tomb) == true);
                            BOOST_REQUIRE(m.tomb == tomb);
                            BOOST_REQUIRE(m.cells.size() == 3);
                            BOOST_REQUIRE(m.cells[0].first == to_bytes("1"));
                            BOOST_REQUIRE(m.cells[1].first == to_bytes("2"));
                            BOOST_REQUIRE(m.cells[2].first == to_bytes("3"));
                        };


                        auto& mp = mutation->partition();
                        auto& ssr = mp.static_row();
                        auto scol = ssr.find_cell(static_set_col.id);
                        BOOST_REQUIRE(scol);

                        // The static set
                        auto t = static_pointer_cast<const collection_type_impl>(static_set_col.type);
                        auto mut = t->deserialize_mutation_form(scol->as_collection_mutation());
                        verify_set(mut);

                        // The clustered set
                        auto m = verifier(mutation);
                        verify_set(m);
                    });
                }).then([sstp, s, verifier] {
                    return do_with(sstables::key("key2"), [sstp, s, verifier] (auto& key) {
                        return sstp->read_row(s, key).then([] (auto sm) {
                            return mutation_from_streamed_mutation(std::move(sm));
                        }).then([sstp, s, verifier] (auto mutation) {
                            auto m = verifier(mutation);
                            BOOST_REQUIRE(!m.tomb);
                            BOOST_REQUIRE(m.cells.size() == 1);
                            BOOST_REQUIRE(m.cells[0].first == to_bytes("4"));
                        });
                    });
                });
            });
        }).then([sst, mt] {});
    });
}

SEASTAR_TEST_CASE(datafile_generation_12) {
    return test_setup::do_with_test_directory([] {
        auto s = complex_schema();

        auto mt = make_lw_shared<memtable>(s);

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto cp = exploded_clustering_prefix({to_bytes("c1") });

        mutation m(key, s);

        tombstone tomb(api::new_timestamp(), gc_clock::now());
        m.partition().apply_delete(*s, cp, tomb);
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 12, la, big);
        return sst->write_components(*mt).then([s, tomb] {
            return reusable_sst(s, "tests/sstables/tests-temporary", 12).then([s, tomb] (auto sstp) mutable {
                return do_with(sstables::key("key1"), [sstp, s, tomb] (auto& key) {
                    return sstp->read_row(s, key).then([] (auto sm) {
                        return mutation_from_streamed_mutation(std::move(sm));
                    }).then([sstp, s, tomb] (auto mutation) {
                        auto& mp = mutation->partition();
                        BOOST_REQUIRE(mp.row_tombstones().size() == 1);
                        for (auto& rt: mp.row_tombstones()) {
                            BOOST_REQUIRE(rt.tomb == tomb);
                        }
                    });
                });
            });
        }).then([sst, mt] {});
    });
}

static future<> sstable_compression_test(compressor c, unsigned generation) {
    return test_setup::do_with_test_directory([c, generation] {
        // NOTE: set a given compressor algorithm to schema.
        schema_builder builder(complex_schema());
        builder.set_compressor_params(c);
        auto s = builder.build(schema_builder::compact_storage::no);

        auto mtp = make_lw_shared<memtable>(s);

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto cp = exploded_clustering_prefix({to_bytes("c1") });

        mutation m(key, s);

        tombstone tomb(api::new_timestamp(), gc_clock::now());
        m.partition().apply_delete(*s, cp, tomb);
        mtp->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", generation, la, big);
        return sst->write_components(*mtp).then([s, tomb, generation] {
            return reusable_sst(s, "tests/sstables/tests-temporary", generation).then([s, tomb] (auto sstp) mutable {
                return do_with(sstables::key("key1"), [sstp, s, tomb] (auto& key) {
                    return sstp->read_row(s, key).then([] (auto sm) {
                            return mutation_from_streamed_mutation(std::move(sm));
                        }).then([sstp, s, tomb] (auto mutation) {
                        auto& mp = mutation->partition();
                        BOOST_REQUIRE(mp.row_tombstones().size() == 1);
                        for (auto& rt: mp.row_tombstones()) {
                            BOOST_REQUIRE(rt.tomb == tomb);
                        }
                    });
                });
            });
        }).then([sst, mtp] {});
    });
}

SEASTAR_TEST_CASE(datafile_generation_13) {
    return sstable_compression_test(compressor::lz4, 13);
}

SEASTAR_TEST_CASE(datafile_generation_14) {
    return sstable_compression_test(compressor::snappy, 14);
}

SEASTAR_TEST_CASE(datafile_generation_15) {
    return sstable_compression_test(compressor::deflate, 15);
}

SEASTAR_TEST_CASE(datafile_generation_16) {
    return test_setup::do_with_test_directory([] {
        auto s = uncompressed_schema();

        auto mtp = make_lw_shared<memtable>(s);
        // Create a number of keys that is a multiple of the sampling level
        for (int i = 0; i < 0x80; ++i) {
            sstring k = "key" + to_sstring(i);
            auto key = partition_key::from_exploded(*s, {to_bytes(k)});
            mutation m(key, s);

            auto c_key = clustering_key::make_empty();
            m.set_clustered_cell(c_key, to_bytes("col2"), i, api::max_timestamp);
            mtp->apply(std::move(m));
        }

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 16, la, big);
        return sst->write_components(*mtp).then([s] {
            return reusable_sst(s, "tests/sstables/tests-temporary", 16).then([] (auto s) {
                // Not crashing is enough
                return make_ready_future<>();
            });
        }).then([sst, mtp] {});
    });
}

////////////////////////////////  Test basic compaction support

// open_sstable() opens the requested sstable for reading only (sstables are
// immutable, so an existing sstable cannot be opened for writing).
// It returns a future because opening requires reading from disk, and
// therefore may block. The future value is a shared sstable - a reference-
// counting pointer to an sstable - allowing for the returned handle to
// be passed around until no longer needed.
static future<sstables::shared_sstable> open_sstable(schema_ptr schema, sstring dir, unsigned long generation) {
    auto sst = make_lw_shared<sstables::sstable>(std::move(schema), dir, generation,
            sstables::sstable::version_types::la,
            sstables::sstable::format_types::big);
    auto fut = sst->load();
    return fut.then([sst = std::move(sst)] { return std::move(sst); });
}

// open_sstables() opens several generations of the same sstable, returning,
// after all the tables have been open, their vector.
static future<std::vector<sstables::shared_sstable>> open_sstables(schema_ptr s, sstring dir, std::vector<unsigned long> generations) {
    return do_with(std::vector<sstables::shared_sstable>(),
            [dir = std::move(dir), generations = std::move(generations), s] (auto& ret) mutable {
        return parallel_for_each(generations, [&ret, &dir, s] (unsigned long generation) {
            return open_sstable(s, dir, generation).then([&ret] (sstables::shared_sstable sst) {
                ret.push_back(std::move(sst));
            });
        }).then([&ret] {
            return std::move(ret);
        });
    });
}

// mutation_reader for sstable keeping all the required objects alive.
static ::mutation_reader sstable_reader(shared_sstable sst, schema_ptr s) {
    // TODO: s is probably not necessary, as the read_rows() object keeps a copy of it.
    return as_mutation_reader(sst, sst->read_rows(s));

}

static ::mutation_reader sstable_reader(shared_sstable sst, schema_ptr s, const dht::partition_range& pr) {
    return as_mutation_reader(sst, sst->read_range_rows(s, pr));
}

SEASTAR_TEST_CASE(compaction_manager_test) {
    BOOST_REQUIRE(smp::count == 1);
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}}, {}, utf8_type));

    auto cm = make_lw_shared<compaction_manager>();
    cm->start();

    auto tmp = make_lw_shared<tmpdir>();

    column_family::config cfg;
    cfg.datadir = tmp->path;
    cfg.enable_commitlog = false;
    cfg.enable_incremental_backups = false;
    auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), *cm);
    cf->start();
    cf->mark_ready_for_writes();
    cf->set_compaction_strategy(sstables::compaction_strategy_type::size_tiered);

    auto generations = make_lw_shared<std::vector<unsigned long>>({1, 2, 3, 4});

    return do_for_each(*generations, [generations, cf, cm, s, tmp] (unsigned long generation) {
        // create 4 sstables of similar size to be compacted later on.

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        sstring k = "key" + to_sstring(generation);
        auto key = partition_key::from_exploded(*s, {to_bytes(k)});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, tmp->path, generation, la, big);

        return sst->write_components(*mt).then([mt, sst, cf] {
            return sst->load().then([sst, cf] {
                column_family_test(cf).add_sstable(std::move(*sst));
                return make_ready_future<>();
            });
        });
    }).then([cf, cm, generations] {
        // submit cf to compaction manager and then check that cf's sstables
        // were compacted.

        BOOST_REQUIRE(cf->sstables_count() == generations->size());
        cf->trigger_compaction();
        BOOST_REQUIRE(cm->get_stats().active_tasks == 1);

        // wait for submitted job to finish.
        auto end = [cm] { return cm->get_stats().active_tasks == 0; };
        return do_until(end, [] {
            // sleep until compaction manager selects cf for compaction.
            return sleep(std::chrono::milliseconds(100));
        }).then([cf, cm] {
            BOOST_REQUIRE(cm->get_stats().completed_tasks == 1);
            BOOST_REQUIRE(cm->get_stats().errors == 0);

            // remove cf from compaction manager; this will wait for the
            // ongoing compaction to finish.
            return cf->stop().then([cf, cm] {
                // expect sstables of cf to be compacted.
                BOOST_REQUIRE(cf->sstables_count() == 1);
                // stop all compaction manager tasks.
                return cm->stop().then([cf, cm] {
                    return make_ready_future<>();
                });
            });
        });
    }).then([s, tmp] {
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(compact) {
    BOOST_REQUIRE(smp::count == 1);
    constexpr int generation = 17;
    // The "compaction" sstable was created with the following schema:
    // CREATE TABLE compaction (
    //        name text,
    //        age int,
    //        height int,
    //        PRIMARY KEY (name)
    //);
    auto builder = schema_builder("tests", "compaction")
        .with_column("name", utf8_type, column_kind::partition_key)
        .with_column("age", int32_type)
        .with_column("height", int32_type);
    builder.set_comment("Example table for compaction");
    builder.set_gc_grace_seconds(std::numeric_limits<int32_t>::max());
    auto s = builder.build();
    auto cm = make_lw_shared<compaction_manager>();
    auto cf = make_lw_shared<column_family>(s, column_family::config(), column_family::no_commitlog(), *cm);
    cf->mark_ready_for_writes();

    return open_sstables(s, "tests/sstables/compaction", {1,2,3}).then([s, cf, cm, generation] (auto sstables) {
        return test_setup::do_with_test_directory([sstables, s, generation, cf, cm] {
            auto new_sstable = [generation, s] {
                return make_lw_shared<sstables::sstable>(s, "tests/sstables/tests-temporary",
                        generation, sstables::sstable::version_types::la, sstables::sstable::format_types::big);
            };
            return sstables::compact_sstables(std::move(sstables), *cf, new_sstable, std::numeric_limits<uint64_t>::max(), 0).then([s, generation, cf, cm] (auto) {
                // Verify that the compacted sstable has the right content. We expect to see:
                //  name  | age | height
                // -------+-----+--------
                //  jerry |  40 |    170
                //    tom |  20 |    180
                //   john |  20 |   deleted
                //   nadav - deleted partition
                return open_sstable(s, "tests/sstables/tests-temporary", generation).then([s] (shared_sstable sst) {
                    auto reader = make_lw_shared(sstable_reader(sst, s)); // reader holds sst and s alive.
                    return (*reader)().then([] (auto sm) {
                        return mutation_from_streamed_mutation(std::move(sm));
                    }).then([reader, s] (mutation_opt m) {
                        BOOST_REQUIRE(m);
                        BOOST_REQUIRE(m->key().equal(*s, partition_key::from_singular(*s, data_value(sstring("jerry")))));
                        BOOST_REQUIRE(!m->partition().partition_tombstone());
                        auto &rows = m->partition().clustered_rows();
                        BOOST_REQUIRE(rows.calculate_size() == 1);
                        auto &row = rows.begin()->row();
                        BOOST_REQUIRE(!row.deleted_at());
                        auto &cells = row.cells();
                        BOOST_REQUIRE(cells.cell_at(s->get_column_definition("age")->id).as_atomic_cell().value() == bytes({0,0,0,40}));
                        BOOST_REQUIRE(cells.cell_at(s->get_column_definition("height")->id).as_atomic_cell().value() == bytes({0,0,0,(char)170}));
                        return (*reader)();
                    }).then([] (auto sm) {
                        return mutation_from_streamed_mutation(std::move(sm));
                    }).then([reader, s] (mutation_opt m) {
                        BOOST_REQUIRE(m);
                        BOOST_REQUIRE(m->key().equal(*s, partition_key::from_singular(*s, data_value(sstring("tom")))));
                        BOOST_REQUIRE(!m->partition().partition_tombstone());
                        auto &rows = m->partition().clustered_rows();
                        BOOST_REQUIRE(rows.calculate_size() == 1);
                        auto &row = rows.begin()->row();
                        BOOST_REQUIRE(!row.deleted_at());
                        auto &cells = row.cells();
                        BOOST_REQUIRE(cells.cell_at(s->get_column_definition("age")->id).as_atomic_cell().value() == bytes({0,0,0,20}));
                        BOOST_REQUIRE(cells.cell_at(s->get_column_definition("height")->id).as_atomic_cell().value() == bytes({0,0,0,(char)180}));
                        return (*reader)();
                    }).then([] (auto sm) {
                        return mutation_from_streamed_mutation(std::move(sm));
                    }).then([reader, s] (mutation_opt m) {
                        BOOST_REQUIRE(m);
                        BOOST_REQUIRE(m->key().equal(*s, partition_key::from_singular(*s, data_value(sstring("john")))));
                        BOOST_REQUIRE(!m->partition().partition_tombstone());
                        auto &rows = m->partition().clustered_rows();
                        BOOST_REQUIRE(rows.calculate_size() == 1);
                        auto &row = rows.begin()->row();
                        BOOST_REQUIRE(!row.deleted_at());
                        auto &cells = row.cells();
                        BOOST_REQUIRE(cells.cell_at(s->get_column_definition("age")->id).as_atomic_cell().value() == bytes({0,0,0,20}));
                        BOOST_REQUIRE(cells.find_cell(s->get_column_definition("height")->id) == nullptr);
                        return (*reader)();
                    }).then([] (auto sm) {
                        return mutation_from_streamed_mutation(std::move(sm));
                    }).then([reader, s] (mutation_opt m) {
                        BOOST_REQUIRE(m);
                        BOOST_REQUIRE(m->key().equal(*s, partition_key::from_singular(*s, data_value(sstring("nadav")))));
                        BOOST_REQUIRE(m->partition().partition_tombstone());
                        auto &rows = m->partition().clustered_rows();
                        BOOST_REQUIRE(rows.calculate_size() == 0);
                        return (*reader)();
                    }).then([reader] (streamed_mutation_opt m) {
                        BOOST_REQUIRE(!m);
                    });
                });
            });
        });
    });

    // verify that the compacted sstable look like
}

// Used to be compatible with API provided by size_tiered_most_interesting_bucket().
static lw_shared_ptr<sstable_list> create_sstable_list(std::vector<sstables::shared_sstable>& sstables) {
    sstable_list list;
    for (auto& sst : sstables) {
        list.insert(sst);
    }
    return make_lw_shared<sstable_list>(std::move(list));
}


static std::vector<sstables::shared_sstable> get_candidates_for_leveled_strategy(column_family& cf) {
    std::vector<sstables::shared_sstable> candidates;
    candidates.reserve(cf.sstables_count());
    for (auto& entry : *cf.get_sstables()) {
        candidates.push_back(entry);
    }
    return candidates;
}

// Return vector of sstables generated by compaction. Only relevant for leveled one.
static future<std::vector<unsigned long>> compact_sstables(std::vector<unsigned long> generations_to_compact, unsigned long new_generation, bool create_sstables,
        uint64_t min_sstable_size, compaction_strategy_type strategy) {
    BOOST_REQUIRE(smp::count == 1);
    schema_builder builder(make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", utf8_type}}, {}, utf8_type)));
    builder.set_compressor_params(compression_parameters({ }));
    auto s = builder.build(schema_builder::compact_storage::no);

    auto cm = make_lw_shared<compaction_manager>();
    auto cf = make_lw_shared<column_family>(s, column_family::config(), column_family::no_commitlog(), *cm);
    cf->mark_ready_for_writes();

    auto generations = make_lw_shared<std::vector<unsigned long>>(std::move(generations_to_compact));
    auto sstables = make_lw_shared<std::vector<sstables::shared_sstable>>();
    auto created = make_lw_shared<std::vector<unsigned long>>();

    auto f = make_ready_future<>();

    return f.then([generations, sstables, s, create_sstables, min_sstable_size] () mutable {
        if (!create_sstables) {
            return open_sstables(s, "tests/sstables/tests-temporary", *generations).then([sstables] (auto opened_sstables) mutable {
                for (auto& sst : opened_sstables) {
                    sstables->push_back(sst);
                }
                return make_ready_future<>();
            });
        }
        return do_for_each(*generations, [generations, sstables, s, min_sstable_size] (unsigned long generation) {
            auto mt = make_lw_shared<memtable>(s);

            const column_definition& r1_col = *s->get_column_definition("r1");

            sstring k = "key" + to_sstring(generation);
            auto key = partition_key::from_exploded(*s, {to_bytes(k)});
            auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

            mutation m(key, s);
            m.set_clustered_cell(c_key, r1_col, make_atomic_cell(bytes(min_sstable_size, 'a')));
            mt->apply(std::move(m));

            auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", generation, la, big);

            return sst->write_components(*mt).then([mt, sst, s, sstables] {
                return sst->load().then([sst, sstables] {
                    sstables->push_back(sst);
                    return make_ready_future<>();
                });
            });
        });
    }).then([cf, sstables, new_generation, generations, strategy, created, min_sstable_size, s] () mutable {
        auto generation = make_lw_shared<unsigned long>(new_generation);
        auto new_sstable = [generation, created, s] {
            auto gen = (*generation)++;
            created->push_back(gen);
            return make_lw_shared<sstables::sstable>(s, "tests/sstables/tests-temporary",
                gen, sstables::sstable::version_types::la, sstables::sstable::format_types::big);
        };
        // We must have opened at least all original candidates.
        BOOST_REQUIRE(generations->size() == sstables->size());

        if (strategy == compaction_strategy_type::size_tiered) {
            auto sstable_list = create_sstable_list(*sstables);
            // Calling function that will return a list of sstables to compact based on size-tiered strategy.
            auto sstables_to_compact = size_tiered_most_interesting_bucket(sstable_list);
            // We do expect that all candidates were selected for compaction (in this case).
            BOOST_REQUIRE(sstables_to_compact.size() == sstables->size());
            return sstables::compact_sstables(std::move(sstables_to_compact), *cf, new_sstable,
                std::numeric_limits<uint64_t>::max(), 0).then([generation] (auto) {});
        } else if (strategy == compaction_strategy_type::leveled) {
            for (auto& sst : *sstables) {
                BOOST_REQUIRE(sst->get_sstable_level() == 0);
                BOOST_REQUIRE(sst->data_size() >= min_sstable_size);
                column_family_test(cf).add_sstable(std::move(*sst));
            }
            auto candidates = get_candidates_for_leveled_strategy(*cf);
            leveled_manifest manifest = leveled_manifest::create(*cf, candidates, 1);
            std::vector<stdx::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
            std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
            auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
            BOOST_REQUIRE(candidate.sstables.size() == sstables->size());
            BOOST_REQUIRE(candidate.level == 1);
            BOOST_REQUIRE(candidate.max_sstable_bytes == 1024*1024);

            return sstables::compact_sstables(std::move(candidate.sstables), *cf, new_sstable,
                1024*1024, candidate.level).then([generation] (auto) {});
        } else {
            throw std::runtime_error("unexpected strategy");
        }
        return make_ready_future<>();
    }).then([cf, cm, created] {
        return std::move(*created);
    });
}

static future<> compact_sstables(std::vector<unsigned long> generations_to_compact, unsigned long new_generation, bool create_sstables = true) {
    uint64_t min_sstable_size = 50;
    return compact_sstables(std::move(generations_to_compact), new_generation, create_sstables, min_sstable_size,
                            compaction_strategy_type::size_tiered).then([new_generation] (auto ret) {
        // size tiered compaction will output at most one sstable, let's assert that.
        BOOST_REQUIRE(ret.size() == 1);
        BOOST_REQUIRE(ret[0] == new_generation);
        return make_ready_future<>();
    });
}

static future<> check_compacted_sstables(unsigned long generation, std::vector<unsigned long> compacted_generations) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}}, {}, utf8_type));

    auto generations = make_lw_shared<std::vector<unsigned long>>(std::move(compacted_generations));

    return open_sstable(s, "tests/sstables/tests-temporary", generation).then([s, generations] (shared_sstable sst) {
        auto reader = sstable_reader(sst, s); // reader holds sst and s alive.
        auto keys = make_lw_shared<std::vector<partition_key>>();

        return do_with(std::move(reader), [generations, s, keys] (::mutation_reader& reader) {
            return do_for_each(*generations, [&reader, s, keys] (unsigned long generation) mutable {
                return reader().then([generation, keys] (streamed_mutation_opt m) {
                    BOOST_REQUIRE(m);
                    keys->push_back(m->key());
                });
            }).then([s, keys, generations] {
                // keys from compacted sstable aren't ordered lexographically,
                // thus we must read all keys into a vector, sort the vector
                // lexographically, then proceed with the comparison.
                std::sort(keys->begin(), keys->end(), partition_key::less_compare(*s));
                BOOST_REQUIRE(keys->size() == generations->size());
                auto i = 0;
                for (auto& k : *keys) {
                    sstring original_k = "key" + to_sstring((*generations)[i++]);
                    BOOST_REQUIRE(k.equal(*s, partition_key::from_singular(*s, data_value(original_k))));
                }
                return make_ready_future<>();
            });
        });
    });
}

SEASTAR_TEST_CASE(compact_02) {
    // NOTE: generations 18 to 38 are used here.

    // This tests size-tiered compaction strategy by creating 4 sstables of
    // similar size and compacting them to create a new tier.
    // The process above is repeated 4 times until you have 4 compacted
    // sstables of similar size. Then you compact these 4 compacted sstables,
    // and make sure that you have all partition keys.
    // By the way, automatic compaction isn't tested here, instead the
    // strategy algorithm that selects candidates for compaction.

    return test_setup::do_with_test_directory([] {
        // Compact 4 sstables into 1 using size-tiered strategy to select sstables.
        // E.g.: generations 18, 19, 20 and 21 will be compacted into generation 22.
        return compact_sstables({ 18, 19, 20, 21 }, 22).then([] {
            // Check that generation 22 contains all keys of generations 18, 19, 20 and 21.
            return check_compacted_sstables(22, { 18, 19, 20, 21 });
        }).then([] {
            return compact_sstables({ 23, 24, 25, 26 }, 27).then([] {
                return check_compacted_sstables(27, { 23, 24, 25, 26 });
            });
        }).then([] {
            return compact_sstables({ 28, 29, 30, 31 }, 32).then([] {
                return check_compacted_sstables(32, { 28, 29, 30, 31 });
            });
        }).then([] {
            return compact_sstables({ 33, 34, 35, 36 }, 37).then([] {
                return check_compacted_sstables(37, { 33, 34, 35, 36 });
            });
        }).then([] {
            // In this step, we compact 4 compacted sstables.
            return compact_sstables({ 22, 27, 32, 37 }, 38, false).then([] {
                // Check that the compacted sstable contains all keys.
                return check_compacted_sstables(38,
                    { 18, 19, 20, 21, 23, 24, 25, 26, 28, 29, 30, 31, 33, 34, 35, 36 });
            });
        });
    });
}

SEASTAR_TEST_CASE(datafile_generation_37) {
    return test_setup::do_with_test_directory([] {
        auto s = compact_simple_dense_schema();

        auto mtp = make_lw_shared<memtable>(s);

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        mutation m(key, s);

        auto c_key = exploded_clustering_prefix({to_bytes("cl1") });
        const column_definition& cl2 = *s->get_column_definition("cl2");

        m.set_clustered_cell(c_key, cl2, make_atomic_cell(bytes_type->decompose(data_value(to_bytes("cl2")))));
        mtp->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 37, la, big);
        return sst->write_components(*mtp).then([s] {
            return reusable_sst(s, "tests/sstables/tests-temporary", 37).then([s] (auto sstp) {
                return do_with(sstables::key("key1"), [sstp, s] (auto& key) {
                    return sstp->read_row(s, key).then([] (auto sm) {
                        return mutation_from_streamed_mutation(std::move(sm));
                    }).then([sstp, s] (auto mutation) {
                        auto& mp = mutation->partition();

                        auto exploded = exploded_clustering_prefix({"cl1"});
                        auto clustering = clustering_key::from_clustering_prefix(*s, exploded);

                        auto row = mp.clustered_row(*s, clustering);
                        match_live_cell(row.cells(), *s, "cl2", data_value(to_bytes("cl2")));
                        return make_ready_future<>();
                    });
                });
            });
        }).then([sst, mtp, s] {});
    });
}

SEASTAR_TEST_CASE(datafile_generation_38) {
    return test_setup::do_with_test_directory([] {
        auto s = compact_dense_schema();

        auto mtp = make_lw_shared<memtable>(s);

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        mutation m(key, s);

        auto exploded = exploded_clustering_prefix({"cl1", "cl2"});
        auto c_key = clustering_key::from_clustering_prefix(*s, exploded);

        const column_definition& cl3 = *s->get_column_definition("cl3");
        m.set_clustered_cell(c_key, cl3, make_atomic_cell(bytes_type->decompose(data_value(to_bytes("cl3")))));
        mtp->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 38, la, big);
        return sst->write_components(*mtp).then([s] {
            return reusable_sst(s, "tests/sstables/tests-temporary", 38).then([s] (auto sstp) {
                return do_with(sstables::key("key1"), [sstp, s] (auto& key) {
                    return sstp->read_row(s, key).then([] (auto sm) {
                        return mutation_from_streamed_mutation(std::move(sm));
                    }).then([sstp, s] (auto mutation) {
                        auto& mp = mutation->partition();
                        auto exploded = exploded_clustering_prefix({"cl1", "cl2"});
                        auto clustering = clustering_key::from_clustering_prefix(*s, exploded);

                        auto row = mp.clustered_row(*s, clustering);
                        match_live_cell(row.cells(), *s, "cl3", data_value(to_bytes("cl3")));
                        return make_ready_future<>();
                    });
                });
            });
        }).then([sst, mtp, s] {});
    });
}

SEASTAR_TEST_CASE(datafile_generation_39) {
    return test_setup::do_with_test_directory([] {
        auto s = compact_sparse_schema();

        auto mtp = make_lw_shared<memtable>(s);

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        mutation m(key, s);

        auto c_key = clustering_key::make_empty();

        const column_definition& cl1 = *s->get_column_definition("cl1");
        m.set_clustered_cell(c_key, cl1, make_atomic_cell(bytes_type->decompose(data_value(to_bytes("cl1")))));
        const column_definition& cl2 = *s->get_column_definition("cl2");
        m.set_clustered_cell(c_key, cl2, make_atomic_cell(bytes_type->decompose(data_value(to_bytes("cl2")))));
        mtp->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 39, la, big);
        return sst->write_components(*mtp).then([s] {
            return reusable_sst(s, "tests/sstables/tests-temporary", 39).then([s] (auto sstp) {
                return do_with(sstables::key("key1"), [sstp, s] (auto& key) {
                    return sstp->read_row(s, key).then([] (auto sm) {
                        return mutation_from_streamed_mutation(std::move(sm));
                    }).then([sstp, s] (auto mutation) {
                        auto& mp = mutation->partition();
                        auto row = mp.clustered_row(*s, clustering_key::make_empty());
                        match_live_cell(row.cells(), *s, "cl1", data_value(data_value(to_bytes("cl1"))));
                        match_live_cell(row.cells(), *s, "cl2", data_value(data_value(to_bytes("cl2"))));
                        return make_ready_future<>();
                    });
                });
            });
        }).then([sst, mtp, s] {});
    });
}

SEASTAR_TEST_CASE(datafile_generation_40) {
    return test_setup::do_with_test_directory([] {
        // Data file with clustering key sorted in descending order
        //
        // Respective CQL table and CQL insert:
        // CREATE TABLE table (
        //    p1 text,
        //    c1 text,
        //    r1 int,
        //    PRIMARY KEY (p1, c1)
        // ) WITH compact storage and compression = {} and clustering order by (cl1 desc);
        // INSERT INTO table (p1, c1, r1) VALUES ('key1', 'a', 1);
        // INSERT INTO table (p1, c1, r1) VALUES ('key1', 'b', 1);

        auto s = [] {
            schema_builder builder(make_lw_shared(schema({}, some_keyspace, some_column_family,
                {{"p1", utf8_type}}, {{"c1", reversed_type_impl::get_instance(utf8_type)}}, {{"r1", int32_type}}, {}, utf8_type
            )));
            builder.set_compressor_params(compression_parameters({ }));
            return builder.build(schema_builder::compact_storage::yes);
        }();

        auto mt = make_lw_shared<memtable>(s);
        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        mutation m(key, s);

        const column_definition& r1_col = *s->get_column_definition("r1");
        auto ca = clustering_key::from_exploded(*s, {to_bytes("a")});
        m.set_clustered_cell(ca, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m));

        auto cb = clustering_key::from_exploded(*s, {to_bytes("b")});
        m.set_clustered_cell(cb, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 40, la, big);

        return sst->write_components(*mt).then([mt, sst, s] {
            auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, 40, big, sstable::component_type::Data);
            return open_file_dma(fname, open_flags::ro).then([] (file f) {
                auto bufptr = allocate_aligned_buffer<char>(4096, 4096);

                auto fut = f.dma_read(0, bufptr.get(), 4096);
                return std::move(fut).then([f = std::move(f), bufptr = std::move(bufptr)] (size_t size) mutable {
                    auto buf = bufptr.get();
                    size_t offset = 0;
                    auto check_chunk = [buf, &offset] (std::vector<uint8_t> vec) {
                        BOOST_REQUIRE(::memcmp(vec.data(), &buf[offset], vec.size()) == 0);
                        offset += vec.size();
                    };
                    check_chunk({ /* first key */ 0, 4, 'k', 'e', 'y', '1' });
                    check_chunk({ /* deletion time */ 0x7f, 0xff, 0xff, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0 });
                    check_chunk({ /* first expected row name */ 0, 1, 'b' });
                    check_chunk(/* row contents, same for both */ {/* mask */ 0, /* timestamp */ 0, 0, 0, 0, 0, 0, 0, 0, /* value */ 0, 0, 0, 4, 0, 0, 0, 1 });
                    check_chunk({ /* second expected row name */ 0, 1, 'a' });
                    check_chunk(/* row contents, same for both */ {/* mask */ 0, /* timestamp */ 0, 0, 0, 0, 0, 0, 0, 0, /* value */ 0, 0, 0, 4, 0, 0, 0, 1 });
                    return f.close().finally([f] {});
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(datafile_generation_41) {
    return test_setup::do_with_test_directory([] {
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}, {"r2", int32_type}}, {}, utf8_type));

        auto mt = make_lw_shared<memtable>(s);

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("c1")});
        mutation m(key, s);

        tombstone tomb(api::new_timestamp(), gc_clock::now());
        m.partition().apply_delete(*s, std::move(c_key), tomb);
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 41, la, big);
        return sst->write_components(*mt).then([s, tomb] {
            return reusable_sst(s, "tests/sstables/tests-temporary", 41).then([s, tomb] (auto sstp) mutable {
                return do_with(sstables::key("key1"), [sstp, s, tomb] (auto& key) {
                    return sstp->read_row(s, key).then([] (auto sm) {
                        return mutation_from_streamed_mutation(std::move(sm));
                    }).then([sstp, s, tomb] (auto mutation) {
                        auto& mp = mutation->partition();
                        BOOST_REQUIRE(mp.clustered_rows().calculate_size() == 1);
                        auto c_row = *(mp.clustered_rows().begin());
                        BOOST_REQUIRE(c_row.row().deleted_at() == tomb);
                    });
                });
            });
        }).then([sst, mt] {});
    });
}

SEASTAR_TEST_CASE(check_compaction_ancestor_metadata) {
    // NOTE: generations 42 to 46 are used here.

    // check that ancestors list of compacted sstable is correct.

    return test_setup::do_with_test_directory([] {
        return compact_sstables({ 42, 43, 44, 45 }, 46).then([] {
            auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
                {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", utf8_type}}, {}, utf8_type));
            return open_sstable(s, "tests/sstables/tests-temporary", 46).then([] (shared_sstable sst) {
                std::set<unsigned long> ancestors;
                const compaction_metadata& cm = sst->get_compaction_metadata();
                for (auto& ancestor : cm.ancestors.elements) {
                    ancestors.insert(ancestor);
                }
                BOOST_REQUIRE(ancestors.find(42) != ancestors.end());
                BOOST_REQUIRE(ancestors.find(43) != ancestors.end());
                BOOST_REQUIRE(ancestors.find(44) != ancestors.end());
                BOOST_REQUIRE(ancestors.find(45) != ancestors.end());

                return make_ready_future<>();
            });
        });
    });
}

SEASTAR_TEST_CASE(datafile_generation_47) {
    // Tests the problem in which the sstable row parser would hang.
    return test_setup::do_with_test_directory([] {
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", utf8_type}}, {}, utf8_type));

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("c1")});
        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(bytes(512*1024, 'a')));
        mt->apply(std::move(m));

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 47, la, big);
        return sst->write_components(*mt).then([s] {
            return reusable_sst(s, "tests/sstables/tests-temporary", 47).then([s] (auto sstp) mutable {
                auto reader = make_lw_shared(sstable_reader(sstp, s));
                return repeat([reader] {
                    return (*reader)().then([] (streamed_mutation_opt m) {
                        if (!m) {
                            return make_ready_future<stop_iteration>(stop_iteration::yes);
                        }
                        return make_ready_future<stop_iteration>(stop_iteration::no);
                    });
                }).then([sstp, reader, s] {});
            });
        }).then([sst, mt] {});
    });
}

SEASTAR_TEST_CASE(test_counter_write) {
    return test_setup::do_with_test_directory([] {
        return seastar::async([] {
            auto s = schema_builder(some_keyspace, some_column_family)
                    .with_column("p1", utf8_type, column_kind::partition_key)
                    .with_column("c1", utf8_type, column_kind::clustering_key)
                    .with_column("r1", counter_type)
                    .with_column("r2", counter_type)
                    .build();
            auto mt = make_lw_shared<memtable>(s);

            auto& r1_col = *s->get_column_definition("r1");
            auto& r2_col = *s->get_column_definition("r2");

            auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
            auto c_key = clustering_key::from_exploded(*s, {to_bytes("c1")});
            auto c_key2 = clustering_key::from_exploded(*s, {to_bytes("c2")});

            mutation m(key, s);

            std::vector<counter_id> ids;
            std::generate_n(std::back_inserter(ids), 3, counter_id::generate_random);
            boost::range::sort(ids);

            counter_cell_builder b1;
            b1.add_shard(counter_shard(ids[0], 5, 1));
            b1.add_shard(counter_shard(ids[1], -4, 1));
            b1.add_shard(counter_shard(ids[2], 9, 1));
            auto ts = api::new_timestamp();
            m.set_clustered_cell(c_key, r1_col, b1.build(ts));

            counter_cell_builder b2;
            b2.add_shard(counter_shard(ids[1], -1, 1));
            b2.add_shard(counter_shard(ids[2], 2, 1));
            m.set_clustered_cell(c_key, r2_col, b2.build(ts));

            m.set_clustered_cell(c_key2, r1_col, make_dead_atomic_cell(1));

            mt->apply(m);

            auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 900, la, big);
            sst->write_components(*mt).get();

            auto sstp = reusable_sst(s, "tests/sstables/tests-temporary", 900).get0();
            assert_that(sstable_reader(sstp, s))
                .produces(m)
                .produces_end_of_stream();
        });
    });
}

// Leveled compaction strategy tests

static dht::token create_token_from_key(sstring key) {
    sstables::key_view key_view = sstables::key_view(bytes_view(reinterpret_cast<const signed char*>(key.c_str()), key.size()));
    dht::token token = dht::global_partitioner().get_token(key_view);
    assert(token == dht::global_partitioner().get_token(key_view));
    return token;
}

static range<dht::token> create_token_range_from_keys(sstring start_key, sstring end_key) {
    dht::token start = create_token_from_key(start_key);
    assert(engine().cpu_id() == dht::global_partitioner().shard_of(start));
    dht::token end = create_token_from_key(end_key);
    assert(engine().cpu_id() == dht::global_partitioner().shard_of(end));
    assert(end >= start);
    return range<dht::token>::make(start, end);
}

static std::vector<std::pair<sstring, dht::token>> token_generation_for_current_shard(unsigned tokens_to_generate) {
    unsigned tokens = 0;
    unsigned key_id = 0;
    std::vector<std::pair<sstring, dht::token>> key_and_token_pair;

    key_and_token_pair.reserve(tokens_to_generate);
    dht::set_global_partitioner(to_sstring("org.apache.cassandra.dht.Murmur3Partitioner"));

    while (tokens < tokens_to_generate) {
        sstring key = to_sstring(key_id++);
        dht::token token = create_token_from_key(key);
        if (engine().cpu_id() != dht::global_partitioner().shard_of(token)) {
            continue;
        }
        tokens++;
        key_and_token_pair.emplace_back(key, token);
    }
    assert(key_and_token_pair.size() == tokens_to_generate);

    std::sort(key_and_token_pair.begin(),key_and_token_pair.end(), [] (auto& i, auto& j) {
        return i.second < j.second;
    });

    return key_and_token_pair;
}

static void add_sstable_for_leveled_test(lw_shared_ptr<column_family>& cf, int64_t gen, uint64_t fake_data_size,
                                         uint32_t sstable_level, sstring first_key, sstring last_key, int64_t max_timestamp = 0) {
    auto sst = make_lw_shared<sstable>(cf->schema(), "", gen, la, big);
    sstables::test(sst).set_values_for_leveled_strategy(fake_data_size, sstable_level, max_timestamp, std::move(first_key), std::move(last_key));
    assert(sst->data_size() == fake_data_size);
    assert(sst->get_sstable_level() == sstable_level);
    assert(sst->get_stats_metadata().max_timestamp == max_timestamp);
    assert(sst->generation() == gen);
    column_family_test(cf).add_sstable(std::move(*sst));
}

static lw_shared_ptr<sstable> add_sstable_for_overlapping_test(lw_shared_ptr<column_family>& cf, int64_t gen, sstring first_key, sstring last_key, stats_metadata stats = {}) {
    auto sst = make_lw_shared<sstable>(cf->schema(), "", gen, la, big);
    sstables::test(sst).set_values(std::move(first_key), std::move(last_key), std::move(stats));
    column_family_test(cf).add_sstable(sst);
    return sst;
}
static lw_shared_ptr<sstable> sstable_for_overlapping_test(const schema_ptr& schema, int64_t gen, sstring first_key, sstring last_key) {
    auto sst = make_lw_shared<sstable>(schema, "", gen, la, big);
    sstables::test(sst).set_values(std::move(first_key), std::move(last_key), {});
    return sst;
}

// ranges: [a,b] and [c,d]
// returns true if token ranges overlap.
static bool key_range_overlaps(sstring a, sstring b, sstring c, sstring d) {
    auto range1 = create_token_range_from_keys(a, b);
    auto range2 = create_token_range_from_keys(c, d);
    return range1.overlaps(range2, dht::token_comparator());
}

static shared_sstable get_sstable(const lw_shared_ptr<column_family>& cf, int64_t generation) {
    auto sstables = cf->get_sstables();
    auto entry = boost::range::find_if(*sstables, [generation] (shared_sstable sst) { return generation == sst->generation(); });
    assert(entry != sstables->end());
    assert((*entry)->generation() == generation);
    return *entry;
}

static bool sstable_overlaps(const lw_shared_ptr<column_family>& cf, int64_t gen1, int64_t gen2) {
    auto candidate1 = get_sstable(cf, gen1);
    auto range1 = range<dht::token>::make(candidate1->get_first_decorated_key()._token, candidate1->get_last_decorated_key()._token);
    auto candidate2 = get_sstable(cf, gen2);
    auto range2 = range<dht::token>::make(candidate2->get_first_decorated_key()._token, candidate2->get_last_decorated_key()._token);
    return range1.overlaps(range2, dht::token_comparator());
}

SEASTAR_TEST_CASE(leveled_01) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));

    column_family::config cfg;
    compaction_manager cm;
    cfg.enable_disk_writes = false;
    cfg.enable_commitlog = false;
    auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), cm);
    cf->mark_ready_for_writes();

    auto key_and_token_pair = token_generation_for_current_shard(50);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;

    // Creating two sstables which key range overlap.
    add_sstable_for_leveled_test(cf, /*gen*/1, /*data_size*/0, /*level*/0, min_key, max_key);
    BOOST_REQUIRE(cf->get_sstables()->size() == 1);

    add_sstable_for_leveled_test(cf, /*gen*/2, /*data_size*/0, /*level*/0, key_and_token_pair[1].first, max_key);
    BOOST_REQUIRE(cf->get_sstables()->size() == 2);

    BOOST_REQUIRE(key_range_overlaps(min_key, max_key, key_and_token_pair[1].first, max_key) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 2) == true);

    auto max_sstable_size_in_mb = 1;
    auto candidates = get_candidates_for_leveled_strategy(*cf);
    leveled_manifest manifest = leveled_manifest::create(*cf, candidates, max_sstable_size_in_mb);
    BOOST_REQUIRE(manifest.get_level_size(0) == 2);
    std::vector<stdx::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.sstables.size() == 2);
    BOOST_REQUIRE(candidate.level == 0);

    std::set<unsigned long> gens = { 1, 2 };
    for (auto& sst : candidate.sstables) {
        auto it = gens.find(sst->generation());
        BOOST_REQUIRE(it != gens.end());
        gens.erase(sst->generation());
        BOOST_REQUIRE(sst->get_sstable_level() == 0);
    }
    BOOST_REQUIRE(gens.empty());

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(leveled_02) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));

    column_family::config cfg;
    compaction_manager cm;
    cfg.enable_disk_writes = false;
    cfg.enable_commitlog = false;
    auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), cm);
    cf->mark_ready_for_writes();

    auto key_and_token_pair = token_generation_for_current_shard(50);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;

    // Generation 1 will overlap only with generation 2.
    // Remember that for level0, leveled strategy prefer choosing older sstables as candidates.

    add_sstable_for_leveled_test(cf, /*gen*/1, /*data_size*/0, /*level*/0, min_key, key_and_token_pair[10].first);
    BOOST_REQUIRE(cf->get_sstables()->size() == 1);

    add_sstable_for_leveled_test(cf, /*gen*/2, /*data_size*/0, /*level*/0, min_key, key_and_token_pair[20].first);
    BOOST_REQUIRE(cf->get_sstables()->size() == 2);

    add_sstable_for_leveled_test(cf, /*gen*/3, /*data_size*/0, /*level*/0, key_and_token_pair[30].first, max_key);
    BOOST_REQUIRE(cf->get_sstables()->size() == 3);

    BOOST_REQUIRE(key_range_overlaps(min_key, key_and_token_pair[10].first, min_key, key_and_token_pair[20].first) == true);
    BOOST_REQUIRE(key_range_overlaps(min_key, key_and_token_pair[20].first, key_and_token_pair[30].first, max_key) == false);
    BOOST_REQUIRE(key_range_overlaps(min_key, key_and_token_pair[10].first, key_and_token_pair[30].first, max_key) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 2) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 1) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 3) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 3) == false);

    auto max_sstable_size_in_mb = 1;
    auto candidates = get_candidates_for_leveled_strategy(*cf);
    leveled_manifest manifest = leveled_manifest::create(*cf, candidates, max_sstable_size_in_mb);
    BOOST_REQUIRE(manifest.get_level_size(0) == 3);
    std::vector<stdx::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.sstables.size() == 3);
    BOOST_REQUIRE(candidate.level == 0);

    std::set<unsigned long> gens = { 1, 2, 3 };
    for (auto& sst : candidate.sstables) {
        auto it = gens.find(sst->generation());
        BOOST_REQUIRE(it != gens.end());
        gens.erase(sst->generation());
        BOOST_REQUIRE(sst->get_sstable_level() == 0);
    }
    BOOST_REQUIRE(gens.empty());

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(leveled_03) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));

    column_family::config cfg;
    compaction_manager cm;
    cfg.enable_disk_writes = false;
    cfg.enable_commitlog = false;
    auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), cm);
    cf->mark_ready_for_writes();

    auto key_and_token_pair = token_generation_for_current_shard(50);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;

    // Creating two sstables of level 0 which overlap
    add_sstable_for_leveled_test(cf, /*gen*/1, /*data_size*/1024*1024, /*level*/0, min_key, key_and_token_pair[10].first);
    add_sstable_for_leveled_test(cf, /*gen*/2, /*data_size*/1024*1024, /*level*/0, min_key, key_and_token_pair[20].first);
    // Creating a sstable of level 1 which overlap with two sstables above.
    add_sstable_for_leveled_test(cf, /*gen*/3, /*data_size*/1024*1024, /*level*/1, min_key, key_and_token_pair[30].first);
    // Creating a sstable of level 1 which doesn't overlap with any sstable.
    add_sstable_for_leveled_test(cf, /*gen*/4, /*data_size*/1024*1024, /*level*/1, key_and_token_pair[40].first, max_key);

    BOOST_REQUIRE(cf->get_sstables()->size() == 4);

    BOOST_REQUIRE(key_range_overlaps(min_key, key_and_token_pair[10].first, min_key, key_and_token_pair[20].first) == true);
    BOOST_REQUIRE(key_range_overlaps(min_key, key_and_token_pair[10].first, min_key, key_and_token_pair[30].first) == true);
    BOOST_REQUIRE(key_range_overlaps(min_key, key_and_token_pair[20].first, min_key, key_and_token_pair[30].first) == true);
    BOOST_REQUIRE(key_range_overlaps(min_key, key_and_token_pair[10].first, key_and_token_pair[40].first, max_key) == false);
    BOOST_REQUIRE(key_range_overlaps(min_key, key_and_token_pair[30].first, key_and_token_pair[40].first, max_key) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 2) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 3) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 3) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 4) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 4) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 3, 4) == false);

    auto max_sstable_size_in_mb = 1;
    auto candidates = get_candidates_for_leveled_strategy(*cf);
    leveled_manifest manifest = leveled_manifest::create(*cf, candidates, max_sstable_size_in_mb);
    BOOST_REQUIRE(manifest.get_level_size(0) == 2);
    BOOST_REQUIRE(manifest.get_level_size(1) == 2);
    std::vector<stdx::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.sstables.size() == 3);
    BOOST_REQUIRE(candidate.level == 1);

    std::set<std::pair<unsigned long, uint32_t>> gen_and_level = { {1,0}, {2,0}, {3,1} };
    for (auto& sst : candidate.sstables) {
        std::pair<unsigned long, uint32_t> pair(sst->generation(), sst->get_sstable_level());
        auto it = gen_and_level.find(pair);
        BOOST_REQUIRE(it != gen_and_level.end());
        BOOST_REQUIRE(sst->get_sstable_level() == it->second);
        gen_and_level.erase(pair);
    }
    BOOST_REQUIRE(gen_and_level.empty());

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(leveled_04) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));

    column_family::config cfg;
    compaction_manager cm;
    cfg.enable_disk_writes = false;
    cfg.enable_commitlog = false;
    auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), cm);
    cf->mark_ready_for_writes();

    auto key_and_token_pair = token_generation_for_current_shard(50);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;

    auto max_sstable_size_in_mb = 1;
    auto max_sstable_size_in_bytes = max_sstable_size_in_mb*1024*1024;

    // add 1 level-0 sstable to cf.
    add_sstable_for_leveled_test(cf, /*gen*/1, /*data_size*/max_sstable_size_in_bytes, /*level*/0, min_key, max_key);

    // create two big sstables in level1 to force leveled compaction on it.
    auto max_bytes_for_l1 = leveled_manifest::max_bytes_for_level(1, max_sstable_size_in_bytes);
    // NOTE: SSTables in level1 cannot overlap.
    add_sstable_for_leveled_test(cf, /*gen*/2, /*data_size*/max_bytes_for_l1, /*level*/1, min_key, key_and_token_pair[25].first);
    add_sstable_for_leveled_test(cf, /*gen*/3, /*data_size*/max_bytes_for_l1, /*level*/1, key_and_token_pair[26].first, max_key);

    // Create SSTable in level2 that overlaps with the ones in level1,
    // so compaction in level1 will select overlapping sstables in
    // level2.
    add_sstable_for_leveled_test(cf, /*gen*/4, /*data_size*/max_sstable_size_in_bytes, /*level*/2, min_key, max_key);

    BOOST_REQUIRE(cf->get_sstables()->size() == 4);

    BOOST_REQUIRE(key_range_overlaps(min_key, max_key, min_key, max_key) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 2) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 3) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 3) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 3, 4) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 4) == true);

    auto candidates = get_candidates_for_leveled_strategy(*cf);
    leveled_manifest manifest = leveled_manifest::create(*cf, candidates, max_sstable_size_in_mb);
    BOOST_REQUIRE(manifest.get_level_size(0) == 1);
    BOOST_REQUIRE(manifest.get_level_size(1) == 2);
    BOOST_REQUIRE(manifest.get_level_size(2) == 1);

    // checks scores; used to determine the level of compaction to proceed with.
    auto level1_score = (double) manifest.get_total_bytes(manifest.get_level(1)) / (double) manifest.max_bytes_for_level(1);
    BOOST_REQUIRE(level1_score > 1.001);
    auto level2_score = (double) manifest.get_total_bytes(manifest.get_level(2)) / (double) manifest.max_bytes_for_level(2);
    BOOST_REQUIRE(level2_score < 1.001);

    std::vector<stdx::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.sstables.size() == 2);
    BOOST_REQUIRE(candidate.level == 2);

    std::set<unsigned long> levels = { 1, 2 };
    for (auto& sst : candidate.sstables) {
        auto it = levels.find(sst->get_sstable_level());
        BOOST_REQUIRE(it != levels.end());
        levels.erase(sst->get_sstable_level());
    }
    BOOST_REQUIRE(levels.empty());

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(leveled_05) {
    // NOTE: Generations from 48 to 51 are used here.
    return test_setup::do_with_test_directory([] {

        // Check compaction code with leveled strategy. In this test, two sstables of level 0 will be created.
        return compact_sstables({ 48, 49 }, 50, true, 1024*1024, compaction_strategy_type::leveled).then([] (auto generations) {
            BOOST_REQUIRE(generations.size() == 2);
            BOOST_REQUIRE(generations[0] == 50);
            BOOST_REQUIRE(generations[1] == 51);

            return seastar::async([&, generations = std::move(generations)] {
                for (auto gen : generations) {
                    auto fname = sstable::filename("tests/sstables/tests-temporary", "ks", "cf", la, gen, big, sstable::component_type::Data);
                    BOOST_REQUIRE(file_size(fname).get0() >= 1024*1024);
                }
            });
        });
    });
}

SEASTAR_TEST_CASE(leveled_06) {
    // Test that we can compact a single L1 compaction into an empty L2.

    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));

    column_family::config cfg;
    compaction_manager cm;
    cfg.enable_disk_writes = false;
    cfg.enable_commitlog = false;
    auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), cm);
    cf->mark_ready_for_writes();

    auto max_sstable_size_in_mb = 1;
    auto max_sstable_size_in_bytes = max_sstable_size_in_mb*1024*1024;

    auto max_bytes_for_l1 = leveled_manifest::max_bytes_for_level(1, max_sstable_size_in_bytes);
    // Create fake sstable that will be compacted into L2.
    add_sstable_for_leveled_test(cf, /*gen*/1, /*data_size*/max_bytes_for_l1*2, /*level*/1, "a", "a");
    BOOST_REQUIRE(cf->get_sstables()->size() == 1);

    auto candidates = get_candidates_for_leveled_strategy(*cf);
    leveled_manifest manifest = leveled_manifest::create(*cf, candidates, max_sstable_size_in_mb);
    BOOST_REQUIRE(manifest.get_level_size(0) == 0);
    BOOST_REQUIRE(manifest.get_level_size(1) == 1);
    BOOST_REQUIRE(manifest.get_level_size(2) == 0);

    std::vector<stdx::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.level == 2);
    BOOST_REQUIRE(candidate.sstables.size() == 1);
    auto& sst = (candidate.sstables)[0];
    BOOST_REQUIRE(sst->get_sstable_level() == 1);
    BOOST_REQUIRE(sst->generation() == 1);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(leveled_07) {
    // Check that sstable, with level > 0, that overlaps with another in the same level is sent back to L0.
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));

    column_family::config cfg;
    compaction_manager cm;
    cfg.enable_disk_writes = false;
    cfg.enable_commitlog = false;
    auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), cm);
    cf->mark_ready_for_writes();

    auto key_and_token_pair = token_generation_for_current_shard(5);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;

    // Creating two sstables which key range overlap.
    add_sstable_for_leveled_test(cf, /*gen*/1, /*data_size*/0, /*level*/1, min_key, max_key);
    BOOST_REQUIRE(cf->get_sstables()->size() == 1);

    add_sstable_for_leveled_test(cf, /*gen*/2, /*data_size*/0, /*level*/1, key_and_token_pair[1].first, max_key);
    BOOST_REQUIRE(cf->get_sstables()->size() == 2);

    BOOST_REQUIRE(sstable_overlaps(cf, 1, 2) == true);

    auto max_sstable_size_in_mb = 1;
    auto candidates = get_candidates_for_leveled_strategy(*cf);
    leveled_manifest manifest = leveled_manifest::create(*cf, candidates, max_sstable_size_in_mb);
    BOOST_REQUIRE(manifest.get_level_size(0) == 1);
    BOOST_REQUIRE(manifest.get_level_size(1) == 1);

    auto& l0 = manifest.get_level(0);
    auto& sst = l0.front();
    BOOST_REQUIRE(sst->generation() == 2);
    BOOST_REQUIRE(sst->get_sstable_level() == 0);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(check_overlapping) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));

    column_family::config cfg;
    compaction_manager cm;
    auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), cm);

    auto key_and_token_pair = token_generation_for_current_shard(4);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;

    auto sst1 = add_sstable_for_overlapping_test(cf, /*gen*/1, min_key, key_and_token_pair[1].first);
    auto sst2 = add_sstable_for_overlapping_test(cf, /*gen*/2, min_key, key_and_token_pair[2].first);
    auto sst3 = add_sstable_for_overlapping_test(cf, /*gen*/3, key_and_token_pair[3].first, max_key);
    auto sst4 = add_sstable_for_overlapping_test(cf, /*gen*/4, min_key, max_key);
    BOOST_REQUIRE(cf->get_sstables()->size() == 4);

    std::vector<shared_sstable> compacting = { sst1, sst2 };
    std::vector<shared_sstable> uncompacting = { sst3, sst4 };

    auto overlapping_sstables = leveled_manifest::overlapping(*s, compacting, uncompacting);
    BOOST_REQUIRE(overlapping_sstables.size() == 1);
    BOOST_REQUIRE(overlapping_sstables.front()->generation() == 4);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(check_read_indexes) {
    auto builder = schema_builder("test", "summary_test")
        .with_column("a", int32_type, column_kind::partition_key);
    builder.set_min_index_interval(256);
    auto s = builder.build();

    auto sst = make_lw_shared<sstable>(s, "tests/sstables/summary_test", 1,
        sstables::sstable::version_types::ka, big);

    auto fut = sst->load();
    return fut.then([sst] {
        return sstables::test(sst).read_indexes(0).then([sst] (index_list list) {
            BOOST_REQUIRE(list.size() == 130);
            return make_ready_future<>();
        });
    });
}

// Must run in a seastar thread
static shared_sstable make_sstable_containing(std::function<shared_sstable()> sst_factory, std::vector<mutation> muts) {
    auto sst = sst_factory();
    schema_ptr s = muts[0].schema();
    auto mt = make_lw_shared<memtable>(s);
    for (auto&& m : muts) {
        mt->apply(m);
    }
    sst->write_components(*mt).get();
    sst->open_data().get();

    std::set<mutation, mutation_decorated_key_less_comparator> merged;
    for (auto&& m : muts) {
        auto result = merged.insert(m);
        if (!result.second) {
            auto old = *result.first;
            merged.erase(result.first);
            merged.insert(old + m);
        }
    }

    // validate the sstable
    auto rd = assert_that(sstable_reader(sst, s));
    for (auto&& m : merged) {
        rd.produces(m);
    }
    rd.produces_end_of_stream();

    return sst;
}

SEASTAR_TEST_CASE(tombstone_purge_test) {
    BOOST_REQUIRE(smp::count == 1);
    return seastar::async([] {
        // In a column family with gc_grace_seconds set to 0, check that a tombstone
        // is purged after compaction.
        auto builder = schema_builder("tests", "tombstone_purge")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type);
        builder.set_gc_grace_seconds(0);
        auto s = builder.build();

        auto tmp = make_lw_shared<tmpdir>();
        auto sst_gen = [s, tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            return make_lw_shared<sstable>(s, tmp->path, (*gen)++, la, big);
        };

        auto compact = [&sst_gen, s] (std::vector<shared_sstable> all, std::vector<shared_sstable> to_compact) -> std::vector<shared_sstable> {
            auto cm = make_lw_shared<compaction_manager>();
            auto cf = make_lw_shared<column_family>(s, column_family::config(), column_family::no_commitlog(), *cm);
            cf->mark_ready_for_writes();
            for (auto&& sst : all) {
                column_family_test(cf).add_sstable(sst);
            }
            return sstables::compact_sstables(to_compact, *cf, sst_gen, std::numeric_limits<uint64_t>::max(), 0).get0();
        };

        auto next_timestamp = [] {
            static thread_local api::timestamp_type next = 1;
            return next++;
        };

        auto make_insert = [&] (partition_key key) {
            mutation m(key, s);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), next_timestamp());
            return m;
        };

        auto make_delete = [&] (partition_key key) {
            mutation m(key, s);
            tombstone tomb(next_timestamp(), gc_clock::now());
            m.partition().apply(tomb);
            return m;
        };

        auto alpha = partition_key::from_exploded(*s, {to_bytes("alpha")});
        auto beta = partition_key::from_exploded(*s, {to_bytes("beta")});

        {
            auto mut1 = make_insert(alpha);
            auto mut2 = make_insert(beta);
            auto mut3 = make_delete(alpha);

            std::vector<shared_sstable> sstables = {
                    make_sstable_containing(sst_gen, {mut1, mut2}),
                    make_sstable_containing(sst_gen, {mut3})
            };

            forward_jump_clocks(std::chrono::seconds(1));

            auto result = compact(sstables, sstables);
            BOOST_REQUIRE_EQUAL(1, result.size());

            assert_that(sstable_reader(result[0], s))
                    .produces(mut2)
                    .produces_end_of_stream();
        }

        {
            auto mut1 = make_insert(alpha);
            auto mut2 = make_insert(alpha);
            auto mut3 = make_delete(alpha);

            auto sst1 = make_sstable_containing(sst_gen, {mut1});
            auto sst2 = make_sstable_containing(sst_gen, {mut2, mut3});

            forward_jump_clocks(std::chrono::seconds(1));

            auto result = compact({sst1, sst2}, {sst2});
            BOOST_REQUIRE_EQUAL(1, result.size());

            assert_that(sstable_reader(result[0], s))
                    .produces(mut3)
                    .produces_end_of_stream();
        }

        {
            auto mut1 = make_insert(alpha);
            auto mut2 = make_delete(alpha);
            auto mut3 = make_insert(beta);
            auto mut4 = make_insert(alpha);

            auto sst1 = make_sstable_containing(sst_gen, {mut1, mut2, mut3});
            auto sst2 = make_sstable_containing(sst_gen, {mut4});

            forward_jump_clocks(std::chrono::seconds(1));

            auto result = compact({sst1, sst2}, {sst1});
            BOOST_REQUIRE_EQUAL(1, result.size());

            assert_that(sstable_reader(result[0], s))
                    .produces(mut3)
                    .produces_end_of_stream();
        }

        {
            auto mut1 = make_insert(alpha);
            auto mut2 = make_delete(alpha);
            auto mut3 = make_insert(beta);
            auto mut4 = make_insert(beta);

            auto sst1 = make_sstable_containing(sst_gen, {mut1, mut2, mut3});
            auto sst2 = make_sstable_containing(sst_gen, {mut4});

            forward_jump_clocks(std::chrono::seconds(1));

            auto result = compact({sst1, sst2}, {sst1});
            BOOST_REQUIRE_EQUAL(1, result.size());

            assert_that(sstable_reader(result[0], s))
                    .produces(mut3)
                    .produces_end_of_stream();
        }
    });
}

SEASTAR_TEST_CASE(check_multi_schema) {
    // Schema used to write sstable:
    // CREATE TABLE multi_schema_test (
    //        a int PRIMARY KEY,
    //        b int,
    //        c int,
    //        d set<int>,
    //        e int
    //);

    // Schema used to read sstable:
    // CREATE TABLE multi_schema_test (
    //        a int PRIMARY KEY,
    //        c set<int>,
    //        d int,
    //        e blob
    //);
    auto set_of_ints_type = set_type_impl::get_instance(int32_type, true);
    auto builder = schema_builder("test", "test_multi_schema")
        .with_column("a", int32_type, column_kind::partition_key)
        .with_column("c", set_of_ints_type)
        .with_column("d", int32_type)
        .with_column("e", bytes_type);
    auto s = builder.build();

    auto sst = make_lw_shared<sstable>(s, "tests/sstables/multi_schema_test", 1, sstables::sstable::version_types::ka, big);
    auto f = sst->load();
    return f.then([sst, s] {
        auto reader = make_lw_shared(sstable_reader(sst, s));
        return (*reader)().then([] (auto sm) {
            return mutation_from_streamed_mutation(std::move(sm));
        }).then([reader, s] (mutation_opt m) {
            BOOST_REQUIRE(m);
            BOOST_REQUIRE(m->key().equal(*s, partition_key::from_singular(*s, 0)));
            auto& rows = m->partition().clustered_rows();
            BOOST_REQUIRE_EQUAL(rows.calculate_size(), 1);
            auto& row = rows.begin()->row();
            BOOST_REQUIRE(!row.deleted_at());
            auto& cells = row.cells();
            BOOST_REQUIRE_EQUAL(cells.size(), 1);
            BOOST_REQUIRE_EQUAL(cells.cell_at(s->get_column_definition("e")->id).as_atomic_cell().value(), int32_type->decompose(5));
            return (*reader)();
        }).then([reader, s] (streamed_mutation_opt m) {
            BOOST_REQUIRE(!m);
        });
    });
}

SEASTAR_TEST_CASE(sstable_rewrite) {
    BOOST_REQUIRE(smp::count == 1);
    return test_setup::do_with_test_directory([] {
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", utf8_type}}, {}, utf8_type));

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        auto key_for_this_shard = token_generation_for_current_shard(1);
        auto apply_key = [mt, s, &r1_col] (sstring key_to_write) {
            auto key = partition_key::from_exploded(*s, {to_bytes(key_to_write)});
            auto c_key = clustering_key::from_exploded(*s, {to_bytes("c1")});
            mutation m(key, s);
            m.set_clustered_cell(c_key, r1_col, make_atomic_cell(bytes("a")));
            mt->apply(std::move(m));
        };
        apply_key(key_for_this_shard[0].first);

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 51, la, big);
        return sst->write_components(*mt).then([s, sst] {
            return reusable_sst(s, "tests/sstables/tests-temporary", 51);
        }).then([s, key = key_for_this_shard[0].first] (auto sstp) mutable {
            auto new_tables = make_lw_shared<std::vector<sstables::shared_sstable>>();
            auto creator = [new_tables, s] {
                auto sst = make_lw_shared<sstables::sstable>(s, "tests/sstables/tests-temporary", 52, la, big);
                sst->set_unshared();
                new_tables->emplace_back(sst);
                return sst;
            };
            auto cm = make_lw_shared<compaction_manager>();
            auto cf = make_lw_shared<column_family>(s, column_family::config(), column_family::no_commitlog(), *cm);
            cf->mark_ready_for_writes();
            std::vector<shared_sstable> sstables;
            sstables.push_back(std::move(sstp));

            return sstables::compact_sstables(std::move(sstables), *cf, creator,
                    std::numeric_limits<uint64_t>::max(), 0).then([s, key, new_tables] (auto) {
                BOOST_REQUIRE(new_tables->size() == 1);
                auto newsst = (*new_tables)[0];
                BOOST_REQUIRE(newsst->generation() == 52);
                auto reader = make_lw_shared(sstable_reader(newsst, s));
                return (*reader)().then([s, reader, key] (streamed_mutation_opt m) {
                    BOOST_REQUIRE(m);
                    auto pkey = partition_key::from_exploded(*s, {to_bytes(key)});
                    BOOST_REQUIRE(m->key().equal(*s, pkey));
                    return (*reader)();
                }).then([reader] (streamed_mutation_opt m) {
                    BOOST_REQUIRE(!m);
                });
            }).then([cm, cf] {});
        }).then([sst, mt, s] {});
    });
}

void test_sliced_read_row_presence(shared_sstable sst, schema_ptr s, const query::partition_slice& ps,
    std::vector<std::pair<partition_key, std::vector<clustering_key>>> expected)
{
    auto reader = make_mutation_reader<test_mutation_reader>(sst,
                    sst->read_range_rows(s, query::full_partition_range, ps));

    partition_key::equality pk_eq(*s);
    clustering_key::equality ck_eq(*s);

    auto smopt = reader().get0();
    while (smopt) {
        auto it = std::find_if(expected.begin(), expected.end(), [&] (auto&& x) {
            return pk_eq(x.first, smopt->key());
        });
        BOOST_REQUIRE(it != expected.end());
        auto expected_cr = std::move(it->second);
        expected.erase(it);

        auto mfopt = (*smopt)().get0();
        while (mfopt) {
            if (mfopt->is_clustering_row()) {
                auto& cr = mfopt->as_clustering_row();
                auto it = std::find_if(expected_cr.begin(), expected_cr.end(), [&] (auto&& x) {
                    return ck_eq(x, cr.key());
                });
                if (it == expected_cr.end()) {
                    std::cout << "unexpected clustering row: " << cr.key() << "\n";
                }
                BOOST_REQUIRE(it != expected_cr.end());
                expected_cr.erase(it);
            }
            mfopt = (*smopt)().get0();
        }
        BOOST_REQUIRE(expected_cr.empty());

        smopt = reader().get0();
    }
    BOOST_REQUIRE(expected.empty());
}

SEASTAR_TEST_CASE(test_sliced_mutation_reads) {
    // CREATE TABLE sliced_mutation_reads_test (
    //        pk int,
    //        ck int,
    //        v1 int,
    //        v2 set<int>,
    //        PRIMARY KEY (pk, ck)
    //);
    //
    // insert into sliced_mutation_reads_test (pk, ck, v1) values (0, 0, 1);
    // insert into sliced_mutation_reads_test (pk, ck, v2) values (0, 1, { 0, 1 });
    // update sliced_mutation_reads_test set v1 = 3 where pk = 0 and ck = 2;
    // insert into sliced_mutation_reads_test (pk, ck, v1) values (0, 3, null);
    // insert into sliced_mutation_reads_test (pk, ck, v2) values (0, 4, null);
    // insert into sliced_mutation_reads_test (pk, ck, v1) values (1, 1, 1);
    // insert into sliced_mutation_reads_test (pk, ck, v1) values (1, 3, 1);
    // insert into sliced_mutation_reads_test (pk, ck, v1) values (1, 5, 1);
    return seastar::async([] {
        auto set_of_ints_type = set_type_impl::get_instance(int32_type, true);
        auto builder = schema_builder("ks", "sliced_mutation_reads_test")
            .with_column("pk", int32_type, column_kind::partition_key)
            .with_column("ck", int32_type, column_kind::clustering_key)
            .with_column("v1", int32_type)
            .with_column("v2", set_of_ints_type);
        auto s = builder.build();

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/sliced_mutation_reads", 1, sstables::sstable::version_types::ka, big);
        sst->load().get0();

        {
            auto ps = partition_slice_builder(*s)
                          .with_range(query::clustering_range::make_singular(
                              clustering_key_prefix::from_single_value(*s, int32_type->decompose(0))))
                          .with_range(query::clustering_range::make_singular(
                              clustering_key_prefix::from_single_value(*s, int32_type->decompose(5))))
                          .build();
            test_sliced_read_row_presence(sst, s, ps, {
                std::make_pair(partition_key::from_single_value(*s, int32_type->decompose(0)),
                    std::vector<clustering_key> { clustering_key_prefix::from_single_value(*s, int32_type->decompose(0)) }),
                std::make_pair(partition_key::from_single_value(*s, int32_type->decompose(1)),
                    std::vector<clustering_key> { clustering_key_prefix::from_single_value(*s, int32_type->decompose(5)) }),
            });
        }
        {
            auto ps = partition_slice_builder(*s)
                          .with_range(query::clustering_range {
                             query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(0)) },
                             query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(3)), false },
                          }).build();
            test_sliced_read_row_presence(sst, s, ps, {
                std::make_pair(partition_key::from_single_value(*s, int32_type->decompose(0)),
                    std::vector<clustering_key> {
                        clustering_key_prefix::from_single_value(*s, int32_type->decompose(0)),
                        clustering_key_prefix::from_single_value(*s, int32_type->decompose(1)),
                        clustering_key_prefix::from_single_value(*s, int32_type->decompose(2)),
                    }),
                std::make_pair(partition_key::from_single_value(*s, int32_type->decompose(1)),
                    std::vector<clustering_key> { clustering_key_prefix::from_single_value(*s, int32_type->decompose(1)) }),
            });
        }
        {
            auto ps = partition_slice_builder(*s)
                          .with_range(query::clustering_range {
                             query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(3)) },
                             query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(9)) },
                          }).build();
            test_sliced_read_row_presence(sst, s, ps, {
                std::make_pair(partition_key::from_single_value(*s, int32_type->decompose(0)),
                    std::vector<clustering_key> {
                        clustering_key_prefix::from_single_value(*s, int32_type->decompose(3)),
                        clustering_key_prefix::from_single_value(*s, int32_type->decompose(4)),
                    }),
                std::make_pair(partition_key::from_single_value(*s, int32_type->decompose(1)),
                    std::vector<clustering_key> {
                        clustering_key_prefix::from_single_value(*s, int32_type->decompose(3)),
                        clustering_key_prefix::from_single_value(*s, int32_type->decompose(5)),
                    }),
            });
        }
    });
}

SEASTAR_TEST_CASE(test_wrong_range_tombstone_order) {
    // create table wrong_range_tombstone_order (
    //        p int,
    //        a int,
    //        b int,
    //        c int,
    //        r int,
    //        primary key (p,a,b,c)
    // ) with compact storage;
    //
    // delete from wrong_range_tombstone_order where p = 0 and a = 0;
    // insert into wrong_range_tombstone_order (p,a,r) values (0,1,1);
    // insert into wrong_range_tombstone_order (p,a,b,r) values (0,1,1,2);
    // insert into wrong_range_tombstone_order (p,a,b,r) values (0,1,2,3);
    // insert into wrong_range_tombstone_order (p,a,b,c,r) values (0,1,2,3,4);
    // delete from wrong_range_tombstone_order where p = 0 and a = 1 and b = 3;
    // insert into wrong_range_tombstone_order (p,a,b,r) values (0,1,3,5);
    // insert into wrong_range_tombstone_order (p,a,b,c,r) values (0,1,3,4,6);
    // insert into wrong_range_tombstone_order (p,a,b,r) values (0,1,4,7);
    // delete from wrong_range_tombstone_order where p = 0 and a = 1 and b = 4 and c = 0;
    // delete from wrong_range_tombstone_order where p = 0 and a = 2;
    // delete from wrong_range_tombstone_order where p = 0 and a = 2 and b = 1;
    // delete from wrong_range_tombstone_order where p = 0 and a = 2 and b = 2;

    return seastar::async([] {
        auto s = schema_builder("ks", "wrong_range_tombstone_order")
            .with(schema_builder::compact_storage::yes)
            .with_column("p", int32_type, column_kind::partition_key)
            .with_column("a", int32_type, column_kind::clustering_key)
            .with_column("b", int32_type, column_kind::clustering_key)
            .with_column("c", int32_type, column_kind::clustering_key)
            .with_column("r", int32_type)
            .build();
        clustering_key::equality ck_eq(*s);

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/wrong_range_tombstone_order", 1, sstables::sstable::version_types::ka, big);
        sst->load().get0();
        auto reader = sstable_reader(sst, s);

        auto smopt = reader().get0();
        BOOST_REQUIRE(smopt);
        
        using kind = mutation_fragment::kind;
        assert_that_stream(std::move(*smopt))
            .produces(kind::range_tombstone, { 0 })
            .produces(kind::clustering_row, { 1 })
            .produces(kind::clustering_row, { 1, 1 })
            .produces(kind::clustering_row, { 1, 2 })
            .produces(kind::clustering_row, { 1, 2, 3 })
            .produces(kind::range_tombstone, { 1, 3 })
            .produces(kind::clustering_row, { 1, 3 })
            .produces(kind::clustering_row, { 1, 3, 4 })
            .produces(kind::clustering_row, { 1, 4 })
            .produces(kind::clustering_row, { 1, 4, 0 })
            .produces(kind::range_tombstone, { 2 })
            .produces(kind::range_tombstone, { 2, 1 })
            .produces(kind::range_tombstone, { 2, 1 })
            .produces(kind::range_tombstone, { 2, 2 })
            .produces(kind::range_tombstone, { 2, 2 })
            .produces_end_of_stream();

        smopt = reader().get0();
        BOOST_REQUIRE(!smopt);
    });
}

SEASTAR_TEST_CASE(test_counter_read) {
        // create table counter_test (
        //      pk int,
        //      ck int,
        //      c1 counter,
        //      c2 counter,
        //      primary key (pk, ck)
        // );
        //
        // Node 1:
        // update counter_test set c1 = c1 + 8 where pk = 0 and ck = 0;
        // update counter_test set c2 = c2 - 99 where pk = 0 and ck = 0;
        // update counter_test set c1 = c1 + 3 where pk = 0 and ck = 0;
        // update counter_test set c1 = c1 + 42 where pk = 0 and ck = 1;
        //
        // Node 2:
        // update counter_test set c2 = c2 + 7 where pk = 0 and ck = 0;
        // update counter_test set c1 = c1 + 2 where pk = 0 and ck = 0;
        // delete c1 from counter_test where pk = 0 and ck = 1;
        //
        // select * from counter_test;
        // pk | ck | c1 | c2
        // ----+----+----+-----
        //  0 |  0 | 13 | -92

        using namespace stdx::literals;

        return seastar::async([] {
            auto s = schema_builder("ks", "counter_test")
                    .with_column("pk", int32_type, column_kind::partition_key)
                    .with_column("ck", int32_type, column_kind::clustering_key)
                    .with_column("c1", counter_type)
                    .with_column("c2", counter_type)
                    .build();

            auto node1 = counter_id(utils::UUID("8379ab99-4507-4ab1-805d-ac85a863092b"sv));
            auto node2 = counter_id(utils::UUID("b8a6c3f3-e222-433f-9ce9-de56a8466e07"sv));

            auto sst = make_lw_shared<sstable>(s, "tests/sstables/counter_test", 5, sstables::sstable::version_types::ka, big);
            sst->load().get();
            auto reader = sstable_reader(sst, s);

            auto smopt = reader().get0();
            BOOST_REQUIRE(smopt);
            auto& sm = *smopt;

            auto mfopt = sm().get0();
            BOOST_REQUIRE(mfopt);
            BOOST_REQUIRE(mfopt->is_clustering_row());
            const clustering_row* cr = &mfopt->as_clustering_row();
            cr->cells().for_each_cell([&] (column_id id, const atomic_cell_or_collection& c) {
                counter_cell_view ccv { c.as_atomic_cell() };
                auto& col = s->column_at(column_kind::regular_column, id);
                if (col.name_as_text() == "c1") {
                    BOOST_REQUIRE_EQUAL(ccv.total_value(), 13);
                    BOOST_REQUIRE_EQUAL(ccv.shard_count(), 2);

                    auto it = ccv.shards().begin();
                    auto shard = *it++;
                    BOOST_REQUIRE_EQUAL(shard.id(), node1);
                    BOOST_REQUIRE_EQUAL(shard.value(), 11);
                    BOOST_REQUIRE_EQUAL(shard.logical_clock(), 2);

                    shard = *it++;
                    BOOST_REQUIRE_EQUAL(shard.id(), node2);
                    BOOST_REQUIRE_EQUAL(shard.value(), 2);
                    BOOST_REQUIRE_EQUAL(shard.logical_clock(), 1);
                } else if (col.name_as_text() == "c2") {
                    BOOST_REQUIRE_EQUAL(ccv.total_value(), -92);
                } else {
                    BOOST_FAIL(sprint("Unexpected column \'%s\'", col.name_as_text()));
                }
            });

            mfopt = sm().get0();
            BOOST_REQUIRE(mfopt);
            BOOST_REQUIRE(mfopt->is_clustering_row());
            cr = &mfopt->as_clustering_row();
            cr->cells().for_each_cell([&] (column_id id, const atomic_cell_or_collection& c) {
                auto& col = s->column_at(column_kind::regular_column, id);
                if (col.name_as_text() == "c1") {
                    BOOST_REQUIRE(!c.as_atomic_cell().is_live());
                } else {
                    BOOST_FAIL(sprint("Unexpected column \'%s\'", col.name_as_text()));
                }
            });

            mfopt = sm().get0();
            BOOST_REQUIRE(!mfopt);

            smopt = reader().get0();
            BOOST_REQUIRE(!smopt);
        });
}

SEASTAR_TEST_CASE(test_sstable_max_local_deletion_time) {
    return test_setup::do_with_test_directory([] {
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", utf8_type}}, {}, utf8_type));
        auto mt = make_lw_shared<memtable>(s);
        int32_t last_expiry = 0;

        for (auto i = 0; i < 10; i++) {
            auto key = partition_key::from_exploded(*s, {to_bytes("key" + to_sstring(i))});
            mutation m(key, s);
            auto c_key = clustering_key::from_exploded(*s, {to_bytes("c1")});
            last_expiry = (gc_clock::now() + gc_clock::duration(3600 + i)).time_since_epoch().count();
            m.set_clustered_cell(c_key, *s->get_column_definition("r1"), make_atomic_cell(bytes("a"), 3600 + i, last_expiry));
            mt->apply(std::move(m));
        }
        auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", 53, la, big);
        return sst->write_components(*mt).then([s, sst] {
            return reusable_sst(s, "tests/sstables/tests-temporary", 53);
        }).then([s, last_expiry] (auto sstp) mutable {
            BOOST_REQUIRE(last_expiry == sstp->get_stats_metadata().max_local_deletion_time);
        }).then([sst, mt, s] {});
    });
}

SEASTAR_TEST_CASE(test_sstable_max_local_deletion_time_2) {
    // Create sstable A with 5x column with TTL 100 and 1x column with TTL 1000
    // Create sstable B with tombstone for column in sstable A with TTL 1000.
    // Compact them and expect that maximum deletion time is that of column with TTL 100.
    return test_setup::do_with_test_directory([] {
        return seastar::async([] {
            auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
                {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", utf8_type}}, {}, utf8_type));
            auto cm = make_lw_shared<compaction_manager>();
            auto cf = make_lw_shared<column_family>(s, column_family::config(), column_family::no_commitlog(), *cm);
            auto mt = make_lw_shared<memtable>(s);
            auto now = gc_clock::now();
            int32_t last_expiry = 0;
            auto add_row = [&now, &mt, &s, &last_expiry] (mutation& m, bytes column_name, uint32_t ttl) {
                auto c_key = clustering_key::from_exploded(*s, {column_name});
                last_expiry = (now + gc_clock::duration(ttl)).time_since_epoch().count();
                m.set_clustered_cell(c_key, *s->get_column_definition("r1"), make_atomic_cell(bytes(""), ttl, last_expiry));
                mt->apply(std::move(m));
            };
            auto get_usable_sst = [s] (memtable& mt, int64_t gen) -> future<sstable_ptr> {
                auto sst = make_lw_shared<sstable>(s, "tests/sstables/tests-temporary", gen, la, big);
                return sst->write_components(mt).then([sst, gen, s] {
                    return reusable_sst(s, "tests/sstables/tests-temporary", gen);
                });
            };

            mutation m(partition_key::from_exploded(*s, {to_bytes("deletetest")}), s);
            for (auto i = 0; i < 5; i++) {
                add_row(m, to_bytes("deletecolumn" + to_sstring(i)), 100);
            }
            add_row(m, to_bytes("todelete"), 1000);
            auto sst1 = get_usable_sst(*mt, 54).get0();
            BOOST_REQUIRE(last_expiry == sst1->get_stats_metadata().max_local_deletion_time);

            mt = make_lw_shared<memtable>(s);
            m = mutation(partition_key::from_exploded(*s, {to_bytes("deletetest")}), s);
            tombstone tomb(api::new_timestamp(), now);
            m.partition().apply_delete(*s, clustering_key::from_exploded(*s, {to_bytes("todelete")}), tomb);
            mt->apply(std::move(m));
            auto sst2 = get_usable_sst(*mt, 55).get0();
            BOOST_REQUIRE(now.time_since_epoch().count() == sst2->get_stats_metadata().max_local_deletion_time);

            auto creator = [s] { return make_lw_shared<sstables::sstable>(s, "tests/sstables/tests-temporary", 56, la, big); };
            auto new_sstables = sstables::compact_sstables({ sst1, sst2 }, *cf, creator, std::numeric_limits<uint64_t>::max(), 0).get0();
            BOOST_REQUIRE(new_sstables.size() == 1);
            BOOST_REQUIRE(((now + gc_clock::duration(100)).time_since_epoch().count()) == new_sstables.front()->get_stats_metadata().max_local_deletion_time);
        });
    });
}

static stats_metadata build_stats(int64_t min_timestamp, int64_t max_timestamp, int32_t max_local_deletion_time) {
    stats_metadata stats = {};
    stats.min_timestamp = min_timestamp;
    stats.max_timestamp = max_timestamp;
    stats.max_local_deletion_time = max_local_deletion_time;
    return stats;
}

SEASTAR_TEST_CASE(get_fully_expired_sstables_test) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));
    compaction_manager cm;
    column_family::config cfg;

    auto key_and_token_pair = token_generation_for_current_shard(4);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;

    {
        auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), cm);
        auto sst1 = add_sstable_for_overlapping_test(cf, /*gen*/1, min_key, key_and_token_pair[1].first, build_stats(0, 10, 10));
        auto sst2 = add_sstable_for_overlapping_test(cf, /*gen*/2, min_key, key_and_token_pair[2].first, build_stats(0, 10, std::numeric_limits<int32_t>::max()));
        auto sst3 = add_sstable_for_overlapping_test(cf, /*gen*/3, min_key, max_key, build_stats(20, 25, std::numeric_limits<int32_t>::max()));
        std::vector<sstables::shared_sstable> compacting = { sst1, sst2 };
        auto expired = get_fully_expired_sstables(*cf, compacting, /*gc before*/15);
        BOOST_REQUIRE(expired.size() == 0);
    }

    {
        auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), cm);
        auto sst1 = add_sstable_for_overlapping_test(cf, /*gen*/1, min_key, key_and_token_pair[1].first, build_stats(0, 10, 10));
        auto sst2 = add_sstable_for_overlapping_test(cf, /*gen*/2, min_key, key_and_token_pair[2].first, build_stats(15, 20, std::numeric_limits<int32_t>::max()));
        auto sst3 = add_sstable_for_overlapping_test(cf, /*gen*/3, min_key, max_key, build_stats(30, 40, std::numeric_limits<int32_t>::max()));
        std::vector<sstables::shared_sstable> compacting = { sst1, sst2 };
        auto expired = get_fully_expired_sstables(*cf, compacting, /*gc before*/25);
        BOOST_REQUIRE(expired.size() == 1);
        BOOST_REQUIRE(expired.front()->generation() == 1);
    }

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(basic_date_tiered_strategy_test) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));
    compaction_manager cm;
    column_family::config cfg;
    auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), cm);

    std::vector<sstables::shared_sstable> candidates;
    int min_threshold = cf->schema()->min_compaction_threshold();
    auto now = db_clock::now();
    auto past_hour = now - std::chrono::seconds(3600);
    int64_t timestamp_for_now = now.time_since_epoch().count() * 1000;
    int64_t timestamp_for_past_hour = past_hour.time_since_epoch().count() * 1000;

    for (auto i = 1; i <= min_threshold; i++) {
        auto sst = add_sstable_for_overlapping_test(cf, /*gen*/i, "a", "a",
            build_stats(timestamp_for_now, timestamp_for_now, std::numeric_limits<int32_t>::max()));
        candidates.push_back(sst);
    }
    // add sstable that belong to a different time tier.
    auto sst = add_sstable_for_overlapping_test(cf, /*gen*/min_threshold + 1, "a", "a",
        build_stats(timestamp_for_past_hour, timestamp_for_past_hour, std::numeric_limits<int32_t>::max()));
    candidates.push_back(sst);

    auto gc_before = gc_clock::now() - cf->schema()->gc_grace_seconds();
    std::map<sstring, sstring> options;
    date_tiered_manifest manifest(options);
    auto sstables = manifest.get_next_sstables(*cf, candidates, gc_before);
    BOOST_REQUIRE(sstables.size() == 4);
    for (auto& sst : sstables) {
        BOOST_REQUIRE(sst->generation() != (min_threshold + 1));
    }

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(date_tiered_strategy_test_2) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));
    compaction_manager cm;
    column_family::config cfg;
    auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), cm);

    // deterministic timestamp for Fri, 01 Jan 2016 00:00:00 GMT.
    auto tp = db_clock::from_time_t(1451606400);
    int64_t timestamp = tp.time_since_epoch().count() * 1000; // in microseconds.

    std::vector<sstables::shared_sstable> candidates;
    int min_threshold = cf->schema()->min_compaction_threshold();

    // add sstables that belong to same time window until min threshold is satisfied.
    for (auto i = 1; i <= min_threshold; i++) {
        auto sst = add_sstable_for_overlapping_test(cf, /*gen*/i, "a", "a",
            build_stats(timestamp, timestamp, std::numeric_limits<int32_t>::max()));
        candidates.push_back(sst);
    }
    // belongs to the time window
    auto tp2 = tp + std::chrono::seconds(1800);
    timestamp = tp2.time_since_epoch().count() * 1000;
    auto sst = add_sstable_for_overlapping_test(cf, /*gen*/min_threshold + 1, "a", "a",
        build_stats(timestamp, timestamp, std::numeric_limits<int32_t>::max()));
    candidates.push_back(sst);

    // doesn't belong to the time window above
    auto tp3 = tp + std::chrono::seconds(4000);
    timestamp = tp3.time_since_epoch().count() * 1000;
    auto sst2 = add_sstable_for_overlapping_test(cf, /*gen*/min_threshold + 2, "a", "a",
        build_stats(timestamp, timestamp, std::numeric_limits<int32_t>::max()));
    candidates.push_back(sst2);

    std::map<sstring, sstring> options;
    // Use a 1-hour time window.
    options.emplace(sstring("base_time_seconds"), sstring("3600"));

    date_tiered_manifest manifest(options);
    auto gc_before = gc_clock::time_point(std::chrono::seconds(0)); // disable gc before.
    auto sstables = manifest.get_next_sstables(*cf, candidates, gc_before);
    std::unordered_set<int64_t> gens;
    for (auto sst : sstables) {
        gens.insert(sst->generation());
    }
    BOOST_REQUIRE(sstables.size() == size_t(min_threshold + 1));
    BOOST_REQUIRE(gens.count(min_threshold + 1));
    BOOST_REQUIRE(!gens.count(min_threshold + 2));

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_promoted_index_read) {
    // create table promoted_index_read (
    //        pk int,
    //        ck1 int,
    //        ck2 int,
    //        v int,
    //        primary key (pk, ck1, ck2)
    // );
    //
    // column_index_size_in_kb: 0
    //
    // delete from promoted_index_read where pk = 0 and ck1 = 0;
    // insert into promoted_index_read (pk, ck1, ck2, v) values (0, 0, 0, 0);
    // insert into promoted_index_read (pk, ck1, ck2, v) values (0, 0, 1, 1);
    //
    // SSTable:
    // [
    // {"key": "0",
    //  "cells": [["0:_","0:!",1468923292708929,"t",1468923292],
    //            ["0:_","0:!",1468923292708929,"t",1468923292],
    //            ["0:0:","",1468923308379491],
    //            ["0:_","0:!",1468923292708929,"t",1468923292],
    //            ["0:0:v","0",1468923308379491],
    //            ["0:_","0:!",1468923292708929,"t",1468923292],
    //            ["0:1:","",1468923311744298],
    //            ["0:_","0:!",1468923292708929,"t",1468923292],
    //            ["0:1:v","1",1468923311744298]]}
    // ]

    return seastar::async([] {
        auto s = schema_builder("ks", "promoted_index_read")
                .with_column("pk", int32_type, column_kind::partition_key)
                .with_column("ck1", int32_type, column_kind::clustering_key)
                .with_column("ck2", int32_type, column_kind::clustering_key)
                .with_column("v", int32_type)
                .build();

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/promoted_index_read", 1, sstables::sstable::version_types::ka, big);
        sst->load().get0();

        auto rd = sstable_reader(sst, s);
        auto smopt = rd().get0();
        BOOST_REQUIRE(smopt);

        using kind = mutation_fragment::kind;
        assert_that_stream(std::move(*smopt))
                .produces(kind::range_tombstone, { 0 })
                .produces(kind::clustering_row, { 0, 0 })
                .produces(kind::clustering_row, { 0, 1 })
                .produces_end_of_stream();
    });
}

static void check_min_max_column_names(const sstable_ptr& sst, std::vector<bytes> min_components, std::vector<bytes> max_components) {
    const auto& st = sst->get_stats_metadata();
    BOOST_REQUIRE(st.min_column_names.elements.size() == min_components.size());
    BOOST_REQUIRE(st.min_column_names.elements.size() == st.max_column_names.elements.size());
    for (auto i = 0U; i < st.min_column_names.elements.size(); i++) {
        BOOST_REQUIRE(min_components[i] == st.min_column_names.elements[i].value);
        BOOST_REQUIRE(max_components[i] == st.max_column_names.elements[i].value);
    }
}

static void test_min_max_clustering_key(schema_ptr s, std::vector<bytes> exploded_pk, std::vector<std::vector<bytes>> exploded_cks,
        std::vector<bytes> min_components, std::vector<bytes> max_components, bool remove = false) {
    auto mt = make_lw_shared<memtable>(s);
    auto insert_data = [&mt, &s] (std::vector<bytes>& exploded_pk, std::vector<bytes>&& exploded_ck) {
        const column_definition& r1_col = *s->get_column_definition("r1");
        auto key = partition_key::from_exploded(*s, exploded_pk);
        auto c_key = clustering_key::make_empty();
        if (!exploded_ck.empty()) {
            c_key = clustering_key::from_exploded(*s, exploded_ck);
        }
        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
        mt->apply(std::move(m));
    };
    auto remove_data = [&mt, &s] (std::vector<bytes>& exploded_pk, std::vector<bytes>&& exploded_ck) {
        auto key = partition_key::from_exploded(*s, exploded_pk);
        auto c_key = clustering_key::from_exploded(*s, exploded_ck);
        mutation m(key, s);
        tombstone tomb(api::new_timestamp(), gc_clock::now());
        m.partition().apply_delete(*s, c_key, tomb);
        mt->apply(std::move(m));
    };

    if (exploded_cks.empty()) {
        insert_data(exploded_pk, {});
    } else {
        for (auto& exploded_ck : exploded_cks) {
            if (remove) {
                remove_data(exploded_pk, std::move(exploded_ck));
            } else {
                insert_data(exploded_pk, std::move(exploded_ck));
            }
        }
    }
    auto tmp = make_lw_shared<tmpdir>();
    auto sst = make_lw_shared<sstable>(s, tmp->path, 1, la, big);
    sst->write_components(*mt).get();
    sst = reusable_sst(s, tmp->path, 1).get0();
    check_min_max_column_names(sst, std::move(min_components), std::move(max_components));
}

SEASTAR_TEST_CASE(min_max_clustering_key_test) {
    return seastar::async([] {
        {
            auto s = schema_builder("ks", "cf")
                .with_column("pk", utf8_type, column_kind::partition_key)
                .with_column("ck1", utf8_type, column_kind::clustering_key)
                .with_column("ck2", utf8_type, column_kind::clustering_key)
                .with_column("r1", int32_type)
                .build();
            test_min_max_clustering_key(s, { "key1" }, { { "a", "b" }, { "a", "c" } }, { "a", "b" }, { "a", "c" });
        }
        {
            auto s = schema_builder("ks", "cf")
                .with(schema_builder::compact_storage::yes)
                .with_column("pk", utf8_type, column_kind::partition_key)
                .with_column("ck1", utf8_type, column_kind::clustering_key)
                .with_column("ck2", utf8_type, column_kind::clustering_key)
                .with_column("r1", int32_type)
                .build();
            test_min_max_clustering_key(s, { "key1" }, { { "a", "b" }, { "a", "c" } }, { "a", "b" }, { "a", "c" });
        }
        {
            auto s = schema_builder("ks", "cf")
                .with_column("pk", utf8_type, column_kind::partition_key)
                .with_column("ck1", utf8_type, column_kind::clustering_key)
                .with_column("r1", int32_type)
                .build();
            test_min_max_clustering_key(s, { "key1" }, { { "a" }, { "z" } }, { "a" }, { "z" });
        }
        {
            auto s = schema_builder("ks", "cf")
                .with_column("pk", utf8_type, column_kind::partition_key)
                .with_column("ck1", utf8_type, column_kind::clustering_key)
                .with_column("r1", int32_type)
                .build();
            test_min_max_clustering_key(s, { "key1" }, { { "a" }, { "z" } }, { "a" }, { "z" }, true);
        }
        {
            auto s = schema_builder("ks", "cf")
                .with_column("pk", utf8_type, column_kind::partition_key)
                .with_column("r1", int32_type)
                .build();
            test_min_max_clustering_key(s, { "key1" }, {}, {}, {});
        }
    });
}

SEASTAR_TEST_CASE(min_max_clustering_key_test_2) {
    return seastar::async([] {
        auto s = schema_builder("ks", "cf")
            .with_column("pk", utf8_type, column_kind::partition_key)
            .with_column("ck1", utf8_type, column_kind::clustering_key)
            .with_column("r1", int32_type)
            .build();
        auto cm = make_lw_shared<compaction_manager>();
        auto cf = make_lw_shared<column_family>(s, column_family::config(), column_family::no_commitlog(), *cm);
        auto tmp = make_lw_shared<tmpdir>();
        auto mt = make_lw_shared<memtable>(s);
        const column_definition& r1_col = *s->get_column_definition("r1");

        for (auto j = 0; j < 8; j++) {
            auto key = partition_key::from_exploded(*s, {to_bytes("key" + to_sstring(j))});
            mutation m(key, s);
            for (auto i = 100; i < 150; i++) {
                auto c_key = clustering_key::from_exploded(*s, {to_bytes(to_sstring(j) + "ck" + to_sstring(i))});
                m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
            }
            mt->apply(std::move(m));
        }
        auto sst = make_lw_shared<sstable>(s, tmp->path, 1, la, big);
        sst->write_components(*mt).get();
        sst = reusable_sst(s, tmp->path, 1).get0();
        check_min_max_column_names(sst, { "0ck100" }, { "7ck149" });

        mt = make_lw_shared<memtable>(s);
        auto key = partition_key::from_exploded(*s, {to_bytes("key9")});
        mutation m(key, s);
        for (auto i = 101; i < 299; i++) {
            auto c_key = clustering_key::from_exploded(*s, {to_bytes(to_sstring(9) + "ck" + to_sstring(i))});
            m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
        }
        mt->apply(std::move(m));
        auto sst2 = make_lw_shared<sstable>(s, tmp->path, 2, la, big);
        sst2->write_components(*mt).get();
        sst2 = reusable_sst(s, tmp->path, 2).get0();
        check_min_max_column_names(sst2, { "9ck101" }, { "9ck298" });

        auto creator = [s, tmp] { return make_lw_shared<sstables::sstable>(s, tmp->path, 3, la, big); };
        auto new_sstables = sstables::compact_sstables({ sst, sst2 }, *cf, creator, std::numeric_limits<uint64_t>::max(), 0).get0();
        BOOST_REQUIRE(new_sstables.size() == 1);
        check_min_max_column_names(new_sstables.front(), { "0ck100" }, { "9ck298" });
    });
}

SEASTAR_TEST_CASE(sstable_tombstone_metadata_check) {
    return seastar::async([] {
        auto s = schema_builder("ks", "cf")
            .with_column("pk", utf8_type, column_kind::partition_key)
            .with_column("ck1", utf8_type, column_kind::clustering_key)
            .with_column("r1", int32_type)
            .build();
        auto tmp = make_lw_shared<tmpdir>();
        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = exploded_clustering_prefix({to_bytes("c1") });
        const column_definition& r1_col = *s->get_column_definition("r1");

        {
            auto mt = make_lw_shared<memtable>(s);
            mutation m(key, s);
            tombstone tomb(api::new_timestamp(), gc_clock::now());
            m.partition().apply_delete(*s, c_key, tomb);
            mt->apply(std::move(m));
            auto sst = make_lw_shared<sstable>(s, tmp->path, 1, la, big);
            sst->write_components(*mt).get();
            sst = reusable_sst(s, tmp->path, 1).get0();
            BOOST_REQUIRE(sst->get_stats_metadata().estimated_tombstone_drop_time.bin.map.size());
        }

        {
            auto mt = make_lw_shared<memtable>(s);
            mutation m(key, s);
            m.set_clustered_cell(c_key, r1_col, make_dead_atomic_cell(3600));
            mt->apply(std::move(m));
            auto sst = make_lw_shared<sstable>(s, tmp->path, 2, la, big);
            sst->write_components(*mt).get();
            sst = reusable_sst(s, tmp->path, 2).get0();
            BOOST_REQUIRE(sst->get_stats_metadata().estimated_tombstone_drop_time.bin.map.size());
        }

        {
            auto mt = make_lw_shared<memtable>(s);
            mutation m(key, s);
            m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
            mt->apply(std::move(m));
            auto sst = make_lw_shared<sstable>(s, tmp->path, 3, la, big);
            sst->write_components(*mt).get();
            sst = reusable_sst(s, tmp->path, 3).get0();
            BOOST_REQUIRE(!sst->get_stats_metadata().estimated_tombstone_drop_time.bin.map.size());
        }

        {
            auto mt = make_lw_shared<memtable>(s);

            mutation m(key, s);
            tombstone tomb(api::new_timestamp(), gc_clock::now());
            m.partition().apply_delete(*s, c_key, tomb);
            mt->apply(std::move(m));

            auto key2 = partition_key::from_exploded(*s, {to_bytes("key2")});
            mutation m2(key2, s);
            m2.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(1)));
            mt->apply(std::move(m2));

            auto sst = make_lw_shared<sstable>(s, tmp->path, 4, la, big);
            sst->write_components(*mt).get();
            sst = reusable_sst(s, tmp->path, 4).get0();
            BOOST_REQUIRE(sst->get_stats_metadata().estimated_tombstone_drop_time.bin.map.size());
        }

        {
            auto mt = make_lw_shared<memtable>(s);
            mutation m(key, s);
            tombstone tomb(api::new_timestamp(), gc_clock::now());
            m.partition().apply(tomb);
            mt->apply(std::move(m));
            auto sst = make_lw_shared<sstable>(s, tmp->path, 5, la, big);
            sst->write_components(*mt).get();
            sst = reusable_sst(s, tmp->path, 5).get0();
            BOOST_REQUIRE(sst->get_stats_metadata().estimated_tombstone_drop_time.bin.map.size());
        }

        {
            auto mt = make_lw_shared<memtable>(s);
            mutation m(key, s);
            tombstone tomb(api::new_timestamp(), gc_clock::now());
            range_tombstone rt(clustering_key_prefix::from_single_value(*s, bytes("a")), clustering_key_prefix::from_single_value(*s, bytes("a")), tomb);
            m.partition().apply_delete(*s, std::move(rt));
            mt->apply(std::move(m));
            auto sst = make_lw_shared<sstable>(s, tmp->path, 6, la, big);
            sst->write_components(*mt).get();
            sst = reusable_sst(s, tmp->path, 6).get0();
            BOOST_REQUIRE(sst->get_stats_metadata().estimated_tombstone_drop_time.bin.map.size());
        }
    });
}

SEASTAR_TEST_CASE(test_partition_skipping) {
    return seastar::async([] {
        auto s = schema_builder("ks", "test_skipping_partitions")
                .with_column("pk", int32_type, column_kind::partition_key)
                .with_column("v", int32_type)
                .build();

        auto sst = make_lw_shared<sstable>(s, "tests/sstables/partition_skipping", 1, sstables::sstable::version_types::ka, big);
        sst->load().get0();

        std::vector<dht::decorated_key> keys;
        for (int i = 0; i < 10; i++) {
            auto pk = partition_key::from_single_value(*s, int32_type->decompose(i));
            keys.emplace_back(dht::global_partitioner().decorate_key(*s, std::move(pk)));
        }
        dht::decorated_key::less_comparator cmp(s);
        std::sort(keys.begin(), keys.end(), cmp);

        assert_that(sstable_reader(sst, s)).produces(keys);

        auto pr = dht::partition_range::make(dht::ring_position(keys[0]), dht::ring_position(keys[1]));
        assert_that(sstable_reader(sst, s, pr))
            .produces(keys[0])
            .produces(keys[1])
            .produces_end_of_stream()
            .fast_forward_to(dht::partition_range::make_starting_with(dht::ring_position(keys[8])))
            .produces(keys[8])
            .produces(keys[9])
            .produces_end_of_stream();

        pr = dht::partition_range::make(dht::ring_position(keys[1]), dht::ring_position(keys[1]));
        assert_that(sstable_reader(sst, s, pr))
            .produces(keys[1])
            .produces_end_of_stream()
            .fast_forward_to(dht::partition_range::make(dht::ring_position(keys[3]), dht::ring_position(keys[4])))
            .produces(keys[3])
            .produces(keys[4])
            .produces_end_of_stream()
            .fast_forward_to(dht::partition_range::make({ dht::ring_position(keys[4]), false }, dht::ring_position(keys[5])))
            .produces(keys[5])
            .produces_end_of_stream()
            .fast_forward_to(dht::partition_range::make(dht::ring_position(keys[6]), dht::ring_position(keys[6])))
            .produces(keys[6])
            .produces_end_of_stream()
            .fast_forward_to(dht::partition_range::make(dht::ring_position(keys[7]), dht::ring_position(keys[8])))
            .produces(keys[7])
            .fast_forward_to(dht::partition_range::make(dht::ring_position(keys[9]), dht::ring_position(keys[9])))
            .produces(keys[9])
            .produces_end_of_stream();

        pr = dht::partition_range::make({ dht::ring_position(keys[0]), false }, { dht::ring_position(keys[1]), false});
        assert_that(sstable_reader(sst, s, pr))
            .produces_end_of_stream()
            .fast_forward_to(dht::partition_range::make(dht::ring_position(keys[6]), dht::ring_position(keys[6])))
            .produces(keys[6])
            .produces_end_of_stream()
            .fast_forward_to(dht::partition_range::make({ dht::ring_position(keys[8]), false }, { dht::ring_position(keys[9]), false }))
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_unknown_component) {
    return seastar::async([] {
        auto tmp = make_lw_shared<tmpdir>();
        auto sstp = reusable_sst(uncompressed_schema(), "tests/sstables/unknown_component", 1).get0();
        sstp->create_links(tmp->path).get();
        // check that create_links() moved unknown component to new dir
        BOOST_REQUIRE(file_exists(tmp->path + "/la-1-big-UNKNOWN.txt").get0());

        sstp = reusable_sst(uncompressed_schema(), tmp->path, 1).get0();
        sstp->set_generation(2).get();
        BOOST_REQUIRE(!file_exists(tmp->path +  "/la-1-big-UNKNOWN.txt").get0());
        BOOST_REQUIRE(file_exists(tmp->path + "/la-2-big-UNKNOWN.txt").get0());

        sstables::delete_atomically({sstp}).get();
        // assure unknown component is deleted
        BOOST_REQUIRE(!file_exists(tmp->path + "/la-2-big-UNKNOWN.txt").get0());
    });
}

SEASTAR_TEST_CASE(size_tiered_beyond_max_threshold_test) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));
    auto cm = make_lw_shared<compaction_manager>();
    auto cf = make_lw_shared<column_family>(s, column_family::config(), column_family::no_commitlog(), *cm);
    auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::size_tiered, s->compaction_strategy_options());

    std::vector<sstables::shared_sstable> candidates;
    int max_threshold = cf->schema()->max_compaction_threshold();
    candidates.reserve(max_threshold+1);
    for (auto i = 0; i < (max_threshold+1); i++) { // (max_threshold+1) sstables of similar size
        auto sst = make_lw_shared<sstable>(s, "", i, la, big);
        sstables::test(sst).set_data_file_size(1);
        candidates.push_back(std::move(sst));
    }
    auto desc = cs.get_sstables_for_compaction(*cf, std::move(candidates));
    BOOST_REQUIRE(desc.sstables.size() == size_t(max_threshold));
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(sstable_set_incremental_selector) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));
    auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::leveled, s->compaction_strategy_options());
    auto key_and_token_pair = token_generation_for_current_shard(8);

    sstable_set set = cs.make_sstable_set(s);
    set.insert(sstable_for_overlapping_test(s, 1, key_and_token_pair[0].first, key_and_token_pair[1].first));
    set.insert(sstable_for_overlapping_test(s, 2, key_and_token_pair[0].first, key_and_token_pair[1].first));
    set.insert(sstable_for_overlapping_test(s, 3, key_and_token_pair[3].first, key_and_token_pair[4].first));
    set.insert(sstable_for_overlapping_test(s, 4, key_and_token_pair[4].first, key_and_token_pair[4].first));
    set.insert(sstable_for_overlapping_test(s, 5, key_and_token_pair[4].first, key_and_token_pair[5].first));

    sstable_set::incremental_selector selector = set.make_incremental_selector();
    auto check = [&selector] (const dht::token& token, std::unordered_set<int64_t> expected_gens) {
        auto sstables = selector.select(token);
        BOOST_REQUIRE(sstables.size() == expected_gens.size());
        for (auto& sst : sstables) {
            BOOST_REQUIRE(expected_gens.count(sst->generation()) == 1);
        }
    };
    check(key_and_token_pair[0].second, {1, 2});
    check(key_and_token_pair[1].second, {1, 2});
    check(key_and_token_pair[2].second, {});
    check(key_and_token_pair[3].second, {3});
    check(key_and_token_pair[4].second, {3, 4, 5});
    check(key_and_token_pair[5].second, {5});
    check(key_and_token_pair[6].second, {});
    check(key_and_token_pair[7].second, {});

    return make_ready_future<>();
}
