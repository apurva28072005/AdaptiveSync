import { FileEntry } from "../App";

// ════════════════════════════════════════════════════════════
// FileList — Sidebar file browser for server files
// ════════════════════════════════════════════════════════════

interface Props {
  files: FileEntry[];
  selectedFiles: string[];
  onSelectionChange: (paths: string[]) => void;
  onSync: (paths: string[]) => void;
  onDownload: (path: string) => void;
  connected: boolean;
}

function formatFileSize(bytes: number): string {
  if (bytes === 0) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB"];
  const i = Math.floor(Math.log(bytes) / Math.log(1024));
  return `${(bytes / Math.pow(1024, i)).toFixed(1)} ${units[i]}`;
}

function getTransferModeBadge(mode?: string) {
  if (!mode) return null;
  switch (mode) {
    case "ZERO_COPY":
      return <span className="badge badge-zero-copy">Zero-Copy</span>;
    case "DELTA_SYNC":
      return <span className="badge badge-delta-sync">Delta-Sync</span>;
    case "UNCHANGED":
      return <span className="badge badge-unchanged">Unchanged</span>;
    case "FRESH":
      return <span className="badge badge-fresh">Fresh</span>;
    default:
      return null;
  }
}

function FileList({ files, selectedFiles, onSelectionChange, onSync, onDownload, connected }: Props) {
  const toggleFile = (path: string) => {
    if (selectedFiles.includes(path)) {
      onSelectionChange(selectedFiles.filter(p => p !== path));
    } else {
      onSelectionChange([...selectedFiles, path]);
    }
  };

  const selectAll = () => {
    onSelectionChange(files.map(f => f.relativePath));
  };

  const selectNone = () => {
    onSelectionChange([]);
  };

  if (!connected) {
    return (
      <div style={{ color: "var(--text-secondary)", fontSize: 13, textAlign: "center", padding: 20 }}>
        Connect to a server to view files
      </div>
    );
  }

  if (files.length === 0) {
    return (
      <div style={{ color: "var(--text-secondary)", fontSize: 13, textAlign: "center", padding: 20 }}>
        No files on server
      </div>
    );
  }

  return (
    <div>
      {/* Selection Controls */}
      <div style={{ display: "flex", gap: 8, marginBottom: 12 }}>
        <button className="btn" style={{ padding: "4px 10px", fontSize: 11 }} onClick={selectAll}>
          Select All
        </button>
        <button className="btn" style={{ padding: "4px 10px", fontSize: 11 }} onClick={selectNone}>
          None
        </button>
        {selectedFiles.length > 0 && (
          <button
            className="btn btn-primary"
            style={{ padding: "4px 10px", fontSize: 11, marginLeft: "auto" }}
            onClick={() => onSync(selectedFiles)}
          >
            Sync ({selectedFiles.length})
          </button>
        )}
      </div>

      {/* File List */}
      <div style={{ display: "flex", flexDirection: "column" }}>
        {files.map((file) => (
          <div
            key={file.relativePath}
            className="file-row"
            style={{ cursor: "pointer", gap: 8, fontSize: 12 }}
            onClick={() => toggleFile(file.relativePath)}
          >
            <input
              type="checkbox"
              checked={selectedFiles.includes(file.relativePath)}
              onChange={() => toggleFile(file.relativePath)}
              style={{ accentColor: "var(--accent)" }}
            />
            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>
                {file.relativePath}
              </div>
              <div style={{ color: "var(--text-secondary)", fontSize: 11, marginTop: 2 }}>
                {formatFileSize(file.fileSize)}
                {getTransferModeBadge(file.transferMode)}
              </div>
            </div>
            <button
              className="btn"
              style={{ padding: "2px 8px", fontSize: 10, flexShrink: 0 }}
              onClick={(e) => { e.stopPropagation(); onDownload(file.relativePath); }}
            >
              Download
            </button>
          </div>
        ))}
      </div>
    </div>
  );
}

export default FileList;
