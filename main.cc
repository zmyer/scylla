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

#include "supervisor.hh"
#include "database.hh"
#include "core/app-template.hh"
#include "core/distributed.hh"
#include "thrift/server.hh"
#include "transport/server.hh"
#include "http/httpd.hh"
#include "api/api_init.hh"
#include "db/config.hh"
#include "service/storage_service.hh"
#include "service/migration_manager.hh"
#include "service/load_broadcaster.hh"
#include "streaming/stream_session.hh"
#include "db/system_keyspace.hh"
#include "db/batchlog_manager.hh"
#include "db/commitlog/commitlog.hh"
#include "db/commitlog/commitlog_replayer.hh"
#include "utils/runtime.hh"
#include "utils/file_lock.hh"
#include "dns.hh"
#include "log.hh"
#include "debug.hh"
#include "init.hh"
#include "release.hh"
#include "repair/repair.hh"
#include <cstdio>
#include <core/file.hh>
#include <sys/time.h>
#include <sys/resource.h>
#include "disk-error-handler.hh"
#include "tracing/tracing.hh"
#include "core/prometheus.hh"
#include "message/messaging_service.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

namespace bpo = boost::program_options;

static boost::filesystem::path relative_conf_dir(boost::filesystem::path path) {
    static auto conf_dir = db::config::get_conf_dir(); // this is not gonna change in our life time
    return conf_dir / path;
}

// Note: would be neat if something like this was in config::string_map directly
// but that cruds up the YAML/boost integration so until I want to spend hairpulling
// time with that, this is an acceptable helper
template<typename K, typename V, typename KK, typename VV = V>
static V get_or_default(const std::unordered_map<K, V>& src, const KK& key, const VV& def = V()) {
    auto i = src.find(key);
    if (i != src.end()) {
        return i->second;
    }
    return def;
}

static future<>
read_config(bpo::variables_map& opts, db::config& cfg) {
    using namespace boost::filesystem;
    sstring file;

    if (opts.count("options-file") > 0) {
        file = opts["options-file"].as<sstring>();
    } else {
        file = relative_conf_dir("scylla.yaml").string();
    }
    return check_direct_io_support(file).then([file, &cfg] {
        return cfg.read_from_file(file);
    }).handle_exception([file](auto ep) {
        startlog.error("Could not read configuration file {}: {}", file, ep);
        return make_exception_future<>(ep);
    });
}

static void do_help_loggers() {
    print("Available loggers:\n");
    for (auto&& name : logging::logger_registry().get_all_logger_names()) {
        print("    %s\n", name);
    }
}

static logging::log_level to_loglevel(sstring level) {
    try {
        return boost::lexical_cast<logging::log_level>(std::string(level));
    } catch(boost::bad_lexical_cast e) {
        throw std::runtime_error("Unknown log level '" + level + "'");
    }
}

static future<> disk_sanity(sstring path, bool developer_mode) {
    return check_direct_io_support(path).then([path, developer_mode] {
        return file_system_at(path).then([path, developer_mode] (auto fs) {
            if (fs != fs_type::xfs) {
                if (!developer_mode) {
                    startlog.error("{} is not on XFS. This is a non-supported setup, and performance is expected to be very bad.\n"
                            "For better performance, placing your data on XFS-formatted directories is required."
                            " To override this error, see the developer_mode configuration option.", path);
                    throw std::runtime_error(sprint("invalid configuration: path \"%s\" on unsupported filesystem", path));
                } else {
                    startlog.warn("{} is not on XFS. This is a non-supported setup, and performance is expected to be very bad.\n"
                            "For better performance, placing your data on XFS-formatted directories is strongly recommended", path);
                }
            }
        });
    });
};

static void apply_logger_settings(sstring default_level, db::config::string_map levels,
        bool log_to_stdout, bool log_to_syslog) {
    logging::logger_registry().set_all_loggers_level(to_loglevel(default_level));
    for (auto&& kv: levels) {
        auto&& k = kv.first;
        auto&& v = kv.second;
        try {
            logging::logger_registry().set_logger_level(k, to_loglevel(v));
        } catch(std::out_of_range e) {
            throw std::runtime_error("Unknown logger '" + k + "'. Use --help-loggers to list available loggers.");
        }
    }
    logging::logger::set_stdout_enabled(log_to_stdout);
    logging::logger::set_syslog_enabled(log_to_syslog);
}

class directories {
public:
    future<> touch_and_lock(sstring path) {
        return io_check(recursive_touch_directory, path).then_wrapped([this, path] (future<> f) {
            try {
                f.get();
                return utils::file_lock::acquire(path + "/.lock").then([this](utils::file_lock lock) {
                   _locks.emplace_back(std::move(lock));
                }).handle_exception([path](auto ep) {
                    // only do this because "normal" unhandled exception exit in seastar
                    // _drops_ system_error message ("what()") and thus does not quite deliver
                    // the relevant info to the user
                    try {
                        std::rethrow_exception(ep);
                    } catch (std::exception& e) {
                        startlog.error("Could not initialize {}: {}", path, e.what());
                        throw;
                    } catch (...) {
                        throw;
                    }
                });
            } catch (...) {
                startlog.error("Directory '{}' cannot be initialized. Tried to do it but failed with: {}", path, std::current_exception());
                throw;
            }
        });
    }
    template<typename _Iter>
    future<> touch_and_lock(_Iter i, _Iter e) {
        return parallel_for_each(i, e, [this](sstring path) {
           return touch_and_lock(std::move(path));
        });
    }
    template<typename _Range>
    future<> touch_and_lock(_Range&& r) {
        return touch_and_lock(std::begin(r), std::end(r));
    }
private:
    std::vector<utils::file_lock>
        _locks;
};

class bad_configuration_error : public std::exception {};

static
void
verify_rlimit(bool developer_mode) {
    struct rlimit lim;
    int r = getrlimit(RLIMIT_NOFILE, &lim);
    if (r == -1) {
        throw std::system_error(errno, std::system_category());
    }
    auto recommended = 200'000U;
    auto min = 10'000U;
    if (lim.rlim_cur < min) {
        if (developer_mode) {
            startlog.warn("NOFILE rlimit too low (recommended setting {}, minimum setting {};"
                          " you may run out of file descriptors.", recommended, min);
        } else {
            startlog.error("NOFILE rlimit too low (recommended setting {}, minimum setting {};"
                          " refusing to start.", recommended, min);
            throw std::runtime_error("NOFILE rlimit too low");
        }
    }
}

static bool cpu_sanity() {
    if (!__builtin_cpu_supports("sse4.2")) {
        std::cerr << "Scylla requires a processor with SSE 4.2 support\n";
        return false;
    }
    return true;
}

static void tcp_syncookies_sanity() {
    try {
        auto f = file_desc::open("/proc/sys/net/ipv4/tcp_syncookies", O_RDONLY | O_CLOEXEC);
        char buf[128] = {};
        f.read(buf, 128);
        if (sstring(buf) == "0\n") {
            startlog.warn("sysctl entry net.ipv4.tcp_syncookies is set to 0.\n"
                          "For better performance, set following parameter on sysctl is strongly recommended:\n"
                          "net.ipv4.tcp_syncookies=1");
        }
    } catch (const std::system_error& e) {
            startlog.warn("Unable to check if net.ipv4.tcp_syncookies is set {}", e);
    }
}

static future<>
verify_seastar_io_scheduler(bool has_max_io_requests, bool developer_mode) {
    auto note_bad_conf = [developer_mode] (sstring cause) {
        sstring msg = "I/O Scheduler is not properly configured! This is a non-supported setup, and performance is expected to be unpredictably bad.\n Reason found: "
                    + cause + "\n"
                    + "To properly configure the I/O Scheduler, run the scylla_io_setup utility shipped with Scylla.\n";

        sstring devmode_msg = msg + "To ignore this, see the developer_mode configuration option.";
        if (developer_mode) {
            startlog.warn(msg.c_str());
        } else {
            startlog.error(devmode_msg.c_str());
            throw std::runtime_error("Bad I/O Scheduler configuration");
        }
    };

    if (!has_max_io_requests) {
        note_bad_conf("--max-io-requests is not set.");
    }
    return smp::invoke_on_all([developer_mode, note_bad_conf] {
        auto capacity = engine().get_io_queue().capacity();
        if (capacity < 4) {
            auto cause = sprint("I/O Queue capacity for this shard is too low (%ld, minimum 4 expected).", capacity);
            note_bad_conf(cause);
        }
    });
}

static
void
verify_adequate_memory_per_shard(bool developer_mode) {
    auto shard_mem = memory::stats().total_memory();
    if (shard_mem >= (1 << 30)) {
        return;
    }
    if (developer_mode) {
        startlog.warn("Only {} MiB per shard; this is below the recommended minimum of 1 GiB/shard;"
                " continuing since running in developer mode", shard_mem >> 20);
    } else {
        startlog.error("Only {} MiB per shard; this is below the recommended minimum of 1 GiB/shard; terminating."
                "Configure more memory (--memory option) or decrease shard count (--smp option).", shard_mem >> 20);
        throw std::runtime_error("configuration (memory per shard too low)");
    }
}

int main(int ac, char** av) {
  int return_value = 0;
  try {
    // early check to avoid triggering
    if (!cpu_sanity()) {
        _exit(71);
    }
    runtime::init_uptime();
    std::setvbuf(stdout, nullptr, _IOLBF, 1000);
    app_template app;
    auto opt_add = app.add_options();

    auto cfg = make_lw_shared<db::config>();
    bool help_loggers = false;
    bool help_version = false;
    cfg->add_options(opt_add)
        // TODO : default, always read?
        ("options-file", bpo::value<sstring>(), "configuration file (i.e. <SCYLLA_HOME>/conf/scylla.yaml)")
        ("help-loggers", bpo::bool_switch(&help_loggers), "print a list of logger names and exit")
        ("version", bpo::bool_switch(&help_version), "print version number and exit")
        ;

    distributed<database> db;
    debug::db = &db;
    auto& qp = cql3::get_query_processor();
    auto& proxy = service::get_storage_proxy();
    auto& mm = service::get_migration_manager();
    api::http_context ctx(db, proxy);
    httpd::http_server_control prometheus_server;
    prometheus::config pctx;
    directories dirs;

    return app.run_deprecated(ac, av, [&] {
        if (help_version) {
            print("%s\n", scylla_version());
            engine().exit(0);
            return make_ready_future<>();
        }
        if (help_loggers) {
            do_help_loggers();
            engine().exit(1);
            return make_ready_future<>();
        }
        print("Scylla version %s starting ...\n", scylla_version());
        auto&& opts = app.configuration();

        // Do this first once set log applied from command line so for example config
        // parse can get right log level.
        apply_logger_settings(cfg->default_log_level(), cfg->logger_log_level(),
                cfg->log_to_stdout(), cfg->log_to_syslog());

        // Check developer mode before even reading the config file, because we may not be
        // able to read it if we need to disable strict dma mode.
        // We'll redo this later and apply it to all reactors.
        if (opts.count("developer-mode")) {
            engine().set_strict_dma(false);
        }

        tcp_syncookies_sanity();

        return seastar::async([cfg, &db, &qp, &proxy, &mm, &ctx, &opts, &dirs, &pctx, &prometheus_server, &return_value] {
            read_config(opts, *cfg).get();
            apply_logger_settings(cfg->default_log_level(), cfg->logger_log_level(),
                    cfg->log_to_stdout(), cfg->log_to_syslog());
            verify_rlimit(cfg->developer_mode());
            verify_adequate_memory_per_shard(cfg->developer_mode());
            dht::set_global_partitioner(cfg->partitioner(), cfg->murmur3_partitioner_ignore_msb_bits());
            auto start_thrift = cfg->start_rpc();
            uint16_t api_port = cfg->api_port();
            ctx.api_dir = cfg->api_ui_dir();
            ctx.api_doc = cfg->api_doc_dir();
            sstring listen_address = cfg->listen_address();
            sstring rpc_address = cfg->rpc_address();
            sstring api_address = cfg->api_address() != "" ? cfg->api_address() : rpc_address;
            sstring broadcast_address = cfg->broadcast_address();
            sstring broadcast_rpc_address = cfg->broadcast_rpc_address();

            if (!broadcast_address.empty()) {
                try {
                    utils::fb_utilities::set_broadcast_address(broadcast_address);
                } catch (...) {
                    startlog.error("Bad configuration: invalid 'broadcast_address': {}: {}", broadcast_address, std::current_exception());
                    throw bad_configuration_error();
                }
            } else if (!listen_address.empty()) {
                try {
                    utils::fb_utilities::set_broadcast_address(listen_address);
                } catch (...) {
                    startlog.error("Bad configuration: invalid 'listen_address': {}: {}", listen_address, std::current_exception());
                    throw bad_configuration_error();
                }
            } else {
                startlog.error("Bad configuration: neither listen_address nor broadcast_address are defined\n");
                throw bad_configuration_error();
            }

            if (!broadcast_rpc_address.empty()) {
                utils::fb_utilities::set_broadcast_rpc_address(broadcast_rpc_address);
            } else {
                if (rpc_address == "0.0.0.0") {
                    startlog.error("If rpc_address is set to a wildcard address {}, then you must set broadcast_rpc_address to a value other than {}", rpc_address, rpc_address);
                    throw bad_configuration_error();
                }
                utils::fb_utilities::set_broadcast_rpc_address(rpc_address);
            }

            // TODO: lib.
            auto is_true = [](sstring val) {
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                return val == "true" || val == "1";
            };

            // The start_native_transport method is invoked by API as well, and uses the config object
            // (through db) directly. Lets fixup default valued right here instead then, so it in turn can be
            // kept simple
            // TODO: make intrinsic part of config defaults instead
            auto& ceo = cfg->client_encryption_options();
            if (is_true(get_or_default(ceo, "enabled", "false"))) {
                ceo["enabled"] = "true";
                ceo["certificate"] = get_or_default(ceo, "certificate", relative_conf_dir("scylla.crt").string());
                ceo["keyfile"] = get_or_default(ceo, "keyfile", relative_conf_dir("scylla.key").string());
            } else {
                ceo["enabled"] = "false";
            }

            using namespace locator;
            // Re-apply strict-dma after we've read the config file, this time
            // to all reactors
            if (opts.count("developer-mode")) {
                smp::invoke_on_all([] { engine().set_strict_dma(false); }).get();
            }
            supervisor::notify("creating tracing");
            tracing::tracing::create_tracing("trace_keyspace_helper").get();
            supervisor::notify("creating snitch");
            i_endpoint_snitch::create_snitch(cfg->endpoint_snitch()).get();
            // #293 - do not stop anything
            // engine().at_exit([] { return i_endpoint_snitch::stop_snitch(); });
            supervisor::notify("determining DNS name");
            dns::hostent e = dns::gethostbyname(api_address).get0();
            supervisor::notify("starting API server");
            auto ip = e.addresses[0].in.s_addr;
            ctx.http_server.start().get();
            api::set_server_init(ctx).get();
            ctx.http_server.listen(ipv4_addr{ip, api_port}).get();
            startlog.info("Scylla API server listening on {}:{} ...", api_address, api_port);
            supervisor::notify("initializing storage service");
            init_storage_service(db);
            supervisor::notify("starting per-shard database core");
            // Note: changed from using a move here, because we want the config object intact.
            db.start(std::ref(*cfg)).get();
            engine().at_exit([&db, &return_value] {
                // A shared sstable must be compacted by all shards before it can be deleted.
                // Since we're stoping, that's not going to happen.  Cancel those pending
                // deletions to let anyone waiting on them to continue.
                sstables::cancel_atomic_deletions();
                // #293 - do not stop anything - not even db (for real)
                //return db.stop();
                // call stop on each db instance, but leave the shareded<database> pointers alive.
                return db.invoke_on_all([](auto& db) {
                    return db.stop();
                }).then([] {
                        return sstables::await_background_jobs_on_all_shards();
                }).then([&return_value] {
                        ::_exit(return_value);
                });
            });
            verify_seastar_io_scheduler(opts.count("max-io-requests"), db.local().get_config().developer_mode()).get();
            supervisor::notify("creating data directories");
            dirs.touch_and_lock(db.local().get_config().data_file_directories()).get();
            supervisor::notify("creating commitlog directory");
            dirs.touch_and_lock(db.local().get_config().commitlog_directory()).get();
            supervisor::notify("verifying data and commitlog directories");
            std::unordered_set<sstring> directories;
            directories.insert(db.local().get_config().data_file_directories().cbegin(),
                    db.local().get_config().data_file_directories().cend());
            directories.insert(db.local().get_config().commitlog_directory());
            parallel_for_each(directories, [&db] (sstring pathname) {
                return disk_sanity(pathname, db.local().get_config().developer_mode());
            }).get();

            // Initialization of a keyspace is done by shard 0 only. For system
            // keyspace, the procedure  will go through the hardcoded column
            // families, and in each of them, it will load the sstables for all
            // shards using distributed database object.
            // Iteration through column family directory for sstable loading is
            // done only by shard 0, so we'll no longer face race conditions as
            // described here: https://github.com/scylladb/scylla/issues/1014
            distributed_loader::init_system_keyspace(db).get();

            supervisor::notify("starting gossip");
            // Moved local parameters here, esp since with the
            // ssl stuff it gets to be a lot.
            uint16_t storage_port = cfg->storage_port();
            uint16_t ssl_storage_port = cfg->ssl_storage_port();
            double phi = cfg->phi_convict_threshold();
            auto seed_provider= cfg->seed_provider();
            sstring cluster_name = cfg->cluster_name();

            const auto& ssl_opts = cfg->server_encryption_options();
            auto encrypt_what = get_or_default(ssl_opts, "internode_encryption", "none");
            auto trust_store = get_or_default(ssl_opts, "truststore");
            auto cert = get_or_default(ssl_opts, "certificate", relative_conf_dir("scylla.crt").string());
            auto key = get_or_default(ssl_opts, "keyfile", relative_conf_dir("scylla.key").string());

            init_ms_fd_gossiper(listen_address
                    , storage_port
                    , ssl_storage_port
                    , encrypt_what
                    , trust_store
                    , cert
                    , key
                    , cfg->internode_compression()
                    , seed_provider
                    , cluster_name
                    , phi
                    , cfg->listen_on_broadcast_address());
            supervisor::notify("starting messaging service");
            supervisor::notify("starting storage proxy");
            proxy.start(std::ref(db)).get();
            // #293 - do not stop anything
            // engine().at_exit([&proxy] { return proxy.stop(); });
            supervisor::notify("starting migration manager");
            mm.start().get();
            // #293 - do not stop anything
            // engine().at_exit([&mm] { return mm.stop(); });
            supervisor::notify("starting query processor");
            qp.start(std::ref(proxy), std::ref(db)).get();
            // #293 - do not stop anything
            // engine().at_exit([&qp] { return qp.stop(); });
            supervisor::notify("initializing batchlog manager");
            db::get_batchlog_manager().start(std::ref(qp)).get();
            // #293 - do not stop anything
            // engine().at_exit([] { return db::get_batchlog_manager().stop(); });
            supervisor::notify("loading sstables");
            auto& ks = db.local().find_keyspace(db::system_keyspace::NAME);
            parallel_for_each(ks.metadata()->cf_meta_data(), [&ks] (auto& pair) {
                auto cfm = pair.second;
                return ks.make_directory_for_column_family(cfm->cf_name(), cfm->id());
            }).get();
            supervisor::notify("loading sstables");
            distributed_loader::init_non_system_keyspaces(db, proxy).get();
            supervisor::notify("setting up system keyspace");
            db::system_keyspace::setup(db, qp).get();
            supervisor::notify("starting commit log");
            auto cl = db.local().commitlog();
            if (cl != nullptr) {
                auto paths = cl->get_segments_to_replay();
                if (!paths.empty()) {
                    supervisor::notify("replaying commit log");
                    auto rp = db::commitlog_replayer::create_replayer(qp).get0();
                    rp.recover(paths).get();
                    supervisor::notify("replaying commit log - flushing memtables");
                    db.invoke_on_all([] (database& db) {
                        return db.flush_all_memtables();
                    }).get();
                    supervisor::notify("replaying commit log - removing old commitlog segments");
                    for (auto& path : paths) {
                        ::unlink(path.c_str());
                    }
                }
            }
            // If the same sstable is shared by several shards, it cannot be
            // deleted until all shards decide to compact it. So we want to
            // start thse compactions now. Note we start compacting only after
            // all sstables in this CF were loaded on all shards - otherwise
            // we will have races between the compaction and loading processes
            // We also want to trigger regular compaction on boot.
            db.invoke_on_all([&proxy] (database& db) {
                // avoid excessive disk usage by making sure all shards reshard
                // shared sstables in the same order. That's done by choosing
                // column families in UUID order, and each individual column
                // family will reshard shared sstables in generation order.
                auto cfs = boost::copy_range<std::map<utils::UUID, lw_shared_ptr<column_family>>>(db.get_column_families());
                for (auto& x : cfs) {
                    column_family& cf = *(x.second);
                    // We start the rewrite, but do not wait for it.
                    cf.start_rewrite();
                    cf.trigger_compaction();
                }
            }).get();
            api::set_server_storage_service(ctx).get();
            api::set_server_gossip(ctx).get();
            api::set_server_snitch(ctx).get();
            api::set_server_storage_proxy(ctx).get();
            api::set_server_load_sstable(ctx).get();
            supervisor::notify("initializing migration manager RPC verbs");
            service::get_migration_manager().invoke_on_all([] (auto& mm) {
                mm.init_messaging_service();
            }).get();
            supervisor::notify("initializing storage proxy RPC verbs");
            proxy.invoke_on_all([] (service::storage_proxy& p) {
                p.init_messaging_service();
            }).get();
            supervisor::notify("starting streaming service");
            streaming::stream_session::init_streaming_service(db).get();
            api::set_server_stream_manager(ctx).get();
            // Start handling REPAIR_CHECKSUM_RANGE messages
            net::get_messaging_service().invoke_on_all([&db] (auto& ms) {
                ms.register_repair_checksum_range([&db] (sstring keyspace, sstring cf, dht::token_range range, rpc::optional<repair_checksum> hash_version) {
                    auto hv = hash_version ? *hash_version : repair_checksum::legacy;
                    return do_with(std::move(keyspace), std::move(cf), std::move(range),
                            [&db, hv] (auto& keyspace, auto& cf, auto& range) {
                        return checksum_range(db, keyspace, cf, range, hv);
                    });
                });
            }).get();
            supervisor::notify("starting storage service", true);
            auto& ss = service::get_local_storage_service();
            ss.init_server().get();
            api::set_server_messaging_service(ctx).get();
            api::set_server_storage_service(ctx).get();
            supervisor::notify("starting batchlog manager");
            db::get_batchlog_manager().invoke_on_all([] (db::batchlog_manager& b) {
                return b.start();
            }).get();
            supervisor::notify("starting load broadcaster");
            // should be unique_ptr, but then lambda passed to at_exit will be non copieable and
            // casting to std::function<> will fail to compile
            auto lb = make_shared<service::load_broadcaster>(db, gms::get_local_gossiper());
            lb->start_broadcasting();
            service::get_local_storage_service().set_load_broadcaster(lb);
            engine().at_exit([lb = std::move(lb)] () mutable { return lb->stop_broadcasting(); });
            gms::get_local_gossiper().wait_for_gossip_to_settle().get();
            api::set_server_gossip_settle(ctx).get();
            supervisor::notify("starting native transport");
            service::get_local_storage_service().start_native_transport().get();
            if (start_thrift) {
                service::get_local_storage_service().start_rpc_server().get();
            }
            if (cfg->defragment_memory_on_idle()) {
                smp::invoke_on_all([] () {
                    engine().set_idle_cpu_handler([] (reactor::work_waiting_on_reactor check_for_work) {
                        return logalloc::shard_tracker().compact_on_idle(check_for_work);
                    });
                }).get();
            }
            smp::invoke_on_all([&cfg] () {
                return logalloc::shard_tracker().set_reclamation_step(cfg->lsa_reclamation_step());
            }).get();
            if (cfg->abort_on_lsa_bad_alloc()) {
                smp::invoke_on_all([&cfg]() {
                    return logalloc::shard_tracker().enable_abort_on_bad_alloc();
                }).get();
            }
            api::set_server_done(ctx).get();
            dns::hostent prom_addr = dns::gethostbyname(cfg->prometheus_address()).get0();
            supervisor::notify("starting prometheus API server");
            uint16_t pport = cfg->prometheus_port();
            if (pport) {
                pctx.metric_help = "Scylla server statistics";
                pctx.prefix = cfg->prometheus_prefix();
                prometheus_server.start().get();
                prometheus::start(prometheus_server, pctx);
                prometheus_server.listen(ipv4_addr{prom_addr.addresses[0].in.s_addr, pport}).handle_exception([pport, &cfg] (auto ep) {
                    startlog.error("Could not start Prometheus API server on {}:{}: {}", cfg->prometheus_address(), pport, ep);
                    return make_exception_future<>(ep);
                }).get();
            }
            supervisor::notify("serving");
            // Register at_exit last, so that storage_service::drain_on_shutdown will be called first
            engine().at_exit([] {
                return repair_shutdown(service::get_local_storage_service().db());
            });
            engine().at_exit([] {
                return service::get_local_storage_service().drain_on_shutdown();
            });

            engine().at_exit([&db] {
                return db.invoke_on_all([](auto& db) {
                    return db.get_compaction_manager().stop();
                });
            });
        }).then_wrapped([&return_value] (auto && f) {
            try {
                f.get();
            } catch (...) {
                return_value = 1;
                engine_exit(std::current_exception());
            }
        });
    });
  } catch (...) {
      // reactor may not have been initialized, so can't use logger
      fprint(std::cerr, "FATAL: Exception during startup, aborting: %s\n", std::current_exception());
      return 7; // 1 has a special meaning for upstart
  }
}

namespace debug {

seastar::sharded<database>* db;

}
