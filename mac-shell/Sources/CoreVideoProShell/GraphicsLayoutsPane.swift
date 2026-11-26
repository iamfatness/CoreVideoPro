// GraphicsLayoutsPane — the SHOT LAYOUTS band and the Edit Shot Layout sheet (G4-G7 Task 16). Ports of
// GraphicsPlayoutControl.RefreshLayouts / LayoutButton / AddLayoutProgress / OpenLayoutEditorAsync
// (:208-418, GraphicsPlayoutControl.Layouts.cs). The decisions (summary line, header buttons, the row
// editor, the form's layout) are pure and tested; these views only draw them.

import SwiftUI

/// SHOT LAYOUTS sits between the PLAYLIST header and the playlist (Windows :73-74).
struct GraphicsShotLayoutsBand: View {
    @ObservedObject var graphics: GraphicsModel
    @State private var editing = false

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Text("SHOT LAYOUTS").font(.plexMono(10, .semibold)).tracking(1.6).foregroundStyle(Studio.secondary)
                if graphics.rows.isEmpty {
                    // First open: the layouts appear once the default playlist exists (its presets install with it).
                    Text("Appear once you have graphics.").font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    Spacer()
                } else {
                    Spacer()
                    ForEach(graphics.layoutHeaderButtons(), id: \.title) { button in
                        Button(button.title) { press(button) }
                            .buttonStyle(GraphicsChipStyle()).disabled(!button.enabled)
                            .help(button.kind == .edit ? "Pick a scene, tick the graphics and set a Shift+F key for each layout. Program and Preview are unchanged." : "")
                    }
                }
            }
            if !graphics.rows.isEmpty {
                if let progress = graphics.layoutProgress { GraphicsLayoutProgressView(graphics: graphics, progress: progress) }
                if graphics.document.shotLayouts.isEmpty {
                    Text("No shot layouts yet. Edit layouts to make one: a scene plus the graphics that ride the Take with it.")
                        .font(.grotesk(11)).foregroundStyle(Studio.secondary).fixedSize(horizontal: false, vertical: true)
                } else {
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: 8) {
                            ForEach(graphics.document.shotLayouts, id: \.id) { layout in GraphicsLayoutButton(graphics: graphics, layout: layout) }
                        }
                    }
                }
            }
        }
        .sheet(isPresented: $editing) { GraphicsLayoutEditorSheet(graphics: graphics, isPresented: $editing) }
    }

    private func press(_ button: GraphicsLayoutHeaderButton) {
        switch button.kind {
        case .take: if let layout = graphics.headerLayout { graphics.cueAndTakeLayout(layout.id) }
        case .cancel: graphics.cancelLayout()
        case .edit: editing = true
        }
    }
}

/// LayoutButton: name + "on preview" + hotkey badge, the summary line; click selects and cues.
struct GraphicsLayoutButton: View {
    @ObservedObject var graphics: GraphicsModel
    let layout: GraphicsShotLayout

    var body: some View {
        let selected = layout.id == graphics.selectedLayoutId
        let onPreview = !layout.sceneId.isEmpty && layout.sceneId == graphics.previewSceneIdForLayouts()
        Button {
            graphics.selectedLayoutId = layout.id
            graphics.cueLayout(layout.id)
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(layout.name).font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary).lineLimit(1)
                    if onPreview { GraphicsTextBadge(text: "on preview") }
                    Spacer(minLength: 8)
                    if !layout.hotkey.isEmpty { GraphicsHotkeyBadge(key: layout.hotkey).fixedSize().layoutPriority(1) }
                }
                Text(graphics.layoutSummary(layout)).font(.grotesk(10)).foregroundStyle(Studio.secondary).lineLimit(1).truncationMode(.tail)
            }
            .padding(.horizontal, 10).padding(.vertical, 7)
            .frame(minWidth: 150, maxWidth: 240, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 8).fill(Studio.panel))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(selected ? Studio.accent : Studio.border, lineWidth: 1))
            .contentShape(RoundedRectangle(cornerRadius: 8))
        }
        .buttonStyle(.plain).disabled(graphics.busy)
        .help("Cue \(layout.name): \(graphics.layoutSummary(layout)). The next Take carries the picture and the graphics together.")
    }
}

struct GraphicsTextBadge: View {
    let text: String
    var body: some View {
        Text(text).font(.plexMono(9)).foregroundStyle(Studio.secondary)
            .padding(.horizontal, 5).padding(.vertical, 1)
            .background(RoundedRectangle(cornerRadius: 4).fill(Studio.surface))
    }
}

/// AddLayoutProgress: "<state> · <scene>", one line per row, the reason in red.
struct GraphicsLayoutProgressView: View {
    @ObservedObject var graphics: GraphicsModel
    let progress: GraphicsLayoutProgress

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(GraphicsLayouts.progressTitle(progress, takePending: graphics.guardedTakePending(),
                                               layout: graphics.document.shotLayouts.first { $0.id == progress.layoutId }, scenes: graphics.sceneNames()))
                .font(.grotesk(12)).foregroundStyle(Studio.textPrimary)
            ForEach(Array(progress.rows.enumerated()), id: \.offset) { _, row in
                Text(GraphicsLayouts.progressLine(row)).font(.grotesk(11)).foregroundStyle(Studio.secondary)
            }
            if !progress.reason.isEmpty {
                Text(progress.reason).font(.grotesk(11)).foregroundStyle(Studio.red).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

extension GraphicsLayouts {
    static func progressTitle(_ progress: GraphicsLayoutProgress, takePending: Bool, layout: GraphicsShotLayout?, scenes: [(id: String, name: String)]) -> String {
        let message = takePending ? "Confirming Take" : progress.state
        let sceneId = layout?.sceneId ?? ""
        let scene = sceneId.isEmpty ? "No scene change" : scenes.first { $0.id == sceneId }?.name ?? sceneId
        return "\(message) · \(scene)"
    }

    static func progressLine(_ row: GraphicsLayoutRowProgress) -> String {
        let revision = row.dataRevision.map { " · data revision \($0)" } ?? ""
        let loaded = row.dataReceivedAt.map { " · loaded \(DateFormatter.localizedString(from: $0, dateStyle: .short, timeStyle: .short))" } ?? ""
        return "\(row.name) · \(row.state)\(revision)\(loaded)"
    }
}

extension GraphicsModel {
    /// The Preview scene the band marks "on preview" (AppModel's previewSceneId, via the stamp wiring).
    func previewSceneIdForLayouts() -> String { previewSceneIdProvider() }
}

// ── the Edit Shot Layout sheet ──────────────────────────────────────────────

/// The editor's form for one layout (or a new one): a pure model of OpenLayoutEditorAsync's Build().
struct GraphicsLayoutEditorForm {
    let existing: GraphicsShotLayout?
    var name: String
    var sceneIndex: Int            // 0 = "No scene change"
    var ticked: Set<String>
    var rowEditors: [String: GraphicsLayoutRowEditor]
    var hotkeyIndex: Int           // 0 = None
    var playOutOthers: Bool
    let scenes: [(id: String, name: String)]
    let rows: [GraphicsRow]

    init(existing: GraphicsShotLayout?, rows: [GraphicsRow], scenes: [(id: String, name: String)],
         editor: (GraphicsRow, GraphicsLayoutRowSettings?, Bool) -> GraphicsLayoutRowEditor) {
        self.existing = existing; self.rows = rows; self.scenes = scenes
        name = existing?.name ?? ""
        sceneIndex = existing.map { layout in (scenes.firstIndex { $0.id == layout.sceneId }).map { $0 + 1 } ?? 0 } ?? 0
        ticked = Set(existing?.rowIds ?? [])
        rowEditors = Dictionary(uniqueKeysWithValues: rows.map { row in
            (row.id, editor(row, existing?.rowSettings[row.id], existing?.rowIds.contains(row.id) == true))
        })
        hotkeyIndex = max(0, GraphicsLayouts.hotkeyChoices.firstIndex(of: existing?.hotkey ?? "") ?? 0)
        playOutOthers = existing?.playOutOthers ?? false
    }

    /// "Scene '<id>' is no longer in this show; this layout is refused until you pick another."
    var missingSceneText: String? {
        guard let existing, !existing.sceneId.isEmpty, !scenes.contains(where: { $0.id == existing.sceneId }) else { return nil }
        return "Scene '\(existing.sceneId)' is no longer in this show; this layout is refused until you pick another."
    }

    mutating func tick(_ rowId: String, _ on: Bool) {
        if on { ticked.insert(rowId) } else { ticked.remove(rowId) }
        rowEditors[rowId]?.included = on
    }

    /// The layout Save sends (graphics.layout.set). Throws "Give the layout a name." or a row editor's refusal.
    func layout(taken: [String]) throws -> GraphicsShotLayout {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { throw GraphicsDataError(message: "Give the layout a name.") }
        var settings: JSONObject = [:]
        for row in rows { if let read = try rowEditors[row.id]?.read() { settings[row.id] = read.json } }
        var layout = GraphicsShotLayout(json: [:])
        layout.id = existing?.id ?? GraphicsLayouts.slug(trimmed, taken: taken)
        layout.name = trimmed
        layout.sceneId = sceneIndex > 0 && sceneIndex <= scenes.count ? scenes[sceneIndex - 1].id : ""
        layout.rowIds = rows.map(\.id).filter(ticked.contains)
        layout.hotkey = hotkeyIndex > 0 ? GraphicsLayouts.hotkeyChoices[hotkeyIndex] : ""
        layout.playOutOthers = playOutOthers
        layout.json["rowSettings"] = settings
        return layout
    }
}

struct GraphicsLayoutEditorSheet: View {
    @ObservedObject var graphics: GraphicsModel
    @Binding var isPresented: Bool
    @State private var pickerIndex = 0
    @State private var form: GraphicsLayoutEditorForm?
    @State private var error = ""

    var layouts: [GraphicsShotLayout] { graphics.document.shotLayouts }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Edit Shot Layout").font(.grotesk(16, .semibold)).foregroundStyle(Studio.textPrimary)
            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    labeled("Layout") {
                        Picker("", selection: Binding(get: { pickerIndex }, set: { pickerIndex = $0; rebuild() })) {
                            ForEach(Array((layouts.map(\.name) + ["New layout"]).enumerated()), id: \.offset) { index, name in Text(name).tag(index) }
                        }.labelsHidden()
                    }
                    if form != nil { formBody }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: 460)
            if !error.isEmpty { Text(error).font(.grotesk(11)).foregroundStyle(Studio.red).fixedSize(horizontal: false, vertical: true) }
            HStack(spacing: 8) {
                Button(form?.existing == nil ? "Add layout" : "Save layout") { save() }.buttonStyle(AccentButtonStyle())
                if let existing = form?.existing {
                    Button("Remove layout") {
                        if graphics.selectedLayoutId == existing.id { graphics.selectedLayoutId = "" }
                        if let refusal = graphics.removeLayout(id: existing.id) { error = refusal } else { isPresented = false }
                    }.buttonStyle(GhostButtonStyle())
                }
                Spacer()
                Button("Close") { isPresented = false }.buttonStyle(GhostButtonStyle()).keyboardShortcut(.cancelAction)
            }
        }
        .padding(18).frame(minWidth: 340, idealWidth: 430, maxWidth: 430)
        .background(Studio.panel).environment(\.colorScheme, .dark)
        .onAppear {
            pickerIndex = max(0, layouts.firstIndex { $0.id == graphics.selectedLayoutId } ?? 0)
            rebuild()
        }
    }

    @ViewBuilder private var formBody: some View {
        let binding = Binding(get: { form! }, set: { form = $0 })
        if let missing = form?.missingSceneText {
            Text(missing).font(.grotesk(11)).foregroundStyle(Studio.red).fixedSize(horizontal: false, vertical: true)
        }
        HStack(alignment: .top, spacing: 8) {
            labeled("Name") {
                TextField("Open, Two-up, Panel...", text: Binding(get: { binding.wrappedValue.name }, set: { binding.wrappedValue.name = String($0.prefix(128)) }))
                    .textFieldStyle(StudioFieldStyle())
            }
            labeled("Scene to Preview") {
                Picker("", selection: binding.sceneIndex) {
                    ForEach(Array((["No scene change"] + binding.wrappedValue.scenes.map(\.name)).enumerated()), id: \.offset) { index, name in Text(name).tag(index) }
                }.labelsHidden()
            }
        }
        Text("Graphics that ride the Take").font(.grotesk(12)).foregroundStyle(Studio.textPrimary)
        ForEach(binding.wrappedValue.rows) { row in
            Toggle(row.name, isOn: Binding(get: { binding.wrappedValue.ticked.contains(row.id) }, set: { binding.wrappedValue.tick(row.id, $0) }))
                .toggleStyle(.checkbox).font(.grotesk(12))
            if binding.wrappedValue.ticked.contains(row.id), let editor = binding.wrappedValue.rowEditors[row.id], editor.visible {
                GraphicsLayoutRowEditorView(editor: Binding(get: { binding.wrappedValue.rowEditors[row.id]! }, set: { binding.wrappedValue.rowEditors[row.id] = $0 }))
            }
        }
        labeled("Hotkey") {
            Picker("", selection: binding.hotkeyIndex) {
                ForEach(Array(GraphicsLayouts.hotkeyChoices.enumerated()), id: \.offset) { index, name in Text(name).tag(index) }
            }.labelsHidden()
        }
        labeled("Other graphics") {
            Toggle(binding.wrappedValue.playOutOthers ? "Play out Program graphics not in this layout" : "Leave other Program graphics alone",
                   isOn: binding.playOutOthers).toggleStyle(.switch).font(.grotesk(12))
        }
        Text("Shift+F1..F11 cue a layout; Shift+F12 plays everything out. Cue selects the scene to Preview and stages the graphics; Take carries both.")
            .font(.grotesk(10)).foregroundStyle(Studio.secondary).fixedSize(horizontal: false, vertical: true)
    }

    private func rebuild() {
        error = ""
        let existing = pickerIndex < layouts.count ? layouts[pickerIndex] : nil
        form = GraphicsLayoutEditorForm(existing: existing, rows: graphics.rows, scenes: graphics.sceneNames()) { row, saved, included in
            graphics.layoutRowEditor(row, saved: saved, included: included)
        }
    }

    private func save() {
        guard let form else { return }
        error = ""
        do {
            let layout = try form.layout(taken: layouts.map(\.id))
            if let refusal = graphics.setLayout(layout) { error = refusal; return }
            graphics.selectedLayoutId = layout.id
            isPresented = false
        } catch { self.error = (error as? GraphicsDataError)?.message ?? error.localizedDescription }
    }

    private func labeled(_ label: String, @ViewBuilder content: () -> some View) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary)
            content()
        }.frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// "Values and data · <row>": the revision picker, one override per field, the unavailable overrides.
struct GraphicsLayoutRowEditorView: View {
    @Binding var editor: GraphicsLayoutRowEditor
    @State private var expanded = false

    var body: some View {
        DisclosureGroup(isExpanded: $expanded) {
            VStack(alignment: .leading, spacing: 6) {
                if !editor.revisionOptions.isEmpty {
                    Text("Data revision").font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    Picker("", selection: $editor.selectedRevision) {
                        ForEach(Array(editor.revisionOptions.enumerated()), id: \.offset) { index, option in Text(option.label).tag(index) }
                    }.labelsHidden()
                } else if editor.hasOrphanPin {
                    Toggle("Remove unavailable data pin", isOn: $editor.removePin).toggleStyle(.checkbox).font(.grotesk(12))
                }
                ForEach(editor.fieldEditors.indices, id: \.self) { index in
                    Toggle(editor.fieldEditors[index].title, isOn: $editor.fieldEditors[index].enabled).toggleStyle(.checkbox).font(.grotesk(12))
                    GraphicsLayoutFieldInput(field: editor.fieldEditors[index].field, value: $editor.fieldEditors[index].value)
                        .disabled(!editor.fieldEditors[index].enabled)
                }
                ForEach(editor.unavailable.indices, id: \.self) { index in
                    Toggle("Remove unavailable override " + editor.unavailable[index].key, isOn: $editor.unavailable[index].remove)
                        .toggleStyle(.checkbox).font(.grotesk(12))
                }
            }.padding(.leading, 4)
        } label: {
            Text("Values and data · " + editor.row.name).font(.grotesk(12)).foregroundStyle(Studio.textPrimary)
        }
        .onAppear { expanded = editor.included && editor.fieldEditors.contains { $0.enabled } || editor.selectedRevision > 0 }
    }
}

/// CreateLayoutFieldInput: a choice picker, a switch, a number box or a text box.
struct GraphicsLayoutFieldInput: View {
    let field: GraphicsTemplateField
    @Binding var value: Any

    var body: some View {
        if !field.choices.isEmpty {
            Picker("", selection: Binding(get: { max(0, field.choices.firstIndex { ($0 as AnyObject).isEqual(value) } ?? 0) },
                                          set: { value = field.choices[$0] })) {
                ForEach(field.choices.indices, id: \.self) { index in Text("\(String(describing: field.choices[index]))").tag(index) }
            }.labelsHidden()
        } else if field.type == "boolean" {
            Toggle("", isOn: Binding(get: { (value as? NSNumber)?.boolValue ?? false }, set: { value = NSNumber(value: $0) })).labelsHidden().toggleStyle(.switch)
        } else if field.type == "number" {
            TextField("", value: Binding(get: { (value as? NSNumber)?.doubleValue ?? min(max(0, field.minimum ?? -1e9), field.maximum ?? 1e9) },
                                         set: { value = NSNumber(value: $0) }), format: .number)
                .textFieldStyle(StudioFieldStyle())
        } else {
            TextField(field.type == "color" ? "#RRGGBB" : "Value",
                      text: Binding(get: { value as? String ?? "" }, set: { value = String($0.prefix(field.maxLength ?? (field.type == "color" ? 9 : 512))) }))
                .textFieldStyle(StudioFieldStyle())
        }
    }
}
