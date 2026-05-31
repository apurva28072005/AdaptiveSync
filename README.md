# AdaptiveSync File Server

**High-performance, cross-platform file server with zero-copy I/O and delta-sync block hashing.**

Designed to showcase bare-metal systems engineering, memory management, and cross-language protocol design.

---

## Architecture

```
┌──────────────────┐     Protobuf/TCP      ┌──────────────────────┐
│   Tauri + React  │◄─────────────────────►│    C++17 Server      │
│   Desktop UI     │                        │                      │
│                  │   Wire Protocol        │  ┌────────────────┐  │
│  ┌────────────┐  │                        │  │   Negotiator   │  │
│  │ Rust Core  │  │   ┌──────────────┐     │  │ (Adaptive I/O) │  │
│  │            │  │   │  Protobuf    │     │  └───────┬────────┘  │
│  │ • Adler-32 │  │   │  Messages    │     │          │           │
│  │ • SHA-256  │  │   │  + Framing   │     │  ┌───────▼────────┐  │
│  │ • TCP Socket│ │   └──────────────┘     │  │ Zero-Copy      │  │
│  │ • File I/O │  │                        │  │ Sender         │  │
│  └────────────┘  │                        │  │ (sendfile/     │  │
│                  │                        │  │  TransmitFile) │  │
└──────────────────┘                        │  └────────────────┘  │
                                            │                      │
                                            │  ┌────────────────┐  │
                                            │  │ Delta-Sync     │  │
                                            │  │ Handler        │  │
                                            │  │ (Block compare │  │
                                            │  │  + merge)      │  │
                                            │  └────────────────┘  │
                                            │                      │
                                            │  ┌────────────────┐  │
                                            │  │ File Mutex     │  │
                                            │  │ (Thread-safe   │  │
                                            │  │  read/write)   │  │
                                            │  └────────────────┘  │
                                            └──────────────────────┘
```

## Core Engineering Features

### 1. Context-Aware Adaptive I/O (The Negotiator)

Before any bytes are transferred, the Rust client and C++ server perform a handshake to determine the optimal I/O path:

- **Fresh Transfer** → Zero-Copy (file doesn't exist on destination)
- **Modified File** → Delta-Sync (only changed blocks are transferred)
- **Unchanged** → No transfer (SHA-256 hashes match)

### 2. Zero-Copy File Streaming

For full file transfers, data moves directly from disk to NIC in kernel space:

- **Linux**: `sendfile()` syscall — DMA from storage to network
- **Windows**: `TransmitFile()` — kernel-space zero-copy
- **Fallback**: `read()/write()` loop for unsupported platforms
- **Result**: Server CPU under 5% even on multi-gigabyte transfers

### 3. Delta-Sync Updates

For modifying large files where only a small portion has changed:

- Client computes **Adler-32** rolling hash + **SHA-256** strong hash per block
- Server compares block signatures and requests only modified blocks
- **Result**: 10MB change to a 1GB file = ~10MB network traffic, not 1GB

### 4. Thread-Safe File Mutexing

- Per-file `std::shared_mutex` with reader/writer locking
- Concurrent readers allowed; exclusive access for writes
- Writes go to `.tmp` file first, then atomic `rename()` on completion

---

## Project Structure

```
AdaptiveSync/
├── proto/
│   └── adaptivesync.proto          # Wire protocol definition
├── server/                         # C++17 server engine
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── wire_protocol.h         # Frame encoding/decoding
│   │   ├── file_manager.h          # OS filesystem operations
│   │   ├── file_mutex.h            # Thread-safe file locking
│   │   ├── negotiator.h            # Adaptive I/O strategy
│   │   ├── zero_copy_sender.h      # sendfile/TransmitFile
│   │   ├── delta_sync_handler.h    # Block comparison & merge
│   │   ├── session.h               # Per-client state machine
│   │   └── server.h                # epoll/IOCP TCP server
│   └── src/                        # Full implementations
├── client/                         # Rust client core
│   ├── Cargo.toml
│   ├── build.rs                    # Protobuf compilation
│   └── src/
│       ├── lib.rs                  # Public API + error types
│       ├── wire.rs                 # Wire frame codec
│       ├── tcp_client.rs           # Connection management
│       ├── hashing.rs              # Adler-32 + SHA-256
│       ├── file_ops.rs             # Local disk I/O
│       └── delta_sync.rs           # Block hashing engine
├── ui/                             # Tauri + React desktop app
│   ├── package.json
│   ├── vite.config.ts
│   ├── src/
│   │   ├── App.tsx                 # Main app layout
│   │   ├── main.tsx
│   │   └── components/
│   │       ├── ConnectionStatus.tsx
│   │       ├── FileList.tsx
│   │       ├── FileUpload.tsx
│   │       └── TransferProgress.tsx
│   └── src-tauri/                  # Tauri Rust backend
│       ├── Cargo.toml
│       ├── tauri.conf.json
│       └── src/main.rs             # IPC commands
├── benchmarks/                     # Benchmarking suite
│   ├── benchmark_zero_copy.sh
│   ├── benchmark_delta_sync.sh
│   └── benchmark_concurrency.sh
├── Makefile                        # Top-level build system
└── README.md
```

---

## Build Instructions

### Prerequisites

| Tool | Version | Purpose |
|------|---------|---------|
| CMake | 3.16+ | C++ build system |
| GCC/Clang | C++17 support | Server compilation |
| Rust | 1.70+ | Client + Tauri backend |
| Node.js | 18+ | Tauri frontend |
| Protobuf | 3.0+ | Wire protocol compiler |

### Quick Build

```bash
# Build everything (server + client)
make all

# Run the server
make run-server

# Build the desktop UI (requires Tauri CLI)
make ui
```

### Individual Components

```bash
# C++ Server only
make server

# Rust Client only
make client

# Tauri UI in development mode
make ui-dev
```

### Running the Server

```bash
./server/build/adaptivesync_server \
    --port 9090 \
    --storage-root ./storage \
    --threads 8 \
    --block-size 65536
```

---

## Wire Protocol

All communication uses a simple binary framing over TCP:

```
Standard Frame:
┌──────────────────────┬───────────────┬──────────────────────┐
│ 4 bytes: length (BE) │ 1 byte: type  │ N bytes: protobuf    │
└──────────────────────┴───────────────┴──────────────────────┘

Raw Data Frame (zero-copy path):
┌──────────────┬─────────┬──────────────────┬──────────────────┐
│ 0xFFFFFFFF   │ 0xFE    │ 8 bytes: len (BE)│ raw file bytes   │
└──────────────┴─────────┴──────────────────┴──────────────────┘
```

Message types are defined in `proto/adaptivesync.proto` and cover:
- **Handshake**: ClientHello → ServerHello
- **Negotiation**: NegotiationRequest → NegotiationResponse
- **Zero-Copy**: DownloadRequest → DownloadHeader → Raw Data
- **Delta-Sync**: SignatureRequest → BlockRequest → BlockData → Complete
- **File Listing**: ListFilesRequest → ListFilesResponse

---

## Benchmarks

### Zero-Copy Proof
Demonstrates that a 5GB file transfer saturates network while server CPU stays under 5%.
```bash
bash benchmarks/benchmark_zero_copy.sh
```

### Delta-Sync Proof
Demonstrates that appending 10MB to a 1GB file syncs in < 2 seconds.
```bash
bash benchmarks/benchmark_delta_sync.sh
```

### Concurrency Proof
10 simultaneous 1GB file transfers without server crash.
```bash
bash benchmarks/benchmark_concurrency.sh
```

---

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| Protobuf over raw structs | Cross-language type safety between C++ and Rust |
| Custom TCP framing | Avoids HTTP overhead; ~40 bytes vs ~200+ bytes per message |
| sendfile() for zero-copy | Kernel-space DMA: disk → NIC without user-space copy |
| Adler-32 + SHA-256 | Two-tier hashing: fast rolling check + cryptographic verification |
| .tmp + atomic rename | Crash-safe writes; no partial file corruption |
| epoll on Linux | O(1) I/O multiplexing for thousands of connections |
| Tauri over Electron | ~10MB binary vs ~200MB Electron; native TCP socket access |

---

## Author

**Prasad Hiwarkhede** — May 2026

Built as a systems engineering showcase for the Zoho engineering assessment.
