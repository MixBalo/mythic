import Foundation
import SwiftUI

final class LogStore: ObservableObject {
    static let shared = LogStore()

    /// One row per unique signature — semantically identical events bucket here.
    @Published var entries: [LogEntry] = []

    private let logFileURL: URL
    private let dateFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    // Tail-file reader (background)
    private var tail: LogTail?
    // Signature → index into `entries` so we can update in O(1)
    private var sigToIndex: [String: Int] = [:]
    // Lock for sigToIndex + pending mutations
    private let stateLock = NSLock()
    // Pending batched diffs to apply on main thread
    private var pendingNew: [LogEntry] = []
    private var pendingUpdates: [(index: Int, count: Int, lastRaw: String, lastTimestamp: Date)] = []
    private var flushTimer: Timer?

    /// When true, UI flush is skipped (entries still tracked in storage).
    var uiPaused = false

    // Cap on distinct entries kept in memory
    private let maxEntries = 200

    struct LogEntry: Identifiable {
        let id = UUID()
        var firstTimestamp: Date
        var lastTimestamp: Date
        var signature: String
        var lastRaw: String
        var count: Int
        var level: Level

        enum Level: String {
            case info = "INFO"
            case success = "OK"
            case error = "ERR"
            case debug = "DBG"
        }
    }

    private init() {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        logFileURL = docs.appendingPathComponent("mythic-log.txt")

        // Clear log file on each launch (was the behavior before)
        try? "".write(to: logFileURL, atomically: true, encoding: .utf8)

        // Start batch flush timer on main thread (200ms = responsive but not
        // hammering SwiftUI)
        DispatchQueue.main.async {
            self.flushTimer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
                self?.flushPending()
            }
        }

        // Tail the log file. Reads everything Wine + DXMT + FEX write via
        // dprintf(STDERR_FILENO, ...), wine_log_write, etc.
        tail = LogTail(path: logFileURL.path) { [weak self] line in
            self?.handleRawLine(line)
        }
        tail?.start()

        // Also accept programmatic logs from Swift/ObjC code via existing
        // C callbacks (kept for compatibility with code that doesn't write
        // to the log file).
        wine_set_ui_log_callback { cStr in
            guard let cStr = cStr else { return }
            let message = String(cString: cStr)
            LogStore.shared.handleRawLine(message)
        }
        jit_set_log_callback { cStr in
            guard let cStr = cStr else { return }
            let message = String(cString: cStr)
            LogStore.shared.handleRawLine(message)
        }
    }

    /// Public entry point for Swift-side logging (kept for ContentView calls)
    func log(_ message: String, level: LogEntry.Level = .info) {
        handleRawLine(message)
        // Also append to the file so it shows up in pulled logs alongside Wine output
        appendToFile(message, level: level)
    }

    /// Called from tail-file callback (background queue) or C callback.
    private func handleRawLine(_ raw: String) {
        // Filter out lines we never want in UI (excessive byte spam, etc.)
        if shouldDropLine(raw) { return }

        let (sig, level) = LogPattern.canonicalize(raw)
        if sig.isEmpty { return }

        let now = Date()
        stateLock.lock()
        if let idx = sigToIndex[sig] {
            pendingUpdates.append((idx, 1, raw, now))
        } else {
            // Reserve an index slot — actual append happens on flush.
            // We can't know the true index here without holding entries,
            // so we'll resolve indices during flush.
            let entry = LogEntry(
                firstTimestamp: now,
                lastTimestamp: now,
                signature: sig,
                lastRaw: raw,
                count: 1,
                level: level
            )
            pendingNew.append(entry)
            // Map sig → -1 sentinel so subsequent same-sig lines from this
            // batch get treated as new too (will be merged during flush).
            sigToIndex[sig] = -1
        }
        stateLock.unlock()
    }

    /// Filter rules for raw lines. Anything that returns true is dropped
    /// before signature canonicalization.
    private func shouldDropLine(_ raw: String) -> Bool {
        // Drop Wine's `trace:file:WriteFile` / `NtWriteFile` / `SysCall` chatter
        // — these are amplified by our own logging path (every dprintf is
        // dup2'd to the log fd, which then goes through Wine's file trace).
        // The signal lives in the original log lines, not these wrappers.
        if raw.contains("trace:file:WriteFile") { return true }
        if raw.contains("trace:file:NtWriteFile") { return true }
        if raw.contains("SysCall  NtWriteFile") { return true }
        if raw.contains("SysCall  NtQueryPerformanceCounter") { return true }
        if raw.contains("SysRet   NtWriteFile") { return true }
        if raw.contains("SysRet   NtQueryPerformanceCounter") { return true }
        // Drop verbose IR dispatch (already silenced in FEX, but defensive)
        if raw.contains("[iOS] Arm64JIT: Dispatching Op") { return true }
        if raw.contains("[iOS] Decoder:") { return true }
        return false
    }

    /// Apply pending changes to @Published entries (main thread, every 200ms).
    private func flushPending() {
        guard !uiPaused else { return }

        stateLock.lock()
        let newBatch = pendingNew
        let updateBatch = pendingUpdates
        pendingNew.removeAll(keepingCapacity: true)
        pendingUpdates.removeAll(keepingCapacity: true)
        stateLock.unlock()

        if newBatch.isEmpty && updateBatch.isEmpty { return }

        // Apply updates (existing entries: bump count, update timestamp)
        for u in updateBatch {
            // Some indices may have been the -1 sentinel — match by signature
            if u.index < 0 || u.index >= entries.count { continue }
            entries[u.index].count += u.count
            entries[u.index].lastTimestamp = u.lastTimestamp
            entries[u.index].lastRaw = u.lastRaw
        }

        // Apply news: dedup against in-batch sigs (so if 5 same-sig lines
        // arrived in one batch, we get one entry with count=5)
        var batchSigToBatchIdx: [String: Int] = [:]
        var collapsedNew: [LogEntry] = []
        for var entry in newBatch {
            if let i = batchSigToBatchIdx[entry.signature] {
                collapsedNew[i].count += 1
                collapsedNew[i].lastTimestamp = entry.lastTimestamp
                collapsedNew[i].lastRaw = entry.lastRaw
            } else {
                // Or against the live entries list (race with this same flush)
                if let existing = entries.firstIndex(where: { $0.signature == entry.signature }) {
                    entries[existing].count += 1
                    entries[existing].lastTimestamp = entry.lastTimestamp
                    entries[existing].lastRaw = entry.lastRaw
                    continue
                }
                batchSigToBatchIdx[entry.signature] = collapsedNew.count
                collapsedNew.append(entry)
            }
        }

        // Append new entries
        for entry in collapsedNew {
            entries.append(entry)
            let newIndex = entries.count - 1
            sigToIndex[entry.signature] = newIndex
        }

        // Reindex if we evicted
        if entries.count > maxEntries {
            // Drop oldest by lastTimestamp
            entries.sort { $0.lastTimestamp < $1.lastTimestamp }
            let drop = entries.count - maxEntries
            let removed = entries.prefix(drop).map { $0.signature }
            entries.removeFirst(drop)
            for sig in removed { sigToIndex.removeValue(forKey: sig) }
            // Reindex remaining
            sigToIndex.removeAll()
            for (i, e) in entries.enumerated() { sigToIndex[e.signature] = i }
            // Sort back to insertion order (by firstTimestamp)
            entries.sort { $0.firstTimestamp < $1.firstTimestamp }
            sigToIndex.removeAll()
            for (i, e) in entries.enumerated() { sigToIndex[e.signature] = i }
        }
    }

    /// Manual clear (used by UI button)
    func clear() {
        stateLock.lock()
        sigToIndex.removeAll()
        pendingNew.removeAll()
        pendingUpdates.removeAll()
        stateLock.unlock()
        entries.removeAll()
        try? "".write(to: logFileURL, atomically: true, encoding: .utf8)
    }

    /// Write to file (called from `log()` for Swift-side messages so they
    /// land in the file alongside Wine/FEX output, picked up by the tail
    /// reader).
    private func appendToFile(_ message: String, level: LogEntry.Level = .info) {
        let line = "[\(dateFormatter.string(from: Date()))] [\(level.rawValue)] \(message)\n"
        if let data = line.data(using: .utf8) {
            if let handle = try? FileHandle(forWritingTo: logFileURL) {
                handle.seekToEndOfFile()
                handle.write(data)
                handle.closeFile()
            }
        }
    }
}
