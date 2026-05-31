import { TransferJob } from "../App";

// ════════════════════════════════════════════════════════════
// TransferProgress — Visualize active and completed transfers
// ════════════════════════════════════════════════════════════

interface Props {
  transfers: TransferJob[];
}

function formatBytes(bytes: number): string {
  if (bytes === 0) return "0 B";
  const units = ["B", "KB", "MB", "GB"];
  const i = Math.floor(Math.log(bytes) / Math.log(1024));
  return `${(bytes / Math.pow(1024, i)).toFixed(1)} ${units[i]}`;
}

function formatDuration(ms: number): string {
  if (ms < 1000) return `${ms}ms`;
  return `${(ms / 1000).toFixed(1)}s`;
}

function TransferProgress({ transfers }: Props) {
  if (transfers.length === 0) {
    return (
      <div className="card" style={{ textAlign: "center", color: "var(--text-secondary)", fontSize: 13 }}>
        No transfers yet
      </div>
    );
  }

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
      {transfers.map((transfer) => {
        const progress = transfer.bytesTotal > 0
          ? (transfer.bytesTransferred / transfer.bytesTotal) * 100
          : 0;

        return (
          <div key={transfer.id} className="card" style={{ padding: 12 }}>
            {/* Header Row */}
            <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 8 }}>
              {/* Status Icon */}
              {transfer.status === "complete" && (
                <span style={{ color: "var(--success)", fontSize: 16 }}>✓</span>
              )}
              {transfer.status === "error" && (
                <span style={{ color: "var(--error)", fontSize: 16 }}>✗</span>
              )}
              {transfer.status === "transferring" && (
                <span style={{ color: "var(--warning)", fontSize: 16 }}>⬆</span>
              )}
              {(transfer.status === "negotiating" || transfer.status === "pending") && (
                <span style={{ color: "var(--text-secondary)", fontSize: 16 }}>⏳</span>
              )}

              {/* File Name */}
              <span style={{ fontSize: 13, fontWeight: 500, flex: 1, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>
                {transfer.relativePath}
              </span>

              {/* Transfer Mode Badge */}
              {transfer.transferMode === "zero_copy" && (
                <span className="badge badge-zero-copy">Zero-Copy</span>
              )}
              {transfer.transferMode === "delta_sync" && (
                <span className="badge badge-delta-sync">Delta-Sync</span>
              )}
              {transfer.transferMode === "unchanged" && (
                <span className="badge badge-unchanged">Unchanged</span>
              )}

              {/* Size */}
              <span style={{ fontSize: 12, color: "var(--text-secondary)" }}>
                {formatBytes(transfer.bytesTotal)}
              </span>
            </div>

            {/* Progress Bar */}
            {transfer.status === "transferring" && (
              <div className="progress-bar" style={{ marginBottom: 4 }}>
                <div
                  className="progress-bar-fill"
                  style={{ width: `${progress}%` }}
                />
              </div>
            )}
            {transfer.status === "complete" && (
              <div className="progress-bar">
                <div className="progress-bar-fill complete" style={{ width: "100%" }} />
              </div>
            )}

            {/* Details Row */}
            <div style={{ display: "flex", justifyContent: "space-between", fontSize: 11, color: "var(--text-secondary)" }}>
              <span>
                {transfer.status === "negotiating" && "Negotiating transfer mode..."}
                {transfer.status === "transferring" && `${formatBytes(transfer.bytesTransferred)} / ${formatBytes(transfer.bytesTotal)} — ${transfer.rateMbps.toFixed(1)} MB/s`}
                {transfer.status === "complete" && `Complete in ${formatDuration(transfer.elapsedMs)}`}
                {transfer.status === "error" && `Error: ${transfer.errorMessage}`}
              </span>
              {transfer.elapsedMs > 0 && transfer.status !== "negotiating" && (
                <span>{formatDuration(transfer.elapsedMs)}</span>
              )}
            </div>
          </div>
        );
      })}
    </div>
  );
}

export default TransferProgress;
