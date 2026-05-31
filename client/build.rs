// ════════════════════════════════════════════════════════════
// Build script — Compile protobuf definitions for Rust
// ════════════════════════════════════════════════════════════

fn main() {
    let proto_path = "../proto/adaptivesync.proto";

    prost_build::Config::new()
        .compile_protos(
            &[proto_path],
            &["../proto/"],  // Include path
        )
        .expect("Failed to compile protobuf definitions");

    // Rerun if proto changes
    println!("cargo:rerun-if-changed={}", proto_path);
}
