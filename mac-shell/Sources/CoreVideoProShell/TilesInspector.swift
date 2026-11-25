// The CoreVideo Tiles workspace (mac Tiles S4/S5): the Windows Scene Builder's
// centre and inspector columns (native-shell/CoreVideoPro.WinUI/Views/SourcesPage.xaml,
// grid `228 | * | 292`; the left column arrives with S6, owner decision D4). With
// the wall selected, the right dock is a Pivot with Layout | Style | Members
// (Transform stays in S6, D7); each tab is its own ScrollViewer, and the footer
// reads "Changes affect this Preview draft."

import SwiftUI

enum TilesInspectorTab: String, CaseIterable {
    case layout = "Layout", style = "Style", members = "Members"
}

/// Windows column spacing and dock width (SourcesPage.xaml ColumnSpacing="12", 292).
enum TilesWorkspaceMetrics {
    static let dockWidth: CGFloat = 292
    static let columnSpacing: CGFloat = 12
}

struct TilesWorkspace: View {
    @EnvironmentObject var model: AppModel

    var sceneId: String { model.tilesEditTargetId ?? "" }
    var scene: SceneDef? { model.scenes.first { $0.id == sceneId } }

    var body: some View {
        HStack(alignment: .top, spacing: TilesWorkspaceMetrics.columnSpacing) {
            centre
            TilesInspectorDock()
                .frame(width: TilesWorkspaceMetrics.dockWidth)
                .frame(maxHeight: .infinity, alignment: .top)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    var centre: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(scene?.name ?? "").font(.grotesk(18, .semibold)).foregroundStyle(Studio.textPrimary)
            HStack(spacing: 8) {
                SceneChip(label: "PVW", color: Studio.accent)
                Text("Changes ride Preview until Take.").font(.grotesk(12)).foregroundStyle(Studio.secondary)
            }
            ZStack {
                Rectangle().fill(Studio.background)
                if model.previewSurfaceId != 0 {
                    SurfaceView(surfaceId: model.previewSurfaceId)
                } else {
                    Text("waiting for preview pixels").font(.plexMono(10)).foregroundStyle(Studio.textDim)
                }
            }
            .aspectRatio(16.0 / 9.0, contentMode: .fit)
            .frame(maxWidth: .infinity)
            .overlay(Rectangle().stroke(Studio.border, lineWidth: 1))
            HStack(spacing: 8) {
                Circle().fill(Studio.red).frame(width: 7, height: 7)
                Text("PROGRAM · Unchanged by Preview edits").font(.plexMono(10, .bold)).foregroundStyle(Studio.secondary)
                Text(model.scenes.first { $0.id == model.programSceneId }?.name ?? "—")
                    .font(.grotesk(12, .medium)).foregroundStyle(Studio.textPrimary).lineLimit(1)
                Spacer()
                Button("Take to Program →") { Task { await model.take() } }
                    .buttonStyle(AccentButtonStyle())
                    .disabled(!model.canTake)
            }
            .padding(10)
            .background(RoundedRectangle(cornerRadius: 8).fill(Studio.card))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
            if model.studioCommandStatus.isEmpty == false {
                Text(model.studioCommandStatus).font(.plexMono(10)).foregroundStyle(Studio.secondary).lineLimit(2)
            }
            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, alignment: .topLeading)
    }
}

struct TilesInspectorDock: View {
    @EnvironmentObject var model: AppModel

    var scene: SceneDef? { model.scenes.first { $0.id == model.tilesEditTargetId } }

    var counts: (selected: Int, available: Int) {
        let members = model.tilesEditTargetId.flatMap { model.tilesPayload(for: $0)?["members"] as? [String] } ?? []
        return (members.filter { !$0.isEmpty }.count, model.tilesMemberChoices.count)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(scene?.name ?? "").font(.grotesk(15, .semibold)).foregroundStyle(Studio.textPrimary)
                .lineLimit(1).truncationMode(.tail)
            Text("Tiles wall · \(counts.selected) selected / \(counts.available) available")
                .font(.grotesk(10)).foregroundStyle(Studio.accent)
            Picker("", selection: $model.tilesInspectorTab) {
                ForEach(TilesInspectorTab.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            // Each tab scrolls on its own (Windows: each PivotItem is a ScrollViewer);
            // the page itself never scrolls while Tiles is being edited.
            ScrollView(.vertical) {
                VStack(alignment: .leading, spacing: 12) {
                    switch model.tilesInspectorTab {
                    case .layout: TilesLayoutTab()
                    case .style: TilesStyleTab()
                    case .members: TilesMembersTab()
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.trailing, 4)
            }
            .frame(maxHeight: .infinity)
            .id(model.tilesInspectorTab)   // each tab keeps its own scroll position (Windows: one ScrollViewer per PivotItem)
            Text("Changes affect this Preview draft.").font(.grotesk(10)).foregroundStyle(Studio.textDim)
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 9).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 9).stroke(Studio.border, lineWidth: 1))
    }
}

/// A section title (plexMono 10, caps), its controls, and help text.
struct TilesSection<Content: View>: View {
    let title: String
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title.uppercased()).font(.plexMono(10, .bold)).foregroundStyle(Studio.secondary)
            content
        }
    }
}

struct TilesHelp: View {
    let text: String
    init(_ text: String) { self.text = text }
    var body: some View {
        Text(text).font(.grotesk(10)).foregroundStyle(Studio.textDim).fixedSize(horizontal: false, vertical: true)
    }
}

/// Windows `SliderValueControl Compact`: header label left, value + unit right
/// (plexMono), a slider below; double-click resets to the Windows default.
struct TilesSliderRow: View {
    let label: String
    let value: Double
    let range: ClosedRange<Double>
    var step: Double = 0
    var unit: String = ""
    var format: (Double) -> String = { String(format: "%g", $0) }
    /// Windows `DefaultValue`; nil where Windows sets none (double-click does nothing).
    let defaultValue: Double?
    var enabled = true
    let set: (Double) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(label).font(.grotesk(11)).foregroundStyle(Studio.textPrimary)
                Spacer()
                Text(format(value) + (unit.isEmpty ? "" : " " + unit)).font(.plexMono(10)).foregroundStyle(Studio.secondary)
            }
            Group {
                if step > 0 {
                    Slider(value: Binding(get: { value }, set: set), in: range, step: step)
                } else {
                    Slider(value: Binding(get: { value }, set: set), in: range)
                }
            }
            .controlSize(.small)
            .tint(Studio.accent)
            .onTapGesture(count: 2) { if let defaultValue { set(defaultValue) } }
        }
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.5)
    }
}

// Tab bodies: filled by Tasks 16 (Layout), 17 (Style) and 19 (Members).
/// The settings the dock edits (the Preview wall), or the defaults.
extension AppModel {
    var tilesEditSettings: TilesSettings {
        tilesEditTargetId.flatMap { id in scenes.first { $0.id == id }?.tiles } ?? TilesSettings()
    }
}

/// Windows SourcesPage.xaml Layout PivotItem, control for control.
struct TilesLayoutTab: View {
    @EnvironmentObject var model: AppModel
    @State private var advanced = false

    var s: TilesSettings { model.tilesEditSettings }
    var enabled: Bool { model.tilesEditTargetId != nil }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if !enabled { TilesHelp("Queue this scene in Preview to edit it.") }
            TilesSection(title: "Automatic layout") {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Tile aspect ratio").font(.grotesk(11)).foregroundStyle(Studio.textPrimary)
                    Picker("", selection: Binding(get: { s.tileAspect }, set: { try? model.setTilesAspect($0) })) {
                        ForEach(AppModel.tilesAspectOptions, id: \.value) { Text($0.label).tag($0.value) }
                    }
                    .labelsHidden().controlSize(.small)
                }
                if s.tileAspect == "custom" {
                    TilesSliderRow(label: "Custom aspect ratio", value: s.customAspectRatio, range: 0.25...4, step: 0.01,
                                   format: { String(format: "%.2f", $0) }, defaultValue: 16.0 / 9.0, enabled: enabled) {
                        try? model.setTilesCustomAspect($0)
                    }
                }
                TilesSliderRow(label: "Tile spacing", value: s.gutterPercent, range: 0...10, step: 0.01, unit: "%",
                               format: TilesFormat.threePlaces, defaultValue: 0.741, enabled: enabled) { try? model.setTilesGutter($0) }
                TilesSliderRow(label: "Outer margin", value: s.marginPercent, range: 0...20, step: 0.01, unit: "%",
                               format: TilesFormat.threePlaces, defaultValue: 0.741, enabled: enabled) { try? model.setTilesMargin($0) }
                TilesHelp("Spacing and margin are percentages of wall width. Tiles keep their aspect ratio as members join or leave.")
            }
            Divider().overlay(Studio.border)
            TilesSection(title: "Wall capacity") {
                TilesSliderRow(label: "Maximum visible tiles", value: Double(s.maxTiles), range: 1...64, step: 1, unit: "tiles",
                               format: { String(Int($0)) }, defaultValue: 16, enabled: enabled) { try? model.setTilesMaxTiles($0) }
                TilesHelp("The maximum is a ceiling, not empty placeholders.")
            }
            DisclosureGroup(isExpanded: $advanced) {
                VStack(alignment: .leading, spacing: 10) {
                    Toggle("Animate roster changes", isOn: Binding(get: { s.animateLayout }, set: { try? model.setTilesAnimateLayout($0) }))
                        .font(.grotesk(11)).toggleStyle(.switch).controlSize(.small).tint(Studio.accent)
                    TilesSliderRow(label: "Animation duration", value: Double(s.animationDurationMs), range: 100...2000, step: 1, unit: "ms",
                                   format: { String(Int($0)) }, defaultValue: 350, enabled: enabled) { try? model.setTilesAnimationDuration($0) }
                }
                .padding(.top, 6)
            } label: {
                Text("Advanced layout").font(.grotesk(11, .medium)).foregroundStyle(Studio.textPrimary)
            }
        }
        .disabled(!enabled)
    }
}

enum TilesFormat {
    /// Windows `Format="0.###"`.
    static let threePlaces: (Double) -> String = { v in
        let f = NumberFormatter(); f.minimumFractionDigits = 0; f.maximumFractionDigits = 3; f.minimumIntegerDigits = 1
        return f.string(from: NSNumber(value: v)) ?? String(v)
    }
}

/// Windows SourcesPage.xaml Style PivotItem, control for control.
struct TilesStyleTab: View {
    @EnvironmentObject var model: AppModel
    @State private var liveBackground = false
    @State private var advancedGlow = false

    var s: TilesSettings { model.tilesEditSettings }
    var enabled: Bool { model.tilesEditTargetId != nil }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if !enabled { TilesHelp("Queue this scene in Preview to edit it.") }
            TilesSection(title: "Quick looks") {
                HStack(spacing: 4) {
                    ForEach(TilesStylePreset.allCases, id: \.self) { preset in
                        // Three equal columns (Windows Grid `* * *`); one line each.
                        Button { try? model.applyTilesPreset(preset) } label: {
                            Text(preset.rawValue.capitalized).lineLimit(1).minimumScaleFactor(0.8).frame(maxWidth: .infinity)
                        }
                        .buttonStyle(GhostButtonStyle())
                    }
                }
                TilesHelp("Presets change styling and spacing. Member crops and inclusion stay intact.")
                // Mac only (owner decision D5): the logo bug rides no Tiles scene.
                TilesHelp("The logo bug is not drawn over a Tiles wall on Mac yet.")
            }
            Divider().overlay(Studio.border)
            TilesSection(title: "Tile edges") {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Tile shape").font(.grotesk(11)).foregroundStyle(Studio.textPrimary)
                    Picker("", selection: Binding(get: { s.borderShape }, set: { try? model.setTilesBorderShape($0) })) {
                        Text("Square").tag("square"); Text("Rounded").tag("rounded")
                    }
                    .labelsHidden().controlSize(.small)
                }
                if s.borderShape == "rounded" {
                    TilesSliderRow(label: "Corner radius", value: s.cornerRadius, range: 0...100, step: 1, unit: "px",
                                   format: { String(Int($0)) }, defaultValue: 16, enabled: enabled) { try? model.setTilesCornerRadius($0) }
                }
                TilesSliderRow(label: "Border thickness", value: s.borderThickness, range: 0...32, step: 0.01, unit: "px",
                               format: TilesFormat.threePlaces, defaultValue: 0, enabled: enabled) { try? model.setTilesBorderThickness($0) }
                TilesColorField(label: "Border color", role: .border)
            }
            Divider().overlay(Studio.border)
            TilesSection(title: "Wall background") {
                Toggle("Transparent wall background", isOn: Binding(get: { s.transparentBackground },
                                                                    set: { try? model.setTilesTransparentBackground($0) }))
                    .font(.grotesk(11)).toggleStyle(.checkbox)
                if !s.transparentBackground { TilesColorField(label: "Background color", role: .background) }
                DisclosureGroup(isExpanded: $liveBackground) {
                    TilesLiveBackground().padding(.top, 6)
                } label: {
                    Text("Live background").font(.grotesk(11, .medium)).foregroundStyle(Studio.textPrimary)
                }
                TilesSliderRow(label: "Wall opacity", value: s.wallOpacity * 100, range: 0...100, step: 0.01, unit: "%",
                               format: { String(format: "%.0f", $0) }, defaultValue: nil, enabled: enabled) {
                    try? model.setTilesWallOpacityPercent($0)
                }
            }
            Divider().overlay(Studio.border)
            TilesSection(title: "Glow") {
                TilesSliderRow(label: "Glow size", value: s.glowSize, range: 0...64, step: 0.01, unit: "px",
                               format: TilesFormat.threePlaces, defaultValue: 0, enabled: enabled) { try? model.setTilesGlowSize($0) }
                TilesColorField(label: "Glow color", role: .glow)
                DisclosureGroup(isExpanded: $advancedGlow) {
                    VStack(alignment: .leading, spacing: 10) {
                        TilesSliderRow(label: "Glow intensity", value: s.glowIntensity, range: 0...100, step: 0.01, unit: "%",
                                       format: TilesFormat.threePlaces, defaultValue: 100, enabled: enabled) { try? model.setTilesGlowIntensity($0) }
                        TilesSliderRow(label: "Glow softness", value: s.glowSoftness, range: 0...100, step: 0.01, unit: "%",
                                       format: TilesFormat.threePlaces, defaultValue: 0, enabled: enabled) { try? model.setTilesGlowSoftness($0) }
                    }
                    .padding(.top, 6)
                } label: {
                    Text("Advanced glow").font(.grotesk(11, .medium)).foregroundStyle(Studio.textPrimary)
                }
            }
            Button("Reset wall style") { try? model.resetTilesStyle() }.buttonStyle(GhostButtonStyle())
        }
        .disabled(!enabled)
    }
}

/// Filled by Task 20 (Live background).
struct TilesLiveBackground: View {
    var body: some View { TilesHelp("Arrives with the Members tab.") }
}

/// Windows grid `Auto | *`: a 36 pt colour well beside a hex field. The hex
/// string is the only source of truth: the well reads it through `TilesHex.parse`
/// (with the role's fallback) and writes `TilesHex.format`; the field commits
/// through the Windows normalizer on Return AND on focus loss (Windows TwoWay
/// TextBox commits on LostFocus). The well is inline: on macOS a ColorPicker
/// drives the shared colour panel, which a transient popover would orphan.
struct TilesColorField: View {
    @EnvironmentObject var model: AppModel
    let label: String
    let role: TilesColorRole
    @State private var draft = ""
    @FocusState private var editing: Bool

    var pickerColor: Color {
        let c = model.tilesPickerColor(role)
        return Color(red: Double(c.r) / 255, green: Double(c.g) / 255, blue: Double(c.b) / 255)
    }

    func commit() {
        guard draft != model.tilesColor(role) else { return }
        try? model.setTilesColor(role, hex: draft)
        draft = model.tilesColor(role)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(label).font(.grotesk(11)).foregroundStyle(Studio.textPrimary)
            HStack(spacing: 8) {
                ColorPicker(label, selection: Binding(get: { pickerColor }, set: { color in
                    let rgb = NSColor(color).usingColorSpace(.sRGB) ?? .black
                    func byte(_ v: CGFloat) -> UInt8 { UInt8(max(0, min(255, (v * 255).rounded()))) }
                    try? model.setTilesPickerColor(role, r: byte(rgb.redComponent), g: byte(rgb.greenComponent), b: byte(rgb.blueComponent))
                }), supportsOpacity: false)
                .labelsHidden()
                .fixedSize()   // the native well's own size (Windows: a 36 pt swatch button)
                .help("Choose \(label.lowercased())")
                TextField("#RRGGBB", text: $draft)
                    .textFieldStyle(StudioFieldStyle())
                    .font(.plexMono(11))
                    .focused($editing)
                    .onSubmit(commit)
                    .onChange(of: editing) { focused in if !focused { commit() } }
            }
        }
        .onAppear { draft = model.tilesColor(role) }
        .onChange(of: model.tilesColor(role)) { value in if !editing { draft = value } }
    }
}

/// Windows SourcesPage.xaml Members PivotItem, control for control.
struct TilesMembersTab: View {
    @EnvironmentObject var model: AppModel
    @State private var advancedPlacement = false
    @State private var advancedMembership = false
    @State private var manualSlot = 1.0

    var s: TilesSettings { model.tilesEditSettings }
    var canEdit: Bool { model.tilesCanEditSelectedMember }
    var override: TilesMemberOverride? { model.tilesSelectedOverride }

    static let modes: [(value: String, label: String)] = [
        ("routed", "Routed show sources only"),
        ("eligible", "All eligible: meeting participants + in-show local sources"),
        ("manual", "Manual slots only"),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            TilesSection(title: "Who appears") {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Membership mode").font(.grotesk(11)).foregroundStyle(Studio.textPrimary)
                    Picker("", selection: Binding(get: { TilesMembershipPolicy.normalizeMode(s.membershipMode) },
                                                  set: { try? model.setTilesMembershipMode($0) })) {
                        ForEach(Self.modes, id: \.value) { Text($0.label).tag($0.value) }
                    }
                    .labelsHidden().controlSize(.small)
                }
                TilesHelp("Routed follows show assignments. All eligible follows the meeting. Manual lets you choose individual feeds. Never show excludes a member from every mode.")
                ScrollView(.vertical) {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(model.tilesMemberRows) { row in
                            HStack(spacing: 6) {
                                Toggle("", isOn: Binding(get: { row.included }, set: { try? model.includeTilesMember(row.id, included: $0) }))
                                    .toggleStyle(.checkbox).labelsHidden().disabled(!row.canInclude)
                                Text(row.label).font(.grotesk(11)).foregroundStyle(Studio.textPrimary).lineLimit(1).truncationMode(.tail)
                                Spacer(minLength: 4)
                                Text(row.status).font(.plexMono(9)).foregroundStyle(Studio.secondary).lineLimit(1)
                            }
                            .padding(.vertical, 3).padding(.horizontal, 4)
                            .background(RoundedRectangle(cornerRadius: 4)
                                .fill(row.id == model.tilesSelectedMemberId ? Studio.accent.opacity(0.12) : Studio.field))
                            .contentShape(Rectangle())
                            .onTapGesture { model.tilesSelectedMemberId = row.id }
                        }
                    }
                }
                .frame(maxHeight: 220)
            }
            Divider().overlay(Studio.border)
            TilesSection(title: model.tilesFramingTitle) {
                Toggle("Never show this member", isOn: Binding(
                    get: { model.tilesMemberStates[model.tilesSelectedMemberId]?.excluded ?? false },
                    set: { model.setTilesNeverShow($0) }))
                    .font(.grotesk(11)).toggleStyle(.checkbox)
                TilesSliderRow(label: "Crop left", value: override?.cropLeftPercent ?? 0, range: 0...45, step: 0.01, unit: "%",
                               format: TilesFormat.threePlaces, defaultValue: nil, enabled: canEdit) { model.setTilesCrop(left: $0) }
                TilesSliderRow(label: "Crop right", value: override?.cropRightPercent ?? 0, range: 0...45, step: 0.01, unit: "%",
                               format: TilesFormat.threePlaces, defaultValue: nil, enabled: canEdit) { model.setTilesCrop(right: $0) }
                Button("Reset this tile crop") { model.resetTilesCrop() }.buttonStyle(GhostButtonStyle())
            }
            .disabled(!canEdit)
            Divider().overlay(Studio.border)
            TilesSection(title: "Tile placement") {
                Toggle("Pin this member in wall", isOn: Binding(get: { model.tilesSelectedPinned }, set: { model.setTilesPinned($0) }))
                    .font(.grotesk(11)).toggleStyle(.checkbox)
                if model.tilesSelectedPinned, let rect = override?.rect {
                    let wall = model.tilesWallPixels
                    TilesSliderRow(label: "Tile X", value: rect.x * wall.width, range: 0...wall.width, step: 1, unit: "px",
                                   format: { String(Int($0)) }, defaultValue: nil, enabled: canEdit) { model.setTilesPin(field: .x, pixels: $0) }
                    TilesSliderRow(label: "Tile Y", value: rect.y * wall.height, range: 0...wall.height, step: 1, unit: "px",
                                   format: { String(Int($0)) }, defaultValue: nil, enabled: canEdit) { model.setTilesPin(field: .y, pixels: $0) }
                    TilesSliderRow(label: "Tile width", value: rect.width * wall.width, range: 1...wall.width, step: 1, unit: "px",
                                   format: { String(Int($0)) }, defaultValue: nil, enabled: canEdit) { model.setTilesPin(field: .width, pixels: $0) }
                    TilesSliderRow(label: "Tile height", value: rect.height * wall.height, range: 1...wall.height, step: 1, unit: "px",
                                   format: { String(Int($0)) }, defaultValue: nil, enabled: canEdit) { model.setTilesPin(field: .height, pixels: $0) }
                    Button("Return this tile to automatic") { model.setTilesPinned(false) }.buttonStyle(GhostButtonStyle())
                    DisclosureGroup(isExpanded: $advancedPlacement) {
                        TilesSliderRow(label: "Tile order", value: Double(override?.z ?? 0), range: 0...63, step: 1,
                                       format: { String(Int($0)) }, defaultValue: nil, enabled: canEdit) { model.setTilesOrder($0) }
                            .padding(.top, 6)
                    } label: {
                        Text("Advanced placement").font(.grotesk(11, .medium)).foregroundStyle(Studio.textPrimary)
                    }
                }
                TilesHelp("Pixels are relative to this wall. Automatic tiles reflow in the remaining space. Unpinning keeps this member’s crop.")
            }
            .disabled(!canEdit)
            Divider().overlay(Studio.border)
            TilesSection(title: "Names") {
                // Owner decision D3: disabled on Mac in Phase 3; the saved value is untouched.
                Toggle("Name each tile", isOn: .constant(s.nameEachTile))
                    .font(.grotesk(11)).toggleStyle(.switch).controlSize(.small).disabled(true)
                TilesHelp("Not on Mac yet: names need the Metal name-line check (Phase 4).")
                TilesHelp("One line per tile in the scene’s lower-third style: the source’s name from Sources, top line only. Zoom participants and capture devices are named; media and browser members are not yet. Names scale with their tiles; tiles too small for a readable line stay unlabelled.")
            }
            DisclosureGroup(isExpanded: $advancedMembership) {
                VStack(alignment: .leading, spacing: 8) {
                    TilesSliderRow(label: "Manual slot", value: manualSlot, range: 1...64, step: 1, unit: "slot",
                                   format: { String(Int($0)) }, defaultValue: nil) { manualSlot = $0 }
                    HStack(spacing: 6) {
                        Button("Assign selected member to slot") {
                            try? model.assignTilesSlot(Int(manualSlot), sourceId: model.tilesSelectedMemberId)
                        }
                        .buttonStyle(GhostButtonStyle())
                        Button("Clear slot") { try? model.assignTilesSlot(Int(manualSlot), sourceId: "") }
                            .buttonStyle(GhostButtonStyle())
                    }
                    TilesHelp(model.tilesMembershipSummary)
                    Button("Clear stale memberships") { try? model.clearStaleTiles() }.buttonStyle(GhostButtonStyle())
                }
                .padding(.top, 6)
            } label: {
                Text("Advanced membership").font(.grotesk(11, .medium)).foregroundStyle(Studio.textPrimary)
            }
        }
        .onAppear {
            // Windows: the selection falls back to the first member when it is gone.
            if !model.tilesMemberRows.contains(where: { $0.id == model.tilesSelectedMemberId }) {
                model.tilesSelectedMemberId = model.tilesMemberRows.first?.id ?? ""
            }
        }
    }
}
