// GraphicsDataSheets — the Create data table / Create data crawl sheets (G4-G7 Task 9; Windows
// GraphicsBuilderControl.Data.cs ShowDataTableDialogAsync / ShowCrawlDialogAsync). The forms are
// pure (defaults and the binding they produce); the views only draw them. A refusal keeps the
// sheet open with the message (Windows args.Cancel = !result.Ok).

import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// The data table dialog's choices (GraphicsBuilderControl.Data.cs:22-27).
struct GraphicsDataTableForm: Equatable {
    var name: String
    var keyIndex: Int          // 0 = "Use file order"
    var sortIndex: Int         // 0 = "Table · file order"
    var limit: Int
    var selected: [String]     // always in source order
    let columns: [GraphicsDataColumn]
    let rowCount: Int

    static func initial(fileName: String, data: GraphicsLocalData) -> GraphicsDataTableForm {
        GraphicsDataTableForm(name: String(fileName.prefix(64)), keyIndex: 0, sortIndex: 0, limit: 12,
                              selected: data.columns.prefix(2).map(\.id), columns: data.columns, rowCount: data.rows.count)
    }

    var keyOptions: [String] { ["Use file order"] + columns.map(\.id) }
    var sortOptions: [String] { ["Table · file order"] + columns.filter { $0.kind == "number" }.map(\.id) }
    var summary: String { "\(rowCount) rows · \(columns.count) columns. Selected columns appear in source order." }
    var keyColumn: String? { keyIndex > 0 && keyIndex < keyOptions.count ? keyOptions[keyIndex] : nil }
    var sortColumn: String? { sortIndex > 0 && sortIndex < sortOptions.count ? sortOptions[sortIndex] : nil }

    mutating func toggle(_ column: String) {
        let on = selected.contains(column) ? Set(selected).subtracting([column]) : Set(selected).union([column])
        selected = columns.map(\.id).filter(on.contains)
    }

    func binding(sourceId: String) -> GraphicsDataBinding {
        GraphicsDataBinding(sourceId: sourceId, keyColumn: keyColumn, columns: selected, sortColumn: sortColumn,
                            limit: min(12, max(1, limit)), revision: 1)
    }

    /// The dataset keyed by the chosen column (data.WithKey(keyColumn)).
    func keyedData(_ data: GraphicsLocalData) throws -> GraphicsLocalData { try data.withKey(keyColumn) }

    /// "data-" + 32 lower-case hex (Guid.NewGuid().ToString("N")).
    static func newSourceId() -> String { "data-" + UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "") }
}

/// The data crawl dialog's choices (GraphicsBuilderControl.Data.cs ShowCrawlDialogAsync).
struct GraphicsDataCrawlForm: Equatable {
    var name: String
    var column: String
    var direction = "left"
    var mode = "loop"
    var speed = 120.0
    var gap = 48.0
    var limit = GraphicsCrawlData.maxItems
    let textColumns: [String]

    /// nil when the file has no text column (Windows: "The crawl needs a text column.").
    static func initial(fileName: String, data: GraphicsLocalData) -> GraphicsDataCrawlForm? {
        let text = data.columns.filter { $0.kind == "text" }.map(\.id)
        guard let first = text.first else { return nil }
        return GraphicsDataCrawlForm(name: String(fileName.prefix(64)), column: first, textColumns: text)
    }

    func binding(sourceId: String) -> GraphicsDataBinding {
        GraphicsDataBinding(sourceId: sourceId, keyColumn: nil, columns: [column], sortColumn: nil, limit: limit, revision: 1,
                            crawl: GraphicsCrawlOptions(speed: speed, gap: gap, direction: direction, mode: mode))
    }

    func sample(_ data: GraphicsLocalData) -> String {
        let items = (try? GraphicsCrawlData.items(data, column: column, limit: limit)) ?? []
        return "\(items.count) items in source order\n" + items.prefix(4).joined(separator: "  •  ")
    }
}

/// Whether a create sheet stays open, and what it says.
struct GraphicsDataSheetState: Equatable {
    var isPresented = false
    var status = ""
    mutating func apply(_ outcome: GraphicsDesignOutcome) {
        status = outcome.message
        if outcome.ok { isPresented = false }
    }
}

/// YOUR DESIGNS bar actions, Windows' order (BuildDesignsBar); only the data buttons work before the Builder (D8).
enum GraphicsDesignsBar {
    struct Action: Equatable { let title: String; let enabled: Bool; let help: String; let accent: Bool }
    static let buttons: [Action] = [
        Action(title: "Open design", enabled: false, help: "Arrives with the Builder", accent: false),
        Action(title: "Duplicate", enabled: false, help: "Arrives with the Builder", accent: false),
        Action(title: "Delete", enabled: false, help: "Arrives with the Builder", accent: false),
        Action(title: "New design", enabled: false, help: "Arrives with the Builder", accent: true),
        Action(title: "Data table…", enabled: true, help: "Create a table design from a CSV or JSON file and add it to the playlist.", accent: false),
        Action(title: "Data crawl…", enabled: true, help: "Create a crawl design from a CSV or JSON file and add it to the playlist.", accent: false),
        Action(title: "Remote feeds…", enabled: false, help: "Arrives with remote feeds", accent: false),
    ]
    static let emptyText = "No designs yet. New design starts one from blank or from a look."
}

/// A file the operator picked for a data sheet.
struct GraphicsPickedData: Identifiable {
    let id = UUID()
    let fileName: String
    let data: GraphicsLocalData
    let crawl: Bool
}

enum GraphicsDataFilePicker {
    /// NSOpenPanel for .csv / .json; reads with GraphicsLocalData.read (no key, as Windows).
    @MainActor
    static func pick() -> Result<(String, GraphicsLocalData), GraphicsDataError>? {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.commaSeparatedText, .json]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return nil }
        do { return .success((url.deletingPathExtension().lastPathComponent, try GraphicsLocalData.read(url: url, key: nil))) }
        catch { return .failure(error as? GraphicsDataError ?? GraphicsDataError(message: error.localizedDescription)) }
    }
}

struct GraphicsDataTableSheet: View {
    @ObservedObject var graphics: GraphicsModel
    let picked: GraphicsPickedData
    @Binding var sheet: GraphicsDataSheetState
    @State private var form: GraphicsDataTableForm
    @State private var working = false

    init(graphics: GraphicsModel, picked: GraphicsPickedData, sheet: Binding<GraphicsDataSheetState>) {
        self.graphics = graphics; self.picked = picked; _sheet = sheet
        _form = State(initialValue: GraphicsDataTableForm.initial(fileName: picked.fileName, data: picked.data))
    }

    var body: some View {
        GraphicsSheetFrame(title: "Create data table", status: sheet.status, working: working, primary: "Create design", cancel: { sheet.isPresented = false }) {
            create()
        } content: {
            field("Design name") { TextField("", text: Binding(get: { form.name }, set: { form.name = String($0.prefix(64)) })).textFieldStyle(StudioFieldStyle()) }
            Text(form.summary).font(.grotesk(12)).foregroundStyle(Studio.secondary)
            field("Row key") { picker(form.keyOptions, $form.keyIndex) }
            field("Leaderboard order") { picker(form.sortOptions, $form.sortIndex) }
            field("Visible rows") { Stepper("\(form.limit)", value: $form.limit, in: 1...12).font(.grotesk(12)) }
            ForEach(form.columns, id: \.id) { column in
                Toggle("\(column.id) · \(column.kind)", isOn: Binding(get: { form.selected.contains(column.id) }, set: { _ in form.toggle(column.id) }))
                    .toggleStyle(.checkbox).font(.grotesk(12))
            }
        }
    }

    func create() {
        working = true
        Task { @MainActor in
            defer { working = false }
            do {
                let data = try form.keyedData(picked.data)
                sheet.apply(await graphics.createDataDesign(name: form.name, data: data, binding: form.binding(sourceId: GraphicsDataTableForm.newSourceId())))
            } catch { sheet.status = (error as? GraphicsDataError)?.message ?? error.localizedDescription }
        }
    }
}

struct GraphicsDataCrawlSheet: View {
    @ObservedObject var graphics: GraphicsModel
    let picked: GraphicsPickedData
    @Binding var sheet: GraphicsDataSheetState
    @State private var form: GraphicsDataCrawlForm
    @State private var working = false

    init(graphics: GraphicsModel, picked: GraphicsPickedData, form: GraphicsDataCrawlForm, sheet: Binding<GraphicsDataSheetState>) {
        self.graphics = graphics; self.picked = picked; _sheet = sheet
        _form = State(initialValue: form)
    }

    var body: some View {
        GraphicsSheetFrame(title: "Create data crawl", status: sheet.status, working: working, primary: "Create design", cancel: { sheet.isPresented = false }) {
            working = true
            Task { @MainActor in
                defer { working = false }
                sheet.apply(await graphics.createDataDesign(name: form.name, data: picked.data, binding: form.binding(sourceId: GraphicsDataTableForm.newSourceId())))
            }
        } content: {
            field("Design name") { TextField("", text: Binding(get: { form.name }, set: { form.name = String($0.prefix(64)) })).textFieldStyle(StudioFieldStyle()) }
            field("Text column") { picker(form.textColumns, Binding(get: { form.textColumns.firstIndex(of: form.column) ?? 0 }, set: { form.column = form.textColumns[$0] })) }
            Text(form.sample(picked.data)).font(.grotesk(11)).foregroundStyle(Studio.secondary).lineLimit(4)
            field("Direction") { picker(["Left", "Right"], Binding(get: { form.direction == "right" ? 1 : 0 }, set: { form.direction = $0 == 1 ? "right" : "left" })) }
            field("Playback") { picker(["Loop", "Single pass"], Binding(get: { form.mode == "single" ? 1 : 0 }, set: { form.mode = $0 == 1 ? "single" : "loop" })) }
            field("Speed (px/s)") { Stepper("\(Int(form.speed))", value: $form.speed, in: 1...1200, step: 10).font(.grotesk(12)) }
            field("Gap (px)") { Stepper("\(Int(form.gap))", value: $form.gap, in: 0...512, step: 8).font(.grotesk(12)) }
            field("Item limit") { Stepper("\(form.limit)", value: $form.limit, in: 1...GraphicsCrawlData.maxItems).font(.grotesk(12)) }
        }
    }
}

/// A WinUI ContentDialog: title, a scrolling form (max height 440), the status, Cancel and the primary button.
struct GraphicsSheetFrame<Content: View>: View {
    let title: String
    let status: String
    let working: Bool
    let primary: String
    let cancel: () -> Void
    let confirm: () -> Void
    @ViewBuilder let content: () -> Content

    init(title: String, status: String, working: Bool, primary: String, cancel: @escaping () -> Void, confirm: @escaping () -> Void,
         @ViewBuilder content: @escaping () -> Content) {
        self.title = title; self.status = status; self.working = working; self.primary = primary
        self.cancel = cancel; self.confirm = confirm; self.content = content
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title).font(.grotesk(16, .semibold)).foregroundStyle(Studio.textPrimary)
            ScrollView { VStack(alignment: .leading, spacing: 8) { content() }.frame(maxWidth: .infinity, alignment: .leading) }
                .frame(maxHeight: 440)
            if !status.isEmpty {
                Text(status).font(.grotesk(11)).foregroundStyle(Studio.amber).fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Spacer()
                Button("Cancel", action: cancel).buttonStyle(GhostButtonStyle()).keyboardShortcut(.cancelAction)
                Button(primary, action: confirm).buttonStyle(AccentButtonStyle()).disabled(working).keyboardShortcut(.defaultAction)
            }
        }
        .padding(18).frame(width: 420)
        .background(Studio.panel).environment(\.colorScheme, .dark)
    }
}

@ViewBuilder
private func field(_ label: String, @ViewBuilder content: () -> some View) -> some View {
    VStack(alignment: .leading, spacing: 4) {
        Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary)
        content()
    }
}

private func picker(_ options: [String], _ selection: Binding<Int>) -> some View {
    Picker("", selection: selection) {
        ForEach(Array(options.enumerated()), id: \.offset) { index, option in Text(option).tag(index) }
    }.labelsHidden().frame(maxWidth: .infinity, alignment: .leading)
}
