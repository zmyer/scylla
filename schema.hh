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

#pragma once

#include <unordered_map>
#include <boost/range/iterator_range.hpp>
#include <boost/range/join.hpp>
#include <boost/range/algorithm/transform.hpp>
#include <boost/lexical_cast.hpp>

#include "cql3/column_specification.hh"
#include "core/shared_ptr.hh"
#include "types.hh"
#include "compound.hh"
#include "gc_clock.hh"
#include "unimplemented.hh"
#include "utils/UUID.hh"
#include "compress.hh"
#include "compaction_strategy.hh"
#include "caching_options.hh"
#include "stdx.hh"

using column_count_type = uint32_t;

// Column ID, unique within column_kind
using column_id = column_count_type;

// Cluster-wide identifier of schema version of particular table.
//
// The version changes the value not only on structural changes but also
// temporal. For example, schemas with the same set of columns but created at
// different times should have different versions. This allows nodes to detect
// if the version they see was already synchronized with or not even if it has
// the same structure as the past versions.
//
// Schema changes merged in any order should result in the same final version.
//
// When table_schema_version changes, schema_tables::calculate_schema_digest() should
// also change when schema mutations are applied.
using table_schema_version = utils::UUID;

class schema;
class schema_registry_entry;
class schema_builder;

// Useful functions to manipulate the schema's comparator field
namespace cell_comparator {
sstring to_sstring(const schema& s);
bool check_compound(sstring comparator);
void read_collections(schema_builder& builder, sstring comparator);
}

// make sure these match the order we like columns back from schema
enum class column_kind { partition_key, clustering_key, static_column, regular_column };

sstring to_sstring(column_kind k);
bool is_compatible(column_kind k1, column_kind k2);

// CMH this is also manually defined in thrift gen file.
enum class index_type {
    keys,
    custom,
    composites,
    none, // cwi: added none to avoid "optional" bs.
};

sstring to_sstring(index_type t);

enum class cf_type : uint8_t {
    standard,
    super,
};

inline sstring cf_type_to_sstring(cf_type t) {
    if (t == cf_type::standard) {
        return "Standard";
    } else if (t == cf_type::super) {
        return "Super";
    }
    throw std::invalid_argument(sprint("unknown type: %d\n", uint8_t(t)));
}

inline cf_type sstring_to_cf_type(sstring name) {
    if (name == "Standard") {
        return cf_type::standard;
    } else if (name == "Super") {
        return cf_type::super;
    }
    throw std::invalid_argument(sprint("unknown type: %s\n", name));
}

struct speculative_retry {
    enum class type {
        NONE, CUSTOM, PERCENTILE, ALWAYS
    };
private:
    type _t;
    double _v;
public:
    speculative_retry(type t, double v) : _t(t), _v(v) {}

    sstring to_sstring() const {
        if (_t == type::NONE) {
            return "NONE";
        } else if (_t == type::ALWAYS) {
            return "ALWAYS";
        } else if (_t == type::CUSTOM) {
            return sprint("%.2fms", _v);
        } else if (_t == type::PERCENTILE) {
            return sprint("%.1fPERCENTILE", 100 * _v);
        } else {
            throw std::invalid_argument(sprint("unknown type: %d\n", uint8_t(_t)));
        }
    }
    static speculative_retry from_sstring(sstring str) {
        std::transform(str.begin(), str.end(), str.begin(), ::toupper);

        sstring ms("MS");
        sstring percentile("PERCENTILE");

        auto convert = [&str] (sstring& t) {
            try {
                return boost::lexical_cast<double>(str.substr(0, str.size() - t.size()));
            } catch (boost::bad_lexical_cast& e) {
                throw std::invalid_argument(sprint("cannot convert %s to speculative_retry\n", str));
            }
        };

        type t;
        double v = 0;
        if (str == "NONE") {
            t = type::NONE;
        } else if (str == "ALWAYS") {
            t = type::ALWAYS;
        } else if (str.compare(str.size() - ms.size(), ms.size(), ms) == 0) {
            t = type::CUSTOM;
            v = convert(ms);
        } else if (str.compare(str.size() - percentile.size(), percentile.size(), percentile) == 0) {
            t = type::PERCENTILE;
            v = convert(percentile) / 100;
        } else {
            throw std::invalid_argument(sprint("cannot convert %s to speculative_retry\n", str));
        }
        return speculative_retry(t, v);
    }
    type get_type() const {
        return _t;
    }
    double get_value() const {
        return _v;
    }
    bool operator==(const speculative_retry& other) const {
        return _t == other._t && _v == other._v;
    }
    bool operator!=(const speculative_retry& other) const {
        return !(*this == other);
    }
};

typedef std::unordered_map<sstring, sstring> index_options_map;

struct index_info {
    index_info(::index_type = ::index_type::none
            , std::experimental::optional<sstring> index_name = std::experimental::optional<sstring>()
            , std::experimental::optional<index_options_map> = std::experimental::optional<index_options_map>());

    enum index_type index_type = ::index_type::none;
    std::experimental::optional<sstring> index_name;
    std::experimental::optional<index_options_map> index_options;
};

class column_definition final {
public:
    struct name_comparator {
        bool operator()(const column_definition& d1, const column_definition& d2) const {
            return d1.name() < d2.name();
        }
    };
private:
    bytes _name;
    api::timestamp_type _dropped_at;
    bool _is_atomic;
    bool _is_counter;

    struct thrift_bits {
        thrift_bits()
            : is_on_all_components(0)
        {}
        uint8_t is_on_all_components : 1;
        // more...?
    };

    thrift_bits _thrift_bits;
    friend class schema;
public:
    column_definition(bytes name, data_type type, column_kind kind,
        column_id component_index = 0, index_info = index_info(),
        api::timestamp_type dropped_at = api::missing_timestamp);

    data_type type;

    // Unique within (kind, schema instance).
    // schema::position() and component_index() depend on the fact that for PK columns this is
    // equivalent to component index.
    column_id id;

    column_kind kind;
    ::shared_ptr<cql3::column_specification> column_specification;
    index_info idx_info;

    bool is_static() const { return kind == column_kind::static_column; }
    bool is_regular() const { return kind == column_kind::regular_column; }
    bool is_partition_key() const { return kind == column_kind::partition_key; }
    bool is_clustering_key() const { return kind == column_kind::clustering_key; }
    bool is_primary_key() const { return kind == column_kind::partition_key || kind == column_kind::clustering_key; }
    bool is_atomic() const { return _is_atomic; }
    bool is_counter() const { return _is_counter; }
    const sstring& name_as_text() const;
    const bytes& name() const;
    friend std::ostream& operator<<(std::ostream& os, const column_definition& cd);
    friend std::ostream& operator<<(std::ostream& os, const column_definition* cd) {
        return cd != nullptr ? os << *cd : os << "(null)";
    }
    bool has_component_index() const {
        return is_primary_key();
    }
    uint32_t component_index() const {
        assert(has_component_index());
        return id;
    }
    uint32_t position() const {
        if (has_component_index()) {
            return component_index();
        }
        return 0;
    }
    bool is_on_all_components() const;
    bool is_indexed() const {
        return idx_info.index_type != index_type::none;
    }
    bool is_part_of_cell_name() const {
        return is_regular() || is_static();
    }
    api::timestamp_type dropped_at() const { return _dropped_at; }
    friend bool operator==(const column_definition&, const column_definition&);
};

class schema_builder;

/*
 * Sub-schema for thrift aspects. Should be kept isolated (and starved)
 */
class thrift_schema {
    bool _compound = true;
    bool _is_dynamic = false;
public:
    bool has_compound_comparator() const;
    bool is_dynamic() const;
    friend class schema;
};

bool operator==(const column_definition&, const column_definition&);

static constexpr int DEFAULT_MIN_COMPACTION_THRESHOLD = 4;
static constexpr int DEFAULT_MAX_COMPACTION_THRESHOLD = 32;
static constexpr int DEFAULT_MIN_INDEX_INTERVAL = 128;
static constexpr int DEFAULT_GC_GRACE_SECONDS = 864000;

// Unsafe to access across shards.
// Safe to copy across shards.
class column_mapping_entry {
    bytes _name;
    data_type _type;
public:
    column_mapping_entry(bytes name, data_type type)
        : _name(std::move(name)), _type(std::move(type)) { }
    column_mapping_entry(bytes name, sstring type_name);
    column_mapping_entry(const column_mapping_entry&);
    column_mapping_entry& operator=(const column_mapping_entry&);
    column_mapping_entry(column_mapping_entry&&) = default;
    column_mapping_entry& operator=(column_mapping_entry&&) = default;
    const bytes& name() const { return _name; }
    const data_type& type() const { return _type; }
    const sstring& type_name() const { return _type->name(); }
};

// Encapsulates information needed for converting mutations between different schema versions.
//
// Unsafe to access across shards.
// Safe to copy across shards.
class column_mapping {
private:
    // Contains _n_static definitions for static columns followed by definitions for regular columns,
    // both ordered by consecutive column_ids.
    // Primary key column sets are not mutable so we don't need to map them.
    std::vector<column_mapping_entry> _columns;
    column_count_type _n_static = 0;
public:
    column_mapping() {}
    column_mapping(std::vector<column_mapping_entry> columns, column_count_type n_static)
            : _columns(std::move(columns))
            , _n_static(n_static)
    { }
    const std::vector<column_mapping_entry>& columns() const { return _columns; }
    column_count_type n_static() const { return _n_static; }
    const column_mapping_entry& column_at(column_kind kind, column_id id) const {
        assert(kind == column_kind::regular_column || kind == column_kind::static_column);
        return kind == column_kind::regular_column ? regular_column_at(id) : static_column_at(id);
    }
    const column_mapping_entry& static_column_at(column_id id) const {
        if (id >= _n_static) {
            throw std::out_of_range(sprint("static column id %d >= %d", id, _n_static));
        }
        return _columns[id];
    }
    const column_mapping_entry& regular_column_at(column_id id) const {
        auto n_regular = _columns.size() - _n_static;
        if (id >= n_regular) {
            throw std::out_of_range(sprint("regular column id %d >= %d", id, n_regular));
        }
        return _columns[id + _n_static];
    }
    friend std::ostream& operator<<(std::ostream& out, const column_mapping& cm);
};

/**
 * Augments a schema with fields related to materialized views.
 * Effectively immutable.
 */
class view_info final {
    utils::UUID _base_id;
    sstring _base_name;
    bool _include_all_columns;
    sstring _where_clause;
public:
    view_info(utils::UUID base_id, sstring base_name, bool include_all_columns, sstring where_clause);

    const utils::UUID& base_id() const {
        return _base_id;
    }

    const sstring& base_name() const {
        return _base_name;
    }

    bool include_all_columns() const {
        return _include_all_columns;
    }

    const sstring& where_clause() const {
        return _where_clause;
    }

    friend bool operator==(const view_info&, const view_info&);
    friend std::ostream& operator<<(std::ostream& os, const view_info& view);
};

bool operator==(const view_info&, const view_info&);
std::ostream& operator<<(std::ostream& os, const view_info& view);
/*
 * Effectively immutable.
 * Not safe to access across cores because of shared_ptr's.
 * Use global_schema_ptr for safe across-shard access.
 */
class schema final : public enable_lw_shared_from_this<schema> {
private:
    // More complex fields are derived from these inside rebuild().
    // Contains only fields which can be safely default-copied.
    struct raw_schema {
        raw_schema(utils::UUID id);
        utils::UUID _id;
        sstring _ks_name;
        sstring _cf_name;
        // regular columns are sorted by name
        // static columns are sorted by name, but present only when there's any clustering column
        std::vector<column_definition> _columns;
        sstring _comment;
        gc_clock::duration _default_time_to_live = gc_clock::duration::zero();
        data_type _default_validator = bytes_type;
        data_type _regular_column_name_type;
        double _bloom_filter_fp_chance = 0.01;
        compression_parameters _compressor_params;
        bool _is_dense = false;
        bool _is_compound = true;
        cf_type _type = cf_type::standard;
        int32_t _gc_grace_seconds = DEFAULT_GC_GRACE_SECONDS;
        double _dc_local_read_repair_chance = 0.1;
        double _read_repair_chance = 0.0;
        int32_t _min_compaction_threshold = DEFAULT_MIN_COMPACTION_THRESHOLD;
        int32_t _max_compaction_threshold = DEFAULT_MAX_COMPACTION_THRESHOLD;
        int32_t _min_index_interval = DEFAULT_MIN_INDEX_INTERVAL;
        int32_t _max_index_interval = 2048;
        int32_t _memtable_flush_period = 0;
        speculative_retry _speculative_retry = ::speculative_retry(speculative_retry::type::PERCENTILE, 0.99);
        // FIXME: SizeTiered doesn't really work yet. Being it marked here only means that this is the strategy
        // we will use by default - when we have the choice.
        sstables::compaction_strategy_type _compaction_strategy = sstables::compaction_strategy_type::size_tiered;
        std::map<sstring, sstring> _compaction_strategy_options;
        caching_options _caching_options;
        table_schema_version _version;
        std::unordered_map<sstring, api::timestamp_type> _dropped_columns;
        std::map<bytes, data_type> _collections;
        stdx::optional<view_info> _view_info;
    };
    raw_schema _raw;
    thrift_schema _thrift;
    mutable schema_registry_entry* _registry_entry = nullptr;

    const std::array<column_count_type, 3> _offsets;

    inline column_count_type column_offset(column_kind k) const {
        return k == column_kind::partition_key ? 0 : _offsets[column_count_type(k) - 1];
    }

    std::unordered_map<bytes, const column_definition*> _columns_by_name;
    std::map<bytes, const column_definition*, serialized_compare> _regular_columns_by_name;
    lw_shared_ptr<compound_type<allow_prefixes::no>> _partition_key_type;
    lw_shared_ptr<compound_type<allow_prefixes::yes>> _clustering_key_type;
    column_mapping _column_mapping;
    bool _is_counter = false;
    friend class schema_builder;
public:
    using row_column_ids_are_ordered_by_name = std::true_type;

    typedef std::vector<column_definition> columns_type;
    typedef typename columns_type::iterator iterator;
    typedef typename columns_type::const_iterator const_iterator;
    typedef boost::iterator_range<iterator> iterator_range_type;
    typedef boost::iterator_range<const_iterator> const_iterator_range_type;

    static constexpr int32_t NAME_LENGTH = 48;


    struct column {
        bytes name;
        data_type type;
        index_info idx_info;
    };
private:
    ::shared_ptr<cql3::column_specification> make_column_specification(const column_definition& def);
    void rebuild();
    schema(const raw_schema&);
public:
    // deprecated, use schema_builder.
    schema(std::experimental::optional<utils::UUID> id,
        sstring ks_name,
        sstring cf_name,
        std::vector<column> partition_key,
        std::vector<column> clustering_key,
        std::vector<column> regular_columns,
        std::vector<column> static_columns,
        data_type regular_column_name_type,
        sstring comment = {});
    schema(const schema&);
    ~schema();
    table_schema_version version() const {
        return _raw._version;
    }
    double bloom_filter_fp_chance() const {
        return _raw._bloom_filter_fp_chance;
    }
    sstring thrift_key_validator() const;
    const compression_parameters& get_compressor_params() const {
        return _raw._compressor_params;
    }
    bool is_dense() const {
        return _raw._is_dense;
    }

    bool is_compound() const {
        return _raw._is_compound;
    }

    bool is_cql3_table() const {
        return !is_super() && !is_dense() && is_compound();
    }

    thrift_schema& thrift() {
        return _thrift;
    }
    const thrift_schema& thrift() const {
        return _thrift;
    }
    const utils::UUID& id() const {
        return _raw._id;
    }
    const sstring& comment() const {
        return _raw._comment;
    }
    bool is_counter() const {
        return _is_counter;
    }

    const cf_type type() const {
        return _raw._type;
    }

    bool is_super() const {
        return _raw._type == cf_type::super;
    }

    gc_clock::duration gc_grace_seconds() const {
        auto seconds = std::chrono::seconds(_raw._gc_grace_seconds);
        return std::chrono::duration_cast<gc_clock::duration>(seconds);
    }

    double dc_local_read_repair_chance() const {
        return _raw._dc_local_read_repair_chance;
    }

    double read_repair_chance() const {
        return _raw._read_repair_chance;
    }

    int32_t min_compaction_threshold() const {
        return _raw._min_compaction_threshold;
    }

    int32_t max_compaction_threshold() const {
        return _raw._max_compaction_threshold;
    }

    int32_t min_index_interval() const {
        return _raw._min_index_interval;
    }

    int32_t max_index_interval() const {
        return _raw._max_index_interval;
    }

    int32_t memtable_flush_period() const {
        return _raw._memtable_flush_period;
    }

    sstables::compaction_strategy_type compaction_strategy() const {
        return _raw._compaction_strategy;
    }

    const std::map<sstring, sstring>& compaction_strategy_options() const {
        return _raw._compaction_strategy_options;
    }

    const ::speculative_retry& speculative_retry() const {
        return _raw._speculative_retry;
    }

    const ::caching_options& caching_options() const {
        return _raw._caching_options;
    }

    const column_definition* get_column_definition(const bytes& name) const;
    const column_definition& column_at(column_kind, column_id) const;
    const_iterator regular_begin() const;
    const_iterator regular_end() const;
    const_iterator regular_lower_bound(const bytes& name) const;
    const_iterator regular_upper_bound(const bytes& name) const;
    data_type column_name_type(const column_definition& def) const;
    const column_definition& regular_column_at(column_id id) const;
    const column_definition& static_column_at(column_id id) const;
    bool is_last_partition_key(const column_definition& def) const;
    bool has_multi_cell_collections() const;
    bool has_static_columns() const;
    column_count_type partition_key_size() const;
    column_count_type clustering_key_size() const;
    column_count_type static_columns_count() const;
    column_count_type regular_columns_count() const;
    // Returns a range of column definitions
    const_iterator_range_type partition_key_columns() const;
    // Returns a range of column definitions
    const_iterator_range_type clustering_key_columns() const;
    // Returns a range of column definitions
    const_iterator_range_type static_columns() const;
    // Returns a range of column definitions
    const_iterator_range_type regular_columns() const;
    // Returns a range of column definitions
    const columns_type& all_columns_in_select_order() const;
    uint32_t position(const column_definition& column) const;

    const auto& all_columns() const { return _columns_by_name; }

    const auto& dropped_columns() const {
        return _raw._dropped_columns;
    }

    const auto& collections() const {
        return _raw._collections;
    }

    gc_clock::duration default_time_to_live() const {
        return _raw._default_time_to_live;
    }
    const data_type& default_validator() const {
        return _raw._default_validator;
    }
    const sstring& ks_name() const {
        return _raw._ks_name;
    }
    const sstring& cf_name() const {
        return _raw._cf_name;
    }
    const lw_shared_ptr<compound_type<allow_prefixes::no>>& partition_key_type() const {
        return _partition_key_type;
    }
    const lw_shared_ptr<compound_type<allow_prefixes::yes>>& clustering_key_type() const {
        return _clustering_key_type;
    }
    const lw_shared_ptr<compound_type<allow_prefixes::yes>>& clustering_key_prefix_type() const {
        return _clustering_key_type;
    }
    const data_type& regular_column_name_type() const {
        return _raw._regular_column_name_type;
    }
    const stdx::optional<::view_info>& view_info() const {
        return _raw._view_info;
    }
    bool is_view() const {
        return bool(_raw._view_info);
    }
    friend std::ostream& operator<<(std::ostream& os, const schema& s);
    friend bool operator==(const schema&, const schema&);
    const column_mapping& get_column_mapping() const;
    friend class schema_registry_entry;
    // May be called from different shard
    schema_registry_entry* registry_entry() const noexcept;
    // Returns true iff this schema version was synced with on current node.
    // Schema version is said to be synced with when its mutations were merged
    // into current node's schema, so that current node's schema is at least as
    // recent as this version.
    bool is_synced() const;
    bool equal_columns(const schema&) const;
};

bool operator==(const schema&, const schema&);

using schema_ptr = lw_shared_ptr<const schema>;

/**
 * Wraper for schema_ptr used by functions that except an engaged view_info field.
 */
class view_ptr final {
    schema_ptr _schema;
public:
    explicit view_ptr(schema_ptr schema) noexcept : _schema(schema) {
        if (schema) {
            assert(_schema->is_view());
        }
    }

    const schema& operator*() const noexcept { return *_schema; }
    const schema* operator->() const noexcept { return _schema.operator->(); }
    const schema* get() const noexcept { return _schema.get(); }

    operator schema_ptr() const noexcept {
        return _schema;
    }

    explicit operator bool() const noexcept {
        return bool(_schema);
    }

    friend std::ostream& operator<<(std::ostream& os, const view_ptr& s);
};

std::ostream& operator<<(std::ostream& os, const view_ptr& view);

utils::UUID generate_legacy_id(const sstring& ks_name, const sstring& cf_name);
