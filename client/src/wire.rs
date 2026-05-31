// ════════════════════════════════════════════════════════════
// Wire Protocol — Frame encoding/decoding for Rust client
// ════════════════════════════════════════════════════════════

use crate::proto::MessageType;
use crate::ClientError;
use prost::bytes::BufMut;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

// Wire frame constants (must match C++ server)
pub const RAW_DATA_SENTINEL: u32 = 0xFFFFFFFF;
pub const MSG_RAW_DATA: u8 = 0xFE;
pub const FRAME_HEADER_SIZE: usize = 5;   // 4 (length) + 1 (type)
pub const RAW_HEADER_SIZE: usize = 13;    // 4 (magic) + 1 (type) + 8 (raw length)

/// A decoded wire frame
#[derive(Debug)]
pub struct WireFrame {
    pub message_type: u8,
    pub payload: Vec<u8>,
}

/// Encode a protobuf message into a wire frame
pub fn encode_frame(message_type: MessageType, payload: &[u8]) -> Vec<u8> {
    let type_byte = message_type as u8;
    let payload_len = payload.len() as u32;
    let mut buffer = Vec::with_capacity(FRAME_HEADER_SIZE + payload.len());
    buffer.put_u32(payload_len);  // put_u32 writes big-endian
    buffer.put_u8(type_byte);
    buffer.put_slice(payload);
    buffer
}

/// Encode a raw-data frame header (for zero-copy upload path)
pub fn encode_raw_data_header(raw_length: u64) -> Vec<u8> {
    let mut buffer = Vec::with_capacity(RAW_HEADER_SIZE);
    buffer.put_u32(RAW_DATA_SENTINEL);  // put_u32 writes big-endian
    buffer.put_u8(MSG_RAW_DATA);
    buffer.put_u64(raw_length);  // put_u64 writes big-endian
    buffer
}

/// Read one complete wire frame from a TCP stream
pub async fn read_frame(stream: &mut TcpStream) -> crate::Result<WireFrame> {
    // Read 4-byte length prefix
    let mut len_buf = [0u8; 4];
    stream.read_exact(&mut len_buf).await
        .map_err(|e| ClientError::Connection(format!("Failed to read frame length: {}", e)))?;

    let payload_len = u32::from_be_bytes(len_buf);

    // Check for raw-data sentinel
    if payload_len == RAW_DATA_SENTINEL {
        // Read remaining: 1 byte type + 8 bytes raw length
        let mut rest = [0u8; 9];
        stream.read_exact(&mut rest).await
            .map_err(|e| ClientError::Connection(format!("Failed to read raw header: {}", e)))?;

        let msg_type = rest[0];
        let raw_len = u64::from_be_bytes(rest[1..9].try_into().unwrap());

        // Read raw data
        let mut raw_data = vec![0u8; raw_len as usize];
        stream.read_exact(&mut raw_data).await
            .map_err(|e| ClientError::Connection(format!("Failed to read raw data: {}", e)))?;

        return Ok(WireFrame {
            message_type: msg_type,
            payload: raw_data,
        });
    }

    // Standard frame: read 1 byte type + payload
    let mut type_buf = [0u8; 1];
    stream.read_exact(&mut type_buf).await
        .map_err(|e| ClientError::Connection(format!("Failed to read frame type: {}", e)))?;

    let message_type = type_buf[0];
    let mut payload = vec![0u8; payload_len as usize];
    if payload_len > 0 {
        stream.read_exact(&mut payload).await
            .map_err(|e| ClientError::Connection(format!("Failed to read frame payload: {}", e)))?;
    }

    Ok(WireFrame {
        message_type,
        payload,
    })
}

/// Write a wire frame to a TCP stream
pub async fn write_frame(stream: &mut TcpStream, message_type: MessageType, payload: &[u8]) -> crate::Result<()> {
    let encoded = encode_frame(message_type, payload);
    stream.write_all(&encoded).await
        .map_err(|e| ClientError::Connection(format!("Failed to write frame: {}", e)))?;
    stream.flush().await
        .map_err(|e| ClientError::Connection(format!("Failed to flush: {}", e)))?;
    Ok(())
}

/// Write raw data with the special header
pub async fn write_raw_data(stream: &mut TcpStream, data: &[u8]) -> crate::Result<()> {
    let header = encode_raw_data_header(data.len() as u64);
    stream.write_all(&header).await
        .map_err(|e| ClientError::Connection(format!("Failed to write raw header: {}", e)))?;
    stream.write_all(data).await
        .map_err(|e| ClientError::Connection(format!("Failed to write raw data: {}", e)))?;
    stream.flush().await
        .map_err(|e| ClientError::Connection(format!("Failed to flush raw data: {}", e)))?;
    Ok(())
}
