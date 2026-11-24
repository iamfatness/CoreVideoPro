// Graphics G4-G7 Task 1 — Operate laid out like Windows (GraphicsPlayoutControl
// :46-93: playlist left, a fixed 300 pt inspector right) and the inspector's
// fields per GraphicsRowFields. Vectors: contracts/graphics-shell/
// rowfields.vectors.json (GraphicsShellVectorTests.RowFieldsVectors).
// Plan: docs/superpowers/plans/2026-11-22-mac-graphics-shell-g4-g7.md.

import AppKit
import SwiftUI

extension ShellTests {
    static var graphicsLayoutParityCases: [(String, () -> Void)] {
        [
            ("graphics-layout/rowfields-vectors", testRowFieldsVectorsReplay),
            ("graphics-layout/inspector-sections-windows-order", testInspectorSectionsFollowWindowsOrder),
            ("graphics-layout/data-row-offers-no-data-field-boxes", testDataRowOffersNoDataFieldBoxes),
            ("graphics-layout/hides-after-labels", testHidesAfterLabels),
            ("graphics-layout/inspector-beside-the-list", testInspectorIsBesideTheList),
            ("graphics-layout/rendered-columns", testRenderedOperateColumns),
            ("graphics-layout/read-only-keeps-navigation", testReadOnlyKeepsNavigation),
            ("graphics-layout/clock-unknown-row-refused", testClockUnknownRowRefused),
            ("graphics-layout/selection-keys", testGraphicsSelectionKeys),
            ("graphics-layout/typeable-text-is-clamped", testTypeableTextIsClamped),
        ]
    }

    static func windowsShowRows() -> [GraphicsRow] {
        let json = loadGraphicsNode("documents/windows-show.json")
        return (json["playlist"] as? [JSONObject] ?? []).map(GraphicsRow.init(json:))
    }

    static func rowFieldsVector(_ id: String) -> JSONObject? {
        loadGraphicsVectors("rowfields").first { $0["id"] as? String == id }?["input"] as? JSONObject
    }

    static func pins(_ json: Any?) -> [GraphicsTemplatePin] {
        GraphicsDocument(json: ["schemaVersion": 3, "templates": json as? [JSONObject] ?? []]).templates
    }

    static func testRowFieldsVectorsReplay() {
        var checked = 0
        for vector in loadGraphicsVectors("rowfields") {
            let id = vector["id"] as? String ?? "?"
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            guard vector["operation"] as? String == "rowFields", let rowJson = input["row"] as? JSONObject
            else { expect(false, "\(id): unknown operation"); continue }
            let row = GraphicsRow(json: rowJson)
            let state = (input["nodeFile"] as? String).map { GraphicsState(node: loadGraphicsNode($0)) }
            let pinned = pins(input["pins"])
            let titleKey = GraphicsRowFields.titleFieldKey(row)
            expectEqual(titleKey, expected["titleFieldKey"] as? String, "\(id): title key")
            let fields = GraphicsRowFields.templateFields(row, state: state, pins: pinned)
            expectEqual(fields.map(\.id), expected["templateFields"] as? [String] ?? [], "\(id): template fields")
            let typeable = GraphicsRowFields.typeable(row, state: state, pins: pinned, titleKey: row.kind == GraphicsRow.guest ? nil : titleKey)
            let want = (expected["typeable"] as? [JSONObject] ?? []).map {
                GraphicsTypeableField(id: $0["id"] as? String ?? "", label: $0["label"] as? String ?? "", maxLength: ($0["maxLength"] as? NSNumber)?.intValue)
            }
            expectEqual(typeable, want, "\(id): typeable")
            expectEqual(fields.filter(GraphicsRowFields.eligibleForMapping).map(\.id), expected["eligible"] as? [String] ?? [], "\(id): eligible")
            checked += 1
        }
        expect(checked >= 14, "every rowfields vector replayed (\(checked))")
    }

    @MainActor
    static func testInspectorSectionsFollowWindowsOrder() {
        let (model, _) = playoutModel(windowsShowRows())
        expectEqual(model.inspectorSections(for: ""), [], "no selection: no sections (the view shows the hint)")
        expectEqual(model.inspectorSections(for: "guest"), [
            .header(title: "Guest name", caption: "Guest · names from Sources"), .name, .titleLine(key: "title", readOnly: true),
            .fromSources, .namesList, .hidesAfter, .hotkey, .hotkeyHint, .openInDesign,
        ], "a Guest row following Sources, in Windows' RefreshInspector order")
        expectEqual(model.inspectorSections(for: "title"), [
            .header(title: "Show title", caption: "Title"), .name, .titleLine(key: "subtitle", readOnly: false),
            .typeable(GraphicsTypeableField(id: "heading", label: "Heading", maxLength: 256)),
            .hidesAfter, .hotkey, .hotkeyHint, .openInDesign,
        ], "a title row: no From Sources, no Names list; its extra text variable is typed")
        let sponsor = model.inspectorSections(for: "sponsor")
        expectEqual(sponsor.last, .noGraphicWarning, "a row with no graphic ends on the warning")
        expectEqual(sponsor.first, .header(title: "Sponsor card", caption: "Sponsor card · no image yet"), "sponsor caption")
        expectEqual(model.inspectorSections(for: "missing-row"), [], "a gone row shows the hint")
        // A names-list line selected: header names the line, caption "Guest · <sourceId>" (Windows verbatim).
        let (names, _) = playoutModel(namesClockRows())
        let line = names.inspectorSections(for: "panel-names|variant:v-ada")
        expectEqual(line.first, .header(title: "Ada Lovelace", caption: "Guest · variant:v-ada"), "names-list line header")
        expect(line.contains(.namesList), "a names-list row offers Names list…")
        expectEqual(names.inspectorSections(for: "panel-names").first,
                    .header(title: "Panel names", caption: "Names list · 3 · hotkey steps through"), "names-list caption")
    }

    @MainActor
    static func testDataRowOffersNoDataFieldBoxes() {
        guard let input = rowFieldsVector("data-table-pin"), let rowJson = input["row"] as? JSONObject
        else { expect(false, "data-table-pin vector must exist"); return }
        let (model, _) = playoutModel([GraphicsRow(json: rowJson)])
        model.document.json["templates"] = input["pins"]
        let sections = model.inspectorSections(for: "scores")
        expect(!sections.contains { if case .typeable(let f) = $0 { return f.id.hasPrefix("table-row-") }; return false },
               "a data table row offers no box for its table-row fields: \(sections)")
        expect(sections.contains(.data), "a data row carries the data section")
        expectEqual(sections.filter { if case .typeable = $0 { return true }; return false }.count, 0, "and no other typed box")
    }

    static func testHidesAfterLabels() {
        expectEqual(GraphicsModel.hidesAfterChoices.map(\.label), ["Manual", "8 s", "15 s"], "Windows PlayOutChoices labels (:42)")
        expectEqual(GraphicsModel.hidesAfterChoices.map(\.ms), [0, 8000, 15000], "and values")
    }

    /// The structure Windows has at every width (GraphicsPlayoutControl :55-56): a flexible list
    /// column and a fixed 300 px inspector column beside it, never below it.
    static func testInspectorIsBesideTheList() {
        for width in [960.0, 1336.0, 1576.0, 1896.0] {
            expectEqual(GraphicsOperateLayout.columns(width: width), [.list, .inspector(width: 300)], "two columns at \(width)")
        }
        expectEqual(GraphicsOperateLayout.inspectorWidth, 300, "the inspector is 300 pt wide")
    }

    @MainActor
    static func testGraphicsSelectionKeys() {
        let (model, _) = playoutModel(namesClockRows())
        let names = model.rows[0], clock = model.rows[1]
        model.selectedKey = "panel-names|variant:v-bo"
        expect(model.isCardSelected(names), "a selected names-list line selects its card (accent border)")
        expect(!model.isCardSelected(clock), "not another card")
        model.selectedKey = "countdown"
        expect(model.isCardSelected(clock), "a single row's card")
        model.selectedKey = "countdown|x"
        expect(!model.isCardSelected(clock), "a single-line row is never selected by a line key")
        model.select("panel-names")
        expectEqual(model.selectedKey, "panel-names", "select a row")
        model.select(GraphicsModel.selectionKey(rowId: "panel-names", sourceId: "variant:v-cy"))
        expectEqual(model.selectedKey, "panel-names|variant:v-cy", "select a line")
    }

    @MainActor
    static func testTypeableTextIsClamped() {
        guard let input = rowFieldsVector("guest-design-pin"), let rowJson = input["row"] as? JSONObject
        else { expect(false, "guest-design-pin vector must exist"); return }
        let (model, _) = playoutModel([GraphicsRow(json: rowJson)])
        model.document.json["templates"] = input["pins"]
        let field = GraphicsTypeableField(id: "location", label: "City and venue", maxLength: 64)
        model.setTypedText("guest-design", field, String(repeating: "é", count: 70))
        expectEqual((model.rows[0].fields["location"] as? String)?.utf16.count, 64, "clamped at the field's maxLength")
        let open = GraphicsTypeableField(id: "pronoun_line", label: "Pronoun line", maxLength: nil)
        model.setTypedText("guest-design", open, String(repeating: "x", count: 600))
        expectEqual((model.rows[0].fields["pronoun_line"] as? String)?.utf16.count, 512, "no maxLength: the core's 512 string cap")
        model.setTitleLine("guest-design", key: "title", String(repeating: "t", count: 700))
        expectEqual((model.rows[0].fields["title"] as? String)?.utf16.count, 512, "Title line MaxLength 512")
    }

    /// Renders Operate offscreen at the page sizes of the Mac's minimum window and a large one and reads
    /// back where each column landed (GraphicsColumnFrames): the inspector must be BESIDE the list (to
    /// its right, 300 pt wide, full height), never below it or inside a card.
    @MainActor
    static func testRenderedOperateColumns() {
        let (graphics, _) = playoutModel(windowsShowRows())
        graphics.selectedKey = "guest"
        final class Box { var frames: [String: CGRect] = [:] }
        for (width, height) in [(1336.0, 640.0), (1896.0, 900.0)] {
            let box = Box()
            let host = NSHostingView(rootView: GraphicsOperatePane(graphics: graphics).environmentObject(AppModel())
                .onPreferenceChange(GraphicsColumnFrames.Key.self) { box.frames = $0 }
                .frame(width: width, height: height).environment(\.colorScheme, .dark))
            let window = NSWindow(contentRect: NSRect(x: -10_000, y: -10_000, width: width, height: height),
                                  styleMask: [.borderless], backing: .buffered, defer: false)
            window.contentView = host
            host.frame = NSRect(x: 0, y: 0, width: width, height: height)
            host.layoutSubtreeIfNeeded()
            RunLoop.main.run(until: Date().addingTimeInterval(0.1))
            guard let list = box.frames["list"], let inspector = box.frames["inspector"]
            else { expect(false, "both columns are rendered at \(width): \(box.frames)"); continue }
            expect(inspector.minX >= list.maxX + 11, "inspector is right of the list at \(width): list \(list) inspector \(inspector)")
            expectEqual(inspector.width, 300, "inspector is 300 pt wide at \(width)")
            expect(inspector.minY == list.minY && inspector.height == height && list.height == height, "same top, full height at \(width): \(list) \(inspector)")
            expectEqual(list.width, width - 300 - 12, "the list takes the rest at \(width)")
            window.contentView = nil
        }
    }

    static func testReadOnlyKeepsNavigation() {
        expect(!GraphicsInspectorPanel.editsTheDocument(.titleLine(key: "title", readOnly: true)), "Edit names in Sources stays live when read-only")
        expect(GraphicsInspectorPanel.editsTheDocument(.titleLine(key: "title", readOnly: false)), "an editable Title line is disabled when read-only")
        expect(GraphicsInspectorPanel.editsTheDocument(.name) && GraphicsInspectorPanel.editsTheDocument(.namesList)
               && GraphicsInspectorPanel.editsTheDocument(.hotkey), "edits are disabled when read-only")
    }

    @MainActor
    static func testClockUnknownRowRefused() {
        let (model, core) = playoutModel(namesClockRows())
        let result = awaitMain { await model.clock(rowId: "gone", action: "start").value }
        expectEqual(result?.reason, "That playlist row does not exist.", "Windows' refusal for an unknown row")
        expectEqual(core.commands.count, 0, "nothing sent")
    }
}
