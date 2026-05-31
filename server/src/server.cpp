// ════════════════════════════════════════════════════════════
// Server — Main TCP server with epoll/IOCP/select
// ════════════════════════════════════════════════════════════

#include "server.h"
#include "session.h"

#include <iostream>
#include <sstream>
#include <thread>
#include <csignal>
#include <cstring>
#include <chrono>

#ifdef __linux__
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#elif _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace adaptivesync {

// Global server pointer for signal handling
static AdaptiveSyncServer* g_server = nullptr;

static void signal_handler(int signum) {
    std::cout << "\n[Server] Caught signal " << signum << ", shutting down..." << std::endl;
    if (g_server) {
        g_server->shutdown();
    }
}

// ─── Constructor / Destructor ──────────────────────────────

AdaptiveSyncServer::AdaptiveSyncServer(const ServerConfig& config)
    : config_(config)
    , file_manager_(config.storage_root)
    , running_(false)
    , server_fd_(static_cast<socket_t>(-1))
    , total_connections_(0)
    , active_connections_(0)
{
    if (config_.thread_count == 0) {
        config_.thread_count = std::thread::hardware_concurrency();
        if (config_.thread_count == 0) config_.thread_count = 4;
    }
}

AdaptiveSyncServer::~AdaptiveSyncServer() {
    shutdown();
    if (server_fd_ != static_cast<socket_t>(-1)) {
        close_socket(server_fd_);
    }
    for (auto& t : worker_threads_) {
        if (t.joinable()) t.join();
    }
}

// ─── Run ───────────────────────────────────────────────────

void AdaptiveSyncServer::run() {
    g_server = this;
    running_ = true;

    init_storage();
    print_banner();

    // Register signal handlers for graceful shutdown
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "[Server] WSAStartup failed" << std::endl;
        return;
    }
#endif

    // Create listening socket
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ == static_cast<socket_t>(-1)) {
        std::cerr << "[Server] Failed to create socket: " << strerror(errno) << std::endl;
        return;
    }

    // Set SO_REUSEADDR
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    // Bind
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.port);

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[Server] Bind failed on port " << config_.port
                  << ": " << strerror(errno) << std::endl;
        return;
    }

    // Listen
    if (::listen(server_fd_, config_.max_connections) < 0) {
        std::cerr << "[Server] Listen failed: " << strerror(errno) << std::endl;
        return;
    }

    std::cout << "[Server] Listening on port " << config_.port << std::endl;

    // Platform-specific accept loop
#ifdef __linux__
    run_epoll();
#elif _WIN32
    run_iocp();
#else
    run_select();
#endif

    std::cout << "[Server] Shutdown complete. Total connections served: "
              << total_connections_.load() << std::endl;
}

void AdaptiveSyncServer::shutdown() {
    running_ = false;
    if (server_fd_ != static_cast<socket_t>(-1)) {
        close_socket(server_fd_);
        server_fd_ = static_cast<socket_t>(-1);
    }
}

// ─── Linux epoll accept loop ──────────────────────────────

void AdaptiveSyncServer::run_epoll() {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        std::cerr << "[Server] epoll_create1 failed" << std::endl;
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd_;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd_, &ev);

    constexpr int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];

    while (running_) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);  // 1s timeout

        if (nfds < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Server] epoll_wait error: " << strerror(errno) << std::endl;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == server_fd_) {
                // New connection
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = ::accept(server_fd_,
                    reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);

                if (client_fd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        std::cerr << "[Server] Accept error: " << strerror(errno) << std::endl;
                    }
                    continue;
                }

                // Set TCP_NODELAY for low latency
                int flag = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                char addr_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, INET_ADDRSTRLEN);
                std::string client_addr_str = std::string(addr_str) + ":"
                    + std::to_string(ntohs(client_addr.sin_port));

                total_connections_++;
                active_connections_++;

                // Spawn a thread for this client
                worker_threads_.emplace_back(&AdaptiveSyncServer::handle_client,
                    this, client_fd, client_addr_str);
            }
        }
    }

    ::close(epoll_fd);
}

// ─── Windows IOCP accept loop ──────────────────────────────

void AdaptiveSyncServer::run_iocp() {
    // For now, fall back to a simple accept loop on Windows
    // A full IOCP implementation would use CreateIoCompletionPort,
    // PostQueuedCompletionStatus, and overlapped I/O.
    run_select();
}

// ─── Fallback select-based accept loop ─────────────────────

void AdaptiveSyncServer::run_select() {
    while (running_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd_, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int result = ::select(server_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Server] select() error: " << strerror(errno) << std::endl;
            break;
        }
        if (result == 0) continue;  // Timeout

        if (FD_ISSET(server_fd_, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            socket_t client_fd = ::accept(server_fd_,
                reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);

            if (client_fd == static_cast<socket_t>(-1)) {
                std::cerr << "[Server] Accept error" << std::endl;
                continue;
            }

            char addr_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, INET_ADDRSTRLEN);
            std::string client_addr_str = std::string(addr_str) + ":"
                + std::to_string(ntohs(client_addr.sin_port));

            total_connections_++;
            active_connections_++;

            worker_threads_.emplace_back(&AdaptiveSyncServer::handle_client,
                this, client_fd, client_addr_str);
        }
    }
}

// ─── Client handler (runs in worker thread) ────────────────

void AdaptiveSyncServer::handle_client(socket_t client_fd, const std::string& client_addr) {
    Session session(client_fd, client_addr, file_manager_, file_mutex_,
                    config_.block_size);

    session.run();

    active_connections_--;

    // Clean up finished threads periodically
    // (In production, use a proper thread pool)
}

// ─── Initialization ────────────────────────────────────────

void AdaptiveSyncServer::init_storage() {
    std::filesystem::create_directories(config_.storage_root);
    std::cout << "[Server] Storage root: "
              << std::filesystem::absolute(config_.storage_root) << std::endl;
}

void AdaptiveSyncServer::print_banner() const {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════╗\n";
    std::cout << "║          AdaptiveSync File Server v1.0                ║\n";
    std::cout << "║          C++17 Engine with Zero-Copy I/O              ║\n";
    std::cout << "╠═══════════════════════════════════════════════════════╣\n";
    std::cout << "║  Port:              " << config_.port << "\n";
    std::cout << "║  Storage Root:      " << config_.storage_root << "\n";
    std::cout << "║  Worker Threads:    " << config_.thread_count << "\n";
    std::cout << "║  Block Size:        " << config_.block_size << " bytes\n";
    std::cout << "║  Max Connections:   " << config_.max_connections << "\n";
    std::cout << "╚═══════════════════════════════════════════════════════╝\n";
    std::cout << std::endl;
}

}  // namespace adaptivesync
