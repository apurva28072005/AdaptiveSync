#pragma once

// ════════════════════════════════════════════════════════════
// Zero-Copy Sender — sendfile/TransmitFile file streaming
// ════════════════════════════════════════════════════════════

#include "wire_protocol.h"
#include "file_manager.h"
#include <string>
#include <cstdint>

namespace adaptivesync {

class ZeroCopySender {
public:
    explicit ZeroCopySender(FileManager& file_manager);

    // Send a file to the connected client using zero-copy where available.
    // Sends the ZeroCopyDownloadHeader protobuf first, then streams raw bytes.
    // On Linux: uses sendfile() for kernel-space DMA.
    // On Windows: uses TransmitFile().
    // Falls back to read()/send() loop if zero-copy unavailable.
    //
    // Returns true on success, false on error.
    bool send_file(socket_t client_fd,
                   const std::string& relative_path,
                   uint64_t offset = 0);

private:
    // Platform-specific zero-copy implementations
    bool send_file_linux(socket_t client_fd, int file_fd,
                         uint64_t offset, uint64_t total_size);
    bool send_file_windows(socket_t client_fd, void* file_handle,
                           uint64_t offset, uint64_t total_size);
    bool send_file_fallback(socket_t client_fd, int file_fd,
                            uint64_t offset, uint64_t total_size);

    FileManager& file_manager_;
};

}  // namespace adaptivesync
