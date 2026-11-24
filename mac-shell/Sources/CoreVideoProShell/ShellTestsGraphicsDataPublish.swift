// Graphics G4-G7 Tasks 10-11 — Operate data actions (load, publish, revision line) and crawl
// control, replayed against transcripts the exporter recorded from the real Windows
// GraphicsPlayoutService (contracts/graphics-shell/data-transcripts.vectors.json).

import Foundation

extension ShellTests {
    static var graphicsDataPublishCases: [(String, () -> Void)] {
        [
            ("graphics-data-publish/transcripts", testDataTranscriptsReplay),
            ("graphics-data-publish/data-line", testDataLine),
            ("graphics-data-publish/publish-carries-typed-text", testPublishCommitsTypedTextFirst),
            ("graphics-crawl/rows-no-longer-refuse", testCrawlRowsNoLongerRefuse),
            ("graphics-crawl/remote-rows-still-refuse", testRemoteRowsStillRefuse),
            ("graphics-crawl/form-defaults-and-sample", testCrawlFormDefaultsAndSample),
            ("graphics-crawl/buttons-enablement", testCrawlButtonsEnablement),
            ("graphics-crawl/program-line", testCrawlProgramLine),
        ]
    }

    /// A model over windows-data.json whose core replays `steps`; sleep advances a stepped clock (no wall time).
    final class SleepCount: @unchecked Sendable { var n = 0 }
    static let dataSleeps = SleepCount()

    @MainActor
    static func dataTranscriptModel(_ vector: JSONObject) -> (GraphicsModel, ScriptedGraphicsCore) {
        let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
        let core = ScriptedGraphicsCore(replaying: expected["steps"] as? [JSONObject] ?? [], initialNode: input["initialNode"] as? JSONObject ?? [:])
        let model = graphicsModel(core)
        model.document = GraphicsDocument(json: documentJSON("windows-data.json"))
        model.documentLoaded = true
        var clock = Date(timeIntervalSince1970: 1_795_867_200)
        model.playout.now = { clock }
        dataSleeps.n = 0
        model.playout.sleep = { [weak core, weak model] ns in
            dataSleeps.n += 1
            clock = clock.addingTimeInterval(Double(ns) / 1e9)
            if let core { model?.observe(snapshot: ["graphics": core.node]) }
        }
        return (model, core)
    }

    @MainActor
    static func runDataIntent(_ model: GraphicsModel, _ intent: JSONObject) -> GraphicsOperationResult? {
        let rowId = intent["rowId"] as? String ?? ""
        switch intent["op"] as? String {
        case "publish": return awaitMain(10) { await model.publishData(rowId: rowId).value }
        case "stage":
            guard let binding = model.document.row(rowId)?.dataBinding.flatMap(GraphicsDataBinding.init(json:)),
                  let data = try? GraphicsLocalData.read(url: repoRoot.appendingPathComponent("contracts/graphics-shell/" + (intent["dataFile"] as? String ?? "")),
                                                         key: binding.keyColumn) else { return nil }
            return awaitMain(10) { await model.stageData(rowId: rowId, binding: binding, data: data).value }
        case "crawl": return awaitMain(10) { await model.controlCrawl(rowId: rowId, action: intent["action"] as? String ?? "").value }
        case "liveFollow": return awaitMain(10) { await model.setLiveFollow(rowId: rowId, enabled: intent["enabled"] as? Bool ?? false).value }
        case "show":
            guard let row = model.document.row(rowId) else { return nil }
            return awaitMain(10) { await model.playout.show(GraphicsPlayout.entry(for: row)) }
        default: return nil
        }
    }

    @MainActor
    static func testDataTranscriptsReplay() {
        var checked = 0
        for vector in loadGraphicsVectors("data-transcripts") {
            let id = vector["id"] as? String ?? "?"
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            let (model, core) = dataTranscriptModel(vector)
            let intent = input["intent"] as? JSONObject ?? [:]
            let result = runDataIntent(model, intent)
            expectEqual(core.replayFaults, [], "\(id): commands as recorded")
            expectEqual(core.commands.count, (expected["steps"] as? [Any])?.count ?? -1, "\(id): command count")
            expectEqual(result?.reason, expected["reason"] as? String, "\(id): reason")
            expectEqual(result?.accepted, expected["accepted"] as? Bool, "\(id): accepted")
            if id == "publish-table-never-adopted" {
                expect((100...101).contains(dataSleeps.n), "\(id): the 5 s adoption budget runs on the injected clock (100 x 50 ms), never wall time (\(dataSleeps.n) sleeps)")
            }
            let after = expected["documentAfter"] as? JSONObject ?? [:]
            if let wantRow = after["row"] as? JSONObject, let row = model.document.row(intent["rowId"] as? String ?? "") {
                expect(jsonEqual(row.fields, wantRow["fields"]), "\(id): row fields after")
                expect(jsonEqual(row.dataBinding, wantRow["dataBinding"]), "\(id): binding after \(row.dataBinding ?? [:])")
            }
            let wantSources = after["dataSources"] as? [JSONObject] ?? []
            let sources = model.document.dataSources
            expectEqual(sources.map(\.id), wantSources.map { $0["id"] as? String ?? "" }, "\(id): datasets after, in Windows' order")
            for (got, want) in zip(sources, wantSources) {
                expectEqual(got.revision, (want["revision"] as? NSNumber)?.int64Value, "\(id): \(got.id) revision")
                expectEqual(got.name, want["name"] as? String, "\(id): \(got.id) name")
                expect(jsonEqual(got.data.json, want["data"]), "\(id): \(got.id) data")
            }
            checked += 1
        }
        expect(checked >= 6, "every data transcript replayed (\(checked))")
    }

    @MainActor
    static func testDataLine() {
        let model = graphicsModel(ScriptedGraphicsCore())
        model.document = GraphicsDocument(json: documentJSON("windows-data.json"))
        expectEqual(model.dataLine(row: model.rows[0]), "Data · published 1 · loaded 2", "table: published 1, source at 2")
        expectEqual(model.dataLine(row: model.rows[1]), "Data · published 1 · loaded 1", "crawl")
        var orphan = model.rows[0]
        var binding = orphan.dataBinding ?? [:]; binding["sourceId"] = "gone"; orphan.json["dataBinding"] = binding
        expectEqual(model.dataLine(row: orphan), "Data · published 1 · loaded unavailable", "a missing dataset")
    }

    /// Windows flushes the inspector's typed text before publishing (_rowText.Flush()); on the Mac every
    /// edit is in the document at once, so a value typed just before Publish rides the Program update.
    @MainActor
    static func testPublishCommitsTypedTextFirst() {
        guard let vector = loadGraphicsVectors("data-transcripts").first(where: { $0["id"] as? String == "publish-table-on-program" }) else {
            expect(false, "vector"); return
        }
        let (model, core) = dataTranscriptModel(vector)
        core.react = ScriptedGraphicsCore.instancesReaction()
        model.setField("leaderboard", "heading", "Typed just now")
        _ = awaitMain(10) { await model.publishData(rowId: "leaderboard").value }
        let update = core.commands.first { $0["type"] as? String == "graphics-instance-update" }?["graphics"] as? JSONObject
        expectEqual((update?["fields"] as? JSONObject)?["heading"] as? String, "Typed just now", "the typed value is in the Program update")
    }

    @MainActor
    static func testCrawlRowsNoLongerRefuse() {
        let model = graphicsModel(ScriptedGraphicsCore())
        model.document = GraphicsDocument(json: documentJSON("windows-data.json"))
        let line = model.lines().first { $0.row.id == "headlines" }
        expect(line.map { if case .failed = $0.state { return false }; return true } == true, "the live-follow crawl row is not refused: \(String(describing: line?.state))")
        expect(line?.canCue == true, "Cue is enabled")
    }

    @MainActor
    static func testRemoteRowsStillRefuse() {
        let model = graphicsModel(ScriptedGraphicsCore())
        var json = documentJSON("windows-data.json")
        var playlist = json["playlist"] as? [JSONObject] ?? []
        var binding = playlist[0]["dataBinding"] as? JSONObject ?? [:]
        binding["remote"] = true
        playlist[0]["dataBinding"] = binding
        json["playlist"] = playlist
        model.document = GraphicsDocument(json: json)
        let line = model.lines().first { $0.row.id == "leaderboard" }
        expectEqual(line?.state, .failed(GraphicsPlayout.unsupportedDataText("Leaderboard")), "a remote feed row keeps the G1-G3 refusal (plan D7)")
        expect(line?.canShow == false, "and cannot Show")
    }

    static func testCrawlFormDefaultsAndSample() {
        guard let data = try? GraphicsLocalData.read(url: repoRoot.appendingPathComponent("contracts/graphics-shell/data/crawl.csv"), key: nil),
              let form = GraphicsDataCrawlForm.initial(fileName: "crawl", data: data) else { expect(false, "crawl.csv form"); return }
        expectEqual(data.rows.count, 69, "fixture invariant: 69 rows, one of them an empty cell")
        expectEqual(form.sample(data).components(separatedBy: "\n").first, "64 items in source order", "the 64-item cap applies after the empty filter")
        expect(form.sample(data).hasSuffix("Headline 1  •  Headline 2  •  Headline 3  •  Headline 4"), "the first four items, joined as Windows")
    }

    static func testCrawlButtonsEnablement() {
        expectEqual(["pause", "resume", "restart"].map { GraphicsCrawlControls.enabled($0, playback: "playing") }, [true, false, true], "playing")
        expectEqual(["pause", "resume", "restart"].map { GraphicsCrawlControls.enabled($0, playback: "paused") }, [false, true, true], "paused")
    }

    static func testCrawlProgramLine() {
        let state = GraphicsState(node: ["schemaVersion": 1, "coreEpoch": "e", "instances": [[
            "instanceId": "playlist-headlines", "revision": 5, "programRevision": 4, "visibility": "on-air", "crawlPlayback": "playing",
            "crawlPendingRevision": 5, "preparation": "preparing", "fields": ["crawl-revision": "2"], "programFields": ["crawl-revision": "1"],
        ]]])
        guard let live = state.instance("playlist-headlines") else { expect(false, "instance"); return }
        expectEqual(GraphicsCrawlControls.programLine(live), "Crawl playing · Program data 1 · pending 2 (preparing)", "Windows' crawl line")
        var quiet = live
        quiet.crawlPendingRevision = 0; quiet.warning = "Font fallback"
        expectEqual(GraphicsCrawlControls.programLine(quiet), "Crawl playing · Program data 1 · Font fallback", "no pending, a warning")
        quiet.programFields = nil
        expectEqual(GraphicsCrawlControls.programLine(quiet), "Crawl playing · Program data unknown · Font fallback", "unknown Program data")
    }
}
