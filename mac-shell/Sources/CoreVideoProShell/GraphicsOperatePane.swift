// GraphicsOperatePane — the Graphics tab when native graphics is available
// (spec 5.4): Operate | Design with the status line beside the switch, the On
// Air strip, then Operate's two columns (G4-G7 Task 1); Design holds "Existing overlay controls"
// (today's pane, unchanged) and the Designer note. When graphics is not
// available the tab is today's pane plus one line saying why.
//
// Windows reference: W/Controls/GraphicsOnAirStripControl.cs (strip: ON AIR
// chips with Hide, CUED chips with Clear cue, Clear all, Clear cued). Chips are
// bound ONLY to GraphicsModel.strip, which is native instance truth: a button
// press never changes a chip, only the next snapshot does.

import SwiftUI

enum GraphicsSurface: String, CaseIterable { case operate = "Operate", design = "Design" }

struct GraphicsTabPane: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var graphics: GraphicsModel
    @State private var surface = GraphicsSurface.operate

    var body: some View {
        if graphics.available {
            // Windows OverlaysPage (:22-48): row 0 = Operate|Design + the status line beside it,
            // row 1 = the On Air strip, row 2 = the surface, filling the rest of the page.
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 12) {
                    Picker("", selection: $surface) {
                        ForEach(GraphicsSurface.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                    }
                    .pickerStyle(.segmented).labelsHidden().frame(width: 220)
                    GraphicsStatusLine(graphics: graphics)
                }
                GraphicsOnAirStrip(graphics: graphics)
                switch surface {
                case .operate: GraphicsOperatePane(graphics: graphics)
                case .design: ScrollView { GraphicsDesignPane() }
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        } else {
            ScrollView {
                VStack(alignment: .leading, spacing: 8) {
                    Text(GraphicsModel.needsNewerCoreText)
                        .font(.grotesk(12, .medium)).foregroundStyle(Studio.amber)
                    OverlaysPane()
                }
            }
        }
    }
}

/// The playout status, one line, truncating (Windows binds GraphicsPlayoutStatus beside the switch).
struct GraphicsStatusLine: View {
    @ObservedObject var graphics: GraphicsModel

    var body: some View {
        HStack(spacing: 8) {
            if graphics.busy { ProgressView().controlSize(.small) }
            Text(graphics.busy && graphics.status.isEmpty ? "Working…" : graphics.status)
                .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                .lineLimit(1).truncationMode(.tail)
            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// Operate: the playlist column and the fixed-width inspector column beside it (Windows
/// GraphicsPlayoutControl :46-93), each scrolling on its own.
struct GraphicsOperatePane: View {
    @ObservedObject var graphics: GraphicsModel

    var body: some View {
        GeometryReader { proxy in
            HStack(alignment: .top, spacing: GraphicsOperateLayout.columnSpacing) {
                ForEach(Array(GraphicsOperateLayout.columns(width: proxy.size.width).enumerated()), id: \.offset) { _, column in
                    switch column {
                    case .list: GraphicsPlaylistPane(graphics: graphics).reportsColumnFrame("list")
                    case .inspector(let width): GraphicsInspectorPanel(graphics: graphics, width: width).reportsColumnFrame("inspector")
                    }
                }
            }
            .frame(width: proxy.size.width, height: proxy.size.height, alignment: .topLeading)
            .coordinateSpace(name: GraphicsColumnFrames.space)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// Where each Operate column landed, in the pane's coordinates (published as a preference; the layout
/// test reads it to prove the inspector renders BESIDE the list at real sizes).
enum GraphicsColumnFrames {
    static let space = "graphics-operate"
    struct Key: PreferenceKey {
        static var defaultValue: [String: CGRect] = [:]
        static func reduce(value: inout [String: CGRect], nextValue: () -> [String: CGRect]) { value.merge(nextValue()) { $1 } }
    }
}

extension View {
    func reportsColumnFrame(_ name: String) -> some View {
        background(GeometryReader { proxy in
            Color.clear.preference(key: GraphicsColumnFrames.Key.self, value: [name: proxy.frame(in: .named(GraphicsColumnFrames.space))])
        })
    }
}

struct GraphicsOnAirStrip: View {
    @ObservedObject var graphics: GraphicsModel

    var body: some View {
        let strip = graphics.strip, busy = graphics.busy
        HStack(alignment: .center, spacing: 12) {
            section("ON AIR", Studio.red, strip.onProgram, empty: "No graphics on air", action: "Hide") { graphics.hide($0) }
            section("CUED", Studio.amber, strip.onPreview, empty: "Nothing staged for the next Take", action: "Clear cue") { graphics.clearCue($0) }
            Button("Clear all") { graphics.clearAll() }
                .buttonStyle(GhostButtonStyle())
                .disabled(busy || strip.onProgram.isEmpty)
                .help("Hide every graphic on Program (F12 with Shift)")
                .accessibilityLabel("Clear all graphics")
            Button("Clear cued") { graphics.clearCued() }
                .buttonStyle(GhostButtonStyle())
                .disabled(busy || strip.onPreview.isEmpty)
                .help("Clear every cued graphic; nothing rides the next Take")
                .accessibilityLabel("Clear cued graphics")
        }
        .padding(.horizontal, 10).padding(.vertical, 6)
        .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
    }

    func section(_ title: String, _ accent: Color, _ items: [GraphicsStripItem], empty: String, action: String,
                 perform: @escaping (String) -> Void) -> some View {
        HStack(spacing: 10) {
            Text(title).font(.plexMono(10, .semibold)).tracking(1.6).foregroundStyle(accent)
            if items.isEmpty {
                Text(empty).font(.grotesk(11)).foregroundStyle(Studio.secondary)
            } else {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        ForEach(items) { item in chip(item, accent, action: action) { perform(item.instanceId) } }
                    }
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    func chip(_ item: GraphicsStripItem, _ accent: Color, action: String, perform: @escaping () -> Void) -> some View {
        HStack(spacing: 8) {
            Circle().fill(accent).frame(width: 8, height: 8)
            Text(item.label).font(.grotesk(12, .semibold)).foregroundStyle(Studio.textPrimary)
                .lineLimit(1).truncationMode(.tail).frame(maxWidth: 180, alignment: .leading)
            if item.owner != "playlist" { badge(item.owner) }
            if item.cuePending { badge("preparing") }
            Button(action, action: perform)
                .buttonStyle(GhostButtonStyle()).controlSize(.small)
                .disabled(graphics.busy)
                .accessibilityLabel("\(action) \(item.label)")
        }
        .padding(.leading, 10).padding(.trailing, 4).padding(.vertical, 3)
        .background(Capsule().fill(Studio.surfaceRaised))
        .overlay(Capsule().stroke(accent, lineWidth: 1))
        .help("\(item.label) · \(item.instanceId) · r\(item.revision) · \(item.visibility)")
    }

    func badge(_ text: String) -> some View {
        Text(text).font(.plexMono(9)).foregroundStyle(Studio.secondary)
            .padding(.horizontal, 5).padding(.vertical, 1)
            .background(RoundedRectangle(cornerRadius: 4).fill(Studio.surface))
    }
}

/// Design: the Designer arrives in a later release (owner decision); until then
/// it holds today's overlay controls in a drawer, as Windows does
/// (W/Views/OverlaysPage.xaml:17,55-58).
struct GraphicsDesignPane: View {
    @State private var drawerOpen = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("The graphics Designer arrives in a later release.")
                .font(.grotesk(12)).foregroundStyle(Studio.secondary)
            DisclosureGroup(isExpanded: $drawerOpen) {
                OverlaysPane().frame(minHeight: 480)
            } label: {
                Text("Existing overlay controls").font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary)
            }
            .accessibilityLabel("Existing overlay controls")
            Spacer(minLength: 0)
        }
    }
}
