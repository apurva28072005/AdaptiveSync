#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::Serialize;
use std::sync::Mutex;
use std::path::Path;
use std::fs;
use tauri::State;

use adaptivesync_client::TcpClient;

// ─── Shared State ──────────────────────────────────────────

struct AppState {
    client: Mutex<Option<TcpClient>>,
    sync_root: Mutex<String>,
}

// ─── RAII Guard: guarantees TcpClient is returned to state ─

struct ClientGuard<'a> {
    client: Option<TcpClient>,
    state: &'a AppState,
}

impl<'a> ClientGuard<'a> {
    fn take(state: &'a AppState, context: &str) -> Result<Self, String> {
        let mut guard = state.client.lock()
            .map_err(|e| format!("Mutex error ({}): {}", context, e))?;
        let client = guard.take()
            .ok_or_else(|| format!("Not connected (in {})", context))?;
        Ok(Self { client: Some(client), state })
    }

    fn get(&mut self) -> &mut TcpClient {
        self.client.as_mut().expect("ClientGuard invariant violated")
    }
}

impl Drop for ClientGuard<'_> {
    fn drop(&mut self) {
        if let Some(client) = self.client.take() {
            if let Ok(mut guard) = self.state.client.lock() {
                *guard = Some(client);
            } else {
                log::error!("[Guard] Failed to return TcpClient: mutex poisoned");
            }
        }
    }
}

// ─── IPC Response Types ────────────────────────────────────

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ConnectionResult {
    success: bool,
    message: String,
    server_id: Option<String>,
    block_size: Option<u32>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct NegotiationResult {
    file_mode: String,
    transfer_mode: String,
    server_file_size: u64,
    server_sha256: String,
    block_size: u32,
}

#[derive(Serialize, Clone)]
#[serde(rename_all = "camelCase")]
struct FileInfo {
    relative_path: String,
    file_size: u64,
    last_modified_ns: u64,
    sha256_hash: String,
    file_mode: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct SyncResult {
    success: bool,
    sha256: String,
    transfer_mode: String,
    error: Option<String>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct DownloadResult {
    success: bool,
    relative_path: String,
    sha256: String,
    error: Option<String>,
}

// ─── IPC Command: Connect ──────────────────────────────────

#[tauri::command]
async fn connect_to_server(
    state: State<'_, AppState>,
    server_addr: String,
    block_size: u32,
    sync_root: String,
) -> Result<ConnectionResult, String> {
    let mut client = TcpClient::new(&server_addr, sync_root.clone(), block_size);

    client.connect().await.map_err(|e| format!("Connection failed: {}", e))?;

    let bs = client.block_size();
    {
        let mut guard = state.client.lock().map_err(|e| e.to_string())?;
        *guard = Some(client);
    }
    {
        let mut sr = state.sync_root.lock().map_err(|e| e.to_string())?;
        *sr = sync_root.clone();
    }

    Ok(ConnectionResult {
        success: true,
        message: format!("Connected to {}", server_addr),
        server_id: Some("adaptivesync-cpp-1.0".to_string()),
        block_size: Some(bs),
    })
}

// ─── IPC Command: Disconnect ───────────────────────────────

#[tauri::command]
async fn disconnect_from_server(state: State<'_, AppState>) -> Result<bool, String> {
    let client = {
        let mut guard = state.client.lock().map_err(|e| e.to_string())?;
        guard.take()
    };

    if let Some(mut client) = client {
        client.disconnect().await;
        Ok(true)
    } else {
        Ok(false)
    }
}

// ─── IPC Command: Negotiate ────────────────────────────────

#[tauri::command]
async fn negotiate_file(
    state: State<'_, AppState>,
    relative_path: String,
) -> Result<NegotiationResult, String> {
    let mut handle = ClientGuard::take(&state, "negotiate_file")?;
    let result = handle.get().negotiate(&relative_path).await
        .map_err(|e| e.to_string())?;

    let file_mode_str = match result.file_mode {
        1 => "FRESH",
        2 => "MODIFIED",
        3 => "UNCHANGED",
        _ => "UNSPECIFIED",
    };
    let transfer_mode_str = match result.transfer_mode {
        1 => "ZERO_COPY",
        2 => "DELTA_SYNC",
        _ => "UNSPECIFIED",
    };

    Ok(NegotiationResult {
        file_mode: file_mode_str.to_string(),
        transfer_mode: transfer_mode_str.to_string(),
        server_file_size: result.server_file_size,
        server_sha256: result.server_sha256,
        block_size: result.block_size,
    })
}

// ─── IPC Command: Sync File ────────────────────────────────

#[tauri::command]
async fn sync_file(
    state: State<'_, AppState>,
    relative_path: String,
    file_mode: String,
    transfer_mode: String,
) -> Result<SyncResult, String> {
    let mut handle = ClientGuard::take(&state, "sync_file")?;

    let sync_result = match file_mode.as_str() {
        "FRESH" => {
            match handle.get().upload_file(&relative_path, None).await {
                Ok(hash) => Ok(SyncResult {
                    success: true,
                    sha256: hash,
                    transfer_mode,
                    error: None,
                }),
                Err(e) => Ok(SyncResult {
                    success: false,
                    sha256: String::new(),
                    transfer_mode,
                    error: Some(e.to_string()),
                }),
            }
        }
        "MODIFIED" => {
            let result = if transfer_mode == "DELTA_SYNC" {
                handle.get().delta_sync_upload(&relative_path).await
            } else {
                handle.get().upload_file(&relative_path, None).await
            };
            match result {
                Ok(hash) => Ok(SyncResult {
                    success: true,
                    sha256: hash,
                    transfer_mode,
                    error: None,
                }),
                Err(e) => Ok(SyncResult {
                    success: false,
                    sha256: String::new(),
                    transfer_mode,
                    error: Some(e.to_string()),
                }),
            }
        }
        _ => Err(format!("Unknown file mode: {}", file_mode)),
    };

    sync_result
}

// ─── IPC Command: Download File ─────────────────────────────

#[tauri::command]
async fn download_file(
    state: State<'_, AppState>,
    relative_path: String,
) -> Result<DownloadResult, String> {
    let mut handle = ClientGuard::take(&state, "download_file")?;
    let result = handle.get().download_file(&relative_path, None).await;

    match result {
        Ok(hash) => Ok(DownloadResult {
            success: true,
            relative_path,
            sha256: hash,
            error: None,
        }),
        Err(e) => Ok(DownloadResult {
            success: false,
            relative_path,
            sha256: String::new(),
            error: Some(e.to_string()),
        }),
    }
}

// ─── IPC Command: Upload Local Files/Folders ────────────────

fn copy_recursive(src: &Path, dst: &Path, base: &Path) -> Result<Vec<String>, String> {
    let mut results = Vec::new();
    if src.is_dir() {
        fs::create_dir_all(dst)
            .map_err(|e| format!("Failed to create dir {}: {}", dst.display(), e))?;
        for entry in fs::read_dir(src).map_err(|e| format!("Failed to read dir: {}", e))? {
            let entry = entry.map_err(|e| format!("Dir entry error: {}", e))?;
            let child_src = entry.path();
            let child_name = entry.file_name();
            let child_dst = dst.join(&child_name);
            let sub = copy_recursive(&child_src, &child_dst, base)?;
            results.extend(sub);
        }
    } else if src.is_file() {
        fs::create_dir_all(dst.parent().unwrap())
            .map_err(|e| format!("Failed to create parent dir: {}", e))?;
        fs::copy(src, dst)
            .map_err(|e| format!("Failed to copy {}: {}", src.display(), e))?;
        let rel = dst.strip_prefix(base)
            .map_err(|_| format!("Path {} is not under sync root", dst.display()))?
            .to_str()
            .ok_or("Invalid path")?
            .to_string();
        results.push(rel);
    }
    Ok(results)
}

#[tauri::command]
async fn upload_local_file(
    state: State<'_, AppState>,
    absolute_path: String,
) -> Result<Vec<String>, String> {
    let sync_root = {
        let guard = state.sync_root.lock().map_err(|e| e.to_string())?;
        guard.clone()
    };

    let src = Path::new(&absolute_path);
    let file_name = src.file_name()
        .ok_or("Invalid file path")?
        .to_str()
        .ok_or("Invalid filename")?;

    let sr = Path::new(&sync_root);
    let dest = sr.join(file_name);

    fs::create_dir_all(sr)
        .map_err(|e| format!("Failed to create sync root: {}", e))?;

    let rel_paths = copy_recursive(src, &dest, sr)?;
    Ok(rel_paths)
}

// ─── IPC Command: List Server Files ────────────────────────

#[tauri::command]
async fn list_server_files(
    state: State<'_, AppState>,
    prefix: String,
    recursive: bool,
) -> Result<Vec<FileInfo>, String> {
    let mut handle = ClientGuard::take(&state, "list_server_files")?;
    let proto_files = handle.get().list_server_files(&prefix, recursive).await
        .map_err(|e| e.to_string())?;

    let files: Vec<FileInfo> = proto_files.into_iter().map(|f| {
        let mode_str = match f.file_mode {
            1 => "FRESH",
            2 => "MODIFIED",
            3 => "UNCHANGED",
            _ => "UNSPECIFIED",
        };
        FileInfo {
            relative_path: f.relative_path,
            file_size: f.file_size,
            last_modified_ns: f.last_modified_ns,
            sha256_hash: f.sha256_hash,
            file_mode: mode_str.to_string(),
        }
    }).collect();

    Ok(files)
}

// ─── Main ──────────────────────────────────────────────────

fn main() {
    env_logger::init();

    tauri::Builder::default()
        .manage(AppState {
            client: Mutex::new(None),
            sync_root: Mutex::new("/tmp/adaptivesync_sync".to_string()),
        })
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_shell::init())
        .invoke_handler(tauri::generate_handler![
            connect_to_server,
            disconnect_from_server,
            negotiate_file,
            sync_file,
            download_file,
            list_server_files,
            upload_local_file,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
