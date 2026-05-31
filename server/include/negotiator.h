#pragma once

// ════════════════════════════════════════════════════════════
// Negotiator — Context-aware adaptive I/O strategy selection
// ════════════════════════════════════════════════════════════

#include "file_manager.h"

// Forward-declare protobuf types to avoid pulling generated headers here
// The implementation file includes the generated protobuf headers.

namespace adaptivesync {

class Negotiator {
public:
    explicit Negotiator(FileManager& file_manager, uint32_t block_size = 65536);

    // Determine the optimal transfer mode for a file.
    // Uses protobuf NegotiationRequest / NegotiationResponse types.
    // Defined in the .cpp after including protobuf headers.

    struct NegotiationResult {
        int      file_mode;        // FileMode enum value
        int      transfer_mode;    // TransferMode enum value
        uint64_t server_file_size;
        std::string server_sha256;
        uint32_t block_size;
    };

    NegotiationResult negotiate(const std::string& relative_path,
                                 uint64_t client_file_size,
                                 uint64_t client_last_modified_ns,
                                 const std::string& client_sha256);

private:
    FileManager& file_manager_;
    uint32_t     block_size_;
};

}  // namespace adaptivesync
