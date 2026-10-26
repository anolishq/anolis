// A provider in the shape of bread 0.8.1 on an unpowered bus (anolis#461): it
// answers Hello and WaitReady, lists no devices, and reports three expected
// devices UNREACHABLE in GetHealth. With `--exit-when <path>` it deletes the
// file and exits once the file appears, so a test can drive one supervised
// restart: the restarted helper finds no file and stays up. With `--mixed` it
// also lists rlht1, which describes but is reported UNREACHABLE, and reports
// dcmt2 OK without listing it: neither is a device its provider says is missing.
// With `--allowed-values` it also lists dcmt3, reported OK, whose CONFIG set_mode takes
// a `mode` limited to open_loop or closed_speed (anolis#467), and answers every
// call OK.
// With `--stop-chain <dir>` it lists gpio0, whose bool signal chain_open reads
// <dir>/chain ("1" true, "0" false, "stale" with stale quality; missing reads
// as a FAULT-quality false), and motor0, an ACTUATE set_pwm. It answers every
// call OK and appends device/function_id to <dir>/calls (#483). With
// `--stop-chain-actuating`, gpio0 also has an ACTUATE function; with
// `--stop-chain-no-hint`, chain_open carries no poll hint, so it is never polled.
// With `--stop-chain-estop`, motor0 also publishes a polled bool `estop` that
// reads <dir>/estop like chain_open, and while <dir>/motor0_off exists its
// reads fail UNAVAILABLE, as a device behind a cut supply does (#484).

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "protocol.pb.h"

namespace {

namespace adpp = anolis::deviceprovider::v1;

uint32_t decode_le32(const std::array<uint8_t, 4> &bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u) |
           (static_cast<uint32_t>(bytes[2]) << 16u) | (static_cast<uint32_t>(bytes[3]) << 24u);
}

std::array<uint8_t, 4> encode_le32(uint32_t value) {
    return {
        static_cast<uint8_t>(value & 0xFFu),
        static_cast<uint8_t>((value >> 8u) & 0xFFu),
        static_cast<uint8_t>((value >> 16u) & 0xFFu),
        static_cast<uint8_t>((value >> 24u) & 0xFFu),
    };
}

bool read_request(adpp::Request &request) {
    std::array<uint8_t, 4> len_bytes{};
    if (!std::cin.read(reinterpret_cast<char *>(len_bytes.data()), static_cast<std::streamsize>(len_bytes.size()))) {
        return false;
    }
    const uint32_t payload_len = decode_le32(len_bytes);
    if (payload_len == 0 || payload_len > (1024u * 1024u)) {
        return false;
    }
    std::vector<uint8_t> payload(payload_len);
    if (!std::cin.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(payload.size()))) {
        return false;
    }
    return request.ParseFromArray(payload.data(), static_cast<int>(payload.size()));
}

void write_response(const adpp::Response &response) {
    std::string bytes;
    response.SerializeToString(&bytes);
    const auto len = encode_le32(static_cast<uint32_t>(bytes.size()));
    std::cout.write(reinterpret_cast<const char *>(len.data()), static_cast<std::streamsize>(len.size()));
    std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    std::cout.flush();
}

const std::array<const char *, 3> kMissing = {"rlht0", "dcmt0", "dcmt1"};

// The test replaces these files by rename, and on Windows an open that lands
// mid-rename fails. Retrying for up to 50 ms hides the swap; a file removed for
// good still reads empty.
std::string read_word(const std::filesystem::path &path) {
    std::string word;
    for (int attempt = 0; attempt < 10 && word.empty(); ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::ifstream(path) >> word;
    }
    return word;
}

// "1" true, "0" false, "stale" with stale quality; anything else a FAULT false.
void set_bool_reading(adpp::SignalValue *value, const std::string &signal_id, const std::string &word) {
    value->set_signal_id(signal_id);
    value->mutable_value()->set_type(adpp::VALUE_TYPE_BOOL);
    value->mutable_value()->set_bool_value(word == "1");
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    value->mutable_timestamp()->set_seconds(seconds.count());
    value->mutable_timestamp()->set_nanos(
        static_cast<int32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count()));
    value->set_quality(word == "1" || word == "0" ? adpp::SignalValue_Quality_QUALITY_OK
                       : word == "stale"          ? adpp::SignalValue_Quality_QUALITY_STALE
                                                  : adpp::SignalValue_Quality_QUALITY_FAULT);
}

}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
    // Protobuf framing is binary; text mode can corrupt bytes (e.g., 0x1A EOF).
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::string exit_when;
    bool mixed = false;
    bool allowed_values = false;
    std::string stop_chain_dir;
    bool stop_chain_actuating = false;
    bool stop_chain_no_hint = false;
    bool stop_chain_estop = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--exit-when" && i + 1 < argc) {
            exit_when = argv[i + 1];
        } else if (std::string(argv[i]) == "--mixed") {
            mixed = true;
        } else if (std::string(argv[i]) == "--allowed-values") {
            allowed_values = true;
        } else if (std::string(argv[i]) == "--stop-chain" && i + 1 < argc) {
            stop_chain_dir = argv[i + 1];
        } else if (std::string(argv[i]) == "--stop-chain-actuating") {
            stop_chain_actuating = true;
        } else if (std::string(argv[i]) == "--stop-chain-no-hint") {
            stop_chain_no_hint = true;
        } else if (std::string(argv[i]) == "--stop-chain-estop") {
            stop_chain_estop = true;
        }
    }
    if (!exit_when.empty()) {
        // A provider with no devices gets no requests between health checks,
        // so the trigger is watched on its own thread rather than per request.
        std::thread([exit_when] {
            while (true) {
                std::error_code ec;
                if (std::filesystem::remove(exit_when, ec)) {
                    std::_Exit(0);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }).detach();
    }

    adpp::Request request;
    while (read_request(request)) {
        adpp::Response response;
        response.set_request_id(request.request_id());
        response.mutable_status()->set_code(adpp::Status_Code_CODE_OK);
        if (request.has_hello()) {
            auto *hello = response.mutable_hello();
            hello->set_protocol_version("v1");
            hello->set_provider_name("empty-inventory-helper");
            hello->set_provider_version("0.0.1");
            (*hello->mutable_metadata())["supports_wait_ready"] = "true";
        } else if (request.has_wait_ready()) {
            (*response.mutable_wait_ready()->mutable_diagnostics())["device_count"] = "0";
        } else if (request.has_list_devices()) {
            auto *list = response.mutable_list_devices();
            if (mixed) {
                list->add_devices()->set_device_id("rlht1");
            }
            if (allowed_values) {
                list->add_devices()->set_device_id("dcmt3");
            }
            if (!stop_chain_dir.empty()) {
                list->add_devices()->set_device_id("gpio0");
                list->add_devices()->set_device_id("motor0");
            }
        } else if (mixed && request.has_describe_device() && request.describe_device().device_id() == "rlht1") {
            auto *describe = response.mutable_describe_device();
            describe->mutable_device()->set_device_id("rlht1");
            auto *fn = describe->mutable_capabilities()->add_functions();
            fn->set_name("set_power");
            fn->set_function_id(1);
        } else if (allowed_values && request.has_describe_device() &&
                   request.describe_device().device_id() == "dcmt3") {
            auto *describe = response.mutable_describe_device();
            describe->mutable_device()->set_device_id("dcmt3");
            auto *fn = describe->mutable_capabilities()->add_functions();
            fn->set_name("set_mode");
            fn->set_function_id(3);
            fn->mutable_policy()->set_category(adpp::FunctionPolicy_Category_CATEGORY_CONFIG);
            auto *mode = fn->add_args();
            mode->set_name("mode");
            mode->set_type(adpp::VALUE_TYPE_STRING);
            mode->set_required(true);
            mode->add_allowed_values("open_loop");
            mode->add_allowed_values("closed_speed");
        } else if (!stop_chain_dir.empty() && request.has_describe_device() &&
                   (request.describe_device().device_id() == "gpio0" ||
                    request.describe_device().device_id() == "motor0")) {
            const std::string id = request.describe_device().device_id();
            auto *describe = response.mutable_describe_device();
            describe->mutable_device()->set_device_id(id);
            auto *caps = describe->mutable_capabilities();
            if (id == "gpio0") {
                auto *signal = caps->add_signals();
                signal->set_signal_id("chain_open");
                signal->set_value_type(adpp::VALUE_TYPE_BOOL);
                if (!stop_chain_no_hint) {
                    signal->set_poll_hint_hz(10.0);  // only hinted signals are polled
                }
            }
            if (id == "motor0" && stop_chain_estop) {
                auto *signal = caps->add_signals();
                signal->set_signal_id("estop");
                signal->set_value_type(adpp::VALUE_TYPE_BOOL);
                signal->set_poll_hint_hz(10.0);
            }
            if (id == "motor0" || stop_chain_actuating) {
                auto *fn = caps->add_functions();
                fn->set_name("set_pwm");
                fn->set_function_id(1);
                fn->mutable_policy()->set_category(adpp::FunctionPolicy_Category_CATEGORY_ACTUATE);
                auto *pwm = fn->add_args();  // numeric, so the zero rung can drive it
                pwm->set_name("pwm");
                pwm->set_type(adpp::VALUE_TYPE_INT64);
                pwm->set_required(true);
            }
        } else if (!stop_chain_dir.empty() && request.has_read_signals()) {
            auto *read = response.mutable_read_signals();
            read->set_device_id(request.read_signals().device_id());
            const std::filesystem::path dir(stop_chain_dir);
            if (request.read_signals().device_id() == "gpio0") {
                set_bool_reading(read->add_values(), "chain_open", read_word(dir / "chain"));
            } else if (request.read_signals().device_id() == "motor0" && stop_chain_estop) {
                if (std::filesystem::exists(dir / "motor0_off")) {
                    response.mutable_status()->set_code(adpp::Status_Code_CODE_UNAVAILABLE);
                    response.mutable_status()->set_message("motor0 did not answer");
                    response.clear_read_signals();
                } else {
                    set_bool_reading(read->add_values(), "estop", read_word(dir / "estop"));
                }
            }
        } else if (!stop_chain_dir.empty() && request.has_call()) {
            std::ofstream(std::filesystem::path(stop_chain_dir) / "calls", std::ios::app)
                << request.call().device_id() << "/" << request.call().function_id() << "\n";
            response.mutable_call();
        } else if (allowed_values && request.has_call()) {
            response.mutable_call();
        } else if (request.has_get_health()) {
            auto *health = response.mutable_get_health();
            health->mutable_provider()->set_state(adpp::ProviderHealth::STATE_OK);
            for (const char *id : kMissing) {
                auto *device = health->add_devices();
                device->set_device_id(id);
                device->set_state(adpp::DeviceHealth::STATE_UNREACHABLE);
                device->set_message(std::string("probe failed (version_read_failed): ") + id + " did not answer");
            }
            if (mixed) {
                auto *listed = health->add_devices();
                listed->set_device_id("rlht1");
                listed->set_state(adpp::DeviceHealth::STATE_UNREACHABLE);
                auto *ok = health->add_devices();
                ok->set_device_id("dcmt2");
                ok->set_state(adpp::DeviceHealth::STATE_OK);
            }
            if (!stop_chain_dir.empty()) {
                for (const char *id : {"gpio0", "motor0"}) {
                    auto *device = health->add_devices();
                    device->set_device_id(id);
                    device->set_state(adpp::DeviceHealth::STATE_OK);
                }
            }
            if (allowed_values) {
                auto *enum_device = health->add_devices();
                enum_device->set_device_id("dcmt3");
                enum_device->set_state(adpp::DeviceHealth::STATE_OK);
            }
        } else {
            response.mutable_status()->set_code(adpp::Status_Code_CODE_NOT_FOUND);
            response.mutable_status()->set_message("no such device");
        }
        write_response(response);
    }
    return 0;
}
