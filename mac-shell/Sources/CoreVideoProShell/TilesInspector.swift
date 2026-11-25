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
    let defaultValue: Double
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
            .onTapGesture(count: 2) { set(defaultValue) }
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

struct TilesStyleTab: View {
    var body: some View { TilesHelp("Style controls arrive with Task 17.") }
}

struct TilesMembersTab: View {
    var body: some View { TilesHelp("Members controls arrive with Task 19.") }
}
