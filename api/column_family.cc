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

#include "column_family.hh"
#include "api/api-doc/column_family.json.hh"
#include <vector>
#include "http/exception.hh"
#include "sstables/sstables.hh"
#include "utils/estimated_histogram.hh"
#include <algorithm>

namespace api {
using namespace httpd;

using namespace std;
using namespace json;
namespace cf = httpd::column_family_json;

const utils::UUID& get_uuid(const sstring& name, const database& db) {
    auto pos = name.find("%3A");
    size_t end;
    if (pos == sstring::npos) {
        pos  = name.find(":");
        if (pos == sstring::npos) {
            throw bad_param_exception("Column family name should be in keyspace:column_family format");
        }
        end = pos + 1;
    } else {
        end = pos + 3;
    }
    try {
        return db.find_uuid(name.substr(0, pos), name.substr(end));
    } catch (std::out_of_range& e) {
        throw bad_param_exception("Column family '" + name.substr(0, pos) + ":"
                + name.substr(end) + "' not found");
    }
}

future<> foreach_column_family(http_context& ctx, const sstring& name, function<void(column_family&)> f) {
    auto uuid = get_uuid(name, ctx.db.local());

    return ctx.db.invoke_on_all([f, uuid](database& db) {
        f(db.find_column_family(uuid));
    });
}

future<json::json_return_type>  get_cf_stats(http_context& ctx, const sstring& name,
        int64_t column_family::stats::*f) {
    return map_reduce_cf(ctx, name, int64_t(0), [f](const column_family& cf) {
        return cf.get_stats().*f;
    }, std::plus<int64_t>());
}

future<json::json_return_type>  get_cf_stats(http_context& ctx,
        int64_t column_family::stats::*f) {
    return map_reduce_cf(ctx, int64_t(0), [f](const column_family& cf) {
        return cf.get_stats().*f;
    }, std::plus<int64_t>());
}

static future<json::json_return_type>  get_cf_stats_count(http_context& ctx, const sstring& name,
        utils::timed_rate_moving_average_and_histogram column_family::stats::*f) {
    return map_reduce_cf(ctx, name, int64_t(0), [f](const column_family& cf) {
        return (cf.get_stats().*f).hist.count;
    }, std::plus<int64_t>());
}

static future<json::json_return_type>  get_cf_stats_sum(http_context& ctx, const sstring& name,
        utils::timed_rate_moving_average_and_histogram column_family::stats::*f) {
    auto uuid = get_uuid(name, ctx.db.local());
    return ctx.db.map_reduce0([uuid, f](database& db) {
        // Histograms information is sample of the actual load
        // so to get an estimation of sum, we multiply the mean
        // with count. The information is gather in nano second,
        // but reported in micro
        column_family& cf = db.find_column_family(uuid);
        return ((cf.get_stats().*f).hist.count/1000.0) * (cf.get_stats().*f).hist.mean;
    }, 0.0, std::plus<double>()).then([](double res) {
        return make_ready_future<json::json_return_type>((int64_t)res);
    });
}


static future<json::json_return_type>  get_cf_stats_count(http_context& ctx,
        utils::timed_rate_moving_average_and_histogram column_family::stats::*f) {
    return map_reduce_cf(ctx, int64_t(0), [f](const column_family& cf) {
        return (cf.get_stats().*f).hist.count;
    }, std::plus<int64_t>());
}

static future<json::json_return_type>  get_cf_histogram(http_context& ctx, const sstring& name,
        utils::timed_rate_moving_average_and_histogram column_family::stats::*f) {
    utils::UUID uuid = get_uuid(name, ctx.db.local());
    return ctx.db.map_reduce0([f, uuid](const database& p) {
        return (p.find_column_family(uuid).get_stats().*f).hist;},
            utils::ihistogram(),
            std::plus<utils::ihistogram>())
            .then([](const utils::ihistogram& val) {
                return make_ready_future<json::json_return_type>(to_json(val));
    });
}

static future<json::json_return_type> get_cf_histogram(http_context& ctx, utils::timed_rate_moving_average_and_histogram column_family::stats::*f) {
    std::function<utils::ihistogram(const database&)> fun = [f] (const database& db)  {
        utils::ihistogram res;
        for (auto i : db.get_column_families()) {
            res += (i.second->get_stats().*f).hist;
        }
        return res;
    };
    return ctx.db.map(fun).then([](const std::vector<utils::ihistogram> &res) {
        std::vector<httpd::utils_json::histogram> r;
        boost::copy(res | boost::adaptors::transformed(to_json), std::back_inserter(r));
        return make_ready_future<json::json_return_type>(r);
    });
}

static future<json::json_return_type>  get_cf_rate_and_histogram(http_context& ctx, const sstring& name,
        utils::timed_rate_moving_average_and_histogram column_family::stats::*f) {
    utils::UUID uuid = get_uuid(name, ctx.db.local());
    return ctx.db.map_reduce0([f, uuid](const database& p) {
        return (p.find_column_family(uuid).get_stats().*f).rate();},
            utils::rate_moving_average_and_histogram(),
            std::plus<utils::rate_moving_average_and_histogram>())
            .then([](const utils::rate_moving_average_and_histogram& val) {
                return make_ready_future<json::json_return_type>(timer_to_json(val));
    });
}

static future<json::json_return_type> get_cf_rate_and_histogram(http_context& ctx, utils::timed_rate_moving_average_and_histogram column_family::stats::*f) {
    std::function<utils::rate_moving_average_and_histogram(const database&)> fun = [f] (const database& db)  {
        utils::rate_moving_average_and_histogram res;
        for (auto i : db.get_column_families()) {
            res += (i.second->get_stats().*f).rate();
        }
        return res;
    };
    return ctx.db.map(fun).then([](const std::vector<utils::rate_moving_average_and_histogram> &res) {
        std::vector<httpd::utils_json::rate_moving_average_and_histogram> r;
        boost::copy(res | boost::adaptors::transformed(timer_to_json), std::back_inserter(r));
        return make_ready_future<json::json_return_type>(r);
    });
}

static future<json::json_return_type> get_cf_unleveled_sstables(http_context& ctx, const sstring& name) {
    return map_reduce_cf(ctx, name, int64_t(0), [](const column_family& cf) {
        return cf.get_unleveled_sstables();
    }, std::plus<int64_t>());
}

static int64_t min_row_size(column_family& cf) {
    int64_t res = INT64_MAX;
    for (auto i: *cf.get_sstables() ) {
        res = std::min(res, i->get_stats_metadata().estimated_row_size.min());
    }
    return (res == INT64_MAX) ? 0 : res;
}

static int64_t max_row_size(column_family& cf) {
    int64_t res = 0;
    for (auto i: *cf.get_sstables() ) {
        res = std::max(i->get_stats_metadata().estimated_row_size.max(), res);
    }
    return res;
}

static double update_ratio(double acc, double f, double total) {
    if (f && !total) {
        throw bad_param_exception("total should include all elements");
    } else if (total) {
        acc += f / total;
    }
    return acc;
}

static integral_ratio_holder mean_row_size(column_family& cf) {
    integral_ratio_holder res;
    for (auto i: *cf.get_sstables() ) {
        auto c = i->get_stats_metadata().estimated_row_size.count();
        res.sub += i->get_stats_metadata().estimated_row_size.mean() * c;
        res.total += c;
    }
    return res;
}

static std::unordered_map<sstring, uint64_t> merge_maps(std::unordered_map<sstring, uint64_t> a,
        const std::unordered_map<sstring, uint64_t>& b) {
    a.insert(b.begin(), b.end());
    return a;
}

static json::json_return_type sum_map(const std::unordered_map<sstring, uint64_t>& val) {
    uint64_t res = 0;
    for (auto i : val) {
        res += i.second;
    }
    return res;
}

static future<json::json_return_type>  sum_sstable(http_context& ctx, const sstring name, bool total) {
    auto uuid = get_uuid(name, ctx.db.local());
    return ctx.db.map_reduce0([uuid, total](database& db) {
        std::unordered_map<sstring, uint64_t> m;
        auto sstables = (total) ? db.find_column_family(uuid).get_sstables_including_compacted_undeleted() :
                db.find_column_family(uuid).get_sstables();
        for (auto t : *sstables) {
            m[t->get_filename()] = t->bytes_on_disk();
        }
        return m;
    }, std::unordered_map<sstring, uint64_t>(), merge_maps).
            then([](const std::unordered_map<sstring, uint64_t>& val) {
        return sum_map(val);
    });
}


static future<json::json_return_type> sum_sstable(http_context& ctx, bool total) {
    return map_reduce_cf_raw(ctx, std::unordered_map<sstring, uint64_t>(), [total](column_family& cf) {
        std::unordered_map<sstring, uint64_t> m;
        auto sstables = (total) ? cf.get_sstables_including_compacted_undeleted() :
                cf.get_sstables();
        for (auto t : *sstables) {
            m[t->get_filename()] = t->bytes_on_disk();
        }
        return m;
    },merge_maps).then([](const std::unordered_map<sstring, uint64_t>& val) {
        return sum_map(val);
    });
}

template <typename T>
class sum_ratio {
    uint64_t _n = 0;
    T _total = 0;
public:
    future<> operator()(T value) {
        if (value > 0) {
            _total += value;
            _n++;
        }
        return make_ready_future<>();
    }
    // Returns average value of all registered ratios.
    T get() && {
        return _n ? (_total / _n) : T(0);
    }
};

static double get_compression_ratio(column_family& cf) {
    sum_ratio<double> result;
    for (auto i : *cf.get_sstables()) {
        auto compression_ratio = i->get_compression_ratio();
        if (compression_ratio != sstables::metadata_collector::NO_COMPRESSION_RATIO) {
            result(compression_ratio);
        }
    }
    return std::move(result).get();
}

static std::vector<uint64_t> concat_sstable_count_per_level(std::vector<uint64_t> a, std::vector<uint64_t>&& b) {
    a.resize(std::max(a.size(), b.size()), 0UL);
    for (auto i = 0U; i < b.size(); i++) {
        a[i] += b[i];
    }
    return a;
}

void set_column_family(http_context& ctx, routes& r) {
    cf::get_column_family_name.set(r, [&ctx] (const_req req){
        vector<sstring> res;
        for (auto i: ctx.db.local().get_column_families_mapping()) {
            res.push_back(i.first.first + ":" + i.first.second);
        }
        return res;
    });

    cf::get_column_family.set(r, [&ctx] (const_req req){
            vector<cf::column_family_info> res;
            for (auto i: ctx.db.local().get_column_families_mapping()) {
                cf::column_family_info info;
                info.ks = i.first.first;
                info.cf =  i.first.second;
                info.type = "ColumnFamilies";
                res.push_back(info);
            }
            return res;
        });

    cf::get_column_family_name_keyspace.set(r, [&ctx] (const_req req){
        vector<sstring> res;
        for (auto i = ctx.db.local().get_keyspaces().cbegin(); i!=  ctx.db.local().get_keyspaces().cend(); i++) {
            res.push_back(i->first);
        }
        return res;
    });

    cf::get_memtable_columns_count.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], 0, [](column_family& cf) {
            return cf.active_memtable().partition_count();
        }, std::plus<int>());
    });

    cf::get_all_memtable_columns_count.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, 0, [](column_family& cf) {
            return cf.active_memtable().partition_count();
        }, std::plus<int>());
    });

    cf::get_memtable_on_heap_size.set(r, [] (const_req req) {
        return 0;
    });

    cf::get_all_memtable_on_heap_size.set(r, [] (const_req req) {
        return 0;
    });

    cf::get_memtable_off_heap_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], int64_t(0), [](column_family& cf) {
            return cf.active_memtable().region().occupancy().total_space();
        }, std::plus<int64_t>());
    });

    cf::get_all_memtable_off_heap_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, int64_t(0), [](column_family& cf) {
            return cf.active_memtable().region().occupancy().total_space();
        }, std::plus<int64_t>());
    });

    cf::get_memtable_live_data_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], int64_t(0), [](column_family& cf) {
            return cf.active_memtable().region().occupancy().used_space();
        }, std::plus<int64_t>());
    });

    cf::get_all_memtable_live_data_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, int64_t(0), [](column_family& cf) {
            return cf.active_memtable().region().occupancy().used_space();
        }, std::plus<int64_t>());
    });

    cf::get_cf_all_memtables_on_heap_size.set(r, [] (const_req req) {
        return 0;
    });

    cf::get_all_cf_all_memtables_on_heap_size.set(r, [] (const_req req) {
        return 0;
    });

    cf::get_cf_all_memtables_off_heap_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        warn(unimplemented::cause::INDEXES);
        return map_reduce_cf(ctx, req->param["name"], int64_t(0), [](column_family& cf) {
            return cf.occupancy().total_space();
        }, std::plus<int64_t>());
    });

    cf::get_all_cf_all_memtables_off_heap_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        warn(unimplemented::cause::INDEXES);
        return ctx.db.map_reduce0([](const database& db){
            return db.dirty_memory_region_group().memory_used();
        }, int64_t(0), std::plus<int64_t>()).then([](int res) {
            return make_ready_future<json::json_return_type>(res);
        });
    });

    cf::get_cf_all_memtables_live_data_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        warn(unimplemented::cause::INDEXES);
        return map_reduce_cf(ctx, req->param["name"], int64_t(0), [](column_family& cf) {
            return cf.occupancy().used_space();
        }, std::plus<int64_t>());
    });

    cf::get_all_cf_all_memtables_live_data_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        warn(unimplemented::cause::INDEXES);
        return map_reduce_cf(ctx, int64_t(0), [](column_family& cf) {
            return cf.active_memtable().region().occupancy().used_space();
        }, std::plus<int64_t>());
    });

    cf::get_memtable_switch_count.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats(ctx,req->param["name"] ,&column_family::stats::memtable_switch_count);
    });

    cf::get_all_memtable_switch_count.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats(ctx, &column_family::stats::memtable_switch_count);
    });

    cf::get_estimated_row_size_histogram.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], utils::estimated_histogram(0), [](column_family& cf) {
            utils::estimated_histogram res(0);
            for (auto i: *cf.get_sstables() ) {
                res.merge(i->get_stats_metadata().estimated_row_size);
            }
            return res;
        },
        utils::estimated_histogram_merge, utils_json::estimated_histogram());
    });

    cf::get_estimated_row_count.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], int64_t(0), [](column_family& cf) {
            uint64_t res = 0;
            for (auto i: *cf.get_sstables() ) {
                res += i->get_stats_metadata().estimated_row_size.count();
            }
            return res;
        },
        std::plus<uint64_t>());
    });

    cf::get_estimated_column_count_histogram.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], utils::estimated_histogram(0), [](column_family& cf) {
            utils::estimated_histogram res(0);
            for (auto i: *cf.get_sstables() ) {
                res.merge(i->get_stats_metadata().estimated_column_count);
            }
            return res;
        },
        utils::estimated_histogram_merge, utils_json::estimated_histogram());
    });

    cf::get_all_compression_ratio.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_pending_flushes.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats(ctx,req->param["name"] ,&column_family::stats::pending_flushes);
    });

    cf::get_all_pending_flushes.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats(ctx, &column_family::stats::pending_flushes);
    });

    cf::get_read.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats_count(ctx,req->param["name"] ,&column_family::stats::reads);
    });

    cf::get_all_read.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats_count(ctx, &column_family::stats::reads);
    });

    cf::get_write.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats_count(ctx, req->param["name"] ,&column_family::stats::writes);
    });

    cf::get_all_write.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats_count(ctx, &column_family::stats::writes);
    });

    cf::get_read_latency_histogram_depricated.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_histogram(ctx, req->param["name"], &column_family::stats::reads);
    });

    cf::get_read_latency_histogram.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_rate_and_histogram(ctx, req->param["name"], &column_family::stats::reads);
    });

    cf::get_read_latency.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats_sum(ctx,req->param["name"] ,&column_family::stats::reads);
    });

    cf::get_write_latency.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats_sum(ctx, req->param["name"] ,&column_family::stats::writes);
    });

    cf::get_all_read_latency_histogram_depricated.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_histogram(ctx, &column_family::stats::writes);
    });

    cf::get_all_read_latency_histogram.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_rate_and_histogram(ctx, &column_family::stats::writes);
    });

    cf::get_write_latency_histogram_depricated.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_histogram(ctx, req->param["name"], &column_family::stats::writes);
    });

    cf::get_write_latency_histogram.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_rate_and_histogram(ctx, req->param["name"], &column_family::stats::writes);
    });

    cf::get_all_write_latency_histogram_depricated.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_histogram(ctx, &column_family::stats::writes);
    });

    cf::get_all_write_latency_histogram.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_rate_and_histogram(ctx, &column_family::stats::writes);
    });

    cf::get_pending_compactions.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], int64_t(0), [](column_family& cf) {
            return cf.get_compaction_strategy().estimated_pending_compactions(cf);
        }, std::plus<int64_t>());
    });

    cf::get_all_pending_compactions.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, int64_t(0), [](column_family& cf) {
            return cf.get_compaction_strategy().estimated_pending_compactions(cf);
        }, std::plus<int64_t>());
    });

    cf::get_live_ss_table_count.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats(ctx, req->param["name"], &column_family::stats::live_sstable_count);
    });

    cf::get_all_live_ss_table_count.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_stats(ctx, &column_family::stats::live_sstable_count);
    });

    cf::get_unleveled_sstables.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_unleveled_sstables(ctx, req->param["name"]);
    });

    cf::get_live_disk_space_used.set(r, [&ctx] (std::unique_ptr<request> req) {
        return sum_sstable(ctx, req->param["name"], false);
    });

    cf::get_all_live_disk_space_used.set(r, [&ctx] (std::unique_ptr<request> req) {
        return sum_sstable(ctx, false);
    });

    cf::get_total_disk_space_used.set(r, [&ctx] (std::unique_ptr<request> req) {
        return sum_sstable(ctx, req->param["name"], true);
    });

    cf::get_all_total_disk_space_used.set(r, [&ctx] (std::unique_ptr<request> req) {
        return sum_sstable(ctx, true);
    });

    cf::get_min_row_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], INT64_MAX, min_row_size, min_int64);
    });

    cf::get_all_min_row_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, INT64_MAX, min_row_size, min_int64);
    });

    cf::get_max_row_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], int64_t(0), max_row_size, max_int64);
    });

    cf::get_all_max_row_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, int64_t(0), max_row_size, max_int64);
    });

    cf::get_mean_row_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        // Cassandra 3.x mean values are truncated as integrals.
        return map_reduce_cf(ctx, req->param["name"], integral_ratio_holder(), mean_row_size, std::plus<integral_ratio_holder>());
    });

    cf::get_all_mean_row_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        // Cassandra 3.x mean values are truncated as integrals.
        return map_reduce_cf(ctx, integral_ratio_holder(), mean_row_size, std::plus<integral_ratio_holder>());
    });

    cf::get_bloom_filter_false_positives.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], uint64_t(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), uint64_t(0), [](uint64_t s, auto& sst) {
                return s + sst->filter_get_false_positive();
            });
        }, std::plus<uint64_t>());
    });

    cf::get_all_bloom_filter_false_positives.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, uint64_t(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), uint64_t(0), [](uint64_t s, auto& sst) {
                return s + sst->filter_get_false_positive();
            });
        }, std::plus<uint64_t>());
    });

    cf::get_recent_bloom_filter_false_positives.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], uint64_t(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), uint64_t(0), [](uint64_t s, auto& sst) {
                return s + sst->filter_get_recent_false_positive();
            });
        }, std::plus<uint64_t>());
    });

    cf::get_all_recent_bloom_filter_false_positives.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, uint64_t(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), uint64_t(0), [](uint64_t s, auto& sst) {
                return s + sst->filter_get_recent_false_positive();
            });
        }, std::plus<uint64_t>());
    });

    cf::get_bloom_filter_false_ratio.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], double(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), double(0), [](double s, auto& sst) {
                double f = sst->filter_get_false_positive();
                return update_ratio(s, f, f + sst->filter_get_true_positive());
            });
        }, std::plus<double>());
    });

    cf::get_all_bloom_filter_false_ratio.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, double(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), double(0), [](double s, auto& sst) {
                double f = sst->filter_get_false_positive();
                return update_ratio(s, f, f + sst->filter_get_true_positive());
            });
        }, std::plus<double>());
    });

    cf::get_recent_bloom_filter_false_ratio.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], double(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), double(0), [](double s, auto& sst) {
                double f = sst->filter_get_recent_false_positive();
                return update_ratio(s, f, f + sst->filter_get_recent_true_positive());
            });
        }, std::plus<double>());
    });

    cf::get_all_recent_bloom_filter_false_ratio.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, double(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), double(0), [](double s, auto& sst) {
                double f = sst->filter_get_recent_false_positive();
                return update_ratio(s, f, f + sst->filter_get_recent_true_positive());
            });
        }, std::plus<double>());
    });

    cf::get_bloom_filter_disk_space_used.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], uint64_t(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), uint64_t(0), [](uint64_t s, auto& sst) {
                return sst->filter_size();
            });
        }, std::plus<uint64_t>());
    });

    cf::get_all_bloom_filter_disk_space_used.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, uint64_t(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), uint64_t(0), [](uint64_t s, auto& sst) {
                return sst->filter_size();
            });
        }, std::plus<uint64_t>());
    });

    cf::get_bloom_filter_off_heap_memory_used.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], uint64_t(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), uint64_t(0), [](uint64_t s, auto& sst) {
                return sst->filter_memory_size();
            });
        }, std::plus<uint64_t>());
    });

    cf::get_all_bloom_filter_off_heap_memory_used.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, uint64_t(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), uint64_t(0), [](uint64_t s, auto& sst) {
                return sst->filter_memory_size();
            });
        }, std::plus<uint64_t>());
    });

    cf::get_index_summary_off_heap_memory_used.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], uint64_t(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), uint64_t(0), [](uint64_t s, auto& sst) {
                return sst->get_summary().memory_footprint();
            });
        }, std::plus<uint64_t>());
    });

    cf::get_all_index_summary_off_heap_memory_used.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, uint64_t(0), [] (column_family& cf) {
            return std::accumulate(cf.get_sstables()->begin(), cf.get_sstables()->end(), uint64_t(0), [](uint64_t s, auto& sst) {
                return sst->get_summary().memory_footprint();
            });
        }, std::plus<uint64_t>());
    });

    cf::get_compression_metadata_off_heap_memory_used.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        // FIXME
        // We are missing the off heap memory calculation
        // Return 0 is the wrong value. It's a work around
        // until the memory calculation will be available
        //auto id = get_uuid(req->param["name"], ctx.db.local());
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_all_compression_metadata_off_heap_memory_used.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_speculative_retries.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        //auto id = get_uuid(req->param["name"], ctx.db.local());
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_all_speculative_retries.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_key_cache_hit_rate.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        //auto id = get_uuid(req->param["name"], ctx.db.local());
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_true_snapshots_size.set(r, [&ctx] (std::unique_ptr<request> req) {
        auto uuid = get_uuid(req->param["name"], ctx.db.local());
        return ctx.db.local().find_column_family(uuid).get_snapshot_details().then([](
                const std::unordered_map<sstring, column_family::snapshot_details>& sd) {
            int64_t res = 0;
            for (auto i : sd) {
                res += i.second.total;
            }
            return make_ready_future<json::json_return_type>(res);
        });
    });

    cf::get_all_true_snapshots_size.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_row_cache_hit_out_of_range.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        //auto id = get_uuid(req->param["name"], ctx.db.local());
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_all_row_cache_hit_out_of_range.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_row_cache_hit.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf_raw(ctx, req->param["name"], utils::rate_moving_average(), [](const column_family& cf) {
            return cf.get_row_cache().stats().hits.rate();
        }, std::plus<utils::rate_moving_average>()).then([](const utils::rate_moving_average& m) {
            return make_ready_future<json::json_return_type>(meter_to_json(m));
        });
    });

    cf::get_all_row_cache_hit.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf_raw(ctx, utils::rate_moving_average(), [](const column_family& cf) {
            return cf.get_row_cache().stats().hits.rate();
        }, std::plus<utils::rate_moving_average>()).then([](const utils::rate_moving_average& m) {
            return make_ready_future<json::json_return_type>(meter_to_json(m));
        });
    });

    cf::get_row_cache_miss.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf_raw(ctx, req->param["name"], utils::rate_moving_average(), [](const column_family& cf) {
            return cf.get_row_cache().stats().misses.rate();
        }, std::plus<utils::rate_moving_average>()).then([](const utils::rate_moving_average& m) {
            return make_ready_future<json::json_return_type>(meter_to_json(m));
        });
    });

    cf::get_all_row_cache_miss.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf_raw(ctx, utils::rate_moving_average(), [](const column_family& cf) {
            return cf.get_row_cache().stats().misses.rate();
        }, std::plus<utils::rate_moving_average>()).then([](const utils::rate_moving_average& m) {
            return make_ready_future<json::json_return_type>(meter_to_json(m));
        });

    });

    cf::get_cas_prepare.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        //auto id = get_uuid(req->param["name"], ctx.db.local());
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_cas_propose.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        //auto id = get_uuid(req->param["name"], ctx.db.local());
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_cas_commit.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        //auto id = get_uuid(req->param["name"], ctx.db.local());
        return make_ready_future<json::json_return_type>(0);
    });

    cf::get_sstables_per_read_histogram.set(r, [&ctx] (std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], utils::estimated_histogram(0), [](column_family& cf) {
            return cf.get_stats().estimated_sstable_per_read;
        },
        utils::estimated_histogram_merge, utils_json::estimated_histogram());
    });

    cf::get_tombstone_scanned_histogram.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_histogram(ctx, req->param["name"], &column_family::stats::tombstone_scanned);
    });

    cf::get_live_scanned_histogram.set(r, [&ctx] (std::unique_ptr<request> req) {
        return get_cf_histogram(ctx, req->param["name"], &column_family::stats::live_scanned);
    });

    cf::get_col_update_time_delta_histogram.set(r, [] (std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        //auto id = get_uuid(req->param["name"], ctx.db.local());
        std::vector<double> res;
        return make_ready_future<json::json_return_type>(res);
    });

    cf::is_auto_compaction_disabled.set(r, [] (const_req req) {
        // FIXME
        // currently auto compaction is disable
        // it should be changed when it would have an API
        return true;
    });

    cf::get_built_indexes.set(r, [](const_req) {
        // FIXME
        // Currently there are no index support
        return std::vector<sstring>();
    });


    cf::get_compression_metadata_off_heap_memory_used.set(r, [](const_req) {
        // FIXME
        // Currently there are no information on the compression
        // metadata, so we return 0
        return 0;
    });

    cf::get_compression_parameters.set(r, [](const_req) {
        // FIXME
        // Currently there are no compression parameters available
        // so we return an empty map
        return std::vector<sstring>();
    });

    cf::get_compression_ratio.set(r, [&ctx](std::unique_ptr<request> req) {
        auto uuid = get_uuid(req->param["name"], ctx.db.local());

        return ctx.db.map_reduce(sum_ratio<double>(), [uuid](database& db) {
            column_family& cf = db.find_column_family(uuid);
            return make_ready_future<double>(get_compression_ratio(cf));
        }).then([] (const double& result) {
            return make_ready_future<json::json_return_type>(result);
        });
    });

    cf::get_read_latency_estimated_histogram.set(r, [&ctx](std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], utils::estimated_histogram(0), [](column_family& cf) {
            return cf.get_stats().estimated_read;
        },
        utils::estimated_histogram_merge, utils_json::estimated_histogram());
    });

    cf::get_write_latency_estimated_histogram.set(r, [&ctx](std::unique_ptr<request> req) {
        return map_reduce_cf(ctx, req->param["name"], utils::estimated_histogram(0), [](column_family& cf) {
            return cf.get_stats().estimated_write;
        },
        utils::estimated_histogram_merge, utils_json::estimated_histogram());
    });

    cf::set_compaction_strategy_class.set(r, [&ctx](std::unique_ptr<request> req) {
        sstring strategy = req->get_query_param("class_name");
        return foreach_column_family(ctx, req->param["name"], [strategy](column_family& cf) {
            cf.set_compaction_strategy(sstables::compaction_strategy::type(strategy));
        }).then([] {
                return make_ready_future<json::json_return_type>(json_void());
        });
    });

    cf::get_compaction_strategy_class.set(r, [&ctx](const_req req) {
        return ctx.db.local().find_column_family(get_uuid(req.param["name"], ctx.db.local())).get_compaction_strategy().name();
    });

    cf::set_compression_parameters.set(r, [&ctx](std::unique_ptr<request> req) {
        // TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(json_void());
    });

    cf::set_crc_check_chance.set(r, [&ctx](std::unique_ptr<request> req) {
        // TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(json_void());
    });

    cf::get_sstable_count_per_level.set(r, [&ctx](std::unique_ptr<request> req) {
        return map_reduce_cf_raw(ctx, req->param["name"], std::vector<uint64_t>(), [](const column_family& cf) {
            return cf.sstable_count_per_level();
        }, concat_sstable_count_per_level).then([](const std::vector<uint64_t>& res) {
            return make_ready_future<json::json_return_type>(res);
        });
    });
}
}
