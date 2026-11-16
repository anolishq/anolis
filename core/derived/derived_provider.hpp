#pragma once

/**
 * @file derived_provider.hpp
 * @brief The derived-signals spike: FluxGraph's version 2 core behind the provider interface, in process.
 *
 * Throwaway (docs/rfcs/derived-signals.md, O10, "In the runtime"). A thread of its own writes the graph's inputs
 * from the state cache and calls the engine; the registry, the poll thread, HTTP, telemetry and health see the
 * graph's published outputs as the signals of one device of one more provider. It calls nothing (P2).
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "derived/derived_config.hpp"
#include "fluxgraph/v2/engine.hpp"
#include "provider/i_provider_handle.hpp"
#include "state/state_cache.hpp"

namespace anolis {
namespace derived {

class DerivedProvider : public provider::IProviderHandle {
public:
    /// `acting`: whether the runtime's automation is acting on a device, by its handle.
    using Acting = std::function<bool(const std::string &)>;

    /// Whether the provider of a device ("provider/device") can be reached.
    using Available = std::function<bool(const std::string &)>;
    DerivedProvider(DerivedConfig config, const state::StateCache &state_cache, Acting acting, Available available);
    ~DerivedProvider() override;

    DerivedProvider(const DerivedProvider &) = delete;
    DerivedProvider &operator=(const DerivedProvider &) = delete;

    bool start() override;
    bool is_available() const override { return running_.load(); }
    bool stop() override;

    bool hello(anolis::deviceprovider::v1::HelloResponse &response) override;
    bool list_devices(std::vector<anolis::deviceprovider::v1::Device> &devices) override;
    bool describe_device(const std::string &device_id,
                         anolis::deviceprovider::v1::DescribeDeviceResponse &response) override;
    bool read_signals(const std::string &device_id, const std::vector<std::string> &signal_ids,
                      anolis::deviceprovider::v1::ReadSignalsResponse &response,
                      anolis::deviceprovider::v1::Status_Code &status) override;
    bool call(const std::string &device_id, uint32_t function_id, const std::string &function_name,
              const std::map<std::string, anolis::deviceprovider::v1::Value> &args,
              anolis::deviceprovider::v1::CallResponse &response) override;
    bool get_health(anolis::deviceprovider::v1::GetHealthResponse &response) override;

    std::string last_error() const override;
    anolis::deviceprovider::v1::Status_Code last_status_code() const override {
        return anolis::deviceprovider::v1::Status_Code_CODE_OK;
    }
    const std::string &provider_id() const override { return config_.id; }

    /// A read of a device has returned (StateCache::SampleSink). Its samples wait for the next call.
    void on_read(const std::string &device_handle, const anolis::deviceprovider::v1::ReadSignalsResponse *response,
                 std::chrono::steady_clock::time_point returned);

    static constexpr const char *kDeviceId = "graph";

private:
    struct Input {
        DerivedInputConfig config;
        bool boolean = false;
        bool lost = false;  // of not_acting_on: its provider was down, and no read of the device has returned since
    };
    struct Published {
        std::string name;
        fluxgraph::v2::Value value;
    };
    /// What the calling thread counts, published as signals beside the graph's own.
    struct Counts {
        double calls = 0;           // calls that ran at least one tick
        double ticks = 0;           // ticks run
        double most_ticks = 0;      // the most ticks one call ran
        double gaps = 0;            // calls that were a gap
        double refused_calls = 0;   // calls the engine refused; each is followed by a rebase
        double refused_writes = 0;  // writes the engine refused
        double thrown = 0;          // writes the engine threw away
        double stale = 0;           // readings given as STALE
    };

    /// A sample that waits for the engine's thread: which input, and what is written to it.
    struct Waiting {
        std::size_t input = 0;
        fluxgraph::v2::State state = fluxgraph::v2::State::bad;
        double number = 0.0;
        std::int64_t at = 0;
    };

    void run();
    fluxgraph::v2::Status write(const Input &input, fluxgraph::v2::State state, double number, std::int64_t at);
    void call_once();
    void set_error(std::string message);

    DerivedConfig config_;
    const state::StateCache &state_cache_;
    Acting acting_;
    Available available_;
    std::unique_ptr<fluxgraph::v2::Engine> engine_;
    std::vector<Input> inputs_;
    std::int64_t interval_ns_ = 0;
    std::chrono::steady_clock::time_point started_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex waiting_mutex_;
    std::vector<Waiting> waiting_;         // in the order the reads returned
    std::vector<std::string> returned_;    // the devices a read has returned from since the last call
    std::atomic<std::uint64_t> stale_{0};  // readings given as STALE

    mutable std::mutex mutex_;  // guards what follows
    std::vector<Published> published_;
    std::int64_t published_at_ns_ = 0;              // the time of the last tick run, on the system clock
    std::chrono::steady_clock::time_point ticked_;  // when the last call that ran a tick returned
    Counts counts_;
    std::string error_;
};

}  // namespace derived
}  // namespace anolis
