// TelemetryCard.swift: the "Diagnostics & telemetry" card at the bottom of the Zoom pane
// (Phase 4 R4, Task 9; placement D-T3). Layout and copy follow Windows
// W/Views/SettingsPage.xaml:187-221 and W/ViewModels/SettingsViewModel.cs:140-152, in order:
// title, toggle, what-is-sent copy, configuration status, "Preview what's sent", preview block.

import SwiftUI

enum TelemetryCardCopy {
    static let title = "Diagnostics & telemetry"
    static let toggle = "Share anonymous health data"
    static let body =
        "Send anonymous health data on app close and once a day: app version, session length, "
        + "output counts (recording / streaming / virtual camera on-off, ISO / capture / participant counts), "
        + "crashes since the last send, and a hardware class (CPU cores, RAM band, GPU tier). "
        + "No names, meeting IDs, stream keys, URLs, or file paths are ever included."
    static let configured = "Telemetry endpoint and API key configured."
    static let missingKey =
        "Telemetry API key is missing in this build (endpoint is configured) — nothing is sent even when enabled."
    static let preview = "Preview what's sent"

    static func status(configured isConfigured: Bool) -> String { isConfigured ? configured : missingKey }
}

extension AppModel {
    /// Windows SettingsViewModel: read the stored consent WITHOUT re-persisting it.
    func loadTelemetryConsent() {
        telemetryConsentOn = telemetryService?.consentStore.load().enabled ?? false
    }

    /// The operator's toggle: persist on change, log the decision (never on load).
    func setTelemetryConsent(_ on: Bool) {
        guard telemetryConsentOn != on else { return }
        telemetryConsentOn = on
        guard let service = telemetryService else { return }
        do {
            try service.consentStore.setEnabled(on)
            service.note("telemetry: consent \(on ? "ENABLED" : "disabled") by operator")
        } catch {
            service.note("telemetry: could not persist consent (\(error.localizedDescription))")
            telemetryConsentOn = service.consentStore.load().enabled
        }
    }

    /// The exact session-end payload, consent on or off (inspectable before egress).
    func previewTelemetry() {
        telemetryPreviewJSON = telemetryService?.previewJSON() ?? ""
    }
}

struct TelemetryCard: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(TelemetryCardCopy.title).font(.grotesk(14, .semibold))
            HStack(spacing: 8) {
                Text(TelemetryCardCopy.toggle).font(.grotesk(12)).foregroundStyle(Studio.textPrimary)
                Spacer()
                Toggle(TelemetryCardCopy.toggle, isOn: Binding(get: { model.telemetryConsentOn },
                                                               set: { model.setTelemetryConsent($0) }))
                    .labelsHidden().toggleStyle(.switch).controlSize(.small)
                Text(model.telemetryConsentOn ? "On" : "Off").font(.grotesk(11)).foregroundStyle(Studio.secondary)
            }
            Text(TelemetryCardCopy.body).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Text(TelemetryCardCopy.status(configured: model.telemetryService?.isConfigured ?? false))
                .font(.grotesk(12)).foregroundStyle(Studio.accent)
                .fixedSize(horizontal: false, vertical: true)
            Button(TelemetryCardCopy.preview) { model.previewTelemetry() }
                .buttonStyle(GhostButtonStyle())
            if !model.telemetryPreviewJSON.isEmpty {
                ScrollView([.horizontal, .vertical]) {
                    Text(model.telemetryPreviewJSON)
                        .font(.plexMono(12)).foregroundStyle(Studio.textPrimary)
                        .textSelection(.enabled)
                        .fixedSize()
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 320)
                .padding(12)
                .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
                .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
            }
        }
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))
    }
}
