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

}  // namespace

int main() {
#ifdef _WIN32
    // Protobuf framing is binary; text mode can corrupt bytes (e.g., 0x1A EOF).
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::array<uint8_t, 4> len_bytes{};
    if (!std::cin.read(reinterpret_cast<char *>(len_bytes.data()), static_cast<std::streamsize>(len_bytes.size()))) {
        return 1;
    }

    const uint32_t payload_len = decode_le32(len_bytes);
    if (payload_len == 0 || payload_len > (1024u * 1024u)) {
        return 1;
    }

    std::vector<uint8_t> payload(payload_len);
    if (!std::cin.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(payload.size()))) {
        return 1;
    }

    anolis::deviceprovider::v1::Request request;
    if (!request.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        return 1;
    }

    anolis::deviceprovider::v1::Response response;
    response.set_request_id(request.request_id());
    auto *status = response.mutable_status();
    status->set_code(anolis::deviceprovider::v1::Status_Code_CODE_OK);

    if (request.has_hello()) {
        auto *hello = response.mutable_hello();
        hello->set_protocol_version("v999");
        hello->set_provider_name("protocol-mismatch-helper");
        hello->set_provider_version("0.0.1");
    } else {
        status->set_code(anolis::deviceprovider::v1::Status_Code_CODE_INVALID_ARGUMENT);
        status->set_message("expected hello request");
    }

    std::string response_bytes;
    if (!response.SerializeToString(&response_bytes)) {
        return 1;
    }

    const auto response_len = encode_le32(static_cast<uint32_t>(response_bytes.size()));
    std::cout.write(reinterpret_cast<const char *>(response_len.data()),
                    static_cast<std::streamsize>(response_len.size()));
    std::cout.write(response_bytes.data(), static_cast<std::streamsize>(response_bytes.size()));
    std::cout.flush();

    char drain = 0;
    while (std::cin.get(drain)) {
    }

    return 0;
}
