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

#include "core/print.hh"
#include "core/future-util.hh"
#include "core/distributed.hh"

#include <chrono>
#include <iosfwd>
#include <boost/range/irange.hpp>

template <typename Func>
static
void time_it(Func func, int iterations = 5, int iterations_between_clock_readings = 1000) {
    using clk = std::chrono::steady_clock;

    for (int i = 0; i < iterations; i++) {
        auto start = clk::now();
        auto end_at = start + std::chrono::seconds(1);
        uint64_t count = 0;

        while (clk::now() < end_at) {
            for (int i = 0; i < iterations_between_clock_readings; i++) { // amortize clock reading cost
                func();
                count++;
            }
        }

        auto end = clk::now();
        auto duration = std::chrono::duration<double>(end - start).count();
        std::cout << sprint("%.2f", (double)count / duration) << " tps\n";
    }
}

template <typename Func>
static
future<> do_n_times(unsigned count, Func func) {
    struct wrapper {
        Func f;
        unsigned c;
    };

    auto w = make_shared<wrapper>({std::move(func), count});
    return do_until([w] { return w->c == 0; }, [w] {
        --w->c;
        return w->f();
    });
}

// Drives concurrent and continuous execution of given asynchronous action
// until a deadline. Counts invocations.
template <typename Func>
class executor {
    const Func _func;
    const lowres_clock::time_point _end_at;
    const unsigned _n_workers;
    uint64_t _count;
private:
    future<> run_worker() {
        return do_until([this] {
            return lowres_clock::now() >= _end_at;
        }, [this] () mutable {
            ++_count;
            return _func();
        });
    }
public:
    executor(unsigned n_workers, Func func, lowres_clock::time_point end_at)
            : _func(std::move(func))
            , _end_at(end_at)
            , _n_workers(n_workers)
            , _count(0)
    { }

    // Returns the number of invocations of @func
    future<uint64_t> run() {
        auto idx = boost::irange(0, (int)_n_workers);
        return parallel_for_each(idx.begin(), idx.end(), [this] (auto idx) mutable {
            return this->run_worker();
        }).then([this] {
            return _count;
        });
    }

    future<> stop() {
        return make_ready_future<>();
    }
};

/**
 * Measures throughput of an asynchronous action. Executes the action on all cores
 * in parallel, with given number of concurrent executions per core.
 *
 * Runs many iterations. Prints partial total throughput after each iteraton.
 */
template <typename Func>
static
future<> time_parallel(Func func, unsigned concurrency_per_core, int iterations = 5) {
    using clk = std::chrono::steady_clock;
    return do_n_times(iterations, [func, concurrency_per_core] {
        auto start = clk::now();
        auto end_at = lowres_clock::now() + std::chrono::seconds(1);
        auto exec = ::make_shared<distributed<executor<Func>>>();
        return exec->start(concurrency_per_core, func, std::move(end_at)).then([exec] {
            return exec->map_reduce(adder<uint64_t>(), [] (auto& oc) { return oc.run(); });
        }).then([start] (auto total) {
            auto end = clk::now();
            auto duration = std::chrono::duration<double>(end - start).count();
            std::cout << sprint("%.2f", (double)total / duration) << " tps\n";
        }).then([exec] {
            return exec->stop().finally([exec] {});
        });
    });
}
