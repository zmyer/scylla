/*
 * Copyright 2016 ScyllaDB
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

#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>

#include "tests/test-utils.hh"

#include <map>
#include <vector>
#include <experimental/optional>

#include "bytes.hh"
#include "bytes_ostream.hh"
#include "serializer.hh"

#include "disk-error-handler.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

namespace stdx = std::experimental;

struct simple_compound {
    // TODO: change this to test for #905
    uint32_t foo;
    uint32_t bar;

    bool operator==(const simple_compound& other) const {
        return foo == other.foo && bar == other.bar;
    }
};

class non_final_composite_test_object {
    simple_compound _x;
public:
    static thread_local int construction_count;
    non_final_composite_test_object(simple_compound x) : _x(x) {
        ++construction_count;
    }
    simple_compound x() const { return _x; }
};

class final_composite_test_object {
    simple_compound _x;
public:
    static thread_local int construction_count;
    final_composite_test_object(simple_compound x) : _x(x) {
        ++construction_count;
    }
    simple_compound x() const { return _x; }
};

thread_local int non_final_composite_test_object::construction_count = 0;
thread_local int final_composite_test_object::construction_count = 0;

std::ostream& operator<<(std::ostream& os, const simple_compound& sc)
{
    return os << " { foo: " << sc.foo << ", bar: " << sc.bar << " }";
}

struct compound_with_optional {
    stdx::optional<simple_compound> first;
    simple_compound second;

    bool operator==(const compound_with_optional& other) const {
        return first == other.first && second == other.second;
    }
};

std::ostream& operator<<(std::ostream& os, const compound_with_optional& v)
{
    os << " { first: ";
    if (v.first) {
        os << *v.first;
    } else {
        os << "<disengaged>";
    }
    os << ", second: " << v.second << " }";
    return os;
}

struct wrapped_vector {
    std::vector<simple_compound> vector;

    bool operator==(const wrapped_vector& v) const { // = default;
        return vector == v.vector;
    }
};

std::ostream& operator<<(std::ostream& os, const wrapped_vector& v)
{
    return os << v.vector;
}

struct vectors_of_compounds {
    std::vector<simple_compound> first;
    wrapped_vector second;
};

#include "serialization_visitors.hh"
#include "idl/idl_test.dist.hh"
#include "serializer_impl.hh"
#include "idl/idl_test.dist.impl.hh"

BOOST_AUTO_TEST_CASE(test_simple_compound)
{
    simple_compound sc = { 0xdeadbeef, 0xbadc0ffe };

    bytes_ostream buf1;
    ser::serialize(buf1, sc);
    BOOST_REQUIRE_EQUAL(buf1.size(), 12);

    bytes_ostream buf2;
    ser::writer_of_writable_simple_compound<bytes_ostream> wowsc(buf2);
    std::move(wowsc).write_foo(sc.foo).write_bar(sc.bar).end_writable_simple_compound();
    BOOST_REQUIRE_EQUAL(buf1.linearize(), buf2.linearize());

    auto bv1 = buf1.linearize();
    auto in1 = ser::as_input_stream(bv1);
    auto deser_sc = ser::deserialize(in1, boost::type<simple_compound>());
    BOOST_REQUIRE_EQUAL(sc, deser_sc);

    auto bv2 = buf2.linearize();
    auto in2 = ser::as_input_stream(bv2);
    auto sc_view = ser::deserialize(in2, boost::type<ser::writable_simple_compound_view>());
    BOOST_REQUIRE_EQUAL(sc.foo, sc_view.foo());
    BOOST_REQUIRE_EQUAL(sc.bar, sc_view.bar());
}

BOOST_AUTO_TEST_CASE(test_vector)
{
    std::vector<simple_compound> vec1 = {
        { 1, 2 },
        { 3, 4 },
        { 5, 6 },
        { 7, 8 },
        { 9, 10 },
    };
    std::vector<simple_compound> vec2 = {
        { 11, 12 },
        { 13, 14 },
        { 15, 16 },
        { 17, 18 },
        { 19, 20 },
    };
    vectors_of_compounds voc = { vec1, wrapped_vector { vec2 } };

    bytes_ostream buf1;
    ser::serialize(buf1, voc);
    BOOST_REQUIRE_EQUAL(buf1.size(), 136);

    bytes_ostream buf2;
    ser::writer_of_writable_vectors_of_compounds<bytes_ostream> wowvoc(buf2);
    auto first_writer = std::move(wowvoc).start_first();
    for (auto& c : vec1) {
        first_writer.add().write_foo(c.foo).write_bar(c.bar).end_writable_simple_compound();
    }
    auto second_writer = std::move(first_writer).end_first().start_second().start_vector();
    for (auto& c : vec2) {
        second_writer.add_vector(c);
    }
    std::move(second_writer).end_vector().end_second().end_writable_vectors_of_compounds();
    BOOST_REQUIRE_EQUAL(buf1.linearize(), buf2.linearize());

    auto bv1 = buf1.linearize();
    auto in1 = ser::as_input_stream(bv1);
    auto deser_voc = ser::deserialize(in1, boost::type<vectors_of_compounds>());
    BOOST_REQUIRE_EQUAL(voc.first, deser_voc.first);
    BOOST_REQUIRE_EQUAL(voc.second, deser_voc.second);

    auto bv2 = buf2.linearize();
    auto in2 = ser::as_input_stream(bv2);
    auto voc_view = ser::deserialize(in2, boost::type<ser::writable_vectors_of_compounds_view>());
    auto&& first_view = voc_view.first();
    BOOST_REQUIRE_EQUAL(vec1.size(), first_view.size());
    for (size_t i = 0; i < first_view.size(); i++) {
        auto fv = first_view[i];
        assert(vec1[i].foo == fv.foo());
        BOOST_REQUIRE_EQUAL(vec1[i].foo, first_view[i].foo());
        BOOST_REQUIRE_EQUAL(vec1[i].bar, first_view[i].bar());
    }

    auto&& second_view = voc_view.second().vector();
    BOOST_REQUIRE_EQUAL(vec2.size(), second_view.size());
    for (size_t i = 0; i < second_view.size(); i++) {
        BOOST_REQUIRE_EQUAL(vec2[i], second_view[i]);
    }
}

BOOST_AUTO_TEST_CASE(test_variant)
{
    std::vector<simple_compound> vec = {
        { 1, 2 },
        { 3, 4 },
        { 5, 6 },
        { 7, 8 },
        { 9, 10 },
    };

    simple_compound sc = { 0xdeadbeef, 0xbadc0ffe };
    simple_compound sc2 = { 0x12344321, 0x56788765 };

    bytes_ostream buf;
    ser::writer_of_writable_variants<bytes_ostream> wowv(buf);
    auto second_writer = std::move(wowv).write_id(17).write_first_simple_compound(sc).start_second_writable_vector().start_vector();
    for (auto&& v : vec) {
        second_writer.add_vector(v);
    }
    auto third_writer = std::move(second_writer).end_vector().end_writable_vector().start_third_writable_final_simple_compound();
    std::move(third_writer).write_foo(sc2.foo).write_bar(sc2.bar).end_writable_final_simple_compound().end_writable_variants();
    BOOST_REQUIRE_EQUAL(buf.size(), 120);

    auto bv = buf.linearize();
    auto in = ser::as_input_stream(bv);
    auto wv_view = ser::deserialize(in, boost::type<ser::writable_variants_view>());
    BOOST_REQUIRE_EQUAL(wv_view.id(), 17);

    struct expect_compound : boost::static_visitor<simple_compound> {
        simple_compound operator()(ser::writable_vector_view&) const {
            throw std::runtime_error("got writable_vector, expected simple_compound");
        }
        simple_compound operator()(simple_compound& sc) const {
            return sc;
        }
        simple_compound operator()(ser::writable_final_simple_compound_view&) const {
            throw std::runtime_error("got writable_final_simple_compound, expected simple_compound");
        }
        simple_compound operator()(ser::unknown_variant_type&) const {
            throw std::runtime_error("unknown type, expected simple_compound");
        }
    };
    auto v1 = wv_view.first();
    auto&& compound = boost::apply_visitor(expect_compound(), v1);
    BOOST_REQUIRE_EQUAL(compound, sc);

    struct expect_vector : boost::static_visitor<std::vector<simple_compound>> {
        std::vector<simple_compound> operator()(ser::writable_vector_view& wvv) const {
            return wvv.vector();
        }
        std::vector<simple_compound> operator()(simple_compound&) const {
            throw std::runtime_error("got simple_compound, expected writable_vector");
        }
        std::vector<simple_compound> operator()(ser::writable_final_simple_compound_view&) const {
            throw std::runtime_error("got writable_final_simple_compound, expected writable_vector");
        }
        std::vector<simple_compound> operator()(ser::unknown_variant_type&) const {
            throw std::runtime_error("unknown type, expected writable_vector");
        }
    };

    auto v2 = wv_view.second();
    auto&& vector = boost::apply_visitor(expect_vector(), v2);
    BOOST_REQUIRE_EQUAL(vector, vec);

    struct expect_writable_compound : boost::static_visitor<simple_compound> {
        simple_compound operator()(ser::writable_vector_view&) const {
            throw std::runtime_error("got writable_vector, expected writable_final_simple_compound");
        }
        simple_compound operator()(simple_compound&) const {
            throw std::runtime_error("got simple_compound, expected writable_final_simple_compound");
        }
        simple_compound operator()(ser::writable_final_simple_compound_view& scv) const {
            return simple_compound { scv.foo(), scv.bar() };
        }
        simple_compound operator()(ser::unknown_variant_type&) const {
            throw std::runtime_error("unknown type, expected writable_final_simple_compound");
        }
    };
    auto v3 = wv_view.third();
    auto&& compound2 = boost::apply_visitor(expect_writable_compound(), v3);
    BOOST_REQUIRE_EQUAL(compound2, sc2);
}

BOOST_AUTO_TEST_CASE(test_compound_with_optional)
{
    simple_compound foo = { 0xdeadbeef, 0xbadc0ffe };
    simple_compound bar = { 0x12345678, 0x87654321 };

    compound_with_optional one = { foo, bar };

    bytes_ostream buf1;
    ser::serialize(buf1, one);
    BOOST_REQUIRE_EQUAL(buf1.size(), 29);

    auto bv1 = buf1.linearize();
    seastar::simple_input_stream in1(reinterpret_cast<const char*>(bv1.data()), bv1.size());
    auto deser_one = ser::deserialize(in1, boost::type<compound_with_optional>());
    BOOST_REQUIRE_EQUAL(one, deser_one);

    compound_with_optional two = { {}, foo };

    bytes_ostream buf2;
    ser::serialize(buf2, two);
    BOOST_REQUIRE_EQUAL(buf2.size(), 17);

    auto bv2 = buf2.linearize();
    seastar::simple_input_stream in2(reinterpret_cast<const char*>(bv2.data()), bv2.size());
    auto deser_two = ser::deserialize(in2, boost::type<compound_with_optional>());
    BOOST_REQUIRE_EQUAL(two, deser_two);
}

BOOST_AUTO_TEST_CASE(test_skip_does_not_deserialize)
{
    {
        non_final_composite_test_object x({1, 2});

        bytes_ostream buf;
        ser::serialize(buf, x);

        auto in = ser::as_input_stream(buf.linearize());
        auto prev = non_final_composite_test_object::construction_count;

        ser::skip(in, boost::type<non_final_composite_test_object>());

        BOOST_REQUIRE(prev == non_final_composite_test_object::construction_count);
    }

    {
        final_composite_test_object x({1, 2});

        bytes_ostream buf;
        ser::serialize(buf, x);

        auto in = ser::as_input_stream(buf.linearize());
        auto prev = final_composite_test_object::construction_count;

        ser::skip(in, boost::type<final_composite_test_object>());

        BOOST_REQUIRE(prev == final_composite_test_object::construction_count);
    }
}
