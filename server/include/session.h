#pragma once

// ════════════════════════════════════════════════════════════
// Session — Per-client connection state machine
// ════════════════════════════════════════════════════════════

#include "wire_protocol.h"
#include "file_manager.h"
#include "file_mutex.h"
#include "negotiator.h"
#include "zero_copy_sender.h"
#include "delta_sync_handler.h"

#include <memory>
#include <atomic>
#include <string>

namespace adaptivesync {

enum class SessionState {
    AWAITING_HELLO,
    READY,
    NEGOTIATING,
    TRANSFERRING,
    ERROR_STATE,
    DISCONNECTED
};

class Session {
public:
    Session(socket_t client_fd,
            const std::string& client_addr,
            FileManager& file_manager,
            FileMutex& file_mutex,
            uint32_t block_size);

    ~Session();

    // Main event loop — reads frames and dispatches
    void run();

    // Request graceful shutdown
    void shutdown();

    SessionState state() const { return state_; }
    const std::string& client_id() const { return client_id_; }

private:
    // ─── Message handlers ──────────────────────────────────
    void handle_client_hello(const std::vector<uint8_t>& payload);
    void handle_negotiation_request(const std::vector<uint8_t>& payload);
    void handle_zero_copy_download_request(const std::vector<uint8_t>& payload);
    void handle_zero_copy_upload_request(const std::vector<uint8_t>& payload);
    void handle_delta_sync_signature_request(const std::vector<uint8_t>& payload);
    void handle_delta_sync_block_data(const std::vector<uint8_t>& payload);
    void handle_list_files_request(const std::vector<uint8_t>& payload);

    // ─── Upload state machine ──────────────────────────────
    void handle_upload_data(const std::vector<uint8_t>& raw_data);

    // ─── Helpers ───────────────────────────────────────────
    void send_error(int status_code, const std::string& message);
    void send_progress(const std::string& path,
                       uint64_t bytes_transferred,
                       uint64_t bytes_total,
                       float rate_mbps,
                       uint32_t elapsed_ms);

    socket_t                      client_fd_;
    std::string                   client_addr_;
    SessionState                  state_;
    std::string                   client_id_;
    std::atomic<bool>             shutdown_requested_;

    // Component references (owned by Server)
    FileManager&                  file_manager_;
    FileMutex&                    file_mutex_;

    // Per-session owned components
    std::unique_ptr<Negotiator>         negotiator_;
    std::unique_ptr<ZeroCopySender>     zero_copy_sender_;
    std::unique_ptr<DeltaSyncHandler>   delta_sync_handler_;

    // Negotiated parameters
    uint32_t                      negotiated_block_size_;
    uint32_t                      protocol_version_;

    // Upload state
    struct UploadState {
        std::string relative_path;
        uint64_t    total_size = 0;
        uint64_t    received   = 0;
        std::string expected_sha256;
        std::vector<uint8_t> buffer;  // For non-zero-copy uploads
    };
    UploadState                   upload_state_;
};

}  // namespace adaptivesync
