// CrashBar.swift: the "CoreVideo Pro crashed last session" bar (Phase 4 R5, Task 12). Layout and
// copy follow Windows W/Views/StudioWorkspace.xaml:1337-1356 and StudioWorkspace.xaml.cs:137-226:
// a closable warning bar over the top of the content area, a "Send report" action, and the
// sending / sent / not-sent / failed states. Mac wording: "crash report" for Windows' "crash dump"
// (the Mac input is an .ips report, D-X1); the rest is verbatim. Nothing is scanned or shown
// without a key, and nothing is sent unless the operator clicks.

import SwiftUI

struct CrashBarState: Equatable {
    enum Style: Equatable { case warning, success, error }
    var style = Style.warning
    var title = "CoreVideo Pro crashed last session"
    var message = ""
    /// nil = the button is hidden.
    var buttonTitle: String? = "Send report"
    var buttonEnabled = true

    static func offered(_ reports: [CrashReportFile]) -> CrashBarState? {
        guard let newest = reports.last else { return nil }
        let count = reports.count == 1 ? "1 report" : "\(reports.count) reports"
        return CrashBarState(message: "A crash report from \(newest.processName) was found (\(count)). "
            + "Send a report to help fix it? The report (crash report + recent logs, secrets redacted) "
            + "is saved locally either way — nothing is sent unless you click Send.")
    }

    func sending() -> CrashBarState {
        var next = self
        next.buttonEnabled = false
        next.message = "Assembling and sending the crash report…"
        return next
    }

    func finished(_ outcome: CrashSendOutcome) -> CrashBarState {
        var next = self
        next.style = outcome.success ? .success : .warning
        next.title = outcome.success ? "Crash report sent" : "Crash report not sent"
        next.message = outcome.message
        if outcome.success || !outcome.canRetry {
            next.buttonTitle = nil
        } else {
            next.buttonTitle = "Try again"
            next.buttonEnabled = true
        }
        return next
    }

    func failed(_ message: String) -> CrashBarState {
        var next = self
        next.style = .error
        next.title = "Crash report not sent"
        next.message = "Sending failed: \(message)"
        next.buttonTitle = "Try again"
        next.buttonEnabled = true
        return next
    }
}

extension AppModel {
    /// The launch scan: once per process, off the main actor, never before first paint, and
    /// only when a key is configured (no coordinator = no scan, no bar).
    func startCrashReports(coordinator: CrashReportCoordinator?) {
        guard !crashScanStarted, let coordinator else { return }
        crashScanStarted = true
        crashCoordinator = coordinator
        let directories = coordinator.directories, store = coordinator.store, now = coordinator.now()
        Task.detached(priority: .utility) {
            let found = CrashReportOffer.scanAndAdvance(directories: directories, store: store, now: now)
            await MainActor.run { [weak self] in
                guard let self else { return }
                self.pendingCrashReports = found
                self.crashBar = CrashBarState.offered(found)
            }
        }
    }

    /// "Send report" / "Try again".
    func sendCrashReport() {
        guard let coordinator = crashCoordinator, !pendingCrashReports.isEmpty, let bar = crashBar else { return }
        crashBar = bar.sending()
        let reports = pendingCrashReports
        Task { @MainActor in
            let outcome = await coordinator.send(reports, writeSupportBundle: { _ in false })
            self.crashBar = (self.crashBar ?? bar).finished(outcome)
        }
    }

    func dismissCrashBar() { crashBar = nil }
}

struct CrashBarView: View {
    let state: CrashBarState
    var onSend: () -> Void = {}
    var onClose: () -> Void = {}

    private var tint: Color {
        switch state.style {
        case .warning: return Studio.amber
        case .success: return Studio.accent
        case .error: return Studio.red
        }
    }

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Circle().fill(tint).frame(width: 8, height: 8).padding(.top, 5)
            VStack(alignment: .leading, spacing: 4) {
                Text(state.title).font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary)
                Text(state.message).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let button = state.buttonTitle {
                    Button(button, action: onSend)
                        .buttonStyle(GhostButtonStyle())
                        .disabled(!state.buttonEnabled)
                        .padding(.top, 2)
                }
            }
            Spacer(minLength: 0)
            Button(action: onClose) { Image(systemName: "xmark").font(.system(size: 11, weight: .semibold)) }
                .buttonStyle(.plain).foregroundStyle(Studio.secondary)
                .help("Close")
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(tint.opacity(0.7), lineWidth: 1))
    }
}
