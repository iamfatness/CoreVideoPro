// Graphics G4-G7 Task 9 — Design pane: YOUR DESIGNS bar (Windows BuildDesignsBar) and the
// Create data table / Create data crawl sheets (GraphicsBuilderControl.Data.cs), as pure forms.

import Foundation

extension ShellTests {
    static var graphicsDataSheetCases: [(String, () -> Void)] {
        [
            ("graphics-data-sheet/table-form-defaults", testDataTableFormDefaults),
            ("graphics-data-sheet/table-form-binding", testFormBindingUsesTheSelectedKeyAndSort),
            ("graphics-data-sheet/crawl-form-defaults-and-binding", testCrawlFormDefaultsAndBinding),
            ("graphics-data-sheet/refusal-keeps-the-sheet", testCreateFromFormRefusalKeepsTheSheet),
            ("graphics-data-sheet/designs-bar-buttons", testDesignsBarButtons),
        ]
    }

    static func scoresFileOrder() -> GraphicsLocalData? {
        try? GraphicsLocalData.read(url: repoRoot.appendingPathComponent("contracts/graphics-shell/data/scores.csv"), key: nil)
    }

    static func testDataTableFormDefaults() {
        guard let data = scoresFileOrder() else { expect(false, "scores.csv"); return }
        let form = GraphicsDataTableForm.initial(fileName: "scores", data: data)
        expectEqual(form.name, "scores", "design name = the file's display name")
        expectEqual(form.limit, 12, "Visible rows 12")
        expectEqual(form.selected, ["team", "points"], "the first two columns are selected")
        expectEqual(form.keyIndex, 0, "Row key: Use file order")
        expectEqual(form.sortIndex, 0, "Leaderboard order: Table · file order")
        expectEqual(form.keyOptions, ["Use file order", "team", "points", "qualified", "city"], "key choices")
        expectEqual(form.sortOptions, ["Table · file order", "points"], "only numeric columns order a leaderboard")
        expectEqual(form.summary, "6 rows · 4 columns. Selected columns appear in source order.", "summary line")
        expectEqual(GraphicsDataTableForm.initial(fileName: String(repeating: "f", count: 70), data: data).name.count, 64, "name cut to 64")
        let binding = form.binding(sourceId: "data-0123456789abcdef0123456789abcdef")
        expectEqual(binding.keyColumn, nil, "no key")
        expectEqual(binding.sortColumn, nil, "no sort")
    }

    static func testFormBindingUsesTheSelectedKeyAndSort() {
        guard let data = scoresFileOrder() else { expect(false, "scores.csv"); return }
        var form = GraphicsDataTableForm.initial(fileName: "scores", data: data)
        form.keyIndex = 1; form.sortIndex = 1; form.limit = 5
        form.toggle("city"); form.toggle("points"); form.toggle("points")   // city on; points off then on again
        let binding = form.binding(sourceId: "data-0123456789abcdef0123456789abcdef")
        expectEqual(binding.keyColumn, "team", "key from the picker")
        expectEqual(binding.sortColumn, "points", "sort from the numeric picker")
        expectEqual(binding.limit, 5, "limit")
        expectEqual(binding.columns, ["team", "points", "city"], "selected columns in source order, not click order")
        expectEqual(binding.revision, 1, "a new dataset's first revision")
        expect(binding.isValid, "a valid binding")
        expectEqual(try? form.keyedData(data).rows.first?.key, "Zebras", "the dataset is keyed by the chosen column")
        expect(GraphicsDataTableForm.newSourceId().hasPrefix("data-") && GraphicsDataTableForm.newSourceId().count == 37, "data- + 32 hex")
    }

    static func testCrawlFormDefaultsAndBinding() {
        guard let data = scoresFileOrder() else { expect(false, "scores.csv"); return }
        guard var form = GraphicsDataCrawlForm.initial(fileName: "headlines", data: data) else { expect(false, "a text column exists"); return }
        expectEqual(form.textColumns, ["team", "city"], "text columns only")
        expectEqual(form.column, "team", "first text column")
        expectEqual([form.direction, form.mode], ["left", "loop"], "Left, Loop")
        expectEqual([form.speed, form.gap], [120, 48], "speed 120 px/s, gap 48 px")
        expectEqual(form.limit, 64, "item limit 64")
        form.direction = "right"; form.mode = "single"; form.column = "city"
        let binding = form.binding(sourceId: "data-0123456789abcdef0123456789abcdef")
        expectEqual(binding.columns, ["city"], "one text column")
        expectEqual(binding.crawl, GraphicsCrawlOptions(speed: 120, gap: 48, direction: "right", mode: "single"), "crawl options")
        expect(binding.isValid, "valid")
        expect(GraphicsDataCrawlForm.initial(fileName: "n", data: GraphicsLocalData(columns: [GraphicsDataColumn(id: "n", kind: "number")], rows: [])) == nil,
               "no text column: no crawl form (Windows: The crawl needs a text column.)")
    }

    static func testCreateFromFormRefusalKeepsTheSheet() {
        var sheet = GraphicsDataSheetState(isPresented: true)
        sheet.apply(GraphicsDesignOutcome(ok: false, message: "The graphics library is full.", rowId: nil))
        expect(sheet.isPresented, "a refusal keeps the sheet open (Windows args.Cancel = !result.Ok)")
        expectEqual(sheet.status, "The graphics library is full.", "with the message")
        sheet.apply(GraphicsDesignOutcome(ok: true, message: "Created Scores.", rowId: "scores"))
        expect(!sheet.isPresented, "success closes it")
    }

    /// D8: Windows' bar order; only Data table… and Data crawl… are live before the Builder.
    static func testDesignsBarButtons() {
        expectEqual(GraphicsDesignsBar.buttons.map(\.title),
                    ["Open design", "Duplicate", "Delete", "New design", "Data table…", "Data crawl…", "Remote feeds…"], "Windows' order")
        expectEqual(GraphicsDesignsBar.buttons.filter(\.enabled).map(\.title), ["Data table…", "Data crawl…"], "only the data buttons are enabled")
        expectEqual(GraphicsDesignsBar.buttons.first { $0.title == "Remote feeds…" }?.help, "Arrives with remote feeds", "feeds help")
        expectEqual(GraphicsDesignsBar.buttons.first { $0.title == "New design" }?.help, "Arrives with the Builder", "builder help")
    }
}
