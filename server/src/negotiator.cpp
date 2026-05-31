// ════════════════════════════════════════════════════════════
// Negotiator — Implementation
// ════════════════════════════════════════════════════════════

#include "negotiator.h"

#include <algorithm>

namespace adaptivesync {

Negotiator::Negotiator(FileManager& file_manager, uint32_t block_size)
    : file_manager_(file_manager), block_size_(block_size)
{}

Negotiator::NegotiationResult Negotiator::negotiate(
    const std::string& relative_path,
    uint64_t client_file_size,
    uint64_t client_last_modified_ns,
    const std::string& client_sha256)
{
    NegotiationResult result;
    result.server_file_size = 0;
    result.server_sha256 = "";
    result.block_size = block_size_;

    // Case 1: File does not exist on the server → FRESH transfer
    if (!file_manager_.file_exists(relative_path)) {
        result.file_mode      = 1;  // FRESH
        result.transfer_mode  = 1;  // ZERO_COPY
        return result;
    }

    // File exists on server — compute its SHA-256
    auto server_info = file_manager_.get_file_info(relative_path);
    if (!server_info) {
        // Shouldn't happen since file_exists returned true
        result.file_mode     = 1;  // FRESH
        result.transfer_mode = 1;  // ZERO_COPY
        return result;
    }

    result.server_file_size = server_info->file_size;
    result.server_sha256    = server_info->sha256_hex;

    // Case 2: File exists and hashes match → UNCHANGED
    if (!client_sha256.empty() && client_sha256 == server_info->sha256_hex) {
        result.file_mode     = 3;  // UNCHANGED
        result.transfer_mode = 0;  // UNSPECIFIED (no transfer needed)
        return result;
    }

    // Case 3: File exists but content differs → MODIFIED, use DELTA_SYNC
    // Delta-sync is preferred when the file already exists server-side,
    // because only changed blocks need to be transferred.
    result.file_mode     = 2;  // MODIFIED
    result.transfer_mode = 2;  // DELTA_SYNC
    return result;
}

}  // namespace adaptivesync
