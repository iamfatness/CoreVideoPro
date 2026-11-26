// GraphicsPlaylistPane — Operate's LEFT column (spec 5.4; Windows
// GraphicsPlayoutControl :58-80): the PLAYLIST header, the SHOT LAYOUTS band,
// the row cards in their own scroll view, and the document status line. The
// inspector is the right column (GraphicsInspector.swift), never inside a card.
//
// Cards follow Windows RowCard (:422-468): hotkey badge, name over caption, then
// the dot and Cue / Show / Hide for a single-line row, or a count for a Guest or
// names-list row whose lines sit below, indented 46 pt (GuestEntry :470-490).
// Line state follows GraphicsRowState: idle (Cue, Show), cued (Show, Clear cue),
// on Program (Hide), preparing (disabled, the core's text), failed (the core's
// warning). Lists are keyed by row id and instance id (spec 5.10).

import SwiftUI

struct GraphicsPlaylistPane: View {
    @ObservedObject var graphics: GraphicsModel
    @State private var socialOpen = false

    var body: some View {
        let lines = graphics.lines()
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Text("PLAYLIST").font(.plexMono(10, .semibold)).tracking(1.6).foregroundStyle(Studio.secondary)
                Spacer()
                // Windows GraphicsPlayoutControl: "Social review…" before the add button,
                // Graphics -> Operate only; disabled while the sheet is open.
                Button("Social review…") { socialOpen = true }
                    .buttonStyle(GhostButtonStyle())
                    .disabled(socialOpen)
                    .help("Review YouTube or Twitch chat; approve one message, then Show approved")
                    .sheet(isPresented: $socialOpen) {
                        SocialReviewSheet(model: graphics.socialReview) { socialOpen = false }
                    }
                Button("Add default rows") { graphics.addDefaultRows() }
                    .buttonStyle(GhostButtonStyle())
                    .disabled(graphics.documentReadOnly || !graphics.available)
                    .help("Add Windows' interview rows: Guest name, Show title, Logo, LIVE bug, Sponsor card")
            }
            GraphicsShotLayoutsBand(graphics: graphics)
            ScrollView(.vertical) {
                LazyVStack(alignment: .leading, spacing: 8) {
                    if graphics.rows.isEmpty {
                        Text(graphics.documentLoaded ? "No playlist rows yet." : "Loading the graphics playlist…")
                            .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                    }
                    ForEach(graphics.rows) { row in
                        GraphicsRowCard(graphics: graphics, row: row, lines: lines.filter { $0.row.id == row.id })
                    }
                }
                .padding(.trailing, 2)
            }
            .frame(maxHeight: .infinity)
            if !graphics.documentStatus.isEmpty {
                Text(graphics.documentStatus).font(.grotesk(11))
                    .foregroundStyle(graphics.documentReadOnly || graphics.documentStatus.hasPrefix("Graphics playlist not saved") ? Studio.amber : Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Graphics playlist")
    }
}


struct GraphicsRowCard: View {
    @ObservedObject var graphics: GraphicsModel
    let row: GraphicsRow
    let lines: [GraphicsRowLine]

    var multi: Bool { row.kind == GraphicsRow.guest || row.hasVariants }

    var body: some View {
        let selected = graphics.isCardSelected(row)
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 10) {
                GraphicsHotkeyBadge(key: row.hotkey)
                VStack(alignment: .leading, spacing: 1) {
                    Text(row.name).font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary).lineLimit(1)
                    Text(GraphicsModel.caption(row) + (row.playOutAfterMs > 0 ? " · out after \(row.playOutAfterMs / 1000) s" : ""))
                        .font(.grotesk(10)).foregroundStyle(Studio.secondary).lineLimit(1)
                }
                Spacer(minLength: 8)
                if multi {
                    Text(countText).font(.grotesk(10)).foregroundStyle(Studio.secondary)
                } else if let line = lines.first {
                    GraphicsLineControls(graphics: graphics, line: line)
                }
            }
            if GraphicsModel.showsClockTransport(row) { GraphicsClockTransport(graphics: graphics, row: row) }
            if multi {
                if lines.isEmpty && row.kind == GraphicsRow.guest {
                    Text("No sources in the show yet. Assign a source on the Sources page and it appears here.")
                        .font(.grotesk(11)).foregroundStyle(Studio.secondary).padding(.leading, 46)
                        .fixedSize(horizontal: false, vertical: true)
                }
                ForEach(lines) { line in GraphicsEntryLine(graphics: graphics, line: line) }
            }
        }
        .padding(.horizontal, 10).padding(.vertical, 8)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(selected ? Studio.accent : Studio.border, lineWidth: 1))
        .contentShape(Rectangle())
        .onTapGesture { graphics.select(row.id) }
    }

    var countText: String {
        let unit = row.hasVariants ? "name" : "guest"
        if lines.isEmpty { return row.hasVariants ? "empty list" : "no sources" }
        return lines.count == 1 ? "1 " + unit : "\(lines.count) \(unit)s"
    }
}

/// A countdown row's transport (Windows :454-465): the readout, then Start / Pause / Resume / Reset,
/// indented 46 pt; each chip is disabled while busy or while the row has no instance.
struct GraphicsClockTransport: View {
    @ObservedObject var graphics: GraphicsModel
    let row: GraphicsRow

    var body: some View {
        let instance = graphics.clockInstance(row)
        HStack(spacing: 6) {
            Text(GraphicsClockReadout.text(row: row, instance: instance)).font(.plexMono(11)).foregroundStyle(Studio.secondary)
            ForEach([("Start", "start"), ("Pause", "pause"), ("Resume", "resume"), ("Reset", "reset")], id: \.1) { label, action in
                Button(label) { graphics.clock(rowId: row.id, action: action) }
                    .buttonStyle(GraphicsChipStyle()).disabled(graphics.busy || instance == nil)
                    .accessibilityLabel("\(label) \(row.name)")
            }
        }
        .padding(.leading, 46)
    }
}

/// One Guest or names-list line (Windows GuestEntry): dot, label over title, actions; tapping selects "rowId|sourceId".
struct GraphicsEntryLine: View {
    @ObservedObject var graphics: GraphicsModel
    let line: GraphicsRowLine

    var body: some View {
        let key = GraphicsModel.selectionKey(rowId: line.row.id, sourceId: line.entry.sourceId)
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 0) {
                Text(line.entry.label.isEmpty ? line.entry.sourceId : line.entry.label)
                    .font(.grotesk(12, .semibold)).foregroundStyle(Studio.textPrimary).lineLimit(1)
                if !line.entry.title.isEmpty {
                    Text(line.entry.title).font(.grotesk(10)).foregroundStyle(Studio.secondary).lineLimit(1)
                }
            }
            Spacer(minLength: 8)
            GraphicsLineControls(graphics: graphics, line: line)
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 6).fill(graphics.selectedKey == key ? Studio.surfaceRaised : Color.clear))
        .padding(.leading, 46)
        .contentShape(Rectangle())
        .onTapGesture { graphics.select(key) }
        .help(line.entry.sourceId)
    }
}

/// The dot, the line's state text, and Cue / Show / Clear cue / Hide (Windows ActionButtons, full form).
struct GraphicsLineControls: View {
    @ObservedObject var graphics: GraphicsModel
    let line: GraphicsRowLine

    var body: some View {
        let busy = graphics.busy
        HStack(spacing: 6) {
            switch line.state {
            case .preparing(let text): Text(text).font(.grotesk(11)).foregroundStyle(Studio.amber)
            case .failed(let warning): Text(warning).font(.grotesk(11)).foregroundStyle(Studio.red).lineLimit(2)
            default: EmptyView()
            }
            if line.row.templateId.isEmpty {
                Text("No graphic yet").font(.grotesk(11)).foregroundStyle(Studio.textDim)
            }
            Circle().fill(dot(line.state)).frame(width: 9, height: 9)
            if line.canHide {
                Button("Hide") { graphics.hide(entry: line.entry) }.buttonStyle(GraphicsChipStyle()).disabled(busy)
                    .accessibilityLabel("Hide \(line.label)")
            } else {
                if line.canClearCue {
                    Button("Show") { graphics.show(line.entry) }.buttonStyle(GraphicsChipStyle()).disabled(busy || !line.canShow)
                        .accessibilityLabel("Show \(line.label)")
                    Button("Clear cue") { graphics.clearCue(line.entry.instanceId) }.buttonStyle(GraphicsChipStyle()).disabled(busy)
                        .accessibilityLabel("Clear cue \(line.label)")
                } else {
                    Button("Cue") { graphics.cue(line.entry) }.buttonStyle(GraphicsChipStyle()).disabled(busy || !line.canCue)
                        .accessibilityLabel("Cue \(line.label)")
                    Button("Show") { graphics.show(line.entry) }.buttonStyle(GraphicsChipStyle(accent: true)).disabled(busy || !line.canShow)
                        .accessibilityLabel("Show \(line.label)")
                }
            }
        }
    }

    func dot(_ state: GraphicsRowState) -> Color {
        switch state {
        case .onProgram: return Studio.red
        case .cued, .preparing: return Studio.amber
        case .failed: return Studio.red
        case .idle: return Studio.textDim
        }
    }
}

struct GraphicsHotkeyBadge: View {
    let key: String
    var body: some View {
        Text(key.isEmpty ? "—" : key).font(.plexMono(10)).foregroundStyle(key.isEmpty ? Studio.textDim : Studio.secondary)
            .frame(minWidth: 30).padding(.horizontal, 5).padding(.vertical, 2)
            .background(RoundedRectangle(cornerRadius: 4).fill(Studio.surface))
    }
}

/// Windows StudioBrushes.ChipButton: a compact bordered button; Show is the accent chip.
struct GraphicsChipStyle: ButtonStyle {
    var accent = false
    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.grotesk(11, .semibold))
            .foregroundStyle(accent ? Studio.onAccent : Studio.textPrimary)
            .padding(.horizontal, 10).padding(.vertical, 4)
            .background(RoundedRectangle(cornerRadius: 6).fill(accent ? Studio.accent.opacity(configuration.isPressed ? 0.85 : 1)
                                                                : (configuration.isPressed ? Studio.surfaceRaised : Studio.surface)))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(accent ? Color.clear : Studio.border, lineWidth: 1))
            .opacity(isEnabled ? 1 : 0.45)
            .contentShape(RoundedRectangle(cornerRadius: 6))
    }
}
