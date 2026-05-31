// ════════════════════════════════════════════════════════════
// AdaptiveSync Client Core — Public API
// ════════════════════════════════════════════════════════════

pub mod proto {
    include!(concat!(env!("OUT_DIR"), "/adaptivesync.rs"));
}

pub mod wire;
pub mod tcp_client;
pub mod hashing;
pub mod file_ops;
pub mod delta_sync;

pub use tcp_client::TcpClient;
pub use delta_sync::DeltaSyncEngine;
pub use hashing::{adler32, sha256_hex};
pub use file_ops::FileOps;

use thiserror::Error;

#[derive(Error, Debug)]
pub enum ClientError {
    #[error("Connection error: {0}")]
    Connection(String),

    #[error("Protocol error: {0}")]
    Protocol(String),

    #[error("File I/O error: {0}")]
    FileIo(String),

    #[error("Hash mismatch: expected {expected}, got {actual}")]
    HashMismatch { expected: String, actual: String },

    #[error("Transfer failed: {0}")]
    TransferFailed(String),

    #[error("Server error: code={code}, message={message}")]
    ServerError { code: i32, message: String },

    #[error("Timeout")]
    Timeout,
}

pub type Result<T> = std::result::Result<T, ClientError>;
