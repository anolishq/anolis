// libFuzzer harness for the ADPP framed-stdio decoder
// (anolis::provider::FramedStdioClient::read_frame).
//
// read_frame() reads a uint32_le length prefix + payload from a file
// descriptor, so the fuzzer bytes are pushed through an in-memory pipe: write
// the input to the write end (non-blocking — truncated at pipe capacity, which
// also exercises partial/short-frame paths), close it for EOF, then decode
// frames from the read end until the decoder reports failure. ASan watches the
// length-driven resize + payload read for over-reads / over-allocations.

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#include "provider/framed_stdio_client.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  int fds[2];
  if (pipe(fds) != 0) {
    return 0;
  }
  // Non-blocking write end: never deadlock when the input exceeds the pipe
  // buffer and nothing has drained it yet.
  fcntl(fds[1], F_SETFL, O_NONBLOCK);
  size_t off = 0;
  while (off < size) {
    ssize_t n = write(fds[1], data + off, size - off);
    if (n <= 0) {
      break; // EAGAIN (pipe full) or error -> truncated frame
    }
    off += static_cast<size_t>(n);
  }
  close(fds[1]); // EOF for the reader

  anolis::provider::FramedStdioClient client;
  // We only read; the stdin-write handle is unused by read_frame().
  // PipeHandle is an int fd on Linux (the only platform the fuzzers build on).
  client.set_handles(-1, fds[0]);

  std::vector<uint8_t> out;
  while (client.read_frame(out, 100)) {
    out.clear();
  }

  close(fds[0]);
  return 0;
}
