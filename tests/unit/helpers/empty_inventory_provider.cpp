// A provider in the shape of bread 0.8.1 on an unpowered bus (anolis#461): it
// answers Hello and WaitReady, lists no devices, and reports three expected
// devices UNREACHABLE in GetHealth. With `--exit-when <path>` it deletes the
// file and exits once the file appears, so a test can drive one supervised
// restart: the restarted helper finds no file and stays up. With `--mixed` it
// also lists rlht1, which describes but is reported UNREACHABLE, and reports
// dcmt2 OK without listing it: neither is a device its provider says is missing.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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

}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
    // Protobuf framing is binary; text mode can corrupt bytes (e.g., 0x1A EOF).
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::string exit_when;
    bool mixed = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--exit-when" && i + 1 < argc) {
            exit_when = argv[i + 1];
        } else if (std::string(argv[i]) == "--mixed") {
            mixed = true;
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
        } else if (mixed && request.has_describe_device() && request.describe_device().device_id() == "rlht1") {
            auto *describe = response.mutable_describe_device();
            describe->mutable_device()->set_device_id("rlht1");
            auto *fn = describe->mutable_capabilities()->add_functions();
            fn->set_name("set_power");
            fn->set_function_id(1);
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
        } else {
            response.mutable_status()->set_code(adpp::Status_Code_CODE_NOT_FOUND);
            response.mutable_status()->set_message("no such device");
        }
        write_response(response);
    }
    return 0;
}
