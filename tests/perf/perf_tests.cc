/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2018 ScyllaDB Ltd.
 */

#include <seastar/testing/perf_tests.hh>

#include <cstdio>
#include <fstream>
#include <regex>
#include <ranges>

#include <fmt/ostream.h>

#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sharded.hh>
#include <seastar/json/formatter.hh>
#include <seastar/util/later.hh>
#include <seastar/testing/random.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/reactor.hh>

#include <signal.h>

#if FMT_VERSION >= 90000
namespace perf_tests::internal {
    struct duration;
}
template <> struct fmt::formatter<perf_tests::internal::duration> : fmt::ostream_formatter {};
#endif

namespace perf_tests {
namespace internal {

namespace {

// We need to use signal-based timer instead of seastar ones so that
// tests that do not suspend can be interrupted.
// This causes no overhead though since the timer is used only in a dry run.
class signal_timer {
    std::function<void()> _fn;
    timer_t _timer;
public:
    explicit signal_timer(std::function<void()> fn) : _fn(fn) {
        sigevent se{};
        se.sigev_notify = SIGEV_SIGNAL;
        se.sigev_signo = SIGALRM;
        se.sigev_value.sival_ptr = this;
        auto ret = timer_create(CLOCK_MONOTONIC, &se, &_timer);
        if (ret) {
            throw std::system_error(ret, std::system_category());
        }
    }

    ~signal_timer() {
        timer_delete(_timer);
    }

    void arm(std::chrono::steady_clock::duration dt) {
        time_t sec = std::chrono::duration_cast<std::chrono::seconds>(dt).count();
        auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
        nsec -= std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(sec)).count();

        itimerspec ts{};
        ts.it_value.tv_sec = sec;
        ts.it_value.tv_nsec = nsec;
        auto ret = timer_settime(_timer, 0, &ts, nullptr);
        if (ret) {
            throw std::system_error(ret, std::system_category());
        }
    }

    void cancel() {
        itimerspec ts{};
        auto ret = timer_settime(_timer, 0, &ts, nullptr);
        if (ret) {
            throw std::system_error(ret, std::system_category());
        }
    }
public:
    static void init() {
        struct sigaction sa{};
        sa.sa_sigaction = &signal_timer::signal_handler;
        sa.sa_flags = SA_SIGINFO;
        auto ret = sigaction(SIGALRM, &sa, nullptr);
        if (ret) {
            throw std::system_error(ret, std::system_category());
        }
    }
private:
    static void signal_handler(int, siginfo_t* si, void*) {
        auto t = static_cast<signal_timer*>(si->si_value.sival_ptr);
        t->_fn();
    }
};

}

uint64_t perf_stats::perf_mallocs() {
    return memory::stats().mallocs();
}

uint64_t perf_stats::perf_tasks_processed() {
    return engine().get_sched_stats().tasks_processed;
}

perf_stats perf_stats::snapshot(linux_perf_event* instructions_retired_counter, linux_perf_event* cpu_cycles_retired_counter) {
    return perf_stats(
        perf_mallocs(),
        perf_tasks_processed(),
        instructions_retired_counter ? instructions_retired_counter->read() : 0,
        cpu_cycles_retired_counter ? cpu_cycles_retired_counter->read() : 0
    );
}

time_measurement measure_time;

struct config;
struct result;

struct result_printer {
    virtual ~result_printer() = default;

    virtual void print_configuration(const config&) = 0;
    virtual void print_result(const result&) = 0;

    void update_name_column_length(size_t length) {
        _name_column_length = std::max(1ul, length);
    }
    size_t name_column_length() const {
        return _name_column_length;
    }

private:
    static constexpr size_t DEFAULT_NAME_COLUMN_LENGTH = 40;
    size_t _name_column_length = DEFAULT_NAME_COLUMN_LENGTH;
};

struct config {
    uint64_t single_run_iterations;
    std::chrono::nanoseconds single_run_duration;
    unsigned number_of_runs;
    std::vector<std::unique_ptr<result_printer>> printers;
    unsigned random_seed = 0;
};

struct result {
    sstring test_name = "";

    uint64_t total_iterations = 0;
    unsigned runs = 0;

    double median = 0.;
    double mad = 0.;
    double min = 0.;
    double max = 0.;

    double allocs = 0.;
    double tasks = 0.;
    double inst = 0.;
    double cycles = 0.;
};


struct duration {
    double value;
};

static inline std::ostream& operator<<(std::ostream& os, duration d)
{
    auto value = d.value;
    if (value < 1'000) {
        os << fmt::format("{:.3f}ns", value);
    } else if (value < 1'000'000) {
        // fmt hasn't discovered unicode yet so we are stuck with uicroseconds
        // See: https://github.com/fmtlib/fmt/issues/628
        os << fmt::format("{:.3f}us", value / 1'000);
    } else if (value < 1'000'000'000) {
        os << fmt::format("{:.3f}ms", value / 1'000'000);
    } else {
        os << fmt::format("{:.3f}s", value / 1'000'000'000);
    }
    return os;
}


struct stdout_printer final : result_printer {
  virtual void print_configuration(const config& c) override {
    fmt::print("{:<25} {}\n{:<25} {}\n{:<25} {}\n{:<25} {}\n{:<25} {}\n\n",
               "single run iterations:", c.single_run_iterations,
               "single run duration:", duration { double(c.single_run_duration.count()) },
               "number of runs:", c.number_of_runs,
               "number of cores:", smp::count,
               "random seed:", c.random_seed);
    fmt::print(header_format_string, "test", name_column_length(), "iterations", "median", "mad", "min", "max", "allocs", "tasks", "inst", "cycles");
  }

  virtual void print_result(const result& r) override {
    fmt::print(format_string, r.test_name, name_column_length(), r.total_iterations / r.runs, duration { r.median },
               duration { r.mad }, duration { r.min }, duration { r.max },
               r.allocs, r.tasks, r.inst, r.cycles);
  }

private:
  static constexpr auto header_format_string ="{:<{}} {:>11} {:>11} {:>11} {:>11} {:>11} {:>11} {:>11} {:>11} {:>11}\n";
  static constexpr auto format_string = "{:<{}} {:>11} {:>11} {:>11} {:>11} {:>11} {:>11.3f} {:>11.3f} {:>11.1f} {:>11.1f}\n";
};

class json_printer final : public result_printer {
    std::string _output_file;
    std::unordered_map<std::string,
                       std::unordered_map<std::string,
                                          std::unordered_map<std::string, double>>> _root;
public:
    explicit json_printer(const std::string& file) : _output_file(file) { }

    ~json_printer() {
        std::ofstream out(_output_file);
        out << json::formatter::to_json(_root);
    }

    virtual void print_configuration(const config&) override { }

    virtual void print_result(const result& r) override {
        auto& result = _root["results"][r.test_name];
        result["runs"] = r.runs;
        result["total_iterations"] = r.total_iterations;
        result["median"] = r.median;
        result["mad"] = r.mad;
        result["min"] = r.min;
        result["max"] = r.max;
        result["allocs"] = r.allocs;
        result["tasks"] = r.tasks;
        result["inst"] = r.inst;
        result["cycles"] = r.cycles;
    }
};

class markdown_printer final : public result_printer {
    static constexpr std::string_view header_format_string = "| {:<{}} | {:>11} | {:>11} | {:>11} | {:>11} | {:>11} | {:>11} | {:>11} | {:>11} |\n";
    static constexpr std::string_view body_format_string = "| {:<{}} | {:>11} | {:>11} | {:>11} | {:>11} | {:>11} | {:>11.3f} | {:>11.3f} | {:>11.1f} |\n";
    std::FILE* _output = nullptr;
public:
    explicit markdown_printer(const std::string& filename) {
        if (filename == "-") {
            _output = stdout;
        } else {
            _output = std::fopen(filename.c_str(), "w");
        }
        if (!_output) {
            throw std::invalid_argument(fmt::format("unable to write to {}", filename));
        }
    }
    ~markdown_printer() {
        if (_output != stdout) {
            std::fclose(_output);
        }
    }
    void print_configuration(const config&) override {
        fmt::print(_output, header_format_string, "test", name_column_length(), "iterations", "median", "mad", "min", "max", "allocs", "tasks", "inst");
        fmt::print(_output, header_format_string, "-", name_column_length(), "-", "-", "-", "-", "-", "-", "-", "-");
    }

    void print_result(const result& r) override {
        fmt::print(_output, body_format_string, r.test_name, name_column_length(), r.total_iterations / r.runs, duration { r.median },
                   duration { r.mad }, duration { r.min }, duration { r.max },
                   r.allocs, r.tasks, r.inst);
    }

};

void performance_test::do_run(const config& conf)
{
    _max_single_run_iterations = conf.single_run_iterations;
    if (!_max_single_run_iterations) {
        _max_single_run_iterations = std::numeric_limits<uint64_t>::max();
    }

    signal_timer tmr([this] {
        _max_single_run_iterations.store(0, std::memory_order_relaxed);
    });

    // dry run, estimate the number of iterations
    if (conf.single_run_duration.count()) {
        // switch out of seastar thread
        yield().then([&] {
            tmr.arm(conf.single_run_duration);
            return do_single_run().finally([&] {
                tmr.cancel();
                _max_single_run_iterations = _single_run_iterations;
            });
        }).get();
    }

    result r{};

    auto results = std::vector<double>(conf.number_of_runs);
    uint64_t total_iterations = 0;
    for (auto i = 0u; i < conf.number_of_runs; i++) {
        // switch out of seastar thread
        yield().then([&] {
            _single_run_iterations = 0;
            return do_single_run().then([&] (run_result rr) {
                clock_type::duration dt = rr.duration;
                double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
                results[i] = ns / _single_run_iterations;

                total_iterations += _single_run_iterations;

                r.allocs += double(rr.stats.allocations) / _single_run_iterations;
                r.tasks += double(rr.stats.tasks_executed) / _single_run_iterations;
                r.inst += double(rr.stats.instructions_retired) / _single_run_iterations;
                r.cycles += double(rr.stats.cpu_cycles_retired) / _single_run_iterations;
            });
        }).get();
    }

    r.test_name = name();
    r.total_iterations = total_iterations;
    r.runs = conf.number_of_runs;

    auto mid = conf.number_of_runs / 2;

    std::ranges::sort(results);
    r.median = results[mid];

    auto diffs =
        results | std::views::transform([&] (double x) { return fabs(x - r.median); })
        | std::ranges::to<std::vector<double>>();
    std::ranges::sort(diffs);
    r.mad = diffs[mid];

    r.min = results[0];
    r.max = results[results.size() - 1];

    r.allocs /= conf.number_of_runs;
    r.tasks /= conf.number_of_runs;
    r.inst /= conf.number_of_runs;
    r.cycles /= conf.number_of_runs;

    for (auto& rp : conf.printers) {
        rp->print_result(r);
    }
}

void performance_test::run(const config& conf)
{
    set_up();
    try {
        do_run(conf);
    } catch (...) {
        tear_down();
        throw;
    }
    tear_down();
}

std::vector<std::unique_ptr<performance_test>>& all_tests()
{
    static std::vector<std::unique_ptr<performance_test>> tests;
    return tests;
}

void performance_test::register_test(std::unique_ptr<performance_test> test)
{
    all_tests().emplace_back(std::move(test));
}

void run_all(const std::vector<std::string>& tests, config& conf)
{
    auto can_run = [tests = tests | std::ranges::to<std::vector<std::regex>>()] (auto&& test) {
        auto it = std::ranges::find_if(tests, [&test] (const std::regex& regex) {
            return std::regex_match(test->name(), regex);
        });
        return tests.empty() || it != tests.end();
    };

    size_t max_name_column_length = 0;
    for (auto&& test : all_tests() | std::views::filter(can_run)) {
        max_name_column_length = std::max(max_name_column_length, test->name().size());
    }

    for (auto& rp : conf.printers) {
        rp->update_name_column_length(max_name_column_length);
        rp->print_configuration(conf);
    }
    for (auto&& test : all_tests() | std::views::filter(std::move(can_run))) {
        test->run(conf);
    }
}

}
}

int main(int ac, char** av)
{
    using namespace perf_tests::internal;
    namespace bpo = boost::program_options;

    app_template app;
    app.add_options()
        ("iterations,i", bpo::value<size_t>()->default_value(0),
            "number of iterations in a single run")
        ("duration,d", bpo::value<double>()->default_value(1),
            "duration of a single run in seconds")
        ("runs,r", bpo::value<size_t>()->default_value(5), "number of runs")
        ("test,t", bpo::value<std::vector<std::string>>(), "tests to execute")
        ("random-seed,S", bpo::value<unsigned>()->default_value(0),
            "random number generator seed")
        ("no-stdout", "do not print to stdout")
        ("json-output", bpo::value<std::string>(), "output json file")
        ("md-output", bpo::value<std::string>(), "output markdown file")
        ("list", "list available tests")
        ;

    return app.run(ac, av, [&] {
        return async([&] {
            signal_timer::init();

            config conf;
            conf.single_run_iterations = app.configuration()["iterations"].as<size_t>();
            auto dur = std::chrono::duration<double>(app.configuration()["duration"].as<double>());
            conf.single_run_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(dur);
            conf.number_of_runs = app.configuration()["runs"].as<size_t>();
            conf.random_seed = app.configuration()["random-seed"].as<unsigned>();

            std::vector<std::string> tests_to_run;
            if (app.configuration().count("test")) {
                tests_to_run = app.configuration()["test"].as<std::vector<std::string>>();
            }

            if (app.configuration().count("list")) {
                fmt::print("available tests:\n");
                for (auto&& t : all_tests()) {
                    fmt::print("\t{}\n", t->name());
                }
                return;
            }

            if (!app.configuration().count("no-stdout")) {
                conf.printers.emplace_back(std::make_unique<stdout_printer>());
            }

            if (app.configuration().count("json-output")) {
                conf.printers.emplace_back(std::make_unique<json_printer>(
                    app.configuration()["json-output"].as<std::string>()
                ));
            }

            if (app.configuration().count("md-output")) {
                conf.printers.emplace_back(std::make_unique<markdown_printer>(
                    app.configuration()["md-output"].as<std::string>()
                ));
            }

            if (!conf.random_seed) {
                conf.random_seed = std::random_device()();
            }
            smp::invoke_on_all([seed = conf.random_seed] {
                auto local_seed = seed + this_shard_id();
                testing::local_random_engine.seed(local_seed);
            }).get();

            run_all(tests_to_run, conf);
        });
    });
}
