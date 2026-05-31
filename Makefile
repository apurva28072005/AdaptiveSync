# ════════════════════════════════════════════════════════════
# AdaptiveSync File Server — Top-Level Build System
# ════════════════════════════════════════════════════════════

.PHONY: all server client ui proto clean benchmark bench-zero bench-delta bench-concurrent help

# ─── Default ────────────────────────────────────────────────
all: proto server client
	@echo ""
	@echo "═════════════════════════════════════════════════════════"
	@echo "  AdaptiveSync build complete!"
	@echo "═════════════════════════════════════════════════════════"
	@echo ""
	@echo "  Server:  ./server/build/adaptivesync_server"
	@echo "  Client:  ./client/target/release/libadaptivesync_client.so"
	@echo "  UI:      cd ui && npm run tauri dev"
	@echo ""

# ─── Protobuf Code Generation ──────────────────────────────
proto:
	@echo "[Proto] Generating C++ and Rust code from protobuf..."
	@mkdir -p proto/generated
	@protoc --cpp_out=proto/generated --proto_path=proto proto/adaptivesync.proto 2>/dev/null || \
		echo "[Proto] Note: C++ protobuf generation will be handled by CMake"
	@echo "[Proto] Rust protobuf generation will be handled by prost-build at compile time"
	@echo "[Proto] Done."

# ─── C++ Server ────────────────────────────────────────────
server:
	@echo "[Server] Building C++17 server..."
	@mkdir -p server/build
	@cd server/build && cmake .. -DCMAKE_BUILD_TYPE=Release
	@cd server/build && $(MAKE) -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
	@echo "[Server] Build complete: server/build/adaptivesync_server"

server-debug:
	@mkdir -p server/build
	@cd server/build && cmake .. -DCMAKE_BUILD_TYPE=Debug
	@cd server/build && $(MAKE) -j$$(nproc 2>/dev/null || echo 4)

# ─── Rust Client ───────────────────────────────────────────
client:
	@echo "[Client] Building Rust client library..."
	@cd client && cargo build --release
	@echo "[Client] Build complete."

client-debug:
	@cd client && cargo build

# ─── Tauri UI ──────────────────────────────────────────────
ui: client
	@echo "[UI] Installing npm dependencies..."
	@cd ui && npm install
	@echo "[UI] Building Tauri desktop app..."
	@cd ui && npm run tauri build
	@echo "[UI] Build complete."

ui-dev: client-debug
	@cd ui && npm install
	@cd ui && npm run tauri dev

# ─── Run Server ────────────────────────────────────────────
run-server: server
	@echo "[Run] Starting AdaptiveSync server on port 9090..."
	@./server/build/adaptivesync_server --port 9090 --storage-root ./storage --block-size 65536

# ─── Benchmarks ────────────────────────────────────────────
benchmark: bench-zero bench-delta bench-concurrent

bench-zero:
	@echo "[Bench] Running zero-copy benchmark..."
	@bash benchmarks/benchmark_zero_copy.sh

bench-delta:
	@echo "[Bench] Running delta-sync benchmark..."
	@bash benchmarks/benchmark_delta_sync.sh

bench-concurrent:
	@echo "[Bench] Running concurrency benchmark..."
	@bash benchmarks/benchmark_concurrency.sh

# ─── Clean ─────────────────────────────────────────────────
clean:
	@echo "[Clean] Removing build artifacts..."
	rm -rf server/build
	rm -rf client/target
	rm -rf ui/node_modules ui/dist ui/src-tauri/target
	rm -rf proto/generated
	rm -rf storage
	@echo "[Clean] Done."

# ─── Help ──────────────────────────────────────────────────
help:
	@echo ""
	@echo "AdaptiveSync File Server — Build Targets"
	@echo "═══════════════════════════════════════════"
	@echo ""
	@echo "  all               Build server + client (default)"
	@echo "  proto             Generate protobuf code"
	@echo "  server            Build C++17 server (Release)"
	@echo "  server-debug      Build C++17 server (Debug)"
	@echo "  client            Build Rust client library (Release)"
	@echo "  client-debug      Build Rust client library (Debug)"
	@echo "  ui                Build Tauri desktop UI (Release)"
	@echo "  ui-dev            Start Tauri dev mode"
	@echo "  run-server        Build and run the server"
	@echo "  benchmark         Run all benchmarks"
	@echo "  bench-zero        Run zero-copy benchmark"
	@echo "  bench-delta       Run delta-sync benchmark"
	@echo "  bench-concurrent  Run concurrency benchmark"
	@echo "  clean             Remove all build artifacts"
	@echo "  help              Show this message"
	@echo ""
	@echo "Quick Start:"
	@echo "  make all && make run-server"
	@echo ""
