// StreamDestinationsView.swift — the destination rows (mac streaming Phase 2,
// D1). Each row's status is the projection in StreamDestinations.swift
// (lifecycle + supervisor, never the published status); Retry appears only
// when the supervisor gave up. The editable SRT/HLS fields live in Settings.

import SwiftUI

extension DestinationTone {
    var color: Color {
        switch self {
        case .good: return Studio.accent
        case .warn: return Studio.amber
        case .bad: return Studio.red
        case .neutral: return Studio.secondary
        }
    }
}

/// Toggle, name, truthful status, Retry: one line per destination.
struct DestinationRowsView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(StreamDestinationPolicy.destinationIds, id: \.self) { id in
                row(id)
            }
            if model.streamingDesired {
                Text("Stop streaming to change destinations.")
                    .font(.grotesk(10)).foregroundStyle(Studio.textDim)
            }
        }
    }

    private func enabled(_ id: String) -> Binding<Bool> {
        Binding(
            get: {
                switch id {
                case "rtmp": return model.streamDestinations.rtmpEnabled
                case "srt": return model.streamDestinations.srtEnabled
                default: return model.streamDestinations.hlsEnabled
                }
            },
            set: { value in
                switch id {
                case "rtmp": model.streamDestinations.rtmpEnabled = value
                case "srt": model.streamDestinations.srtEnabled = value
                default: model.streamDestinations.hlsEnabled = value
                }
            })
    }

    @ViewBuilder
    private func row(_ id: String) -> some View {
        let name = StreamDestinationPolicy.displayName(id)
        let status = model.destinationRows[id] ?? DestinationStatus(label: "Off", tone: .neutral)
        let problem = StreamDestinationPolicy.rowError(id, model.streamConfig, model.streamSecrets)
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 8) {
                Toggle(name, isOn: enabled(id))
                    .toggleStyle(StudioToggleStyle())
                    .font(.grotesk(12, .semibold)).foregroundStyle(Studio.textPrimary)
                    .disabled(model.streamingDesired)
                    .accessibilityLabel("\(name) destination")
                Spacer()
                Text(status.label)
                    .font(.grotesk(11, .semibold)).foregroundStyle(status.tone.color)
                    .accessibilityLabel("\(name) status: \(status.label)")
                if status.showsRetry {
                    Button("Retry") { model.retryDestination(id) }
                        .buttonStyle(GhostButtonStyle())
                        .accessibilityLabel("Retry \(name)")
                        .help("Ask the media core to try \(name) again")
                }
            }
            if !status.detail.isEmpty {
                Text(status.detail)
                    .font(.grotesk(10)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if let problem {
                Text(problem)
                    .font(.grotesk(10)).foregroundStyle(Studio.amber)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

/// Settings card: the rows plus the SRT and HLS fields (Windows
/// ProductionSettingsWindow stream destinations, minus NDI which is its own toggle).
struct StreamDestinationSettingsView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            DestinationRowsView()
            Divider().overlay(Studio.border)
            Text("SRT").font(.grotesk(12, .semibold)).foregroundStyle(Studio.textPrimary)
            field("Mode") {
                Picker("", selection: $model.streamDestinations.srtMode) {
                    ForEach(StreamDestinationPolicy.srtModes, id: \.self) { Text($0).tag($0) }
                }
                .labelsHidden().frame(width: 140)
                .accessibilityLabel("SRT mode")
            }
            field("Host") {
                TextField("Host name or IP address", text: $model.streamDestinations.srtHost)
                    .textFieldStyle(StudioFieldStyle()).frame(width: 240)
                    .accessibilityLabel("SRT host")
            }
            field("Port") {
                TextField("9000", text: $model.streamDestinations.srtPort)
                    .textFieldStyle(StudioFieldStyle()).frame(width: 90)
                    .accessibilityLabel("SRT port")
            }
            field("Latency (ms)") {
                TextField("120", text: $model.streamDestinations.srtLatencyMs)
                    .textFieldStyle(StudioFieldStyle()).frame(width: 90)
                    .accessibilityLabel("SRT latency in milliseconds")
            }
            field("Stream ID") {
                TextField("optional", text: $model.streamDestinations.srtStreamId)
                    .textFieldStyle(StudioFieldStyle()).frame(width: 240)
                    .accessibilityLabel("SRT stream ID")
            }
            field("Encryption") {
                Picker("", selection: $model.streamDestinations.srtKeyLength) {
                    Text("Off").tag("0")
                    Text("AES-128").tag("16")
                    Text("AES-192").tag("24")
                    Text("AES-256").tag("32")
                }
                .labelsHidden().frame(width: 140)
                .accessibilityLabel("SRT encryption key length")
            }
            field("Passphrase") {
                SecureField("Stored in the Keychain", text: $model.srtPassphrase)
                    .textFieldStyle(StudioFieldStyle()).frame(width: 240)
                    .accessibilityLabel("SRT passphrase")
            }
            Divider().overlay(Studio.border)
            Text("HLS").font(.grotesk(12, .semibold)).foregroundStyle(Studio.textPrimary)
            field("Playlist URL") {
                TextField("https://…/playlist.m3u8", text: $model.streamDestinations.hlsPlaylistUrl)
                    .textFieldStyle(StudioFieldStyle()).frame(width: 320)
                    .accessibilityLabel("HLS playlist URL")
            }
            Text("HLS pushes over HTTP PUT; it needs H.264 and a URL ending in .m3u8.")
                .font(.grotesk(11)).foregroundStyle(Studio.textDim)
            Divider().overlay(Studio.border)
            Text(runtimeLine)
                .font(.plexMono(10)).foregroundStyle(Studio.secondary)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityLabel("Streaming runtime")
        }
    }

    /// The Health readout F1 asks for: which ffmpeg the core actually resolved.
    private var runtimeLine: String {
        let detail = model.streamRuntimeDetail
        guard let colon = detail.firstIndex(of: ":") else {
            return "ffmpeg: reported by the media core once a stream starts."
        }
        let path = String(detail[detail.index(after: colon)...])
        let bundled = path.hasSuffix(".app/Contents/Resources/ffmpeg")
        return "ffmpeg: \(path)\(bundled ? " (bundled, LGPL)" : "")"
    }

    private func field(_ label: String, @ViewBuilder content: () -> some View) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                .frame(width: 130, alignment: .leading)
            content()
            Spacer()
        }
    }
}
