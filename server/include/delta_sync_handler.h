#pragma once

// ════════════════════════════════════════════════════════════
// Delta-Sync Handler — Server-side block comparison & merge
// ════════════════════════════════════════════════════════════

#include "file_manager.h"
#include "file_mutex.h"
#include <string>
#include <vector>
#include <cstdint>

namespace adaptivesync {

struct DeltaBlockSignature {
    uint32_t    block_index;
    uint32_t    rolling_hash;    // Adler-32
    std::string strong_hash;     // SHA-256 hex
    uint64_t    block_offset;
    uint32_t    block_size;
};

struct DeltaBlockRequest {
    std::string relative_path;
    std::vector<uint32_t> requested_indices;
};

class DeltaSyncHandler {
public:
    explicit DeltaSyncHandler(FileManager& file_manager,
                               FileMutex& file_mutex,
                               uint32_t block_size = 65536);

    // Compare client's block signatures against server's file blocks.
    // Returns indices of blocks that differ (need to be sent by client).
    DeltaBlockRequest handle_signature_request(
        const std::string& relative_path,
        uint64_t client_file_size,
        uint32_t block_size,
        const std::vector<DeltaBlockSignature>& client_signatures);

    // Receive and write a single block of data.
    // Writes to .tmp file, verifies on completion.
    // Returns OK on success, error code on failure.
    int handle_block_data(const std::string& relative_path,
                           uint32_t block_index,
                           uint64_t block_offset,
                           const std::vector<uint8_t>& data,
                           const std::string& strong_hash,
                           bool is_final_block);

private:
    // Compute Adler-32 rolling hash
    static uint32_t adler32(const uint8_t* data, size_t len);

    // Compute SHA-256 of a block
    static std::string sha256_hex(const uint8_t* data, size_t len);

    FileManager& file_manager_;
    FileMutex&   file_mutex_;
    uint32_t     block_size_;

    // Track in-progress delta-sync operations: path -> set of received block indices
    std::unordered_map<std::string, std::unordered_map<uint32_t, bool>> in_progress_;
    std::mutex progress_mutex_;
};

}  // namespace adaptivesync
