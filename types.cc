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

#include <boost/lexical_cast.hpp>
#include <algorithm>
#include "cql3/cql3_type.hh"
#include "cql3/lists.hh"
#include "cql3/maps.hh"
#include "cql3/sets.hh"
#include "types.hh"
#include "core/print.hh"
#include "net/ip.hh"
#include "database.hh"
#include "utils/serialization.hh"
#include "combine.hh"
#include <cmath>
#include <chrono>
#include <sstream>
#include <string>
#include <regex>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/numeric.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/c_local_time_adjustor.hpp>
#include <boost/locale/encoding_utf.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include "utils/big_decimal.hh"
#include "utils/date.h"

template<typename T>
sstring time_point_to_string(const T& tp)
{
    auto timestamp = tp.time_since_epoch().count();
    auto time = boost::posix_time::from_time_t(0) + boost::posix_time::milliseconds(timestamp);
    return boost::posix_time::to_iso_extended_string(time);
}

static const char* byte_type_name      = "org.apache.cassandra.db.marshal.ByteType";
static const char* short_type_name     = "org.apache.cassandra.db.marshal.ShortType";
static const char* int32_type_name     = "org.apache.cassandra.db.marshal.Int32Type";
static const char* long_type_name      = "org.apache.cassandra.db.marshal.LongType";
static const char* ascii_type_name     = "org.apache.cassandra.db.marshal.AsciiType";
static const char* utf8_type_name      = "org.apache.cassandra.db.marshal.UTF8Type";
static const char* bytes_type_name     = "org.apache.cassandra.db.marshal.BytesType";
static const char* boolean_type_name   = "org.apache.cassandra.db.marshal.BooleanType";
static const char* timeuuid_type_name  = "org.apache.cassandra.db.marshal.TimeUUIDType";
static const char* timestamp_type_name = "org.apache.cassandra.db.marshal.TimestampType";
static const char* date_type_name      = "org.apache.cassandra.db.marshal.DateType";
static const char* simple_date_type_name = "org.apache.cassandra.db.marshal.SimpleDateType";
static const char* time_type_name      = "org.apache.cassandra.db.marshal.TimeType";
static const char* uuid_type_name      = "org.apache.cassandra.db.marshal.UUIDType";
static const char* inet_addr_type_name = "org.apache.cassandra.db.marshal.InetAddressType";
static const char* double_type_name    = "org.apache.cassandra.db.marshal.DoubleType";
static const char* float_type_name     = "org.apache.cassandra.db.marshal.FloatType";
static const char* varint_type_name    = "org.apache.cassandra.db.marshal.IntegerType";
static const char* decimal_type_name    = "org.apache.cassandra.db.marshal.DecimalType";
static const char* counter_type_name   = "org.apache.cassandra.db.marshal.CounterColumnType";
static const char* empty_type_name     = "org.apache.cassandra.db.marshal.EmptyType";

template<typename T>
struct simple_type_traits {
    static T read_nonempty(bytes_view v) {
        return read_simple_exactly<T>(v);
    }
};

template<>
struct simple_type_traits<bool> {
    static bool read_nonempty(bytes_view v) {
        return read_simple_exactly<int8_t>(v) != 0;
    }
};

template<>
struct simple_type_traits<db_clock::time_point> {
    static db_clock::time_point read_nonempty(bytes_view v) {
        return db_clock::time_point(db_clock::duration(read_simple_exactly<int64_t>(v)));
    }
};

template <typename T>
struct simple_type_impl : concrete_type<T> {
    simple_type_impl(sstring name) : concrete_type<T>(std::move(name)) {}
    virtual int32_t compare(bytes_view v1, bytes_view v2) const override {
        if (v1.empty()) {
            return v2.empty() ? 0 : -1;
        }
        if (v2.empty()) {
            return 1;
        }
        T a = simple_type_traits<T>::read_nonempty(v1);
        T b = simple_type_traits<T>::read_nonempty(v2);
        return a == b ? 0 : a < b ? -1 : 1;
    }
    virtual bool less(bytes_view v1, bytes_view v2) const override {
        return compare(v1, v2) < 0;
    }
    virtual bool is_byte_order_equal() const override {
        return true;
    }
    virtual size_t hash(bytes_view v) const override {
        return std::hash<bytes_view>()(v);
    }
    virtual bool references_user_type(const sstring& keyspace, const bytes& name) const {
        return false;
    }
    virtual std::experimental::optional<data_type> update_user_type(const shared_ptr<const user_type_impl> updated) const {
        return std::experimental::nullopt;
    }
};

template<typename T>
struct integer_type_impl : simple_type_impl<T> {
    integer_type_impl(sstring name) : simple_type_impl<T>(name) {}
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        auto v1 = this->from_value(value);
        if (v1.empty()) {
            return;
        }
        auto v = v1.get();
        auto u = net::hton(v);
        out = std::copy_n(reinterpret_cast<const char*>(&u), sizeof(u), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value) {
            return 0;
        }
        auto v = this->from_value(value);
        if (v.empty()) {
            return 0;
        }
        return sizeof(v.get());
    }
    virtual data_value deserialize(bytes_view v) const override {
        auto x = read_simple_opt<T>(v);
        if (!x) {
            return this->make_empty();
        } else {
            return this->make_value(*x);
        }
    }
    T compose_value(const bytes& b) const {
        if (b.size() != sizeof(T)) {
            throw marshal_exception();
        }
        return (T)net::ntoh(*reinterpret_cast<const T*>(b.begin()));
    }
    bytes decompose_value(T v) const {
        bytes b(bytes::initialized_later(), sizeof(v));
        *reinterpret_cast<T*>(b.begin()) = (T)net::hton(v);
        return b;
    }
    virtual void validate(bytes_view v) const override {
        if (v.size() != 0 && v.size() != sizeof(T)) {
            throw marshal_exception();
        }
    }
    T parse_int(sstring_view s) const {
        try {
            auto value64 = boost::lexical_cast<int64_t>(s.begin(), s.size());
            auto value = static_cast<T>(value64);
            if (value != value64) {
                throw marshal_exception(sprint("Value out of range for type %s: '%s'", this->name(), s));
            }
            return static_cast<T>(value);
        } catch (const boost::bad_lexical_cast& e) {
            throw marshal_exception(sprint("Invalid number format '%s'", s));
        }
    }
    virtual bytes from_string(sstring_view s) const override {
        return decompose_value(parse_int(s));
    }
    virtual sstring to_string(const bytes& b) const override {
        if (b.empty()) {
            return {};
        }
        return to_sstring(compose_value(b));
    }
};

struct byte_type_impl : integer_type_impl<int8_t> {
    byte_type_impl() : integer_type_impl{byte_type_name}
    { }

    virtual void validate(bytes_view v) const override {
        if (v.size() != 0 && v.size() != 1) {
            throw marshal_exception(sprint("Expected 1 byte for a tinyint (%d)", v.size()));
        }
    }

    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::tinyint;
    }
};

struct short_type_impl : integer_type_impl<int16_t> {
    short_type_impl() : integer_type_impl{short_type_name}
    { }

    virtual void validate(bytes_view v) const override {
        if (v.size() != 0 && v.size() != 2) {
            throw marshal_exception(sprint("Expected 2 bytes for a smallint (%d)", v.size()));
        }
    }

    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::smallint;
    }
};

struct int32_type_impl : integer_type_impl<int32_t> {
    int32_type_impl() : integer_type_impl{int32_type_name}
    { }

    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::int_;
    }
};

struct long_type_impl : integer_type_impl<int64_t> {
    long_type_impl() : integer_type_impl{long_type_name}
    { }

    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::bigint;
    }
    virtual bool is_value_compatible_with_internal(const abstract_type& other) const override {
        return &other == this || &other == date_type.get() || &other == timestamp_type.get();
    }
};

struct string_type_impl : public concrete_type<sstring> {
    string_type_impl(sstring name)
        : concrete_type(name) {}
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        auto& v = from_value(value);
        out = std::copy(v.begin(), v.end(), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value) {
            return 0;
        }
        auto& v = from_value(value);
        return v.size();
    }
    virtual data_value deserialize(bytes_view v) const override {
        // FIXME: validation?
        return make_value(std::make_unique<native_type>(reinterpret_cast<const char*>(v.begin()), v.size()));
    }
    virtual bool less(bytes_view v1, bytes_view v2) const override {
        return less_unsigned(v1, v2);
    }
    virtual bool is_byte_order_equal() const override {
        return true;
    }
    virtual bool is_byte_order_comparable() const override {
        return true;
    }
    virtual size_t hash(bytes_view v) const override {
        return std::hash<bytes_view>()(v);
    }
    virtual void validate(bytes_view v) const override {
        if (as_cql3_type() == cql3::cql3_type::ascii) {
            if (std::any_of(v.begin(), v.end(), [] (int8_t b) { return b < 0; })) {
                throw marshal_exception();
            }
        } else {
            try {
                boost::locale::conv::utf_to_utf<char>(v.begin(), v.end(), boost::locale::conv::stop);
            } catch (const boost::locale::conv::conversion_error& ex) {
                throw marshal_exception(ex.what());
            }
        }
    }
    virtual bytes from_string(sstring_view s) const override {
        return to_bytes(bytes_view(reinterpret_cast<const int8_t*>(s.begin()), s.size()));
    }
    virtual sstring to_string(const bytes& b) const override {
        return sstring(reinterpret_cast<const char*>(b.begin()), b.size());
    }
};

struct ascii_type_impl final : public string_type_impl {
    ascii_type_impl() : string_type_impl(ascii_type_name) {}
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::ascii;
    }
};

struct utf8_type_impl final : public string_type_impl {
    static const char* name;
    utf8_type_impl() : string_type_impl(utf8_type_name) {}
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::text;
    }
    virtual bool is_compatible_with(const abstract_type& other) const override {
        // Anything that is ascii is also utf8, and they both use bytes
        // comparison
        return this == &other || &other == ascii_type.get();
    }
    using concrete_type::from_value;
};

struct bytes_type_impl final : public concrete_type<bytes> {
    bytes_type_impl() : concrete_type(bytes_type_name) {}
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        auto& v = from_value(value);
        out = std::copy(v.begin(), v.end(), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value) {
            return 0;
        }
        auto& v = from_value(value);
        return v.size();
    }
    virtual data_value deserialize(bytes_view v) const override {
        return make_value(std::make_unique<native_type>(v.begin(), v.end()));
    }
    virtual bool less(bytes_view v1, bytes_view v2) const override {
        return less_unsigned(v1, v2);
    }
    virtual bool is_byte_order_equal() const override {
        return true;
    }
    virtual bool is_byte_order_comparable() const override {
        return true;
    }
    virtual size_t hash(bytes_view v) const override {
        return std::hash<bytes_view>()(v);
    }
    virtual bytes from_string(sstring_view s) const override {
        return from_hex(s);
    }
    virtual sstring to_string(const bytes& b) const override {
        return to_hex(b);
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::blob;
    }
    virtual bool is_value_compatible_with_internal(const abstract_type& other) const override {
        return true;
    }
    virtual bool is_compatible_with(const abstract_type& other) const override {
        // Both asciiType and utf8Type really use bytes comparison and
        // bytesType validate everything, so it is compatible with the former.
        return this == &other || &other == ascii_type.get() || &other == utf8_type.get();
    }
};

struct boolean_type_impl : public simple_type_impl<bool> {
    boolean_type_impl() : simple_type_impl<bool>(boolean_type_name) {}
    void serialize_value(maybe_empty<bool> value, bytes::iterator& out) const {
        if (!value.empty()) {
            *out++ = char(value);
        }
    }
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        serialize_value(from_value(value), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value) {
            return 0;
        }
        if (from_value(value).empty()) {
            return 0;
        }
        return 1;
    }
    size_t serialized_size(bool value) const {
        return 1;
    }
    virtual data_value deserialize(bytes_view v) const override {
        if (v.empty()) {
            return make_empty();
        }
        if (v.size() != 1) {
            throw marshal_exception();
        }
        return make_value(*v.begin() != 0);
    }
    virtual void validate(bytes_view v) const override {
        if (v.size() != 0 && v.size() != 1) {
            throw marshal_exception();
        }
    }
    virtual bytes from_string(sstring_view s) const override {
        sstring s_lower(s.begin(), s.end());
        std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);
        if (s.empty() || s_lower == "false") {
            return ::serialize_value(*this, false);
        } else if (s_lower == "true") {
            return ::serialize_value(*this, true);
        } else {
            throw marshal_exception(sprint("unable to make boolean from '%s'", s));
        }
    }
    virtual sstring to_string(const bytes& b) const override {
        if (b.empty()) {
            return "";
        }
        if (b.size() != 1) {
            throw marshal_exception();
        }
        return *b.begin() ? "true" : "false";
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::boolean;
    }
};

class date_type_impl : public concrete_type<db_clock::time_point> {
    static logging::logger _logger;
public:
    date_type_impl() : concrete_type(date_type_name) {}
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        auto& v = from_value(value);
        if (v.empty()) {
            return;
        }
        int64_t i = v.get().time_since_epoch().count();
        i = net::hton(uint64_t(i));
        out = std::copy_n(reinterpret_cast<const char*>(&i), sizeof(i), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value || from_value(value).empty()) {
            return 0;
        }
        return 8;
    }
    virtual data_value deserialize(bytes_view v) const override {
        if (v.empty()) {
            return make_empty();
        }
        auto tmp = read_simple_exactly<uint64_t>(v);
        return make_value(db_clock::time_point(db_clock::duration(tmp)));
    }
    virtual bool less(bytes_view b1, bytes_view b2) const override {
        return compare_unsigned(b1, b2);
    }
    virtual bool is_byte_order_comparable() const override {
        return true;
    }
    virtual size_t hash(bytes_view v) const override {
        return std::hash<bytes_view>()(v);
    }
    virtual bytes from_string(sstring_view s) const override;
    virtual sstring to_string(const bytes& b) const override {
        auto v = deserialize(b);
        if (v.is_null()) {
            return "";
        }
        return time_point_to_string(from_value(v).get());
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::timestamp;
    }
    virtual bool is_value_compatible_with_internal(const abstract_type& other) const override {
        return &other == this || &other == timestamp_type.get() || &other == long_type.get();
    }
    virtual bool is_compatible_with(const abstract_type& other) const override {
        if (&other == this) {
            return true;
        }
        if (&other == timestamp_type.get()) {
            _logger.warn("Changing from TimestampType to DateType is allowed, but be wary that they sort differently for pre-unix-epoch timestamps "
                         "(negative timestamp values) and thus this change will corrupt your data if you have such negative timestamp. There is no "
                         "reason to switch from DateType to TimestampType except if you were using DateType in the first place and switched to "
                         "TimestampType by mistake.");
            return true;
        }
        return false;
    }
};
logging::logger date_type_impl::_logger(date_type_name);

struct timeuuid_type_impl : public concrete_type<utils::UUID> {
    timeuuid_type_impl() : concrete_type<utils::UUID>(timeuuid_type_name) {}
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        auto& uuid1 = from_value(value);
        if (uuid1.empty()) {
            return;
        }
        auto uuid = uuid1.get();
        out = std::copy_n(uuid.to_bytes().begin(), sizeof(uuid), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value || from_value(value).empty()) {
            return 0;
        }
        return 16;
    }
    virtual data_value deserialize(bytes_view v) const override {
        if (v.empty()) {
            return make_empty();
        }
        uint64_t msb, lsb;
        msb = read_simple<uint64_t>(v);
        lsb = read_simple<uint64_t>(v);
        if (!v.empty()) {
            throw marshal_exception();
        }
        return make_value(utils::UUID(msb, lsb));
    }
    virtual bool less(bytes_view b1, bytes_view b2) const override {
        if (b1.empty()) {
            return b2.empty() ? false : true;
        }
        if (b2.empty()) {
            return false;
        }
        auto r = compare_bytes(b1, b2);
        if (r != 0) {
            return r < 0;
        } else {
            return std::lexicographical_compare(b1.begin(), b1.end(), b2.begin(), b2.end());
        }
    }
    virtual bool is_byte_order_equal() const override {
        return true;
    }
    virtual size_t hash(bytes_view v) const override {
        return std::hash<bytes_view>()(v);
    }
    virtual void validate(bytes_view v) const override {
        if (v.size() != 0 && v.size() != 16) {
            throw marshal_exception();
        }
        auto msb = read_simple<uint64_t>(v);
        auto lsb = read_simple<uint64_t>(v);
        utils::UUID uuid(msb, lsb);
        if (uuid.version() != 1) {
            throw marshal_exception();
        }
    }
    virtual bytes from_string(sstring_view s) const override {
        if (s.empty()) {
            return bytes();
        }
        static const std::regex re("^[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}$");
        if (!std::regex_match(s.begin(), s.end(), re)) {
            throw marshal_exception();
        }
        utils::UUID v(s);
        if (v.version() != 1) {
            throw marshal_exception();
        }
        return v.to_bytes();
    }
    virtual sstring to_string(const bytes& b) const override {
        auto v = deserialize(b);
        if (v.is_null()) {
            return "";
        }
        return from_value(v).get().to_sstring();
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::timeuuid;
    }
private:
    static int compare_bytes(bytes_view o1, bytes_view o2) {
        auto compare_pos = [&] (unsigned pos, int mask, int ifequal) {
            int d = (o1[pos] & mask) - (o2[pos] & mask);
            return d ? d : ifequal;
        };
        return compare_pos(6, 0xf,
            compare_pos(7, 0xff,
                compare_pos(4, 0xff,
                    compare_pos(5, 0xff,
                        compare_pos(0, 0xff,
                            compare_pos(1, 0xff,
                                compare_pos(2, 0xff,
                                    compare_pos(3, 0xff, 0))))))));
    }
    friend class uuid_type_impl;
};

class timestamp_type_impl : public simple_type_impl<db_clock::time_point> {
    static logging::logger _logger;
public:
    timestamp_type_impl() : simple_type_impl(timestamp_type_name) {}
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        auto&& v1 = from_value(value);
        if (v1.empty()) {
            return;
        }
        uint64_t v = v1.get().time_since_epoch().count();
        v = net::hton(v);
        out = std::copy_n(reinterpret_cast<const char*>(&v), sizeof(v), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value || from_value(value).empty()) {
            return 0;
        }
        return 8;
    }
    virtual data_value deserialize(bytes_view in) const override {
        if (in.empty()) {
            return make_empty();
        }
        auto v = read_simple_exactly<uint64_t>(in);
        return make_value(db_clock::time_point(db_clock::duration(v)));
    }
    // FIXME: isCompatibleWith(timestampuuid)
    virtual void validate(bytes_view v) const override {
        if (v.size() != 0 && v.size() != sizeof(uint64_t)) {
            throw marshal_exception();
        }
    }
    static boost::posix_time::ptime get_time(const std::smatch& sm) {
        // Unfortunately boost::date_time  parsers are more strict with regards
        // to the expected date format than we need to be.
        auto year = boost::lexical_cast<int>(sm[1]);
        auto month =  boost::lexical_cast<int>(sm[2]);
        auto day =  boost::lexical_cast<int>(sm[3]);
        boost::gregorian::date date(year, month, day);

        auto hour = sm[5].length() ? boost::lexical_cast<int>(sm[5]) : 0;
        auto minute = sm[6].length() ? boost::lexical_cast<int>(sm[6]) : 0;
        auto second = sm[8].length() ? boost::lexical_cast<int>(sm[8]) : 0;
        boost::posix_time::time_duration time(hour, minute, second);

        if (sm[10].length()) {
            static constexpr auto milliseconds_string_length = 3;
            auto length = sm[10].length();
            if (length > milliseconds_string_length) {
                throw marshal_exception();
            }
            auto value = boost::lexical_cast<int>(sm[10]);
            while (length < milliseconds_string_length) {
                value *= 10;
                length++;
            }
            time += boost::posix_time::milliseconds(value);
        }

        return boost::posix_time::ptime(date, time);
    }
    static boost::posix_time::time_duration get_utc_offset(const std::string& s) {
        static constexpr const char* formats[] = {
            "%H:%M",
            "%H%M",
        };
        for (auto&& f : formats) {
            auto tif = new boost::posix_time::time_input_facet(f);
            std::istringstream ss(s);
            ss.imbue(std::locale(ss.getloc(), tif));
            auto sign = ss.get();
            boost::posix_time::ptime p;
            ss >> p;
            if (ss.good() && ss.peek() == std::istringstream::traits_type::eof()) {
                return p.time_of_day() * (sign == '-' ? -1 : 1);
            }
        }
        throw marshal_exception();
    }
    static int64_t timestamp_from_string(sstring_view s) {
        try {
            std::string str;
            str.resize(s.size());
            std::transform(s.begin(), s.end(), str.begin(), ::tolower);
            if (str == "now") {
                return db_clock::now().time_since_epoch().count();
            }

            char* end;
            auto v = std::strtoll(s.begin(), &end, 10);
            if (end == s.begin() + s.size()) {
                return v;
            }

            std::regex date_re("^(\\d{4})-(\\d+)-(\\d+)([ t](\\d+):(\\d+)(:(\\d+)(\\.(\\d+))?)?)?");
            std::smatch dsm;
            if (!std::regex_search(str, dsm, date_re)) {
                throw marshal_exception();
            }
            auto t = get_time(dsm);

            auto tz = dsm.suffix().str();
            std::regex tz_re("([\\+-]\\d{2}:?(\\d{2})?)");
            std::smatch tsm;
            if (std::regex_match(tz, tsm, tz_re)) {
                t -= get_utc_offset(tsm.str());
            } else if (tz.empty()) {
                typedef boost::date_time::c_local_adjustor<boost::posix_time::ptime> local_tz;
                // local_tz::local_to_utc(), where are you?
                auto t1 = local_tz::utc_to_local(t);
                auto tz_offset = t1 - t;
                auto t2 = local_tz::utc_to_local(t - tz_offset);
                auto dst_offset = t2 - t;
                t -= tz_offset + dst_offset;
            } else {
                throw marshal_exception();
            }
            return (t - boost::posix_time::from_time_t(0)).total_milliseconds();
        } catch (...) {
            throw marshal_exception(sprint("unable to parse date '%s'", s));
        }
    }
    virtual bytes from_string(sstring_view s) const override {
        if (s.empty()) {
            return bytes();
        }
        int64_t ts = net::hton(timestamp_from_string(s));
        bytes b(bytes::initialized_later(), sizeof(int64_t));
        std::copy_n(reinterpret_cast<const int8_t*>(&ts), sizeof(ts), b.begin());
        return b;
    }
    virtual sstring to_string(const bytes& b) const override {
        auto v = deserialize(b);
        if (v.is_null()) {
            return "";
        }
        return time_point_to_string(from_value(v).get());
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::timestamp;
    }
    virtual bool is_value_compatible_with_internal(const abstract_type& other) const override {
        return &other == this || &other == date_type.get() || &other == long_type.get();
    }
    virtual bool is_compatible_with(const abstract_type& other) const override {
        if (&other == this) {
            return true;
        }
        if (&other == date_type.get()) {
            _logger.warn("Changing from DateType to TimestampType is allowed, but be wary that they sort differently for pre-unix-epoch timestamps "
                         "(negative timestamp values) and thus this change will corrupt your data if you have such negative timestamp. So unless you "
                         "know that you don't have *any* pre-unix-epoch timestamp you should change back to DateType");
            return true;
        }
        return false;
    }
};
logging::logger timestamp_type_impl::_logger(timestamp_type_name);

struct simple_date_type_impl : public simple_type_impl<uint32_t> {
    simple_date_type_impl() : simple_type_impl{simple_date_type_name}
    { }
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        auto&& v1 = from_value(value);
        if (v1.empty()) {
            return;
        }
        uint32_t v = v1.get();
        v = net::hton(v);
        out = std::copy_n(reinterpret_cast<const char*>(&v), sizeof(v), out);

    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value || from_value(value).empty()) {
            return 0;
        }
        return 4;
    }
    virtual data_value deserialize(bytes_view in) const override {
        if (in.empty()) {
            return make_empty();
        }
        auto v = read_simple_exactly<uint32_t>(in);
        return make_value(v);
    }
    virtual void validate(bytes_view v) const override {
        if (v.size() != 0 && v.size() != 4) {
            throw marshal_exception(sprint("Expected 4 byte long for date (%d)", v.size()));
        }
    }
    virtual bytes from_string(sstring_view s) const override {
        if (s.empty()) {
            return bytes();
        }
        uint32_t ts = net::hton(days_from_string(s));
        bytes b(bytes::initialized_later(), sizeof(int32_t));
        std::copy_n(reinterpret_cast<const int8_t*>(&ts), sizeof(ts), b.begin());
        return b;
    }
    static uint32_t days_from_string(sstring_view s) {
        std::string str;
        str.resize(s.size());
        std::transform(s.begin(), s.end(), str.begin(), ::tolower);
        char* end;
        auto v = std::strtoll(s.begin(), &end, 10);
        if (end == s.begin() + s.size()) {
            return v;
        }
        static std::regex date_re("^(-?\\d+)-(\\d+)-(\\d+)");
        std::smatch dsm;
        if (!std::regex_match(str, dsm, date_re)) {
            throw marshal_exception(sprint("Unable to coerce '%s' to a formatted date (long)", str));
        }
        auto t = get_time(dsm);
        return serialize(str, date::local_days(t).time_since_epoch().count());
    }
    static date::year_month_day get_time(const std::smatch& sm) {
        auto year = boost::lexical_cast<long>(sm[1]);
        auto month = boost::lexical_cast<unsigned>(sm[2]);
        auto day = boost::lexical_cast<unsigned>(sm[3]);
        return date::year_month_day{date::year{year}, date::month{month}, date::day{day}};
    }
    static uint32_t serialize(const std::string& input, int64_t days) {
        if (days < std::numeric_limits<int32_t>::min()) {
            throw marshal_exception(sprint("Input date %s is less than min supported date -5877641-06-23", input));
        }
        if (days > std::numeric_limits<int32_t>::max()) {
            throw marshal_exception(sprint("Input date %s is greater than max supported date 5881580-07-11", input));
        }
        days += 1UL << 31;
        return static_cast<uint32_t>(days);
    }
    virtual sstring to_string(const bytes& b) const override {
        auto v = deserialize(b);
        if (v.is_null()) {
            return "";
        }
        date::days days{from_value(v).get() - (1UL << 31)};
        date::year_month_day ymd{date::local_days{days}};
        std::ostringstream str;
        str << ymd;
        return str.str();
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::date;
    }
};

struct time_type_impl : public simple_type_impl<int64_t> {
    time_type_impl() : simple_type_impl{time_type_name}
    { }
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        auto&& v1 = from_value(value);
        if (v1.empty()) {
            return;
        }
        uint64_t v = v1.get();
        v = net::hton(v);
        out = std::copy_n(reinterpret_cast<const char*>(&v), sizeof(v), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value || from_value(value).empty()) {
            return 0;
        }
        return 8;
    }
    virtual data_value deserialize(bytes_view in) const override {
        if (in.empty()) {
            return make_empty();
        }
        auto v = read_simple_exactly<int64_t>(in);
        return make_value(v);
    }
    virtual void validate(bytes_view v) const override {
        if (v.size() != 0 && v.size() != 8) {
            throw marshal_exception(sprint("Expected 8 byte long for time (%d)", v.size()));
        }
    }
    virtual bytes from_string(sstring_view s) const override {
        if (s.empty()) {
            return bytes();
        }
        int64_t ts = net::hton(parse_time(s));
        bytes b(bytes::initialized_later(), sizeof(int64_t));
        std::copy_n(reinterpret_cast<const int8_t*>(&ts), sizeof(ts), b.begin());
        return b;
    }
    static int64_t parse_time(sstring_view s) {
        static auto format_error = "Timestamp format must be hh:mm:ss[.fffffffff]";
        auto hours_end = s.find(':');
        if (hours_end == std::string::npos) {
            throw marshal_exception(format_error);
        }
        int64_t hours = std::stol(s.substr(0, hours_end).to_string());
        if (hours < 0 || hours >= 24) {
            throw marshal_exception("Hour out of bounds.");
        }
        auto minutes_end = s.find(':', hours_end+1);
        if (minutes_end == std::string::npos) {
            throw marshal_exception(format_error);
        }
        int64_t minutes = std::stol(s.substr(hours_end + 1, hours_end-minutes_end).to_string());
        if (minutes < 0 || minutes >= 60) {
            throw marshal_exception("Minute out of bounds.");
        }
        auto seconds_end = s.find('.', minutes_end+1);
        if (seconds_end == std::string::npos) {
            seconds_end = s.length();
        }
        int64_t seconds = std::stol(s.substr(minutes_end + 1, minutes_end-seconds_end).to_string());
        if (seconds < 0 || seconds >= 60) {
            throw marshal_exception("Second out of bounds.");
        }
        int64_t nanoseconds = 0;
        if (seconds_end < s.length()) {
            nanoseconds = std::stol(s.substr(seconds_end + 1).to_string());
            nanoseconds *= std::pow(10, 9-(s.length() - (seconds_end + 1)));
            if (nanoseconds < 0 || nanoseconds >= 1000 * 1000 * 1000) {
                throw marshal_exception("Nanosecond out of bounds.");
            }
        }
        std::chrono::nanoseconds result{};
        result += std::chrono::hours(hours);
        result += std::chrono::minutes(minutes);
        result += std::chrono::seconds(seconds);
        result += std::chrono::nanoseconds(nanoseconds);
        return result.count();
    }
    virtual sstring to_string(const bytes& b) const override {
        auto v = deserialize(b);
        if (v.is_null()) {
            return "";
        }
        std::chrono::nanoseconds nanoseconds{from_value(v).get()};
        auto time = date::make_time(nanoseconds);
        std::ostringstream str;
        str << time;
        return str.str();
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::time;
    }
};

struct uuid_type_impl : concrete_type<utils::UUID> {
    uuid_type_impl() : concrete_type(uuid_type_name) {}
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        auto& uuid = from_value(value);
        out = std::copy_n(uuid.get().to_bytes().begin(), sizeof(uuid.get()), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value) {
            return 0;
        }
        return 16;
    }
    virtual data_value deserialize(bytes_view v) const override {
        if (v.empty()) {
            return make_empty();
        }
        auto msb = read_simple<uint64_t>(v);
        auto lsb = read_simple<uint64_t>(v);
        if (!v.empty()) {
            throw marshal_exception();
        }
        return make_value(utils::UUID(msb, lsb));
    }
    virtual bool less(bytes_view b1, bytes_view b2) const override {
        if (b1.size() < 16) {
            return b2.size() < 16 ? false : true;
        }
        if (b2.size() < 16) {
            return false;
        }
        auto v1 = (b1[6] >> 4) & 0x0f;
        auto v2 = (b2[6] >> 4) & 0x0f;

        if (v1 != v2) {
            return v1 < v2;
        }

        if (v1 == 1) {
            auto c = timeuuid_type_impl::compare_bytes(b1, b2);
            if (c) {
                return c < 0;
            }
        }
        return less_unsigned(b1, b2);
    }
    // FIXME: isCompatibleWith(uuid)
    virtual bool is_byte_order_equal() const override {
        return true;
    }
    virtual size_t hash(bytes_view v) const override {
        return std::hash<bytes_view>()(v);
    }
    virtual void validate(bytes_view v) const override {
        if (v.size() != 0 && v.size() != 16) {
            throw marshal_exception();
        }
    }
    virtual bytes from_string(sstring_view s) const override {
        if (s.empty()) {
            return bytes();
        }
        static const std::regex re("^[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}$");
        if (!std::regex_match(s.begin(), s.end(), re)) {
            throw marshal_exception();
        }
        utils::UUID v(s);
        return v.to_bytes();
    }
    virtual sstring to_string(const bytes& b) const override {
        auto v = deserialize(b);
        if (v.is_null()) {
            return "";
        }
        return from_value(v).get().to_sstring();
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::uuid;
    }
    virtual bool is_value_compatible_with_internal(const abstract_type& other) const override {
        return &other == this || &other == timeuuid_type.get();
    }
};

struct inet_addr_type_impl : concrete_type<net::ipv4_address> {
    inet_addr_type_impl() : concrete_type<net::ipv4_address>(inet_addr_type_name) {}
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        // FIXME: support ipv6
        auto& ipv4e = from_value(value);
        if (ipv4e.empty()) {
            return;
        }
        auto& ipv4 = ipv4e.get();
        uint32_t u = htonl(ipv4.ip);
        out = std::copy_n(reinterpret_cast<const char*>(&u), sizeof(u), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value || from_value(value).empty()) {
            return 0;
        }
        return 4;
    }
    virtual data_value deserialize(bytes_view v) const override {
        if (v.empty()) {
            return make_empty();
        }
        if (v.size() == 16) {
            throw std::runtime_error("IPv6 addresses not supported");
        }
        auto ip = read_simple<int32_t>(v);
        if (!v.empty()) {
            throw marshal_exception();
        }
        return make_value(net::ipv4_address(ip));
    }
    virtual bool less(bytes_view v1, bytes_view v2) const override {
        return less_unsigned(v1, v2);
    }
    virtual bool is_byte_order_equal() const override {
        return true;
    }
    virtual bool is_byte_order_comparable() const override {
        return true;
    }
    virtual size_t hash(bytes_view v) const override {
        return std::hash<bytes_view>()(v);
    }
    virtual void validate(bytes_view v) const override {
        if (v.size() != 0 && v.size() != sizeof(uint32_t)) {
            throw marshal_exception();
        }
    }
    virtual bytes from_string(sstring_view s) const override {
        // FIXME: support host names
        if (s.empty()) {
            return bytes();
        }
        native_type ipv4;
        try {
            ipv4 = net::ipv4_address(std::string(s.data(), s.size()));
        } catch (...) {
            throw marshal_exception();
        }
        bytes b(bytes::initialized_later(), sizeof(uint32_t));
        auto out = b.begin();
        serialize(&ipv4, out);
        return b;
    }
    virtual sstring to_string(const bytes& b) const override {
        auto v = deserialize(b);
        if (v.is_null()) {
            return  "";
        }
        boost::asio::ip::address_v4 ipv4(from_value(v).get().ip);
        return ipv4.to_string();
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::inet;
    }
};

// Integer of same length of a given type. This is useful because our
// ntoh functions only know how to operate on integers.
template <typename T> struct int_of_size;
template <typename D, typename I> struct int_of_size_impl {
    using dtype = D;
    using itype = I;
    static_assert(sizeof(dtype) == sizeof(itype), "size mismatch");
    static_assert(alignof(dtype) == alignof(itype), "align mismatch");
};
template <> struct int_of_size<double> :
    public int_of_size_impl<double, uint64_t> {};
template <> struct int_of_size<float> :
    public int_of_size_impl<float, uint32_t> {};

template <typename T>
struct float_type_traits {
    static double read_nonempty(bytes_view v) {
        union {
            T d;
            typename int_of_size<T>::itype i;
        } x;
        x.i = read_simple_exactly<typename int_of_size<T>::itype>(v);
        return x.d;
    }
};
template<> struct simple_type_traits<float> : public float_type_traits<float> {};
template<> struct simple_type_traits<double> : public float_type_traits<double> {};


template <typename T>
struct floating_type_impl : public simple_type_impl<T> {
    floating_type_impl(sstring name) : simple_type_impl<T>(std::move(name)) {}
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        T d = this->from_value(value);
        if (std::isnan(d)) {
            // Java's Double.doubleToLongBits() documentation specifies that
            // any nan must be serialized to the same specific value
            d = std::numeric_limits<T>::quiet_NaN();
        }
        union {
            T d;
            typename int_of_size<T>::itype i;
        } x;
        x.d = d;
        auto u = net::hton(x.i);
        out = std::copy_n(reinterpret_cast<const char*>(&u), sizeof(u), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value) {
            return 0;
        }
        return sizeof(T);
    }

    virtual data_value deserialize(bytes_view v) const override {
        if (v.empty()) {
            return this->make_empty();
        }
        union {
            T d;
            typename int_of_size<T>::itype i;
        } x;
        x.i = read_simple<typename int_of_size<T>::itype>(v);
        if (!v.empty()) {
            throw marshal_exception();
        }
        return this->make_value(x.d);
    }
    virtual int32_t compare(bytes_view v1, bytes_view v2) const override {
        if (v1.empty()) {
            return v2.empty() ? 0 : -1;
        }
        if (v2.empty()) {
            return 1;
        }
        T a = simple_type_traits<T>::read_nonempty(v1);
        T b = simple_type_traits<T>::read_nonempty(v2);

        // in java world NaN == NaN and NaN is greater than anything else
        if (std::isnan(a) && std::isnan(b)) {
            return 0;
        } else if (std::isnan(a)) {
            return 1;
        } else if (std::isnan(b)) {
            return -1;
        }
        // also -0 < 0
        if (std::signbit(a) && !std::signbit(b)) {
            return -1;
        } else if (!std::signbit(a) && std::signbit(b))  {
            return 1;
        }
        return a == b ? 0 : a < b ? -1 : 1;
    }
    virtual void validate(bytes_view v) const override {
        if (v.size() != 0 && v.size() != sizeof(T)) {
            throw marshal_exception();
        }
    }
    virtual bytes from_string(sstring_view s) const override {
        if (s.empty()) {
            return bytes();
        }
        try {
            auto d = boost::lexical_cast<T>(s.begin(), s.size());
            bytes b(bytes::initialized_later(), sizeof(T));
            auto out = b.begin();
            auto val = this->make_value(d);
            serialize(this->get_value_ptr(val), out);
            return b;
        }
        catch(const boost::bad_lexical_cast& e) {
            throw marshal_exception(sprint("Invalid number format '%s'", s));
        }
    }
    virtual sstring to_string(const bytes& b) const override {
        auto v = deserialize(b);
        if (v.is_null()) {
            return "";
        }
        return to_sstring(this->from_value(v));
    }
};

struct double_type_impl : floating_type_impl<double> {
    double_type_impl() : floating_type_impl{double_type_name} { }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::double_;
    }
};

struct float_type_impl : floating_type_impl<float> {
    float_type_impl() : floating_type_impl{float_type_name} { }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::float_;
    }
};


class varint_type_impl : public concrete_type<boost::multiprecision::cpp_int> {
public:
    varint_type_impl() : concrete_type{varint_type_name} { }
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        auto& num1 = from_value(value);
        if (num1.empty()) {
            return;
        }
        auto& num = std::move(num1).get();
        boost::multiprecision::cpp_int pnum = boost::multiprecision::abs(num);

        std::vector<uint8_t> b;
        auto size = serialized_size(value);
        b.reserve(size);
        if (num.sign() < 0) {
            pnum -= 1;
        }
        for (unsigned i = 0; i < size; i++) {
            auto v = boost::multiprecision::integer_modulus(pnum, 256);
            pnum >>= 8;
            if (num.sign() < 0) {
                v = ~v;
            }
            b.push_back(v);
        }
        std::copy(b.crbegin(), b.crend(), out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value) {
            return 0;
        }
        auto& num1 = from_value(value);
        if (num1.empty()) {
            return 0;
        }
        auto&& num = num1.get();
        if (!num) {
            return 1;
        }
        auto pnum = abs(num);
        return align_up(boost::multiprecision::msb(pnum) + 2, 8u) / 8;
    }
    virtual int32_t compare(bytes_view v1, bytes_view v2) const override {
        if (v1.empty()) {
            return v2.empty() ? 0 : -1;
        }
        if (v2.empty()) {
            return 1;
        }
        auto a = from_value(deserialize(v1));
        auto b = from_value(deserialize(v2));
        return a == b ? 0 : a < b ? -1 : 1;
    }
    virtual bool less(bytes_view v1, bytes_view v2) const override {
        return compare(v1, v2) < 0;
    }
    virtual size_t hash(bytes_view v) const override {
        bytes b(v.begin(), v.end());
        return std::hash<sstring>()(to_string(b));
    }
    virtual data_value deserialize(bytes_view v) const override {
        if (v.empty()) {
            return make_empty();
        }
        auto negative = v.front() < 0;
        boost::multiprecision::cpp_int num;
        for (uint8_t b : v) {
            if (negative) {
                b = ~b;
            }
            num <<= 8;
            num += b;
        }
        if (negative) {
            num += 1;
        }
        return make_value(negative ? -num : num);
    }
    virtual sstring to_string(const bytes& b) const override {
        auto v = deserialize(b);
        if (v.is_null()) {
            return "";
        }
        return from_value(v).get().str();
    }
    virtual bytes from_string(sstring_view text) const override {
        if (text.empty()) {
            return bytes();
        }
        try {
            std::string str(text.begin(), text.end());
            native_type num(str);
            bytes b(bytes::initialized_later(), serialized_size(&num));
            auto out = b.begin();
            serialize(&num, out);
            return b;
        } catch (...) {
            throw marshal_exception(sprint("unable to make int from '%s'", text));
        }
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::varint;
    }
    virtual bool is_value_compatible_with_internal(const abstract_type& other) const override {
        return &other == this || int32_type->is_value_compatible_with(other) || long_type->is_value_compatible_with(other);
    }
    friend class decimal_type_impl;
};

class decimal_type_impl : public concrete_type<big_decimal> {
public:
    decimal_type_impl() : concrete_type{decimal_type_name} { }
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        if (!value) {
            return;
        }
        auto bd1 = from_value(value);
        if (bd1.empty()) {
            return;
        }
        auto&& bd = std::move(bd1).get();
        auto u = net::hton(bd.scale());
        out = std::copy_n(reinterpret_cast<const char*>(&u), sizeof(int32_t), out);
        varint_type_impl::native_type unscaled_value = bd.unscaled_value();
        varint_type->serialize(&unscaled_value, out);
    }
    virtual size_t serialized_size(const void* value) const override {
        if (!value) {
            return 0;
        }
        auto& bd1 = from_value(value);
        if (bd1.empty()) {
            return 0;
        }
        auto&& bd = std::move(bd1).get();
        varint_type_impl::native_type unscaled_value = bd.unscaled_value();
        return sizeof(int32_t) + varint_type->serialized_size(&unscaled_value);
    }
    virtual int32_t compare(bytes_view v1, bytes_view v2) const override {
        if (v1.empty()) {
            return v2.empty() ? 0 : -1;
        }
        if (v2.empty()) {
            return 1;
        }
        auto a = from_value(deserialize(v1));
        auto b = from_value(deserialize(v2));

        if (a.empty() && b.empty()) {
            return 0;
        }
        if (a.empty() && !b.empty()) {
            return -1;
        }
        if (!a.empty() && b.empty()) {
            return 1;
        }
        return a.get().compare(b.get());
    }
    virtual bool less(bytes_view v1, bytes_view v2) const override {
        return compare(v1, v2) < 0;
    }
    virtual size_t hash(bytes_view v) const override {
        bytes b(v.begin(), v.end());
        return std::hash<sstring>()(to_string(b));
    }
    virtual data_value deserialize(bytes_view v) const override {
        if (v.empty()) {
            return make_empty();
        }
        auto scale = read_simple<int32_t>(v);
        data_value unscaled = varint_type->deserialize(v);
        auto real_varint_type = static_cast<const varint_type_impl*>(varint_type.get()); // yuck
        return make_value(big_decimal(scale, real_varint_type->from_value(unscaled).get()));
    }
    virtual sstring to_string(const bytes& b) const override {
        auto v = deserialize(b);
        if (v.is_null()) {
            return "";
        }
        return from_value(v).get().to_string();
    }
    virtual bytes from_string(sstring_view text) const override {
        if (text.empty()) {
            return bytes();
        }
        try {
            native_type bd(text);
            bytes b(bytes::initialized_later(), serialized_size(&bd));
            auto out = b.begin();
            serialize(&bd, out);
            return b;
        } catch (...) {
            throw marshal_exception(sprint("unable to make BigDecimal from '%s'", text));
        }
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::decimal;
    }
};

class counter_type_impl : public abstract_type {
public:
    counter_type_impl() : abstract_type{counter_type_name} { }
    virtual void serialize(const void* value, bytes::iterator& out) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual size_t serialized_size(const void* value) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual int32_t compare(bytes_view v1, bytes_view v2) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual bool less(bytes_view v1, bytes_view v2) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual size_t hash(bytes_view v) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual data_value deserialize(bytes_view v) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual sstring to_string(const bytes& b) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual bytes from_string(sstring_view text) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual bool is_counter() const override {
        return true;
    }
    virtual ::shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        return cql3::cql3_type::counter;
    }
    virtual size_t native_value_size() const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual size_t native_value_alignment() const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual void native_value_copy(const void* from, void* to) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual void native_value_move(void* from, void* to) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual void native_value_destroy(void* object) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual void native_value_delete(void* object) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual void* native_value_clone(const void* object) const override {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual const std::type_info& native_typeid() const {
        fail(unimplemented::cause::COUNTERS);
    }
    virtual bool references_user_type(const sstring& keyspace, const bytes& name) const override {
        return false;
    }
    virtual std::experimental::optional<data_type> update_user_type(const shared_ptr<const user_type_impl> updated) const {
        return std::experimental::nullopt;
    }
};

struct empty_type_impl : abstract_type {
    empty_type_impl() : abstract_type(empty_type_name) {}
    virtual void serialize(const void* value, bytes::iterator& out) const override {
    }
    virtual size_t serialized_size(const void* value) const override {
        return 0;
    }

    virtual bool less(bytes_view v1, bytes_view v2) const override {
        return false;
    }
    virtual size_t hash(bytes_view v) const override {
        return 0;
    }
    virtual data_value deserialize(bytes_view v) const override {
        return data_value::make_null(shared_from_this());
    }
    virtual sstring to_string(const bytes& b) const override {
        // FIXME:
        abort();
    }
    virtual bytes from_string(sstring_view text) const override {
        // FIXME:
        abort();
    }
    virtual shared_ptr<cql3::cql3_type> as_cql3_type() const override {
        // Can't happen
        abort();
    }
    virtual size_t native_value_size() const override {
        // Can't happen
        abort();
    }
    virtual size_t native_value_alignment() const override {
        // Can't happen
        abort();
    }
    virtual void native_value_copy(const void* from, void* to) const override {
        // Can't happen
        abort();
    }
    virtual void native_value_move(void* from, void* to) const override {
        // Can't happen
        abort();
    }
    virtual void native_value_destroy(void* object) const override {
        // Can't happen
        abort();
    }
    virtual void native_value_delete(void* object) const override {
        // Can't happen
        abort();
    }
    virtual void* native_value_clone(const void* object) const override {
        // Can't happen
        abort();
    }
    virtual const std::type_info& native_typeid() const {
        // Can't happen
        abort();
    }
    virtual bool references_user_type(const sstring& keyspace, const bytes& name) const override {
        // Can't happen
        abort();
    }
    virtual std::experimental::optional<data_type> update_user_type(const shared_ptr<const user_type_impl> updated) const {
        // Can't happen
        abort();
    }
};


logging::logger collection_type_impl::_logger("collection_type_impl");
const size_t collection_type_impl::max_elements;

thread_local std::unordered_map<data_type, shared_ptr<cql3::cql3_type>> collection_type_impl::_cql3_type_cache;

const collection_type_impl::kind collection_type_impl::kind::map(
        [] (shared_ptr<cql3::column_specification> collection, bool is_key) -> shared_ptr<cql3::column_specification> {
            return is_key ? cql3::maps::key_spec_of(*collection) : cql3::maps::value_spec_of(*collection);
        });
const collection_type_impl::kind collection_type_impl::kind::set(
        [] (shared_ptr<cql3::column_specification> collection, bool is_key) -> shared_ptr<cql3::column_specification> {
            return cql3::sets::value_spec_of(collection);
        });
const collection_type_impl::kind collection_type_impl::kind::list(
        [] (shared_ptr<cql3::column_specification> collection, bool is_key) -> shared_ptr<cql3::column_specification> {
            return cql3::lists::value_spec_of(collection);
        });

shared_ptr<cql3::column_specification>
collection_type_impl::kind::make_collection_receiver(shared_ptr<cql3::column_specification> collection, bool is_key) const {
    return _impl(std::move(collection), is_key);
}

shared_ptr<cql3::column_specification>
collection_type_impl::make_collection_receiver(shared_ptr<cql3::column_specification> collection, bool is_key) const {
    return _kind.make_collection_receiver(std::move(collection), is_key);
}

std::vector<atomic_cell>
collection_type_impl::enforce_limit(std::vector<atomic_cell> cells, int version) const {
    assert(is_multi_cell());
    if (version >= 3 || cells.size() <= max_elements) {
        return cells;
    }
    _logger.error("Detected collection with {} elements, more than the {} limit. Only the first {} elements will be returned to the client. "
            "Please see http://cassandra.apache.org/doc/cql3/CQL.html#collections for more details.", cells.size(), max_elements, max_elements);
    cells.erase(cells.begin() + max_elements, cells.end());
    return cells;
}

bytes
collection_type_impl::serialize_for_native_protocol(std::vector<atomic_cell> cells, int version) const {
    assert(is_multi_cell());
    cells = enforce_limit(std::move(cells), version);
    std::vector<bytes> values = serialized_values(std::move(cells));
    // FIXME: implement
    abort();
    // return CollectionSerializer.pack(values, cells.size(), version);
}

bool
collection_type_impl::is_compatible_with(const abstract_type& previous) const {
    if (this == &previous) {
        return true;
    }
    if (!previous.is_collection()) {
        return false;
    }
    auto& cprev = static_cast<const collection_type_impl&>(previous);
    if (&_kind != &cprev._kind) {
        return false;
    }
    if (is_multi_cell() != cprev.is_multi_cell()) {
        return false;
    }

    if (!is_multi_cell()) {
        return is_compatible_with_frozen(cprev);
    }

    if (!name_comparator()->is_compatible_with(*cprev.name_comparator())) {
        return false;
    }

    // the value comparator is only used for Cell values, so sorting doesn't matter
    return value_comparator()->is_value_compatible_with(*cprev.value_comparator());
}

bool
collection_type_impl::is_value_compatible_with_internal(const abstract_type& previous) const {
    // for multi-cell collections, compatibility and value-compatibility are the same
    if (is_multi_cell() || previous.is_multi_cell()) {
        return is_compatible_with(previous);
    }
    if (!previous.is_collection()) {
        return false;
    }
    auto& cprev = static_cast<const collection_type_impl&>(previous);
    if (&_kind != &cprev._kind) {
        return false;
    }
    return is_value_compatible_with_frozen(cprev);
}

shared_ptr<cql3::cql3_type>
collection_type_impl::as_cql3_type() const {
    auto ret = _cql3_type_cache[shared_from_this()];
    if (!ret) {
        auto name = cql3_type_name();
        if (!is_multi_cell()) {
            name = "frozen<" + name + ">";
        }
        ret = make_shared<cql3::cql3_type>(name, shared_from_this(), false);
        _cql3_type_cache[shared_from_this()] = ret;
    }
    return ret;
}

bytes
collection_type_impl::to_value(collection_mutation_view mut, cql_serialization_format sf) const {
    return to_value(deserialize_mutation_form(mut), sf);
}

collection_type_impl::mutation
collection_type_impl::mutation_view::materialize() const {
    collection_type_impl::mutation m;
    m.tomb = tomb;
    m.cells.reserve(cells.size());
    for (auto&& e : cells) {
        m.cells.emplace_back(bytes(e.first.begin(), e.first.end()), e.second);
    }
    return m;
}


size_t collection_size_len(cql_serialization_format sf) {
    if (sf.using_32_bits_for_collections()) {
        return sizeof(int32_t);
    }
    return sizeof(uint16_t);
}

size_t collection_value_len(cql_serialization_format sf) {
    if (sf.using_32_bits_for_collections()) {
        return sizeof(int32_t);
    }
    return sizeof(uint16_t);
}


int read_collection_size(bytes_view& in, cql_serialization_format sf) {
    if (sf.using_32_bits_for_collections()) {
        return read_simple<int32_t>(in);
    } else {
        return read_simple<uint16_t>(in);
    }
}

void write_collection_size(bytes::iterator& out, int size, cql_serialization_format sf) {
    if (sf.using_32_bits_for_collections()) {
        serialize_int32(out, size);
    } else {
        serialize_int16(out, uint16_t(size));
    }
}

bytes_view read_collection_value(bytes_view& in, cql_serialization_format sf) {
    auto size = sf.using_32_bits_for_collections() ? read_simple<int32_t>(in) : read_simple<uint16_t>(in);
    return read_simple_bytes(in, size);
}

void write_collection_value(bytes::iterator& out, cql_serialization_format sf, bytes_view val_bytes) {
    if (sf.using_32_bits_for_collections()) {
        serialize_int32(out, int32_t(val_bytes.size()));
    } else {
        serialize_int16(out, uint16_t(val_bytes.size()));
    }
    out = std::copy_n(val_bytes.begin(), val_bytes.size(), out);
}

void write_collection_value(bytes::iterator& out, cql_serialization_format sf, data_type type, const data_value& value) {
    size_t val_len = type->serialized_size(type->get_value_ptr(value));

    if (sf.using_32_bits_for_collections()) {
        serialize_int32(out, val_len);
    } else {
        serialize_int16(out, val_len);
    }

    type->serialize(type->get_value_ptr(value), out);
}

map_type
map_type_impl::get_instance(data_type keys, data_type values, bool is_multi_cell) {
    return intern::get_instance(std::move(keys), std::move(values), is_multi_cell);
}

sstring make_map_type_name(data_type keys, data_type values, bool is_multi_cell)
{
    sstring ret = "";
    if (!is_multi_cell) {
        ret = "org.apache.cassandra.db.marshal.FrozenType(";
    }
    ret += "org.apache.cassandra.db.marshal.MapType(" + keys->name() + "," + values->name() + ")";
    if (!is_multi_cell) {
        ret += ")";
    }
    return ret;
}

map_type_impl::map_type_impl(data_type keys, data_type values, bool is_multi_cell)
        : concrete_type(make_map_type_name(keys, values, is_multi_cell), kind::map)
        , _keys(std::move(keys))
        , _values(std::move(values))
        , _is_multi_cell(is_multi_cell) {
}

data_type
map_type_impl::freeze() const {
    if (_is_multi_cell) {
        return get_instance(_keys, _values, false);
    } else {
        return shared_from_this();
    }
}

bool
map_type_impl::is_compatible_with_frozen(const collection_type_impl& previous) const {
    assert(!_is_multi_cell);
    auto* p = dynamic_cast<const map_type_impl*>(&previous);
    if (!p) {
        return false;
    }
    return _keys->is_compatible_with(*p->_keys)
            && _values->is_compatible_with(*p->_values);
}

bool
map_type_impl::is_value_compatible_with_frozen(const collection_type_impl& previous) const {
    assert(!_is_multi_cell);
    auto* p = dynamic_cast<const map_type_impl*>(&previous);
    if (!p) {
        return false;
    }
    return _keys->is_compatible_with(*p->_keys)
            && _values->is_value_compatible_with(*p->_values);
}

bool
map_type_impl::less(bytes_view o1, bytes_view o2) const {
    return compare_maps(_keys, _values, o1, o2) < 0;
}

int32_t
map_type_impl::compare_maps(data_type keys, data_type values, bytes_view o1, bytes_view o2) {
    if (o1.empty()) {
        return o2.empty() ? 0 : -1;
    } else if (o2.empty()) {
        return 1;
    }
    auto sf = cql_serialization_format::internal();
    int size1 = read_collection_size(o1, sf);
    int size2 = read_collection_size(o2, sf);
    // FIXME: use std::lexicographical_compare()
    for (int i = 0; i < std::min(size1, size2); ++i) {
        auto k1 = read_collection_value(o1, sf);
        auto k2 = read_collection_value(o2, sf);
        auto cmp = keys->compare(k1, k2);
        if (cmp != 0) {
            return cmp;
        }
        auto v1 = read_collection_value(o1, sf);
        auto v2 = read_collection_value(o2, sf);
        cmp = values->compare(v1, v2);
        if (cmp != 0) {
            return cmp;
        }
    }
    return size1 == size2 ? 0 : (size1 < size2 ? -1 : 1);
}

void
map_type_impl::serialize(const void* value, bytes::iterator& out) const {
    return serialize(value, out, cql_serialization_format::internal());
}

size_t
map_type_impl::serialized_size(const void* value) const {
    auto& m = from_value(value);
    size_t len = collection_size_len(cql_serialization_format::internal());
    size_t psz = collection_value_len(cql_serialization_format::internal());
    for (auto&& kv : m) {
        len += psz + _keys->serialized_size(get_value_ptr(kv.first));
        len += psz + _values->serialized_size(get_value_ptr(kv.second));
    }

    return len;
}

void
map_type_impl::serialize(const void* value, bytes::iterator& out, cql_serialization_format sf) const {
    auto& m = from_value(value);
    write_collection_size(out, m.size(), sf);
    for (auto&& kv : m) {
        write_collection_value(out, sf, _keys, kv.first);
        write_collection_value(out, sf, _values, kv.second);
    }
}

data_value
map_type_impl::deserialize(bytes_view v) const {
    return deserialize(v, cql_serialization_format::internal());
}

data_value
map_type_impl::deserialize(bytes_view in, cql_serialization_format sf) const {
    native_type m;
    auto size = read_collection_size(in, sf);
    for (int i = 0; i < size; ++i) {
        auto kb = read_collection_value(in, sf);
        auto k = _keys->deserialize(kb);
        auto vb = read_collection_value(in, sf);
        auto v = _values->deserialize(vb);
        m.insert(m.end(), std::make_pair(std::move(k), std::move(v)));
    }
    return make_value(std::move(m));
}

sstring
map_type_impl::to_string(const bytes& b) const {
    bool include_frozen_type = !is_multi_cell();
    std::ostringstream out;
    bool first = true;
    auto v = bytes_view(b);
    auto sf = cql_serialization_format::internal();

    if (include_frozen_type) {
        out << "(";
    }

    auto size = read_collection_size(v, sf);
    for (int i = 0; i < size; ++i) {
        auto kb = read_collection_value(v, sf);
        auto vb = read_collection_value(v, sf);

        if (first) {
            first = false;
        } else {
            out << ", ";
        }

        out << "{" << _keys->to_string(bytes(kb.begin(), kb.end())) << " : ";
        out << _values->to_string(bytes(vb.begin(), vb.end())) << "}";
    }

    if (include_frozen_type) {
        out << ")";
    }

    return out.str();
}

size_t
map_type_impl::hash(bytes_view v) const {
    // FIXME:
    abort();
}

bytes
map_type_impl::from_string(sstring_view text) const {
    // FIXME:
    abort();
}

std::vector<bytes>
map_type_impl::serialized_values(std::vector<atomic_cell> cells) const {
    // FIXME:
    abort();
}

bytes
map_type_impl::to_value(mutation_view mut, cql_serialization_format sf) const {
    std::vector<bytes_view> tmp;
    tmp.reserve(mut.cells.size() * 2);
    for (auto&& e : mut.cells) {
        if (e.second.is_live(mut.tomb, false)) {
            tmp.emplace_back(e.first);
            tmp.emplace_back(e.second.value());
        }
    }
    return pack(tmp.begin(), tmp.end(), tmp.size() / 2, sf);
}

bytes
map_type_impl::serialize_partially_deserialized_form(
        const std::vector<std::pair<bytes_view, bytes_view>>& v, cql_serialization_format sf) {
    size_t len = collection_value_len(sf) * v.size() * 2 + collection_size_len(sf);
    for (auto&& e : v) {
        len += e.first.size() + e.second.size();
    }
    bytes b(bytes::initialized_later(), len);
    bytes::iterator out = b.begin();

    write_collection_size(out, v.size(), sf);
    for (auto&& e : v) {
        write_collection_value(out, sf, e.first);
        write_collection_value(out, sf, e.second);
    }
    return b;


}
sstring
map_type_impl::cql3_type_name() const {
    return sprint("map<%s, %s>", _keys->as_cql3_type(), _values->as_cql3_type());
}

bool
map_type_impl::references_user_type(const sstring& keyspace, const bytes& name) const {
    return _keys->references_user_type(keyspace, name) || _values->references_user_type(keyspace, name);
}

std::experimental::optional<data_type>
map_type_impl::update_user_type(const shared_ptr<const user_type_impl> updated) const {
    auto k = _keys->update_user_type(updated);
    auto v = _values->update_user_type(updated);
    if (!k && !v) {
        return std::experimental::nullopt;
    }
    return std::experimental::make_optional(static_pointer_cast<const abstract_type>(
        get_instance(k ? *k : _keys, v ? *v : _values, _is_multi_cell)));
}

auto collection_type_impl::deserialize_mutation_form(collection_mutation_view cm) -> mutation_view {
    auto&& in = cm.data;
    mutation_view ret;
    auto has_tomb = read_simple<bool>(in);
    if (has_tomb) {
        auto ts = read_simple<api::timestamp_type>(in);
        auto ttl = read_simple<gc_clock::duration::rep>(in);
        ret.tomb = tombstone{ts, gc_clock::time_point(gc_clock::duration(ttl))};
    }
    auto nr = read_simple<uint32_t>(in);
    ret.cells.reserve(nr);
    for (uint32_t i = 0; i != nr; ++i) {
        // FIXME: we could probably avoid the need for size
        auto ksize = read_simple<uint32_t>(in);
        auto key = read_simple_bytes(in, ksize);
        auto vsize = read_simple<uint32_t>(in);
        auto value = atomic_cell_view::from_bytes(read_simple_bytes(in, vsize));
        ret.cells.emplace_back(key, value);
    }
    assert(in.empty());
    return ret;
}

bool collection_type_impl::is_empty(collection_mutation_view cm) const {
    auto&& in = cm.data;
    auto has_tomb = read_simple<bool>(in);
    if (has_tomb) {
        in.remove_prefix(sizeof(api::timestamp_type) + sizeof(gc_clock::duration::rep));
    }
    return read_simple<uint32_t>(in) == 0;
}

bool collection_type_impl::is_any_live(collection_mutation_view cm, tombstone tomb, gc_clock::time_point now) const {
    auto&& in = cm.data;
    auto has_tomb = read_simple<bool>(in);
    if (has_tomb) {
        auto ts = read_simple<api::timestamp_type>(in);
        auto ttl = read_simple<gc_clock::duration::rep>(in);
        tomb.apply(tombstone{ts, gc_clock::time_point(gc_clock::duration(ttl))});
    }
    auto nr = read_simple<uint32_t>(in);
    for (uint32_t i = 0; i != nr; ++i) {
        auto ksize = read_simple<uint32_t>(in);
        in.remove_prefix(ksize);
        auto vsize = read_simple<uint32_t>(in);
        auto value = atomic_cell_view::from_bytes(read_simple_bytes(in, vsize));
        if (value.is_live(tomb, now, false)) {
            return true;
        }
    }
    return false;
}

api::timestamp_type collection_type_impl::last_update(collection_mutation_view cm) const {
    auto&& in = cm.data;
    api::timestamp_type max = api::missing_timestamp;
    auto has_tomb = read_simple<bool>(in);
    if (has_tomb) {
        max = std::max(max, read_simple<api::timestamp_type>(in));
        (void)read_simple<gc_clock::duration::rep>(in);
    }
    auto nr = read_simple<uint32_t>(in);
    for (uint32_t i = 0; i != nr; ++i) {
        auto ksize = read_simple<uint32_t>(in);
        in.remove_prefix(ksize);
        auto vsize = read_simple<uint32_t>(in);
        auto value = atomic_cell_view::from_bytes(read_simple_bytes(in, vsize));
        max = std::max(value.timestamp(), max);
    }
    return max;
}

template <typename Iterator>
collection_mutation
do_serialize_mutation_form(
        const tombstone& tomb,
        boost::iterator_range<Iterator> cells) {
    auto element_size = [] (size_t c, auto&& e) -> size_t {
        return c + 8 + e.first.size() + e.second.serialize().size();
    };
    auto size = accumulate(cells, (size_t)4, element_size);
    size += 1;
    if (tomb) {
        size += sizeof(tomb.timestamp) + sizeof(tomb.deletion_time);
    }
    bytes ret(bytes::initialized_later(), size);
    bytes::iterator out = ret.begin();
    *out++ = bool(tomb);
    if (tomb) {
        write(out, tomb.timestamp);
        write(out, tomb.deletion_time.time_since_epoch().count());
    }
    auto writeb = [&out] (bytes_view v) {
        serialize_int32(out, v.size());
        out = std::copy_n(v.begin(), v.size(), out);
    };
    // FIXME: overflow?
    serialize_int32(out, boost::distance(cells));
    for (auto&& kv : cells) {
        auto&& k = kv.first;
        auto&& v = kv.second;
        writeb(k);
        writeb(v.serialize());
    }
    return collection_mutation{std::move(ret)};
}

bool collection_type_impl::mutation::compact_and_expire(tombstone base_tomb, gc_clock::time_point query_time,
    can_gc_fn& can_gc, gc_clock::time_point gc_before)
{
    bool any_live = false;
    tomb.apply(base_tomb);
    std::vector<std::pair<bytes, atomic_cell>> survivors;
    for (auto&& name_and_cell : cells) {
        atomic_cell& cell = name_and_cell.second;
        if (cell.is_covered_by(tomb, false)) {
            continue;
        }
        if (cell.has_expired(query_time)) {
            survivors.emplace_back(std::make_pair(
                std::move(name_and_cell.first), atomic_cell::make_dead(cell.timestamp(), cell.deletion_time())));
        } else if (!cell.is_live()) {
            if (cell.deletion_time() >= gc_before || !can_gc(tombstone(cell.timestamp(), cell.deletion_time()))) {
                survivors.emplace_back(std::move(name_and_cell));
            }
        } else {
            any_live |= true;
            survivors.emplace_back(std::move(name_and_cell));
        }
    }
    cells = std::move(survivors);
    if (tomb.deletion_time < gc_before && can_gc(tomb)) {
        tomb = tombstone();
    }
    return any_live;
}

collection_mutation
collection_type_impl::serialize_mutation_form(const mutation& mut) {
    return do_serialize_mutation_form(mut.tomb, boost::make_iterator_range(mut.cells.begin(), mut.cells.end()));
}

collection_mutation
collection_type_impl::serialize_mutation_form(mutation_view mut) {
    return do_serialize_mutation_form(mut.tomb, boost::make_iterator_range(mut.cells.begin(), mut.cells.end()));
}

collection_mutation
collection_type_impl::serialize_mutation_form_only_live(mutation_view mut, gc_clock::time_point now) {
    return do_serialize_mutation_form(mut.tomb, mut.cells | boost::adaptors::filtered([t = mut.tomb, now] (auto&& e) {
        return e.second.is_live(t, now, false);
    }));
}

collection_mutation
collection_type_impl::merge(collection_mutation_view a, collection_mutation_view b) const {
    auto aa = deserialize_mutation_form(a);
    auto bb = deserialize_mutation_form(b);
    mutation_view merged;
    merged.cells.reserve(aa.cells.size() + bb.cells.size());
    using element_type = std::pair<bytes_view, atomic_cell_view>;
    auto key_type = name_comparator();
    auto compare = [key_type] (const element_type& e1, const element_type& e2) {
        return key_type->less(e1.first, e2.first);
    };
    auto merge = [this] (const element_type& e1, const element_type& e2) {
        // FIXME: use std::max()?
        return std::make_pair(e1.first, compare_atomic_cell_for_merge(e1.second, e2.second) > 0 ? e1.second : e2.second);
    };
    // applied to a tombstone, returns a predicate checking whether a cell is killed by
    // the tombstone
    auto cell_killed = [] (const std::experimental::optional<tombstone>& t) {
        return [&t] (const element_type& e) {
            if (!t) {
                return false;
            }
            // tombstone wins if timestamps equal here, unlike row tombstones
            if (t->timestamp < e.second.timestamp()) {
                return false;
            }
            return true;
            // FIXME: should we consider TTLs too?
        };
    };
    combine(aa.cells.begin(), std::remove_if(aa.cells.begin(), aa.cells.end(), cell_killed(bb.tomb)),
            bb.cells.begin(), std::remove_if(bb.cells.begin(), bb.cells.end(), cell_killed(aa.tomb)),
            std::back_inserter(merged.cells),
            compare,
            merge);
    merged.tomb = std::max(aa.tomb, bb.tomb);
    return serialize_mutation_form(merged);
}

collection_mutation
collection_type_impl::difference(collection_mutation_view a, collection_mutation_view b) const
{
    auto aa = deserialize_mutation_form(a);
    auto bb = deserialize_mutation_form(b);
    mutation_view diff;
    diff.cells.reserve(std::max(aa.cells.size(), bb.cells.size()));
    auto key_type = name_comparator();
    auto it = bb.cells.begin();
    for (auto&& c : aa.cells) {
        while (it != bb.cells.end() && key_type->less(it->first, c.first)) {
            ++it;
        }
        if (it == bb.cells.end() || !key_type->equal(it->first, c.first)
            || compare_atomic_cell_for_merge(c.second, it->second) > 0) {

            auto cell = std::make_pair(c.first, c.second);
            diff.cells.emplace_back(std::move(cell));
        }
    }
    diff.tomb = std::max(aa.tomb, bb.tomb);
    return serialize_mutation_form(diff);
}

bytes_opt
collection_type_impl::reserialize(cql_serialization_format from, cql_serialization_format to, bytes_view_opt v) const {
    if (!v) {
        return std::experimental::nullopt;
    }
    auto val = deserialize(*v, from);
    bytes ret(bytes::initialized_later(), serialized_size(get_value_ptr(val)));  // FIXME: serialized_size want @to
    auto out = ret.begin();
    serialize(get_value_ptr(val), out, to);
    return ret;
}

// iterator that takes a set or list in serialized form, and emits
// each element, still in serialized form
class listlike_partial_deserializing_iterator
          : public std::iterator<std::input_iterator_tag, bytes_view> {
    bytes_view* _in;
    int _remain;
    bytes_view _cur;
    cql_serialization_format _sf;
private:
    struct end_tag {};
    listlike_partial_deserializing_iterator(bytes_view& in, cql_serialization_format sf)
            : _in(&in), _sf(sf) {
        _remain = read_collection_size(*_in, _sf);
        parse();
    }
    listlike_partial_deserializing_iterator(end_tag)
            : _remain(0), _sf(cql_serialization_format::internal()) {  // _sf is bogus, but doesn't matter
    }
public:
    bytes_view operator*() const { return _cur; }
    listlike_partial_deserializing_iterator& operator++() {
        --_remain;
        parse();
        return *this;
    }
    void operator++(int) {
        --_remain;
        parse();
    }
    bool operator==(const listlike_partial_deserializing_iterator& x) const {
        return _remain == x._remain;
    }
    bool operator!=(const listlike_partial_deserializing_iterator& x) const {
        return _remain != x._remain;
    }
    static listlike_partial_deserializing_iterator begin(bytes_view& in, cql_serialization_format sf) {
        return { in, sf };
    }
    static listlike_partial_deserializing_iterator end(bytes_view in, cql_serialization_format sf) {
        return { end_tag() };
    }
private:
    void parse() {
        if (_remain) {
            _cur = read_collection_value(*_in, _sf);
        } else {
            _cur = {};
        }
    }
};

set_type
set_type_impl::get_instance(data_type elements, bool is_multi_cell) {
    return intern::get_instance(elements, is_multi_cell);
}

sstring make_set_type_name(data_type elements, bool is_multi_cell)
{
    sstring ret = "";
    if (!is_multi_cell) {
        ret = "org.apache.cassandra.db.marshal.FrozenType(";
    }
    ret += "org.apache.cassandra.db.marshal.SetType(" + elements->name() + ")";
    if (!is_multi_cell) {
        ret += ")";
    }
    return ret;
}

set_type_impl::set_type_impl(data_type elements, bool is_multi_cell)
        : concrete_type(make_set_type_name(elements, is_multi_cell), kind::set)
        , _elements(std::move(elements))
        , _is_multi_cell(is_multi_cell) {
}

data_type
set_type_impl::value_comparator() const {
    return empty_type;
}

data_type
set_type_impl::freeze() const {
    if (_is_multi_cell) {
        return get_instance(_elements, false);
    } else {
        return shared_from_this();
    }
}

bool
set_type_impl::is_compatible_with_frozen(const collection_type_impl& previous) const {
    assert(!_is_multi_cell);
    auto* p = dynamic_cast<const set_type_impl*>(&previous);
    if (!p) {
        return false;
    }
    return _elements->is_compatible_with(*p->_elements);

}

bool
set_type_impl::is_value_compatible_with_frozen(const collection_type_impl& previous) const {
    return is_compatible_with(previous);
}

bool
set_type_impl::less(bytes_view o1, bytes_view o2) const {
    using llpdi = listlike_partial_deserializing_iterator;
    auto sf = cql_serialization_format::internal();
    return std::lexicographical_compare(
            llpdi::begin(o1, sf), llpdi::end(o1, sf),
            llpdi::begin(o2, sf), llpdi::end(o2, sf),
            [this] (bytes_view o1, bytes_view o2) { return _elements->less(o1, o2); });
}

void
set_type_impl::serialize(const void* value, bytes::iterator& out) const {
    return serialize(value, out, cql_serialization_format::internal());
}

size_t
set_type_impl::serialized_size(const void* value) const {
    auto& s = from_value(value);
    size_t len = collection_size_len(cql_serialization_format::internal());
    size_t psz = collection_value_len(cql_serialization_format::internal());
    for (auto&& e : s) {
        len += psz + _elements->serialized_size(_elements->get_value_ptr(e));
    }
    return len;
}



void
set_type_impl::serialize(const void* value, bytes::iterator& out, cql_serialization_format sf) const {
    auto& s = from_value(value);
    write_collection_size(out, s.size(), sf);
    for (auto&& e : s) {
        write_collection_value(out, sf, _elements, e);
    }
}

data_value
set_type_impl::deserialize(bytes_view in) const {
    return deserialize(in, cql_serialization_format::internal());
}

data_value
set_type_impl::deserialize(bytes_view in, cql_serialization_format sf) const {
    auto nr = read_collection_size(in, sf);
    native_type s;
    s.reserve(nr);
    for (int i = 0; i != nr; ++i) {
        auto e = _elements->deserialize(read_collection_value(in, sf));
        if (e.is_null()) {
            throw marshal_exception();
        }
        s.push_back(std::move(e));
    }
    return make_value(std::move(s));
}

sstring
set_type_impl::to_string(const bytes& b) const {
    using llpdi = listlike_partial_deserializing_iterator;
    std::ostringstream out;
    bool first = true;
    auto v = bytes_view(b);
    auto sf = cql_serialization_format::internal();
    std::for_each(llpdi::begin(v, sf), llpdi::end(v, sf), [&first, &out, this] (bytes_view e) {
        if (first) {
            first = false;
        } else {
            out << "; ";
        }
        out << _elements->to_string(bytes(e.begin(), e.end()));
    });
    return out.str();
}

size_t
set_type_impl::hash(bytes_view v) const {
    // FIXME:
    abort();
}

bytes
set_type_impl::from_string(sstring_view text) const {
    // FIXME:
    abort();
}

std::vector<bytes>
set_type_impl::serialized_values(std::vector<atomic_cell> cells) const {
    // FIXME:
    abort();
}

bytes
set_type_impl::to_value(mutation_view mut, cql_serialization_format sf) const {
    std::vector<bytes_view> tmp;
    tmp.reserve(mut.cells.size());
    for (auto&& e : mut.cells) {
        if (e.second.is_live(mut.tomb, false)) {
            tmp.emplace_back(e.first);
        }
    }
    return pack(tmp.begin(), tmp.end(), tmp.size(), sf);
}

bytes
set_type_impl::serialize_partially_deserialized_form(
        const std::vector<bytes_view>& v, cql_serialization_format sf) const {
    return pack(v.begin(), v.end(), v.size(), sf);
}

sstring
set_type_impl::cql3_type_name() const {
    return sprint("set<%s>", _elements->as_cql3_type());
}

bool
set_type_impl::references_user_type(const sstring& keyspace, const bytes& name) const {
    return _elements->references_user_type(keyspace, name);
}

std::experimental::optional<data_type>
set_type_impl::update_user_type(const shared_ptr<const user_type_impl> updated) const {
    auto e = _elements->update_user_type(updated);
    if (e) {
        return std::experimental::make_optional(static_pointer_cast<const abstract_type>(
            get_instance(std::move(*e), _is_multi_cell)));
    }
    return std::experimental::nullopt;
}

list_type
list_type_impl::get_instance(data_type elements, bool is_multi_cell) {
    return intern::get_instance(elements, is_multi_cell);
}

sstring make_list_type_name(data_type elements, bool is_multi_cell)
{
    sstring ret = "";
    if (!is_multi_cell) {
        ret = "org.apache.cassandra.db.marshal.FrozenType(";
    }
    ret += "org.apache.cassandra.db.marshal.ListType(" + elements->name() + ")";
    if (!is_multi_cell) {
        ret += ")";
    }
    return ret;
}

list_type_impl::list_type_impl(data_type elements, bool is_multi_cell)
        : concrete_type(make_list_type_name(elements, is_multi_cell), kind::list)
        , _elements(std::move(elements))
        , _is_multi_cell(is_multi_cell) {
}

data_type
list_type_impl::name_comparator() const {
    return timeuuid_type;
}

data_type
list_type_impl::value_comparator() const {
    return _elements;
}

data_type
list_type_impl::freeze() const {
    if (_is_multi_cell) {
        return get_instance(_elements, false);
    } else {
        return shared_from_this();
    }
}

bool
list_type_impl::is_compatible_with_frozen(const collection_type_impl& previous) const {
    assert(!_is_multi_cell);
    auto* p = dynamic_cast<const list_type_impl*>(&previous);
    if (!p) {
        return false;
    }
    return _elements->is_compatible_with(*p->_elements);

}

bool
list_type_impl::is_value_compatible_with_frozen(const collection_type_impl& previous) const {
    auto& lp = dynamic_cast<const list_type_impl&>(previous);
    return _elements->is_value_compatible_with_internal(*lp._elements);
}

bool
list_type_impl::less(bytes_view o1, bytes_view o2) const {
    using llpdi = listlike_partial_deserializing_iterator;
    auto sf = cql_serialization_format::internal();
    return std::lexicographical_compare(
            llpdi::begin(o1, sf), llpdi::end(o1, sf),
            llpdi::begin(o2, sf), llpdi::end(o2, sf),
            [this] (bytes_view o1, bytes_view o2) { return _elements->less(o1, o2); });
}

void
list_type_impl::serialize(const void* value, bytes::iterator& out) const {
    return serialize(value, out, cql_serialization_format::internal());
}

void
list_type_impl::serialize(const void* value, bytes::iterator& out, cql_serialization_format sf) const {
    auto& s = from_value(value);
    write_collection_size(out, s.size(), sf);
    for (auto&& e : s) {
        write_collection_value(out, sf, _elements, e);
    }
}

size_t
list_type_impl::serialized_size(const void* value) const {
    auto& s = from_value(value);
    size_t len = collection_size_len(cql_serialization_format::internal());
    size_t psz = collection_value_len(cql_serialization_format::internal());
    for (auto&& e : s) {
        len += psz + _elements->serialized_size(_elements->get_value_ptr(e));
    }
    return len;
}

data_value
list_type_impl::deserialize(bytes_view in) const {
    return deserialize(in, cql_serialization_format::internal());
}

data_value
list_type_impl::deserialize(bytes_view in, cql_serialization_format sf) const {
    auto nr = read_collection_size(in, sf);
    native_type s;
    s.reserve(nr);
    for (int i = 0; i != nr; ++i) {
        auto e = _elements->deserialize(read_collection_value(in, sf));
        if (e.is_null()) {
            throw marshal_exception();
        }
        s.push_back(std::move(e));
    }
    return make_value(std::move(s));
}

sstring
list_type_impl::to_string(const bytes& b) const {
    using llpdi = listlike_partial_deserializing_iterator;
    std::ostringstream out;
    bool first = true;
    auto v = bytes_view(b);
    auto sf = cql_serialization_format::internal();
    std::for_each(llpdi::begin(v, sf), llpdi::end(v, sf), [&first, &out, this] (bytes_view e) {
        if (first) {
            first = false;
        } else {
            out << ", ";
        }
        out << _elements->to_string(bytes(e.begin(), e.end()));
    });
    return out.str();
}

size_t
list_type_impl::hash(bytes_view v) const {
    // FIXME:
    abort();
}

bytes
list_type_impl::from_string(sstring_view text) const {
    // FIXME:
    abort();
}

std::vector<bytes>
list_type_impl::serialized_values(std::vector<atomic_cell> cells) const {
    // FIXME:
    abort();
}

bytes
list_type_impl::to_value(mutation_view mut, cql_serialization_format sf) const {
    std::vector<bytes_view> tmp;
    tmp.reserve(mut.cells.size());
    for (auto&& e : mut.cells) {
        if (e.second.is_live(mut.tomb, false)) {
            tmp.emplace_back(e.second.value());
        }
    }
    return pack(tmp.begin(), tmp.end(), tmp.size(), sf);
}

sstring
list_type_impl::cql3_type_name() const {
    return sprint("list<%s>", _elements->as_cql3_type());
}

bool
list_type_impl::references_user_type(const sstring& keyspace, const bytes& name) const {
    return _elements->references_user_type(keyspace, name);
}

std::experimental::optional<data_type>
list_type_impl::update_user_type(const shared_ptr<const user_type_impl> updated) const {
    auto e = _elements->update_user_type(updated);
    if (e) {
        return std::experimental::make_optional(static_pointer_cast<const abstract_type>(
            get_instance(std::move(*e), _is_multi_cell)));
    }
    return std::experimental::nullopt;
}

tuple_type_impl::tuple_type_impl(sstring name, std::vector<data_type> types)
        : concrete_type(std::move(name)), _types(std::move(types)) {
    for (auto& t : _types) {
        t = t->freeze();
    }
}

tuple_type_impl::tuple_type_impl(std::vector<data_type> types)
        : concrete_type(make_name(types)), _types(std::move(types)) {
    for (auto& t : _types) {
        t = t->freeze();
    }
}

shared_ptr<tuple_type_impl>
tuple_type_impl::get_instance(std::vector<data_type> types) {
    return ::make_shared<tuple_type_impl>(std::move(types));
}

int32_t
tuple_type_impl::compare(bytes_view v1, bytes_view v2) const {
    return lexicographical_tri_compare(_types.begin(), _types.end(),
            tuple_deserializing_iterator::start(v1), tuple_deserializing_iterator::finish(v1),
            tuple_deserializing_iterator::start(v2), tuple_deserializing_iterator::finish(v2),
            tri_compare_opt);
}

bool
tuple_type_impl::less(bytes_view v1, bytes_view v2) const {
    return tuple_type_impl::compare(v1, v2) < 0;
}

size_t
tuple_type_impl::serialized_size(const void* value) const {
    size_t size = 0;
    if (!value) {
        return size;
    }
    auto& v = from_value(value);
    auto find_serialized_size = [] (auto&& t_v) {
        const data_type& t = boost::get<0>(t_v);
        const data_value& v = boost::get<1>(t_v);
        if (!v.is_null() && t != v.type()) {
            throw std::runtime_error("tuple element type mismatch");
        }
        return 4 + (v.is_null() ? 0 : t->serialized_size(t->get_value_ptr(v)));
    };
    return boost::accumulate(boost::combine(_types, v) | boost::adaptors::transformed(find_serialized_size), 0);
}

void
tuple_type_impl::serialize(const void* value, bytes::iterator& out) const {
    if (!value) {
        return;
    }
    auto& v = from_value(value);
    auto do_serialize = [&out] (auto&& t_v) {
        const data_type& t = boost::get<0>(t_v);
        const data_value& v = boost::get<1>(t_v);
        if (!v.is_null() && t != v.type()) {
            throw std::runtime_error("tuple element type mismatch");
        }
        if (v.is_null()) {
            write(out, int32_t(-1));
        } else {
            write(out, int32_t(t->serialized_size(t->get_value_ptr(v))));
            t->serialize(t->get_value_ptr(v), out);
        }
    };
    boost::range::for_each(boost::combine(_types, v), do_serialize);
}

data_value
tuple_type_impl::deserialize(bytes_view v) const {
    native_type ret;
    ret.reserve(_types.size());
    auto ti = _types.begin();
    auto vi = tuple_deserializing_iterator::start(v);
    while (ti != _types.end() && vi != tuple_deserializing_iterator::finish(v)) {
        data_value obj = data_value::make_null(*ti);
        if (*vi) {
            obj = (*ti)->deserialize(**vi);
        }
        ret.push_back(std::move(obj));
        ++ti;
        ++vi;
    }
    while (ti != _types.end()) {
        ret.push_back(data_value::make_null(*ti++));
    }
    return make_value(std::move(ret));
}

std::vector<bytes_view_opt>
tuple_type_impl::split(bytes_view v) const {
    return { tuple_deserializing_iterator::start(v), tuple_deserializing_iterator::finish(v) };
}

// Count number of ':' which are not preceded by '\'.
static std::size_t count_segments(sstring_view v) {
    std::size_t segment_count = 1;
    char prev_ch = '.';
    for (char ch : v) {
        if (ch == ':' && prev_ch != '\\') {
            ++segment_count;
        }
        prev_ch = ch;
    }
    return segment_count;
}

// Split on ':', unless it's preceded by '\'.
static std::vector<sstring_view> split_field_strings(sstring_view v) {
    if (v.empty()) {
        return std::vector<sstring_view>();
    }
    std::vector<sstring_view> result;
    result.reserve(count_segments(v));
    std::size_t prev = 0;
    char prev_ch = '.';
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == ':' && prev_ch != '\\') {
            result.push_back(v.substr(prev, i - prev));
            prev = i + 1;
        }
        prev_ch = v[i];
    }
    result.push_back(v.substr(prev, v.size() - prev));
    return std::move(result);
}

// Replace "\:" with ":" and "\@" with "@".
static std::string unescape(sstring_view s) {
    static thread_local std::regex escaped_colon_re("\\\\:");
    static thread_local std::regex escaped_at_re("\\\\@");
    std::string result = s.to_string();
    result = std::regex_replace(result, escaped_colon_re, ":");
    result = std::regex_replace(result, escaped_at_re, "@");
    return std::move(result);
}

// Concat list of bytes into a single bytes.
static bytes concat_fields(const std::vector<bytes>& fields, const std::vector<int32_t> field_len) {
    std::size_t result_size = 4 * fields.size();
    for (int32_t len : field_len) {
        result_size += len > 0 ? len : 0;
    }
    bytes result{bytes::initialized_later(), result_size};
    bytes::iterator it = result.begin();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        int32_t tmp = net::hton(field_len[i]);
        it = std::copy_n(reinterpret_cast<const int8_t*>(&tmp), sizeof(tmp), it);
        if (field_len[i] > 0) {
            it = std::copy(std::begin(fields[i]), std::end(fields[i]), it);
        }
    }
    return std::move(result);
}

bytes
tuple_type_impl::from_string(sstring_view s) const {
    std::vector<sstring_view> field_strings = split_field_strings(s);
    if (field_strings.size() > size()) {
        throw marshal_exception(sprint("Invalid tuple literal: too many elements. Type %s expects %d but got %d", as_cql3_type(), size(), field_strings.size()));
    }
    std::vector<bytes> fields(field_strings.size());
    std::vector<int32_t> field_len(field_strings.size(), -1);
    for (std::size_t i = 0; i < field_strings.size(); ++i) {
        if (field_strings[i] != "@") {
            std::string field_string = unescape(field_strings[i]);
            fields[i] = type(i)->from_string(field_string);
            field_len[i] = fields[i].size();
        }
    }
    return std::move(concat_fields(fields, field_len));
}

sstring
tuple_type_impl::to_string(const bytes& b) const {
    throw std::runtime_error(sprint("%s not implemented", __PRETTY_FUNCTION__));
}

bool
tuple_type_impl::equals(const abstract_type& other) const {
    auto x = dynamic_cast<const tuple_type_impl*>(&other);
    return x && std::equal(_types.begin(), _types.end(), x->_types.begin(), x->_types.end(),
            [] (auto&& a, auto&& b) { return a->equals(b); });
}

bool
tuple_type_impl::is_compatible_with(const abstract_type& previous) const {
    return check_compatibility(previous, &abstract_type::is_compatible_with);
}

bool
tuple_type_impl::is_value_compatible_with_internal(const abstract_type& previous) const {
    return check_compatibility(previous, &abstract_type::is_value_compatible_with);
}

bool
tuple_type_impl::check_compatibility(const abstract_type& previous, bool (abstract_type::*predicate)(const abstract_type&) const) const {
    auto* x = dynamic_cast<const tuple_type_impl*>(&previous);
    if (!x) {
        return false;
    }
    auto c = std::mismatch(
                _types.begin(), _types.end(),
                x->_types.begin(), x->_types.end(),
                [predicate] (data_type a, data_type b) { return ((*a).*predicate)(*b); });
    return c.second == x->_types.end();  // this allowed to be longer
}

size_t
tuple_type_impl::hash(bytes_view v) const {
    auto apply_hash = [] (auto&& type_value) {
        auto&& type = boost::get<0>(type_value);
        auto&& value = boost::get<1>(type_value);
        return value ? type->hash(*value) : 0;
    };
    // FIXME: better accumulation function
    return boost::accumulate(combine(_types, make_range(v))
                             | boost::adaptors::transformed(apply_hash),
                             0,
                             std::bit_xor<>());
}

shared_ptr<cql3::cql3_type>
tuple_type_impl::as_cql3_type() const {
    return cql3::make_cql3_tuple_type(static_pointer_cast<const tuple_type_impl>(shared_from_this()));
}

sstring
tuple_type_impl::make_name(const std::vector<data_type>& types) {
    return sprint("org.apache.cassandra.db.marshal.TupleType(%s)", ::join(", ", types | boost::adaptors::transformed(std::mem_fn(&abstract_type::name))));
}

bool
tuple_type_impl::references_user_type(const sstring& keyspace, const bytes& name) const {
    return std::any_of(_types.begin(), _types.end(), [&](auto&& dt) { return dt->references_user_type(keyspace, name); });
}

static std::experimental::optional<std::vector<data_type>>
update_types(const std::vector<data_type> types, const user_type updated) {
    std::experimental::optional<std::vector<data_type>> new_types = std::experimental::nullopt;
    for (uint32_t i = 0; i < types.size(); ++i) {
        auto&& ut = types[i]->update_user_type(updated);
        if (ut) {
            if (!new_types) {
                new_types = types;
            }
            (*new_types)[i] = std::move(*ut);
        }
    }
    return new_types;
}

std::experimental::optional<data_type>
tuple_type_impl::update_user_type(const shared_ptr<const user_type_impl> updated) const {
    auto new_types = update_types(_types, updated);
    if (new_types) {
        return std::experimental::make_optional(static_pointer_cast<const abstract_type>(
            get_instance(std::move(*new_types))));
    }
    return std::experimental::nullopt;
}

sstring
user_type_impl::get_name_as_string() const {
    auto real_utf8_type = static_cast<const utf8_type_impl*>(utf8_type.get());
    return real_utf8_type->from_value(utf8_type->deserialize(_name));
}

shared_ptr<cql3::cql3_type>
user_type_impl::as_cql3_type() const {
    return make_shared<cql3::cql3_type>(get_name_as_string(), shared_from_this(), false);
}

sstring
user_type_impl::make_name(sstring keyspace, bytes name, std::vector<bytes> field_names, std::vector<data_type> field_types) {
    std::ostringstream os;
    os << "org.apache.cassandra.db.marshal.UserType(" << keyspace << "," << to_hex(name);
    for (size_t i = 0; i < field_names.size(); ++i) {
        os << ",";
        os << to_hex(field_names[i]) << ":";
        os << field_types[i]->name(); // FIXME: ignore frozen<>
    }
    os << ")";
    return os.str();
}

bool
user_type_impl::equals(const abstract_type& other) const {
    auto x = dynamic_cast<const user_type_impl*>(&other);
    return x
        && _keyspace == x->_keyspace
        && _name == x->_name
        && std::equal(_field_names.begin(), _field_names.end(), x->_field_names.begin(), x->_field_names.end())
        && tuple_type_impl::equals(other);
}

bool
user_type_impl::references_user_type(const sstring& keyspace, const bytes& name) const {
    return (_keyspace == keyspace && _name == name)
        || tuple_type_impl::references_user_type(keyspace, name);
}

std::experimental::optional<data_type>
user_type_impl::update_user_type(const shared_ptr<const user_type_impl> updated) const {
    if (_keyspace == updated->_keyspace && _name == updated->_name) {
        return std::experimental::make_optional(static_pointer_cast<const abstract_type>(updated));
    }
    auto new_types = update_types(_types, updated);
    if (new_types) {
        return std::experimental::make_optional(static_pointer_cast<const abstract_type>(
            get_instance(_keyspace, _name, _field_names, *new_types)));
    }
    return std::experimental::nullopt;
}

size_t
reversed_type_impl::native_value_size() const {
    return _underlying_type->native_value_size();
}

size_t
reversed_type_impl::native_value_alignment() const {
    return _underlying_type->native_value_alignment();
}

void
reversed_type_impl::native_value_copy(const void* from, void* to) const {
    return _underlying_type->native_value_copy(from, to);
}

void
reversed_type_impl::native_value_move(void* from, void* to) const {
    return _underlying_type->native_value_move(from, to);
}

void
reversed_type_impl::native_value_destroy(void* object) const {
    return _underlying_type->native_value_destroy(object);
}

void*
reversed_type_impl::native_value_clone(const void* object) const {
    return _underlying_type->native_value_clone(object);
}

void
reversed_type_impl::native_value_delete(void* object) const {
    return _underlying_type->native_value_delete(object);
}

const std::type_info&
reversed_type_impl::native_typeid() const {
    return _underlying_type->native_typeid();
}

thread_local const shared_ptr<const abstract_type> byte_type(make_shared<byte_type_impl>());
thread_local const shared_ptr<const abstract_type> short_type(make_shared<short_type_impl>());
thread_local const shared_ptr<const abstract_type> int32_type(make_shared<int32_type_impl>());
thread_local const shared_ptr<const abstract_type> long_type(make_shared<long_type_impl>());
thread_local const shared_ptr<const abstract_type> ascii_type(make_shared<ascii_type_impl>());
thread_local const shared_ptr<const abstract_type> bytes_type(make_shared<bytes_type_impl>());
thread_local const shared_ptr<const abstract_type> utf8_type(make_shared<utf8_type_impl>());
thread_local const shared_ptr<const abstract_type> boolean_type(make_shared<boolean_type_impl>());
thread_local const shared_ptr<const abstract_type> date_type(make_shared<date_type_impl>());
thread_local const shared_ptr<const abstract_type> timeuuid_type(make_shared<timeuuid_type_impl>());
thread_local const shared_ptr<const abstract_type> timestamp_type(make_shared<timestamp_type_impl>());
thread_local const shared_ptr<const abstract_type> simple_date_type(make_shared<simple_date_type_impl>());
thread_local const shared_ptr<const abstract_type> time_type(make_shared<time_type_impl>());
thread_local const shared_ptr<const abstract_type> uuid_type(make_shared<uuid_type_impl>());
thread_local const shared_ptr<const abstract_type> inet_addr_type(make_shared<inet_addr_type_impl>());
thread_local const shared_ptr<const abstract_type> float_type(make_shared<float_type_impl>());
thread_local const shared_ptr<const abstract_type> double_type(make_shared<double_type_impl>());
thread_local const shared_ptr<const abstract_type> varint_type(make_shared<varint_type_impl>());
thread_local const shared_ptr<const abstract_type> decimal_type(make_shared<decimal_type_impl>());
thread_local const shared_ptr<const abstract_type> counter_type(make_shared<counter_type_impl>());
thread_local const data_type empty_type(make_shared<empty_type_impl>());

data_type abstract_type::parse_type(const sstring& name)
{
    static thread_local const std::unordered_map<sstring, data_type> types = {
        { byte_type_name,      byte_type      },
        { short_type_name,     short_type     },
        { int32_type_name,     int32_type     },
        { long_type_name,      long_type      },
        { ascii_type_name,     ascii_type     },
        { bytes_type_name,     bytes_type     },
        { utf8_type_name,      utf8_type      },
        { boolean_type_name,   boolean_type   },
        { date_type_name,      date_type      },
        { timeuuid_type_name,  timeuuid_type  },
        { timestamp_type_name, timestamp_type },
        { simple_date_type_name, simple_date_type },
        { time_type_name,      time_type },
        { uuid_type_name,      uuid_type      },
        { inet_addr_type_name, inet_addr_type },
        { float_type_name,     float_type     },
        { double_type_name,    double_type    },
        { varint_type_name,    varint_type    },
        { decimal_type_name,   decimal_type   },
        { counter_type_name,   counter_type   },
        { empty_type_name,     empty_type     },
    };
    auto it = types.find(name);
    if (it == types.end()) {
        throw std::invalid_argument(sprint("unknown type: %s\n", name));
    }
    return it->second;
}

data_value::~data_value() {
    if (_value) {
        _type->native_value_delete(_value);
    }
}

data_value::data_value(const data_value& v) : _value(nullptr), _type(v._type) {
    if (v._value) {
        _value = _type->native_value_clone(v._value);
    }
}

data_value&
data_value::operator=(data_value&& x) {
    auto tmp = std::move(x);
    std::swap(tmp._value, this->_value);
    std::swap(tmp._type, this->_type);
    return *this;
}

data_value::data_value(bytes v) : data_value(make_new(bytes_type, v)) {
}

data_value::data_value(sstring v) : data_value(make_new(utf8_type, v)) {
}

data_value::data_value(const char* v) : data_value(make_new(utf8_type, sstring(v))) {
}

data_value::data_value(bool v) : data_value(make_new(boolean_type, v)) {
}

data_value::data_value(int8_t v) : data_value(make_new(byte_type, v)) {
}

data_value::data_value(int16_t v) : data_value(make_new(short_type, v)) {
}

data_value::data_value(int32_t v) : data_value(make_new(int32_type, v)) {
}

data_value::data_value(int64_t v) : data_value(make_new(long_type, v)) {
}

data_value::data_value(utils::UUID v) : data_value(make_new(uuid_type, v)) {
}

data_value::data_value(float v) : data_value(make_new(float_type, v)) {
}

data_value::data_value(double v) : data_value(make_new(double_type, v)) {
}

data_value::data_value(net::ipv4_address v) : data_value(make_new(inet_addr_type, v)) {
}

data_value::data_value(db_clock::time_point v) : data_value(make_new(date_type, v)) {
}

data_value::data_value(boost::multiprecision::cpp_int v) : data_value(make_new(varint_type, v)) {
}

data_value::data_value(big_decimal v) : data_value(make_new(decimal_type, v)) {
}

data_value
make_list_value(data_type type, list_type_impl::native_type value) {
    return data_value::make_new(std::move(type), std::move(value));
}

data_value
make_set_value(data_type type, set_type_impl::native_type value) {
    return data_value::make_new(std::move(type), std::move(value));
}

data_value
make_map_value(data_type type, map_type_impl::native_type value) {
    return data_value::make_new(std::move(type), std::move(value));
}

data_value
make_tuple_value(data_type type, tuple_type_impl::native_type value) {
    return data_value::make_new(std::move(type), std::move(value));
}

data_value
make_user_value(data_type type, user_type_impl::native_type value) {
    return data_value::make_new(std::move(type), std::move(value));
}

bytes
date_type_impl::from_string(sstring_view s) const {
    native_type n = db_clock::time_point(db_clock::duration(timestamp_type_impl::timestamp_from_string(s)));
    bytes ret(bytes::initialized_later(), serialized_size(&n));
    auto iter = ret.begin();
    serialize(&n, iter);
    return ret;
}

std::ostream& operator<<(std::ostream& out, const data_value& v) {
    if (v.is_null()) {
        return out << "null";
    }
    bytes b(bytes::initialized_later(), v.serialized_size());
    auto i = b.begin();
    v.serialize(i);
    return out << v.type()->to_string(b);
}
