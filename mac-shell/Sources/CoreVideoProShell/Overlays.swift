// Overlays — bringing the macOS tab up to the WinUI OverlaysPage.
//
// The macOS tab was one card (lower-third name/title/position/Show) against
// four Windows sections. That is the gap the owner could feel but not point at:
// no build timing, no brand kit, no caption controls, no keyer status.
//
// All of this is SHELL work — the core already accepts:
//   set-overlay-asset   text/imageUri/position/title/org/keyPosition/keyer
//                       + buildInMs/buildOutMs (clamped 50-2000 core-side)
//   set-brand-kit       name/logoText/brandColor/accentColor/backgroundColor
//                       /fontFamily/lowerThirdStyle/captionStyle
//                       /defaultOverlayBehavior
//   set-caption-enabled, push-caption-cue
// and publishes `brandKit` in the snapshot.
//
// DELIBERATELY NOT BUILT: browser overlays (DSK). That needs a macOS browser
// host — the Windows one is a WebView2 process — so the section states it is
// unavailable rather than being silently omitted.

import SwiftUI

// The brand kit stores colours as "#rrggbb" strings because that is what the
// core's set-brand-kit takes; DesignSystem's Color(hex:) is UInt32-based, so
// these bridge the two without duplicating the swatch logic.
extension Color {
    init?(brandHex: String) {
        let cleaned = brandHex.trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: "#", with: "")
        guard cleaned.count == 6, let value = UInt32(cleaned, radix: 16) else { return nil }
        self.init(hex: value)
    }

    /// "#rrggbb" for the core. Falls back to the input on an unconvertible
    /// colour rather than silently writing black.
    func brandHexString(fallback: String) -> String {
        guard let components = NSColor(self).usingColorSpace(.sRGB) else { return fallback }
        let r = Int((components.redComponent * 255).rounded())
        let g = Int((components.greenComponent * 255).rounded())
        let b = Int((components.blueComponent * 255).rounded())
        return String(format: "#%02x%02x%02x", r, g, b)
    }
}

// Motion timing presets, matching the WinUI dropdown. "Custom" keeps whatever
// the operator dialled in.
enum MotionTiming: String, CaseIterable {
    case snap, standard, gentle, custom

    var label: String {
        switch self {
        case .snap: return "Snap"
        case .standard: return "Standard"
        case .gentle: return "Gentle"
        case .custom: return "Custom"
        }
    }

    var timing: (inMs: Int, outMs: Int)? {
        switch self {
        case .snap: return (150, 120)
        case .standard: return (1000, 500)
        case .gentle: return (1600, 900)
        case .custom: return nil
        }
    }
}

struct OverlaysState: Codable, Equatable {
    // The core clamps both to 50-2000ms; the UI must not offer dead travel.
    var buildInMs = 1000
    var buildOutMs = 500
    var motionTiming = MotionTiming.standard.rawValue
    var lowerThirdStyle = "gradient"
    var brandName = "CoreVideo"
    var logoText = "CoreVideo"
    var brandColor = "#44c1a1"
    var accentColor = "#f0a85c"
    var backgroundColor = "#0c1118"
    var defaultOverlayBehavior = "all-off"
    var captionStyle = "standard"
    var captionsEnabled = false
}

extension AppModel {
    static let buildMsRange: ClosedRange<Double> = 50...2000
    static let lowerThirdStyles = ["gradient", "solid", "outline", "minimal"]
    static let defaultOverlayBehaviors = ["all-off", "restore-last", "lower-third-only"]

    /// Pushes the brand kit. Coalesced: colour pickers and text fields emit a
    /// change per keystroke/drag, and one request per delta is what starved the
    /// command queue until everything timed out.
    func applyBrandKit() {
        brandSyncTask?.cancel()
        brandSyncTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 200_000_000)
            guard !Task.isCancelled else { return }
            await self?.pushBrandKit()
        }
    }

    @MainActor
    func pushBrandKit() async {
        let o = overlays
        sendOverlay([
            "type": "set-brand-kit",
            "name": o.brandName,
            "logoText": o.logoText,
            "brandColor": o.brandColor,
            "accentColor": o.accentColor,
            "backgroundColor": o.backgroundColor,
            "lowerThirdStyle": o.lowerThirdStyle,
            "captionStyle": o.captionStyle,
            "defaultOverlayBehavior": o.defaultOverlayBehavior,
        ])
    }

    func setCaptionsEnabled(_ enabled: Bool) {
        overlays.captionsEnabled = enabled
        sendOverlay(["type": "set-caption-enabled", "enabled": enabled])
    }

    /// Applies a motion preset to the build times, or leaves them alone for
    /// Custom. Selecting a preset must MOVE the numbers so the readout never
    /// disagrees with what will actually be sent.
    func applyMotionTiming(_ timing: MotionTiming) {
        overlays.motionTiming = timing.rawValue
        if let preset = timing.timing {
            overlays.buildInMs = preset.inMs
            overlays.buildOutMs = preset.outMs
        }
    }
}

struct OverlaysPane: View {
    @EnvironmentObject var model: AppModel

    var onAir: Bool {
        model.lowerThirdPhase == "on-air" || model.lowerThirdPhase == "building-in"
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                HStack(alignment: .top, spacing: 12) {
                    liveKeyer
                    brandKit
                }
                browserOverlays
                captions
                Spacer(minLength: 0)
            }
            .padding(.vertical, 4)
        }
    }

    // ── Live keyer ───────────────────────────────────────────────────────────

    var liveKeyer: some View {
        card("Live keyer") {
            // Keyer state first: an operator needs to know what is on air before
            // they touch anything.
            HStack(spacing: 8) {
                Circle().fill(onAir ? Studio.red : Studio.textDim)
                    .frame(width: 8, height: 8)
                Text(onAir ? "Key on air" : "Key off — no source keyed")
                    .font(.grotesk(12, .medium))
                    .foregroundStyle(onAir ? Studio.textPrimary : Studio.secondary)
                Spacer()
                Text("\(model.lowerThirdOnAirCount) on air")
                    .font(.plexMono(10)).foregroundStyle(Studio.textDim)
            }
            Text("The keyed name stays locked while that source remains on "
                 + "program. A source change builds the old name out once, then "
                 + "builds the new name in.")
                .font(.grotesk(11)).foregroundStyle(Studio.textDim)

            labeled("Name") {
                TextField("Name", text: $model.lowerThirdName)
                    .textFieldStyle(StudioFieldStyle()).frame(width: 240)
            }
            labeled("Title") {
                TextField("Title", text: $model.lowerThirdTitle)
                    .textFieldStyle(StudioFieldStyle()).frame(width: 240)
            }
            labeled("Position") {
                Picker("", selection: $model.lowerThirdPosition) {
                    Text("Lower left").tag("lower-left")
                    Text("Lower right").tag("lower-right")
                    Text("Center").tag("center")
                }.labelsHidden().frame(width: 170)
            }
            labeled("Style") {
                Picker("", selection: Binding(
                    get: { model.overlays.lowerThirdStyle },
                    set: { model.overlays.lowerThirdStyle = $0; model.applyBrandKit() })) {
                    ForEach(AppModel.lowerThirdStyles, id: \.self) { Text($0).tag($0) }
                }.labelsHidden().frame(width: 170)
            }

            // Build timing — previously hardcoded at 250/200ms with no way to
            // change it. The core clamps to 50-2000, so the stepper matches.
            labeled("Build in") { msField($model.overlays.buildInMs) }
            labeled("Build out") { msField($model.overlays.buildOutMs) }
            labeled("Motion timing") {
                HStack(spacing: 6) {
                    Picker("", selection: Binding(
                        get: { MotionTiming(rawValue: model.overlays.motionTiming) ?? .custom },
                        set: { model.applyMotionTiming($0) })) {
                        ForEach(MotionTiming.allCases, id: \.self) { Text($0.label).tag($0) }
                    }.labelsHidden().frame(width: 130)
                    Text("in \(model.overlays.buildInMs) ms · out \(model.overlays.buildOutMs) ms")
                        .font(.plexMono(10)).foregroundStyle(Studio.textDim)
                }
            }

            HStack(spacing: 8) {
                Button(onAir ? "Key out" : "Key in") {
                    onAir ? model.hideLowerThird() : model.showLowerThird()
                }
                .buttonStyle(AccentButtonStyle())
                .disabled(!onAir && model.lowerThirdName.isEmpty)
                Button("Rebuild") {
                    // Build out then back in, so a style or timing change is
                    // visible without the operator toggling twice.
                    model.rebuildLowerThird()
                }
                .buttonStyle(GhostButtonStyle())
                .disabled(!onAir)
                Spacer()
            }
        }
    }

    func msField(_ value: Binding<Int>) -> some View {
        HStack(spacing: 6) {
            Stepper(value: value, in: 50...2000, step: 50) {
                Text("\(value.wrappedValue) ms").font(.plexMono(11))
            }
            .frame(width: 150)
            // Any manual edit means the preset no longer describes the numbers.
            .onChange(of: value.wrappedValue) { _ in
                model.overlays.motionTiming = MotionTiming.custom.rawValue
            }
        }
    }

    // ── Brand kit ────────────────────────────────────────────────────────────

    var brandKit: some View {
        card("Brand kit") {
            labeled("Kit name") {
                TextField("CoreVideo", text: Binding(
                    get: { model.overlays.brandName },
                    set: { model.overlays.brandName = $0; model.applyBrandKit() }))
                    .textFieldStyle(StudioFieldStyle()).frame(width: 200)
            }
            labeled("Logo text") {
                TextField("CoreVideo", text: Binding(
                    get: { model.overlays.logoText },
                    set: { model.overlays.logoText = $0; model.applyBrandKit() }))
                    .textFieldStyle(StudioFieldStyle()).frame(width: 200)
            }
            labeled("Logo asset") {
                Text(model.logoBug?.name ?? "No logo asset selected")
                    .font(.grotesk(11))
                    .foregroundStyle(model.logoBug == nil ? Studio.textDim : Studio.textPrimary)
            }
            labeled("Default overlay") {
                Picker("", selection: Binding(
                    get: { model.overlays.defaultOverlayBehavior },
                    set: { model.overlays.defaultOverlayBehavior = $0; model.applyBrandKit() })) {
                    ForEach(AppModel.defaultOverlayBehaviors, id: \.self) { Text($0).tag($0) }
                }.labelsHidden().frame(width: 190)
            }
            colorRow("Primary", Binding(
                get: { model.overlays.brandColor },
                set: { model.overlays.brandColor = $0; model.applyBrandKit() }))
            colorRow("Accent", Binding(
                get: { model.overlays.accentColor },
                set: { model.overlays.accentColor = $0; model.applyBrandKit() }))
            colorRow("Background", Binding(
                get: { model.overlays.backgroundColor },
                set: { model.overlays.backgroundColor = $0; model.applyBrandKit() }))
        }
    }

    func colorRow(_ label: String, _ hex: Binding<String>) -> some View {
        labeled(label) {
            HStack(spacing: 6) {
                RoundedRectangle(cornerRadius: 4)
                    .fill(Color(brandHex: hex.wrappedValue) ?? Studio.surface)
                    .frame(width: 22, height: 22)
                    .overlay(RoundedRectangle(cornerRadius: 4)
                        .stroke(Studio.border, lineWidth: 1))
                TextField("#000000", text: hex)
                    .textFieldStyle(StudioFieldStyle()).frame(width: 110)
                ColorPicker("", selection: Binding(
                    get: { Color(brandHex: hex.wrappedValue) ?? Studio.accent },
                    set: { hex.wrappedValue = $0.brandHexString(fallback: hex.wrappedValue) }),
                    supportsOpacity: false)
                    .labelsHidden()
            }
        }
    }

    // ── Browser overlays (honest unavailable state) ──────────────────────────

    var browserOverlays: some View {
        card("Browser overlays (DSK)") {
            Text("Unavailable on macOS — browser sources run in a WebView2 host "
                 + "process that exists only on Windows. A macOS host (WKWebView) "
                 + "is core work, not a shell setting.")
                .font(.grotesk(11)).foregroundStyle(Studio.textDim)
        }
    }

    // ── Captions ─────────────────────────────────────────────────────────────

    var captions: some View {
        card("Captions") {
            Toggle("Render caption cues on program", isOn: Binding(
                get: { model.overlays.captionsEnabled },
                set: { model.setCaptionsEnabled($0) }))
                .font(.grotesk(12))
            labeled("Caption style") {
                Picker("", selection: Binding(
                    get: { model.overlays.captionStyle },
                    set: { model.overlays.captionStyle = $0; model.applyBrandKit() })) {
                    Text("Standard").tag("standard")
                    Text("Boxed").tag("boxed")
                    Text("Minimal").tag("minimal")
                }.labelsHidden().frame(width: 170)
            }
            // Honest about the whole feature: the core RENDERS cues a shell
            // pushes, but nothing in this product generates them. Saying so
            // beats an enabled-looking control that can never show text.
            Text("The core renders caption cues, but this build has no "
                 + "transcription source — no captions will appear until one "
                 + "pushes cues.")
                .font(.grotesk(11)).foregroundStyle(Studio.amber)
        }
    }

    // ── chrome ───────────────────────────────────────────────────────────────

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
                .frame(width: 110, alignment: .leading)
            content()
            Spacer()
        }
    }
}
