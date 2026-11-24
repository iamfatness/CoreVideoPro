// GraphicsInspector — Operate's right column (G4-G7 Task 1). Windows
// GraphicsPlayoutControl (:46-93) is a two-column grid: the playlist on the
// left, a fixed 300 px bordered INSPECTOR on the right that scrolls on its own.
// The section list and its order come from GraphicsModel.inspectorSections
// (Windows RefreshInspector :598-692); this file only draws them.

import SwiftUI
import UniformTypeIdentifiers

/// One block of the inspector, top to bottom.
enum GraphicsInspectorSection: Equatable {
    case header(title: String, caption: String)
    case name
    case titleLine(key: String, readOnly: Bool)
    case typeable(GraphicsTypeableField)
    case fromSources
    case namesList
    case data
    case hidesAfter
    case hotkey
    case hotkeyHint
    case openInDesign
    case noGraphicWarning
}

/// The Operate grid's columns. Windows has the same two at every width: a flexible list and a
/// fixed 300 px inspector beside it (never below it; Compact mode is the Studio panel, not Operate).
enum GraphicsOperateColumn: Equatable {
    case list
    case inspector(width: CGFloat)
}

enum GraphicsOperateLayout {
    static let inspectorWidth: CGFloat = 300
    static let columnSpacing: CGFloat = 12

    static func columns(width: CGFloat) -> [GraphicsOperateColumn] { [.list, .inspector(width: inspectorWidth)] }
}

struct GraphicsInspectorPanel: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var graphics: GraphicsModel
    let width: CGFloat

    static let emptyText = "Select a row or a guest to edit its name, title line and timing."
    static let hotkeyHintText = "Hotkeys work on the Studio and Graphics tabs when no text box has focus. Shift+F12 plays everything out."
    static let noGraphicText = "This row has no graphic yet. Open it in Design and import a full-canvas image to give it one."

    var body: some View {
        let key = graphics.selectedKey
        let selection = graphics.selection(key)
        ScrollView(.vertical) {
            VStack(alignment: .leading, spacing: 10) {
                Text("INSPECTOR").font(.plexMono(10, .semibold)).tracking(1.6).foregroundStyle(Studio.secondary)
                if let (row, entry) = selection {
                    ForEach(Array(graphics.inspectorSections(for: key).enumerated()), id: \.offset) { _, section in
                        sectionView(section, row: row, entry: entry)
                            .disabled(graphics.documentReadOnly && Self.editsTheDocument(section))
                    }
                } else {
                    Text(Self.emptyText).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(12)
        .frame(width: width)
        .frame(maxHeight: .infinity, alignment: .top)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Graphics inspector")
    }

    /// A read-only show disables every edit; navigation (Edit names in Sources →) stays live, as on Windows.
    static func editsTheDocument(_ section: GraphicsInspectorSection) -> Bool {
        switch section {
        case .header, .hotkeyHint, .openInDesign, .noGraphicWarning, .data: return false
        case .titleLine(_, let readOnly): return !readOnly
        default: return true
        }
    }

    @ViewBuilder
    func sectionView(_ section: GraphicsInspectorSection, row: GraphicsRow, entry: GraphicsPlaylistEntry?) -> some View {
        switch section {
        case .header(let title, let caption):
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.grotesk(16, .semibold)).foregroundStyle(Studio.textPrimary)
                    .fixedSize(horizontal: false, vertical: true)
                Text(caption).font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        case .name:
            field("Name") { GraphicsNameField(graphics: graphics, rowId: row.id, saved: row.name) }
                .help("The playlist row name shown on chips and in the list.")
        case .titleLine(let key, let readOnly):
            if readOnly {
                field("Title line") {
                    TextField("From the Sources page", text: .constant(entry?.title ?? ""))
                        .textFieldStyle(StudioFieldStyle()).disabled(true)
                }
                Button { model.selectedTab = .sources } label: { Text("Edit names in Sources →").frame(maxWidth: .infinity) }
                    .buttonStyle(GhostButtonStyle())
            } else {
                field("Title line") {
                    TextField("", text: Binding(get: { GraphicsRowFields.text(row, key) },
                                                set: { graphics.setTitleLine(row.id, key: key, $0) }))
                        .textFieldStyle(StudioFieldStyle())
                }
            }
        case .typeable(let typed):
            field(typed.label) {
                TextField("Typed for this row", text: Binding(get: { GraphicsRowFields.text(row, typed.id) },
                                                             set: { graphics.setTypedText(row.id, typed, $0) }))
                    .textFieldStyle(StudioFieldStyle())
                    .accessibilityLabel("Row variable " + typed.id)
            }
            .help("The \(typed.id) variable of this graphic. It goes on air with the next Show.")
        case .fromSources:
            field("From Sources") {
                Toggle(row.followSources ? "Names and titles follow the Sources page" : "Uses this row's own text", isOn: Binding(
                    get: { row.followSources }, set: { graphics.setFollowSources(row.id, $0) }))
                    .toggleStyle(StudioToggleStyle()).font(.grotesk(12)).foregroundStyle(Studio.textPrimary)
            }
        case .namesList:
            GraphicsNamesListButton(graphics: graphics, row: row)
        case .data:
            // Tasks 10-11 fill this with the data actions.
            Text("Data: published values").font(.grotesk(11)).foregroundStyle(Studio.secondary)
        case .hidesAfter:
            field("Hides after") {
                Picker("", selection: Binding(get: { row.playOutAfterMs }, set: { graphics.setHidesAfter(row.id, $0) })) {
                    ForEach(GraphicsModel.hidesAfterChoices, id: \.ms) { Text($0.label).tag($0.ms) }
                }.labelsHidden().frame(maxWidth: .infinity)
            }
        case .hotkey:
            field("Hotkey") {
                Picker("", selection: Binding(get: { row.hotkey }, set: { graphics.setHotkey(row.id, $0) })) {
                    ForEach(GraphicsModel.hotkeys, id: \.self) { Text($0.isEmpty ? "None" : $0).tag($0) }
                }.labelsHidden().frame(maxWidth: .infinity)
            }
        case .hotkeyHint:
            VStack(alignment: .leading, spacing: 4) {
                Text(Self.hotkeyHintText)
                if graphics.anyHotkey { Text(GraphicsModel.fKeyHint).foregroundStyle(Studio.textDim) }
            }
            .font(.grotesk(10)).foregroundStyle(Studio.secondary).fixedSize(horizontal: false, vertical: true)
        case .openInDesign:
            Button {} label: { Text("Open in Design").frame(maxWidth: .infinity) }.buttonStyle(GhostButtonStyle()).disabled(true)
                .help("The Designer arrives in a later release")
        case .noGraphicWarning:
            Text(Self.noGraphicText).font(.grotesk(11)).foregroundStyle(Studio.amber)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    /// A WinUI TextBox/ComboBox header: the label above a full-width control.
    func field(_ label: String, @ViewBuilder content: () -> some View) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary)
            content().frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

/// "Names list…" (Windows DropDownButton + Flyout, GraphicsPlayoutControl.NamesList.cs): a 320 pt
/// popover with the CSV text, Import CSV…, Save names list and a status line. Opening refills the
/// text from the row; Save remembers the template the editor opened on and is refused if it changed.
struct GraphicsNamesListButton: View {
    @ObservedObject var graphics: GraphicsModel
    let row: GraphicsRow
    @State private var open = false
    @State private var text = ""
    @State private var status = ""
    @State private var openedOn: (id: String, version: Int) = ("", 0)

    var body: some View {
        Button {
            text = GraphicsNamesList.text(graphics.rows.first { $0.id == row.id }?.variants ?? [])
            status = ""
            openedOn = (row.templateId, row.templateVersion)
            open = true
        } label: { Text("Names list…").frame(maxWidth: .infinity) }
        .buttonStyle(GhostButtonStyle())
        .popover(isPresented: $open, arrowEdge: .leading) {
            VStack(alignment: .leading, spacing: 8) {
                Text("Names list CSV").font(.grotesk(12)).foregroundStyle(Studio.secondary)
                ZStack(alignment: .topLeading) {
                    TextEditor(text: $text).font(.plexMono(11)).scrollContentBackground(.hidden)
                        .padding(4).background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
                        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
                        .accessibilityLabel("Names list CSV")
                    if text.isEmpty {
                        Text("name,title,location").font(.plexMono(11)).foregroundStyle(Studio.textDim)
                            .padding(.horizontal, 9).padding(.vertical, 4).allowsHitTesting(false)
                    }
                }
                .frame(minHeight: 130, maxHeight: 260)
                Button("Import CSV…") { importCsv() }.buttonStyle(GhostButtonStyle())
                Button("Save names list") {
                    switch graphics.saveNamesList(rowId: row.id, text: text, templateId: openedOn.id, templateVersion: openedOn.version) {
                    case nil: open = false
                    case let message?: status = message
                    }
                }
                .buttonStyle(AccentButtonStyle()).disabled(graphics.documentReadOnly)
                if !status.isEmpty {
                    Text(status).font(.grotesk(11)).foregroundStyle(Studio.amber).fixedSize(horizontal: false, vertical: true)
                }
            }
            .padding(12).frame(width: 320)
            .background(Studio.panel).environment(\.colorScheme, .dark)
        }
    }

    func importCsv() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.commaSeparatedText]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do { text = try String(contentsOf: url, encoding: .utf8) } catch { status = error.localizedDescription }
    }
}

/// The row Name, edited through a local draft so the field can pass through
/// empty while the operator retypes it; only a valid name is stored
/// (GraphicsModel.setName), and the status line says why otherwise.
struct GraphicsNameField: View {
    @ObservedObject var graphics: GraphicsModel
    let rowId: String
    let saved: String
    @State private var draft: String?

    var body: some View {
        TextField("Row name", text: Binding(get: { draft ?? saved }, set: { draft = $0; graphics.setName(rowId, $0) }))
            .textFieldStyle(StudioFieldStyle())
            .onSubmit { draft = nil }   // an invalid draft falls back to the stored name on commit
            .onChange(of: rowId) { _ in draft = nil }
    }
}
