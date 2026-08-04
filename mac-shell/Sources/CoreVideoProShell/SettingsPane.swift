// Settings — the app-level surface the shell was missing entirely: the Zoom
// account, output destinations and folders, monitor device, engine/app paths
// and the diagnostics the operator needs when something is wrong. Everything
// here is real state the shell already owns; nothing is a stub.

import AppKit
import SwiftUI

struct SettingsPane: View {
    @EnvironmentObject var model: AppModel

    var recordingFolder: String {
        (NSSearchPathForDirectoriesInDomains(.moviesDirectory, .userDomainMask, true).first
         ?? NSTemporaryDirectory()) + "/CoreVideoPro"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            card("Zoom account") {
                if model.zoomSignedIn {
                    HStack(spacing: 8) {
                        Text("Signed in").font(.grotesk(12, .medium))
                            .foregroundStyle(Studio.accent)
                        Text("Joins use your account — host privileges, no waiting room.")
                            .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                        Spacer()
                        Button("Sign out") { model.signOutZoom() }
                            .buttonStyle(GhostButtonStyle())
                    }
                } else {
                    HStack(spacing: 8) {
                        Button("Sign in with Zoom") { model.signInWithZoom() }
                            .buttonStyle(AccentButtonStyle())
                        Text(model.zoomOAuthStatus.isEmpty
                             ? "Guest joins wait in the waiting room and must be granted "
                               + "recording permission."
                             : model.zoomOAuthStatus)
                            .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    }
                }
                labeled("Display name") {
                    TextField("CoreVideo Pro (mac)", text: $model.displayName)
                        .textFieldStyle(StudioFieldStyle())
                        .frame(width: 240)
                }
            }

            // CANVAS — the render size everything downstream inherits
            // (ProductionSettingsWindow "Canvas" group: Canvas size, Canvas FPS).
            card("Canvas") {
                labeled("Canvas size") {
                    Picker("", selection: Binding(
                        get: { "\(model.canvasWidth)x\(model.canvasHeight)" },
                        set: { value in
                            let parts = value.split(separator: "x").compactMap { Int($0) }
                            if parts.count == 2 {
                                model.canvasWidth = parts[0]
                                model.canvasHeight = parts[1]
                                model.applyOutputProfile()
                            }
                        })) {
                        Text("3840×2160 (4K)").tag("3840x2160")
                        Text("2560×1440").tag("2560x1440")
                        Text("1920×1080").tag("1920x1080")
                        Text("1280×720").tag("1280x720")
                    }
                    .labelsHidden().frame(width: 180)
                }
                labeled("Canvas FPS") {
                    Picker("", selection: Binding(
                        get: { model.canvasFps },
                        set: { model.canvasFps = $0; model.applyOutputProfile() })) {
                        Text("60").tag(60)
                        Text("30").tag(30)
                        Text("24").tag(24)
                    }
                    .labelsHidden().frame(width: 100)
                }
                Text("The compositor renders at this size; stream and recording "
                     + "inherit it unless overridden.")
                    .font(.grotesk(11)).foregroundStyle(Studio.textDim)
            }

            // MULTIVIEW — layout mode + tile count (the reference's Layout group).
            card("Multiview") {
                labeled("Layout mode") {
                    Picker("", selection: Binding(
                        get: { model.multiviewLayoutMode },
                        set: { model.configureMultiviewer(mode: $0) })) {
                        Text("P/P top").tag("pgmPvwTop")
                        Text("P/P large").tag("pgmPvwLarge")
                        Text("P/P side").tag("pgmPvwSide")
                        Text("Grid").tag("grid")
                    }
                    .labelsHidden().frame(width: 160)
                }
                labeled("Tile count") {
                    Stepper(value: Binding(
                        get: { model.multiviewTileCount },
                        set: { model.multiviewTileCount = $0; model.applyMultiviewConfig() }),
                        in: 1...16) {
                        Text("\(model.multiviewTileCount)").font(.plexMono(11))
                    }
                    .frame(width: 120)
                }
            }

            card("Outputs") {
                labeled("Stream bitrate") {
                    HStack(spacing: 6) {
                        Slider(value: Binding(
                            get: { model.streamBitrateMbps },
                            set: { model.streamBitrateMbps = $0 }),
                            in: 1...20, step: 0.5)
                            .frame(width: 180)
                        Text(String(format: "%.1f Mbps", model.streamBitrateMbps))
                            .font(.plexMono(10)).foregroundStyle(Studio.secondary)
                    }
                }
                labeled("Stream codec") {
                    Picker("", selection: $model.streamCodec) {
                        Text("H.264").tag("h264")
                        Text("H.265 / HEVC").tag("h265")
                    }
                    .labelsHidden().frame(width: 160)
                }
                labeled("Recording format") {
                    Picker("", selection: $model.recordFormat) {
                        Text("MP4").tag("mp4")
                        Text("MOV").tag("mov")
                    }
                    .labelsHidden().frame(width: 120)
                }
                labeled("Filename prefix") {
                    TextField("show", text: $model.recordPrefix)
                        .textFieldStyle(StudioFieldStyle())
                        .frame(width: 160)
                }
                labeled("Recording folder") {
                    HStack(spacing: 6) {
                        Text(recordingFolder).font(.plexMono(10))
                            .foregroundStyle(Studio.secondary)
                            .lineLimit(1).truncationMode(.head)
                        Button("Reveal") {
                            NSWorkspace.shared.selectFile(
                                nil, inFileViewerRootedAtPath: recordingFolder)
                        }
                        .buttonStyle(GhostButtonStyle())
                    }
                }
                labeled("Stream URL") {
                    TextField("rtmp://…", text: $model.streamUrl)
                        .textFieldStyle(StudioFieldStyle())
                        .frame(width: 320)
                }
                labeled("Stream key") {
                    HStack(spacing: 6) {
                        SecureField("Stored in the Keychain", text: $model.streamKey)
                            .textFieldStyle(StudioFieldStyle())
                            .frame(width: 240)
                        Text("Keychain").font(.plexMono(9))
                            .foregroundStyle(Studio.textDim)
                    }
                }
                Toggle("Record ISO tracks alongside the program",
                       isOn: $model.isoRecordingEnabled)
                    .font(.grotesk(12))
            }

            card("Audio monitor") {
                Toggle("Monitor through the system default output", isOn: Binding(
                    get: { model.monitorEnabled },
                    set: { model.setMonitor(enabled: $0, volume: model.monitorVolume) }))
                    .font(.grotesk(12))
                labeled("Volume") {
                    Slider(value: Binding(
                        get: { model.monitorVolume },
                        set: { model.setMonitor(enabled: model.monitorEnabled, volume: $0) }),
                        in: 0...1)
                        .frame(width: 200)
                }
                if !model.monitorStatus.isEmpty {
                    Text("Engine reports: \(model.monitorStatus)")
                        .font(.plexMono(10)).foregroundStyle(Studio.secondary)
                }
            }

            card("System") {
                row("Media core", statusText)
                row("Renderer", rendererText)
                row("Virtual camera", "Unavailable — needs an Apple Developer ID "
                    + "(signed + notarized system extension)")
                labeled("Logs") {
                    HStack(spacing: 6) {
                        Button("Shell log") { reveal("CoreVideoPro-shell.log") }
                            .buttonStyle(GhostButtonStyle())
                        Button("Core log") { reveal("CoreVideoPro-core.log") }
                            .buttonStyle(GhostButtonStyle())
                        Button("Preferences") {
                            NSWorkspace.shared.selectFile(
                                ShellPrefs.path,
                                inFileViewerRootedAtPath:
                                    (ShellPrefs.path as NSString).deletingLastPathComponent)
                        }
                        .buttonStyle(GhostButtonStyle())
                    }
                }
            }
            Spacer(minLength: 0)
        }
        .frame(maxWidth: 720, alignment: .leading)
        .onAppear { model.refreshZoomSignedIn() }
    }

    var statusText: String {
        switch model.status {
        case .connected(let renderer): return "Connected (\(renderer))"
        case .launching: return "Launching…"
        case .exited(let code): return "Exited (\(code)) — relaunching"
        case .failed(let why): return why
        }
    }

    var rendererText: String {
        if case .connected(let renderer) = model.status { return renderer }
        return "—"
    }

    func reveal(_ name: String) {
        let path = NSHomeDirectory() + "/Library/Logs/" + name
        NSWorkspace.shared.selectFile(path,
                                      inFileViewerRootedAtPath: NSHomeDirectory() + "/Library/Logs")
    }

    func card(_ title: String, @ViewBuilder content: () -> some View) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.grotesk(14, .semibold))
            content()
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))
    }

    func labeled(_ label: String, @ViewBuilder content: () -> some View) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                .frame(width: 130, alignment: .leading)
            content()
            Spacer()
        }
    }

    func row(_ label: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                .frame(width: 130, alignment: .leading)
            Text(value).font(.grotesk(12)).foregroundStyle(Studio.textPrimary)
            Spacer()
        }
    }
}
