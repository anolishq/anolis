// Answers Hello and GetHealth, and never answers ReadSignals: a provider whose
// read is stuck, so a test can hold the session the way a hung bus call does
// (#411).

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "protocol.pb.h"

namespace {

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

bool read_request(anolis::deviceprovider::v1::Request &request) {
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

void write_response(const anolis::deviceprovider::v1::Response &response) {
    std::string bytes;
    response.SerializeToString(&bytes);
    const auto len = encode_le32(static_cast<uint32_t>(bytes.size()));
    std::cout.write(reinterpret_cast<const char *>(len.data()), static_cast<std::streamsize>(len.size()));
    std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    std::cout.flush();
}

}  // namespace

int main() {
#ifdef _WIN32
    // Protobuf framing is binary; text mode can corrupt bytes (e.g., 0x1A EOF).
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    anolis::deviceprovider::v1::Request request;
    while (read_request(request)) {
        if (request.has_read_signals()) {
            continue;  // stuck: never answers
        }
        anolis::deviceprovider::v1::Response response;
        response.set_request_id(request.request_id());
        response.mutable_status()->set_code(anolis::deviceprovider::v1::Status_Code_CODE_OK);
        if (request.has_hello()) {
            auto *hello = response.mutable_hello();
            hello->set_protocol_version("v1");
            hello->set_provider_name("stalled-read-helper");
            hello->set_provider_version("0.0.1");
        } else if (request.has_get_health()) {
            response.mutable_get_health();
        } else {
            response.mutable_status()->set_code(anolis::deviceprovider::v1::Status_Code_CODE_INVALID_ARGUMENT);
        }
        write_response(response);
    }
    return 0;
}
