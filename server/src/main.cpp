// ════════════════════════════════════════════════════════════
// AdaptiveSync Server — Entry Point
// ════════════════════════════════════════════════════════════

#include "server.h"

#include <iostream>
#include <string>
#include <cstdlib>

static void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  --port <port>          TCP port to listen on (default: 9090)\n"
              << "  --storage-root <path>  Root directory for file storage (default: ./storage)\n"
              << "  --threads <count>      Number of worker threads (default: hardware_concurrency)\n"
              << "  --block-size <bytes>   Block size for delta-sync (default: 65536)\n"
              << "  --max-connections <n>  Maximum concurrent connections (default: 100)\n"
              << "  --help                 Show this help message\n\n"
              << "Examples:\n"
              << "  " << program << " --port 8080 --storage-root /data/sync\n"
              << "  " << program << " --threads 8 --block-size 131072\n";
}

int main(int argc, char* argv[]) {
    adaptivesync::ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--port" && i + 1 < argc) {
            config.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--storage-root" && i + 1 < argc) {
            config.storage_root = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            config.thread_count = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--block-size" && i + 1 < argc) {
            config.block_size = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--max-connections" && i + 1 < argc) {
            config.max_connections = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    try {
        adaptivesync::AdaptiveSyncServer server(config);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
