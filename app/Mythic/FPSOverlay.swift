import SwiftUI

/// Small overlay shown over the Metal render view. Reads DXMT's present
/// counter at 100ms intervals, displays current count + computed FPS.
/// Tap to hide/show.
struct FPSOverlay: View {
    @State private var presentCount: UInt64 = 0
    @State private var fps: Double = 0
    @State private var lastSampleCount: UInt64 = 0
    @State private var lastSampleTime = CFAbsoluteTimeGetCurrent()
    @State private var visible: Bool = true
    @State private var timer: Timer? = nil

    var body: some View {
        Group {
            if visible {
                HStack(spacing: 8) {
                    Text("Present:")
                        .foregroundColor(.secondary)
                    Text("\(presentCount)")
                        .foregroundColor(.primary)
                    Text("|")
                        .foregroundColor(.secondary)
                    Text("FPS:")
                        .foregroundColor(.secondary)
                    Text(String(format: "%.1f", fps))
                        .foregroundColor(fpsColor)
                        .frame(width: 40, alignment: .trailing)
                }
                .font(.system(.caption, design: .monospaced))
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
                .background(Color.black.opacity(0.55))
                .cornerRadius(6)
            } else {
                // Tiny tap target while hidden
                Circle()
                    .fill(Color.black.opacity(0.3))
                    .frame(width: 12, height: 12)
            }
        }
        .onTapGesture { visible.toggle() }
        .onAppear { startTimer() }
        .onDisappear { stopTimer() }
    }

    private var fpsColor: Color {
        if fps >= 50 { return .green }
        if fps >= 30 { return .yellow }
        if fps > 0 { return .orange }
        return .secondary
    }

    private func startTimer() {
        stopTimer()
        lastSampleTime = CFAbsoluteTimeGetCurrent()
        lastSampleCount = mythic_get_present_count()
        presentCount = lastSampleCount
        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { _ in
            let now = CFAbsoluteTimeGetCurrent()
            let cur = mythic_get_present_count()
            let dt = now - lastSampleTime
            let dc = cur > lastSampleCount ? cur - lastSampleCount : 0
            // Exponential smoothing over the 100ms window so the number
            // doesn't jitter wildly between consecutive samples
            let instFps = dt > 0 ? Double(dc) / dt : 0
            fps = (fps * 0.6) + (instFps * 0.4)
            presentCount = cur
            lastSampleCount = cur
            lastSampleTime = now
        }
    }

    private func stopTimer() {
        timer?.invalidate()
        timer = nil
    }
}
