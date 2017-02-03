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
 * Copyright (C) 2015 ScyllaDB
 *
 * Modified by ScyllaDB
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

#include "cql3/selection/selection.hh"
#include "cql3/selection/selector_factories.hh"
#include "cql3/result_set.hh"

namespace cql3 {

namespace selection {

selection::selection(schema_ptr schema,
    std::vector<const column_definition*> columns,
    std::vector<::shared_ptr<column_specification>> metadata_,
    bool collect_timestamps,
    bool collect_TTLs)
        : _schema(std::move(schema))
        , _columns(std::move(columns))
        , _metadata(::make_shared<metadata>(std::move(metadata_)))
        , _collect_timestamps(collect_timestamps)
        , _collect_TTLs(collect_TTLs)
        , _contains_static_columns(std::any_of(_columns.begin(), _columns.end(), std::mem_fn(&column_definition::is_static)))
{ }

query::partition_slice::option_set selection::get_query_options() {
    query::partition_slice::option_set opts;

    opts.set_if<query::partition_slice::option::send_timestamp>(_collect_timestamps);
    opts.set_if<query::partition_slice::option::send_expiry>(_collect_TTLs);

    opts.set_if<query::partition_slice::option::send_partition_key>(
        std::any_of(_columns.begin(), _columns.end(),
            std::mem_fn(&column_definition::is_partition_key)));

    opts.set_if<query::partition_slice::option::send_clustering_key>(
        std::any_of(_columns.begin(), _columns.end(),
            std::mem_fn(&column_definition::is_clustering_key)));

    return opts;
}

// Special cased selection for when no function is used (this save some allocations).
class simple_selection : public selection {
private:
    const bool _is_wildcard;
public:
    static ::shared_ptr<simple_selection> make(schema_ptr schema, std::vector<const column_definition*> columns, bool is_wildcard) {
        std::vector<::shared_ptr<column_specification>> metadata;
        metadata.reserve(columns.size());
        for (auto&& col : columns) {
            metadata.emplace_back(col->column_specification);
        }
        return ::make_shared<simple_selection>(schema, std::move(columns), std::move(metadata), is_wildcard);
    }

    /*
     * In theory, even a simple selection could have multiple time the same column, so we
     * could filter those duplicate out of columns. But since we're very unlikely to
     * get much duplicate in practice, it's more efficient not to bother.
     */
    simple_selection(schema_ptr schema, std::vector<const column_definition*> columns,
        std::vector<::shared_ptr<column_specification>> metadata, bool is_wildcard)
            : selection(schema, std::move(columns), std::move(metadata), false, false)
            , _is_wildcard(is_wildcard)
    { }

    virtual bool is_wildcard() const override { return _is_wildcard; }
    virtual bool is_aggregate() const override { return false; }
protected:
    class simple_selectors : public selectors {
    private:
        std::vector<bytes_opt> _current;
    public:
        virtual void reset() override {
            _current.clear();
        }

        virtual std::vector<bytes_opt> get_output_row(cql_serialization_format sf) override {
            return std::move(_current);
        }

        virtual void add_input_row(cql_serialization_format sf, result_set_builder& rs) override {
            _current = std::move(*rs.current);
        }

        virtual bool is_aggregate() {
            return false;
        }
    };

    std::unique_ptr<selectors> new_selectors() const override {
        return std::make_unique<simple_selectors>();
    }
};

class selection_with_processing : public selection {
private:
    ::shared_ptr<selector_factories> _factories;
public:
    selection_with_processing(schema_ptr schema, std::vector<const column_definition*> columns,
            std::vector<::shared_ptr<column_specification>> metadata, ::shared_ptr<selector_factories> factories)
        : selection(schema, std::move(columns), std::move(metadata),
            factories->contains_write_time_selector_factory(),
            factories->contains_ttl_selector_factory())
        , _factories(std::move(factories))
    {
        if (_factories->does_aggregation() && !_factories->contains_only_aggregate_functions()) {
            throw exceptions::invalid_request_exception("the select clause must either contains only aggregates or none");
        }
    }

    virtual bool uses_function(const sstring& ks_name, const sstring& function_name) const override {
        return _factories->uses_function(ks_name, function_name);
    }

    virtual uint32_t add_column_for_ordering(const column_definition& c) override {
        uint32_t index = selection::add_column_for_ordering(c);
        _factories->add_selector_for_ordering(c, index);
        return index;
    }

    virtual bool is_aggregate() const override {
        return _factories->contains_only_aggregate_functions();
    }
protected:
    class selectors_with_processing : public selectors {
    private:
        ::shared_ptr<selector_factories> _factories;
        std::vector<::shared_ptr<selector>> _selectors;
    public:
        selectors_with_processing(::shared_ptr<selector_factories> factories)
            : _factories(std::move(factories))
            , _selectors(_factories->new_instances())
        { }

        virtual void reset() override {
            for (auto&& s : _selectors) {
                s->reset();
            }
        }

        virtual bool is_aggregate() override {
            return _factories->contains_only_aggregate_functions();
        }

        virtual std::vector<bytes_opt> get_output_row(cql_serialization_format sf) override {
            std::vector<bytes_opt> output_row;
            output_row.reserve(_selectors.size());
            for (auto&& s : _selectors) {
                output_row.emplace_back(s->get_output(sf));
            }
            return output_row;
        }

        virtual void add_input_row(cql_serialization_format sf, result_set_builder& rs) {
            for (auto&& s : _selectors) {
                s->add_input(sf, rs);
            }
        }
    };

    std::unique_ptr<selectors> new_selectors() const override  {
        return std::make_unique<selectors_with_processing>(_factories);
    }
};

::shared_ptr<selection> selection::wildcard(schema_ptr schema) {
    std::vector<const column_definition*> cds;
    auto& columns = schema->all_columns_in_select_order();
    cds.reserve(columns.size());
    for (auto& c : columns) {
        if (!schema->is_dense() || !c.is_regular() || !c.name().empty()) {
            cds.emplace_back(&c);
        }
    }
    return simple_selection::make(schema, std::move(cds), true);
}

::shared_ptr<selection> selection::for_columns(schema_ptr schema, std::vector<const column_definition*> columns) {
    return simple_selection::make(schema, std::move(columns), false);
}

uint32_t selection::add_column_for_ordering(const column_definition& c) {
    _columns.push_back(&c);
    _metadata->add_non_serialized_column(c.column_specification);
    return _columns.size() - 1;
}

::shared_ptr<selection> selection::from_selectors(database& db, schema_ptr schema, const std::vector<::shared_ptr<raw_selector>>& raw_selectors) {
    std::vector<const column_definition*> defs;

    ::shared_ptr<selector_factories> factories =
        selector_factories::create_factories_and_collect_column_definitions(
            raw_selector::to_selectables(raw_selectors, schema), db, schema, defs);

    auto metadata = collect_metadata(schema, raw_selectors, *factories);
    if (processes_selection(raw_selectors) || raw_selectors.size() != defs.size()) {
        return ::make_shared<selection_with_processing>(schema, std::move(defs), std::move(metadata), std::move(factories));
    } else {
        return ::make_shared<simple_selection>(schema, std::move(defs), std::move(metadata), false);
    }
}

std::vector<::shared_ptr<column_specification>>
selection::collect_metadata(schema_ptr schema, const std::vector<::shared_ptr<raw_selector>>& raw_selectors,
        const selector_factories& factories) {
    std::vector<::shared_ptr<column_specification>> r;
    r.reserve(raw_selectors.size());
    auto i = raw_selectors.begin();
    for (auto&& factory : factories) {
        ::shared_ptr<column_specification> col_spec = factory->get_column_specification(schema);
        ::shared_ptr<column_identifier> alias = (*i++)->alias;
        r.push_back(alias ? col_spec->with_alias(alias) : col_spec);
    }
    return r;
}

result_set_builder::result_set_builder(const selection& s, gc_clock::time_point now, cql_serialization_format sf)
    : _result_set(std::make_unique<result_set>(::make_shared<metadata>(*(s.get_result_metadata()))))
    , _selectors(s.new_selectors())
    , _now(now)
    , _cql_serialization_format(sf)
{
    if (s._collect_timestamps) {
        _timestamps.resize(s._columns.size(), 0);
    }
    if (s._collect_TTLs) {
        _ttls.resize(s._columns.size(), 0);
    }
}

void result_set_builder::add_empty() {
    current->emplace_back();
    if (!_timestamps.empty()) {
        _timestamps[current->size() - 1] = api::missing_timestamp;
    }
    if (!_ttls.empty()) {
        _ttls[current->size() - 1] = -1;
    }
}

void result_set_builder::add(bytes_opt value) {
    current->emplace_back(std::move(value));
}

void result_set_builder::add(const column_definition& def, const query::result_atomic_cell_view& c) {
    current->emplace_back(get_value(def.type, c));
    if (!_timestamps.empty()) {
        _timestamps[current->size() - 1] = c.timestamp();
    }
    if (!_ttls.empty()) {
        gc_clock::duration ttl_left(-1);
        expiry_opt e = c.expiry();
        if (e) {
            ttl_left = *e - _now;
        }
        _ttls[current->size() - 1] = ttl_left.count();
    }
}

void result_set_builder::add_collection(const column_definition& def, bytes_view c) {
    current->emplace_back(to_bytes(c));
    // timestamps, ttls meaningless for collections
}

void result_set_builder::new_row() {
    if (current) {
        _selectors->add_input_row(_cql_serialization_format, *this);
        if (!_selectors->is_aggregate()) {
            _result_set->add_row(_selectors->get_output_row(_cql_serialization_format));
            _selectors->reset();
        }
        current->clear();
    } else {
        // FIXME: we use optional<> here because we don't have an end_row() signal
        //        instead, !current means that new_row has never been called, so this
        //        call to new_row() does not end a previous row.
        current.emplace();
    }
}

std::unique_ptr<result_set> result_set_builder::build() {
    if (current) {
        _selectors->add_input_row(_cql_serialization_format, *this);
        _result_set->add_row(_selectors->get_output_row(_cql_serialization_format));
        _selectors->reset();
        current = std::experimental::nullopt;
    }
    if (_result_set->empty() && _selectors->is_aggregate()) {
        _result_set->add_row(_selectors->get_output_row(_cql_serialization_format));
    }
    return std::move(_result_set);
}

result_set_builder::visitor::visitor(
        cql3::selection::result_set_builder& builder, const schema& s,
        const selection& selection)
        : _builder(builder), _schema(s), _selection(selection), _row_count(0) {
}

void result_set_builder::visitor::add_value(const column_definition& def,
        query::result_row_view::iterator_type& i) {
    if (def.type->is_multi_cell()) {
        auto cell = i.next_collection_cell();
        if (!cell) {
            _builder.add_empty();
            return;
        }
        _builder.add_collection(def, *cell);
    } else {
        auto cell = i.next_atomic_cell();
        if (!cell) {
            _builder.add_empty();
            return;
        }
        _builder.add(def, *cell);
    }
}

void result_set_builder::visitor::accept_new_partition(const partition_key& key,
        uint32_t row_count) {
    _partition_key = key.explode(_schema);
    _row_count = row_count;
}

void result_set_builder::visitor::accept_new_partition(uint32_t row_count) {
    _row_count = row_count;
}

void result_set_builder::visitor::accept_new_row(const clustering_key& key,
        const query::result_row_view& static_row,
        const query::result_row_view& row) {
    _clustering_key = key.explode(_schema);
    accept_new_row(static_row, row);
}

void result_set_builder::visitor::accept_new_row(
        const query::result_row_view& static_row,
        const query::result_row_view& row) {
    auto static_row_iterator = static_row.iterator();
    auto row_iterator = row.iterator();
    _builder.new_row();
    for (auto&& def : _selection.get_columns()) {
        switch (def->kind) {
        case column_kind::partition_key:
            _builder.add(_partition_key[def->component_index()]);
            break;
        case column_kind::clustering_key:
            if (_clustering_key.size() > def->component_index()) {
                _builder.add(_clustering_key[def->component_index()]);
            } else {
                _builder.add({});
            }
            break;
        case column_kind::regular_column:
            add_value(*def, row_iterator);
            break;
        case column_kind::static_column:
            add_value(*def, static_row_iterator);
            break;
        default:
            assert(0);
        }
    }
}

void result_set_builder::visitor::accept_partition_end(
        const query::result_row_view& static_row) {
    if (_row_count == 0) {
        _builder.new_row();
        auto static_row_iterator = static_row.iterator();
        for (auto&& def : _selection.get_columns()) {
            if (def->is_partition_key()) {
                _builder.add(_partition_key[def->component_index()]);
            } else if (def->is_static()) {
                add_value(*def, static_row_iterator);
            } else {
                _builder.add_empty();
            }
        }
    }
}

api::timestamp_type result_set_builder::timestamp_of(size_t idx) {
    return _timestamps[idx];
}

int32_t result_set_builder::ttl_of(size_t idx) {
    return _ttls[idx];
}

bytes_opt result_set_builder::get_value(data_type t, query::result_atomic_cell_view c) {
    return {to_bytes(c.value())};
}

}

}
