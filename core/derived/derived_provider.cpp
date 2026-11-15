#include "derived/derived_provider.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <format>
#include <utility>

#include "logging/logger.hpp"

namespace anolis {
namespace derived {

namespace {

namespace adpp = anolis::deviceprovider::v1;
namespace fg = fluxgraph::v2;

// The names the calling thread's counts are published under.
constexpr const char *kCountNames[] = {"host_calls",         "host_ticks",          "host_most_ticks", "host_gaps",
                                       "host_refused_calls", "host_refused_writes", "host_thrown"};

std::int64_t system_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// A scalar of a graph file. A quoted scalar is text; a plain one is a whole number, a number or a boolean where
/// it reads as one, and text otherwise ("200ms", a formula).
fg::Setting scalar_setting(const YAML::Node &node) {
    const std::string &text = node.Scalar();
    if (node.Tag() == "!") {
        return fg::Setting::text(text);
    }
    if (text == "true" || text == "false") {
        return fg::Setting::boolean(text == "true");
    }
    if (!text.empty()) {
        char *end = nullptr;
        errno = 0;
        const long long whole = std::strtoll(text.c_str(), &end, 10);
        if (errno == 0 && end == text.c_str() + text.size()) {
            return fg::Setting::whole(whole);
        }
        errno = 0;
        const double number = std::strtod(text.c_str(), &end);
        if (errno == 0 && end == text.c_str() + text.size()) {
            return fg::Setting::number(number);
        }
    }
    return fg::Setting::text(text);
}

fg::Setting to_setting(const YAML::Node &node) {
    if (node.IsMap()) {
        fg::Setting map = fg::Setting::map();
        for (const auto &entry : node) {
            map.set(entry.first.as<std::string>(), to_setting(entry.second));
        }
        return map;
    }
    if (node.IsSequence()) {
        fg::Setting list = fg::Setting::list();
        for (const auto &item : node) {
            list.add(to_setting(item));
        }
        return list;
    }
    return scalar_setting(node);
}

/// O1, going in. A value the cache does not hold is written `bad`.
fg::State state_of(adpp::SignalValue_Quality quality) {
    switch (quality) {
        case adpp::SignalValue_Quality_QUALITY_OK:
            return fg::State::good;
        case adpp::SignalValue_Quality_QUALITY_STALE:
            return fg::State::uncertain;
        case adpp::SignalValue_Quality_QUALITY_FAULT:
            return fg::State::bad;
        default:
            return fg::State::not_ready;
    }
}

/// O1, coming out.
adpp::SignalValue_Quality quality_of(fg::State state) {
    switch (state) {
        case fg::State::good:
            return adpp::SignalValue_Quality_QUALITY_OK;
        case fg::State::uncertain:
            return adpp::SignalValue_Quality_QUALITY_STALE;
        case fg::State::bad:
            return adpp::SignalValue_Quality_QUALITY_FAULT;
        default:
            return adpp::SignalValue_Quality_QUALITY_UNKNOWN;
    }
}

double as_double(const adpp::Value &value) {
    switch (value.kind_case()) {
        case adpp::Value::kDoubleValue:
            return value.double_value();
        case adpp::Value::kInt64Value:
            return static_cast<double>(value.int64_value());
        case adpp::Value::kUint64Value:
            return static_cast<double>(value.uint64_value());
        case adpp::Value::kBoolValue:
            return value.bool_value() ? 1.0 : 0.0;
        default:
            return 0.0;
    }
}

void set_timestamp(google::protobuf::Timestamp *timestamp, std::int64_t ns) {
    timestamp->set_seconds(ns / 1'000'000'000);
    timestamp->set_nanos(static_cast<std::int32_t>(ns % 1'000'000'000));
}

}  // namespace

DerivedProvider::DerivedProvider(DerivedConfig config, const state::StateCache &state_cache, Acting acting)
    : config_(std::move(config)), state_cache_(state_cache), acting_(std::move(acting)) {}

DerivedProvider::~DerivedProvider() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DerivedProvider::set_error(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::move(message);
}

std::string DerivedProvider::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool DerivedProvider::start() {
    fg::Setting graph;
    try {
        graph = to_setting(YAML::LoadFile(config_.graph_path));
    } catch (const YAML::Exception &e) {
        set_error(std::format("graph file '{}': {}", config_.graph_path, e.what()));
        return false;
    }
    auto compiled = fg::Engine::compile(graph);
    if (!compiled) {
        set_error(std::format("graph file '{}' refused: {}", config_.graph_path, compiled.error().message));
        return false;
    }
    engine_ = std::make_unique<fg::Engine>(std::move(compiled.value()));
    interval_ns_ =
        config_.call_interval_ms > 0 ? std::int64_t{config_.call_interval_ms} * 1'000'000 : engine_->period();

    const fg::Setting *declared = graph.find("inputs");
    for (const auto &input : config_.inputs) {
        Input one{input, false};
        if (declared != nullptr && declared->is(fg::Setting::Kind::map)) {
            const fg::Setting *spec = declared->find(input.name);
            const fg::Setting *type =
                spec != nullptr && spec->is(fg::Setting::Kind::map) ? spec->find("type") : nullptr;
            one.boolean = type != nullptr && type->is(fg::Setting::Kind::text) && type->as_text() == "boolean";
        }
        inputs_.push_back(std::move(one));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &output : engine_->outputs()) {
            published_.push_back({std::string(output.name), output.value});
        }
    }
    LOG_INFO("[Derived] " << config_.id << ": graph " << config_.graph_path << " compiled, period "
                          << engine_->period() / 1'000'000 << " ms, called every " << interval_ns_ / 1'000'000
                          << " ms, " << inputs_.size() << " inputs, " << published_.size() << " outputs");

    running_ = true;
    thread_ = std::thread([this]() { run(); });
    return true;
}

bool DerivedProvider::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    return true;
}

void DerivedProvider::run() {
    auto next = std::chrono::steady_clock::now();
    while (running_) {
        call_once();
        next += std::chrono::nanoseconds(interval_ns_);
        // A call that overran is not made up for: the engine catches up by its own rule.
        next = std::max(next, std::chrono::steady_clock::now());
        std::this_thread::sleep_until(next);
    }
}

fg::Status DerivedProvider::write(const Input &input, fg::State state, double number, std::int64_t at) {
    if (!fg::usable(state)) {
        return engine_->write(input.config.name, state, at);
    }
    if (input.boolean) {
        return engine_->write(input.config.name, state, number != 0.0, at);
    }
    return engine_->write(input.config.name, state, number, at);
}

void DerivedProvider::on_read(const std::string &device_handle, const adpp::ReadSignalsResponse *response,
                              std::chrono::steady_clock::time_point returned) {
    const std::int64_t at = std::chrono::duration_cast<std::chrono::nanoseconds>(returned.time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(waiting_mutex_);
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        const auto &config = inputs_[i].config;
        if (config.device_handle != device_handle) {
            continue;
        }
        if (response == nullptr) {
            waiting_.push_back({i, fg::State::bad, 0.0, at});
            continue;
        }
        for (const auto &value : response->values()) {
            if (value.signal_id() == config.signal_id) {
                waiting_.push_back({i, state_of(value.quality()), as_double(value.value()), at});
            }
        }
    }
}

void DerivedProvider::call_once() {
    // O7: one clock for every sample time and every call. On arrival, it is the runtime's monotonic clock, and a
    // sample's time is when its read returned. Otherwise the cache's times are each provider's wall clock, the
    // calls are on the wall clock too, and a step of it reaches the engine.
    const bool on_arrival = config_.sample_time_on_arrival;
    const std::int64_t now =
        on_arrival
            ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                  .count()
            : system_now_ns();
    Counts counts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        counts = counts_;
    }
    const auto count = [&counts](const fg::Status &written, const Input &input) {
        if (!written) {
            counts.refused_writes += 1;
            LOG_WARN("[Derived] write to " << input.config.name << " refused: " << written.error().message);
        }
    };

    if (on_arrival) {
        std::vector<Waiting> waiting;
        {
            std::lock_guard<std::mutex> lock(waiting_mutex_);
            waiting.swap(waiting_);
        }
        for (const auto &sample : waiting) {
            count(write(inputs_[sample.input], sample.state, sample.number, sample.at), inputs_[sample.input]);
        }
    }
    for (const auto &input : inputs_) {
        if (input.config.constant) {
            count(engine_->write(input.config.name, fg::State::good, *input.config.constant, now), input);
        } else if (!input.config.not_acting_on.empty()) {
            count(engine_->write(input.config.name, fg::State::good, !acting_(input.config.not_acting_on), now), input);
        } else if (!on_arrival) {
            const auto cached = state_cache_.get_signal_value(input.config.device_handle, input.config.signal_id);
            if (!cached) {
                count(engine_->write(input.config.name, fg::State::bad, now), input);
            } else {
                const std::int64_t at =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(cached->timestamp.time_since_epoch()).count() +
                    std::int64_t{config_.sample_skew_ms} * 1'000'000;
                count(write(input, state_of(cached->quality), as_double(cached->value), at), input);
            }
        }
    }

    auto report = engine_->tick(now);
    if (!report) {
        counts.refused_calls += 1;
        LOG_WARN("[Derived] call refused, rebasing: " << report.error().message);
        engine_->rebase();
        report = engine_->tick(now);
    }
    if (report && report->ran > 0) {
        counts.calls += 1;
        counts.ticks += report->ran;
        counts.most_ticks = std::max(counts.most_ticks, static_cast<double>(report->ran));
        counts.gaps += report->gap ? 1 : 0;
        counts.thrown += static_cast<double>(report->thrown);
        if (report->gap) {
            LOG_WARN("[Derived] gap: " << (report->skipped_known ? std::to_string(report->skipped) : "unknown")
                                       << " ticks passed over");
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    counts_ = counts;
    if (report && report->ran > 0) {
        const auto outputs = engine_->outputs();
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            published_[i].value = outputs[i].value;
        }
        published_at_ns_ = system_now_ns();
    }
}

bool DerivedProvider::hello(adpp::HelloResponse &response) {
    response.set_protocol_version("v1");
    response.set_provider_name("anolis-derived-spike");
    response.set_provider_version("0");
    return true;
}

bool DerivedProvider::list_devices(std::vector<adpp::Device> &devices) {
    adpp::Device device;
    device.set_device_id(kDeviceId);
    device.set_provider_name("anolis-derived-spike");
    device.set_type_id("derived.graph");
    device.set_type_version("0");
    device.set_label("Derived signals");
    devices.push_back(std::move(device));
    return true;
}

bool DerivedProvider::describe_device(const std::string &device_id, adpp::DescribeDeviceResponse &response) {
    if (device_id != kDeviceId) {
        set_error("no such device: " + device_id);
        return false;
    }
    std::vector<adpp::Device> devices;
    list_devices(devices);
    *response.mutable_device() = devices.front();

    // Polled as often as the engine is called: a value is seen at most one poll after its tick.
    const double hint_hz = 1e9 / static_cast<double>(interval_ns_);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &published : published_) {
        auto *signal = response.mutable_capabilities()->add_signals();
        signal->set_signal_id(published.name);
        signal->set_name(published.name);
        signal->set_value_type(published.value.type == fg::Type::boolean ? adpp::VALUE_TYPE_BOOL
                                                                         : adpp::VALUE_TYPE_DOUBLE);
        signal->set_poll_hint_hz(hint_hz);
    }
    for (const char *name : kCountNames) {
        auto *signal = response.mutable_capabilities()->add_signals();
        signal->set_signal_id(name);
        signal->set_name(name);
        signal->set_value_type(adpp::VALUE_TYPE_DOUBLE);
        signal->set_poll_hint_hz(hint_hz);
    }
    return true;
}

bool DerivedProvider::read_signals(const std::string &device_id, const std::vector<std::string> &signal_ids,
                                   adpp::ReadSignalsResponse &response, adpp::Status_Code &status) {
    status = adpp::Status_Code_CODE_OK;
    if (device_id != kDeviceId) {
        status = adpp::Status_Code_CODE_NOT_FOUND;
        set_error("no such device: " + device_id);
        return false;
    }
    response.set_device_id(device_id);
    const auto wanted = [&signal_ids](const std::string &name) {
        return signal_ids.empty() || std::find(signal_ids.begin(), signal_ids.end(), name) != signal_ids.end();
    };

    std::lock_guard<std::mutex> lock(mutex_);
    const std::int64_t at = published_at_ns_ != 0 ? published_at_ns_ : system_now_ns();
    for (const auto &published : published_) {
        if (!wanted(published.name)) {
            continue;
        }
        auto *out = response.add_values();
        out->set_signal_id(published.name);
        // A value that is not usable has no value (rule 3.2); the protocol has no way to say so but quality.
        const bool usable = published.value.is_usable();
        if (published.value.type == fg::Type::boolean) {
            out->mutable_value()->set_type(adpp::VALUE_TYPE_BOOL);
            out->mutable_value()->set_bool_value(usable && published.value.boolean);
        } else {
            out->mutable_value()->set_type(adpp::VALUE_TYPE_DOUBLE);
            out->mutable_value()->set_double_value(usable ? published.value.number : 0.0);
        }
        out->set_quality(quality_of(published.value.state));
        set_timestamp(out->mutable_timestamp(), at);
    }
    const double counts[] = {counts_.calls,         counts_.ticks,          counts_.most_ticks, counts_.gaps,
                             counts_.refused_calls, counts_.refused_writes, counts_.thrown};
    for (std::size_t i = 0; i < std::size(kCountNames); ++i) {
        if (!wanted(kCountNames[i])) {
            continue;
        }
        auto *out = response.add_values();
        out->set_signal_id(kCountNames[i]);
        out->mutable_value()->set_type(adpp::VALUE_TYPE_DOUBLE);
        out->mutable_value()->set_double_value(counts[i]);
        out->set_quality(adpp::SignalValue_Quality_QUALITY_OK);
        set_timestamp(out->mutable_timestamp(), at);
    }
    return true;
}

bool DerivedProvider::call(const std::string & /*device_id*/, uint32_t /*function_id*/,
                           const std::string &function_name, const std::map<std::string, adpp::Value> & /*args*/,
                           adpp::CallResponse & /*response*/) {
    set_error("the derived-signals device has no functions: " + function_name);
    return false;
}

bool DerivedProvider::get_health(adpp::GetHealthResponse &response) {
    const bool up = running_.load();
    response.mutable_provider()->set_state(up ? adpp::ProviderHealth::STATE_OK : adpp::ProviderHealth::STATE_DEGRADED);
    auto *device = response.add_devices();
    device->set_device_id(kDeviceId);
    device->set_state(up ? adpp::DeviceHealth::STATE_OK : adpp::DeviceHealth::STATE_UNREACHABLE);
    std::lock_guard<std::mutex> lock(mutex_);
    set_timestamp(device->mutable_last_seen(), published_at_ns_ != 0 ? published_at_ns_ : system_now_ns());
    return true;
}

}  // namespace derived
}  // namespace anolis
