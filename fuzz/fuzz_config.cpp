// libFuzzer harness for the runtime YAML config loader.
//
// load_config() takes a file path, so each input is written to a temp file and
// fed through the full parse + validate path. Errors are returned via bool/string
// (no exceptions escape), so we ignore the result and let ASan catch memory bugs.

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "runtime/config.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char path[] = "/tmp/anolis_fuzz_cfg_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return 0;
    }
    if (size > 0) {
        ssize_t written = write(fd, data, size);
        (void)written;
    }
    close(fd);

    anolis::runtime::RuntimeConfig config;
    std::string error;
    (void)anolis::runtime::load_config(path, config, error);

    unlink(path);
    return 0;
}
