#pragma once

// ════════════════════════════════════════════════════════════
// Server — Main TCP server with epoll/IOCP
// ════════════════════════════════════════════════════════════

#include "wire_protocol.h"
#include "file_manager.h"
#include "file_mutex.h"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>

namespace adaptivesync {

struct ServerConfig {
    uint16_t    port              = 9090;
    std::string storage_root      = "./storage";
    uint32_t    thread_count      = 0;  // 0 = hardware_concurrency
    uint32_t    block_size        = 65536;
    uint32_t    max_connections   = 100;
};

class AdaptiveSyncServer {
public:
    explicit AdaptiveSyncServer(const ServerConfig& config);
    ~AdaptiveSyncServer();

    // Start the server — blocks until shutdown
    void run();

    // Signal shutdown (thread-safe)
    void shutdown();

private:
    // ─── Platform-specific accept loops ────────────────────
    void run_epoll();       // Linux
    void run_iocp();        // Windows
    void run_select();      // Fallback

    // ─── Client handling ──────────────────────────────────
    void handle_client(socket_t client_fd, const std::string& client_addr);

    // ─── Initialization ───────────────────────────────────
    void init_storage();
    void print_banner() const;

    ServerConfig               config_;
    FileManager                file_manager_;
    FileMutex                  file_mutex_;
    std::atomic<bool>          running_;

    // Thread pool
    std::vector<std::thread>   worker_threads_;

    // Server socket
    socket_t                   server_fd_;

    // Statistics
    std::atomic<uint64_t>      total_connections_;
    std::atomic<uint64_t>      active_connections_;
};

}  // namespace adaptivesync
