// GraphicsPlaylistPane — Operate's playlist and row inspector (spec 5.4).
// Row buttons follow GraphicsRowState: idle (Cue, Show), cued (Show, Clear cue),
// on Program (Hide), preparing (disabled, the core's text), failed (the core's
// warning). A Show never appears without its Hide in the same row: an on-air
// line always shows Hide. Lists are keyed by instance id and row id, never
// rebuilt per snapshot (spec 5.10).

import SwiftUI

struct GraphicsPlaylistPane: View {
    @ObservedObject var graphics: GraphicsModel
    @State private var selectedRowId: String?

    var body: some View {
        let lines = graphics.lines()
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Text("PLAYLIST").font(.plexMono(10, .semibold)).tracking(1.6).foregroundStyle(Studio.secondary)
                Spacer()
                if !graphics.documentStatus.isEmpty && (graphics.documentReadOnly || graphics.documentStatus.hasPrefix("Graphics playlist not saved")) {
                    Text(graphics.documentStatus).font(.grotesk(11)).foregroundStyle(Studio.amber).lineLimit(2)
                }
                Button("Add default rows") { graphics.addDefaultRows() }
                    .buttonStyle(GhostButtonStyle())
                    .disabled(graphics.documentReadOnly || !graphics.available)
                    .help("Add Windows' interview rows: Guest name, Show title, Logo, LIVE bug, Sponsor card")
            }
            if graphics.anyHotkey {
                Text(GraphicsModel.fKeyHint).font(.grotesk(11)).foregroundStyle(Studio.textDim)
            }
            if graphics.rows.isEmpty {
                Text(graphics.documentLoaded ? "No playlist rows yet." : "Loading the graphics playlist…")
                    .font(.grotesk(12)).foregroundStyle(Studio.secondary)
            }
            ForEach(graphics.rows) { row in
                let rowLines = lines.filter { $0.row.id == row.id }
                VStack(alignment: .leading, spacing: 4) {
                    if rowLines.isEmpty {
                        rowHeader(row, empty: row.kind == GraphicsRow.guest ? "No guests: assign Show Inputs on the Sources page" : "")
                    }
                    ForEach(rowLines) { line in lineView(line) }
                    if selectedRowId == row.id { GraphicsRowInspector(graphics: graphics, row: row) }
                }
                .padding(8)
                .background(RoundedRectangle(cornerRadius: 8).fill(selectedRowId == row.id ? Studio.surfaceRaised : Studio.panel))
                .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
                .contentShape(Rectangle())
                .onTapGesture { selectedRowId = selectedRowId == row.id ? nil : row.id }
            }
        }
    }

    func rowHeader(_ row: GraphicsRow, empty: String) -> some View {
        HStack(spacing: 8) {
            Text(row.name).font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary)
            if !row.hotkey.isEmpty { hotkeyBadge(row.hotkey) }
            Text(empty).font(.grotesk(11)).foregroundStyle(Studio.textDim)
            Spacer()
        }
    }

    func hotkeyBadge(_ key: String) -> some View {
        Text(key).font(.plexMono(10)).foregroundStyle(Studio.secondary)
            .padding(.horizontal, 5).padding(.vertical, 1)
            .background(RoundedRectangle(cornerRadius: 4).fill(Studio.surface))
    }

    func lineView(_ line: GraphicsRowLine) -> some View {
        let busy = graphics.busy
        return HStack(spacing: 8) {
            Circle().fill(dot(line.state)).frame(width: 8, height: 8)
            Text(line.label).font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary).lineLimit(1)
            if !line.row.hotkey.isEmpty { hotkeyBadge(line.row.hotkey) }
            switch line.state {
            case .preparing(let text): Text(text).font(.grotesk(11)).foregroundStyle(Studio.amber)
            case .failed(let warning): Text(warning).font(.grotesk(11)).foregroundStyle(Studio.red).lineLimit(2)
            default: EmptyView()
            }
            if line.row.templateId.isEmpty {
                Text("No graphic yet").font(.grotesk(11)).foregroundStyle(Studio.textDim)
            }
            Spacer()
            if line.canHide {
                Button("Hide") { graphics.hide(entry: line.entry) }.buttonStyle(AccentButtonStyle()).disabled(busy)
                    .accessibilityLabel("Hide \(line.label)")
            } else {
                Button("Cue") { graphics.cue(line.entry) }.buttonStyle(GhostButtonStyle()).disabled(busy || !line.canCue)
                    .accessibilityLabel("Cue \(line.label)")
                if line.canClearCue {
                    Button("Clear cue") { graphics.clearCue(line.entry.instanceId) }.buttonStyle(GhostButtonStyle()).disabled(busy)
                        .accessibilityLabel("Clear cue \(line.label)")
                }
                Button("Show") { graphics.show(line.entry) }.buttonStyle(AccentButtonStyle()).disabled(busy || !line.canShow)
                    .accessibilityLabel("Show \(line.label)")
            }
        }
    }

    func dot(_ state: GraphicsRowState) -> Color {
        switch state {
        case .onProgram: return Studio.red
        case .cued: return Studio.amber
        case .preparing: return Studio.amber
        case .failed: return Studio.red
        case .idle: return Studio.textDim
        }
    }
}

struct GraphicsRowInspector: View {
    @ObservedObject var graphics: GraphicsModel
    let row: GraphicsRow

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Divider().overlay(Studio.border)
            labeled("Name") {
                TextField("Row name", text: Binding(get: { row.name }, set: { graphics.setName(row.id, $0) }))
                    .textFieldStyle(StudioFieldStyle()).frame(width: 240)
            }
            if row.kind == GraphicsRow.guest {
                labeled("From Sources") {
                    Toggle("Names and titles come from the Sources page", isOn: Binding(
                        get: { row.followSources }, set: { graphics.setFollowSources(row.id, $0) }))
                        .font(.grotesk(12))
                }
            } else {
                ForEach(graphics.editableFields(row), id: \.id) { field in
                    labeled(field.id.capitalized) {
                        TextField(field.id, text: Binding(
                            get: { (row.fields[field.id] as? String) ?? (row.fields[field.id] as? NSNumber)?.stringValue ?? "" },
                            set: { text in
                                if field.type == "number" { if let n = Double(text) { graphics.setField(row.id, field.id, n) } }
                                else { graphics.setField(row.id, field.id, text) }
                            }))
                            .textFieldStyle(StudioFieldStyle()).frame(width: 240)
                    }
                }
            }
            labeled("Hides after") {
                Picker("", selection: Binding(get: { row.playOutAfterMs }, set: { graphics.setHidesAfter(row.id, $0) })) {
                    ForEach(GraphicsModel.hidesAfterChoices, id: \.ms) { Text($0.label).tag($0.ms) }
                }.labelsHidden().frame(width: 120)
            }
            labeled("Hotkey") {
                Picker("", selection: Binding(get: { row.hotkey }, set: { graphics.setHotkey(row.id, $0) })) {
                    ForEach(GraphicsModel.hotkeys, id: \.self) { Text($0.isEmpty ? "None" : $0).tag($0) }
                }.labelsHidden().frame(width: 120)
            }
            labeled("Design") {
                Button("Open in Design") {}.buttonStyle(GhostButtonStyle()).disabled(true)
                    .help("The Designer arrives in a later release")
                Text("Designer arrives in a later release").font(.grotesk(11)).foregroundStyle(Studio.textDim)
            }
        }
        .disabled(graphics.documentReadOnly)
    }

    func labeled(_ label: String, @ViewBuilder content: () -> some View) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary).frame(width: 110, alignment: .leading)
            content()
            Spacer()
        }
    }
}
