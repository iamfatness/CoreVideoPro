// Graphics G2 Task 6 — the graphics document store.
// Vectors: contracts/graphics-shell/document.vectors.json (GraphicsShellVectorTests.DocumentVectors).

import Foundation

extension ShellTests {
    static var graphicsDocumentCases: [(String, () -> Void)] {
        [
            ("graphics-doc/row-vectors", testGraphicsRowVectors),
            ("graphics-doc/load-vectors", testGraphicsDocumentLoadVectors),
            ("graphics-doc/windows-show-round-trips", testGraphicsWindowsShowRoundTrips),
            ("graphics-doc/save-keeps-bak-and-replaces", testGraphicsDocumentSaveKeepsBackup),
            ("graphics-doc/read-only-refuses-save", testGraphicsDocumentReadOnlyRefusesSave),
            ("graphics-doc/external-change-refuses-save", testGraphicsDocumentExternalChangeRefusesSave),
            ("graphics-doc/v2-first-save-keeps-v2-bak", testGraphicsDocumentV2KeepsV2Backup),
            ("graphics-doc/recovered-save-keeps-corrupt", testGraphicsDocumentRecoveredKeepsCorrupt),
            ("graphics-doc/oversize-is-unavailable", testGraphicsDocumentOversize),
            ("graphics-doc/invalid-row-never-saved", testGraphicsDocumentInvalidRowNeverSaved),
            ("graphics-doc/unique-keys-scanner", testGraphicsUniqueKeysScanner),
        ]
    }

    static func graphicsTempDir() -> String {
        let dir = (NSTemporaryDirectory() as NSString).appendingPathComponent("cvp-graphics-doc-" + UUID().uuidString)
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir
    }

    static func graphicsWrite(_ text: String, _ path: String) {
        try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: path, contents: Data(text.utf8))
    }

    static func graphicsReadJSON(_ path: String) -> JSONObject? {
        guard let data = FileManager.default.contents(atPath: path) else { return nil }
        return try? JSONSerialization.jsonObject(with: data) as? JSONObject
    }

    // ── vectors ────────────────────────────────────────────────────────────

    static func testGraphicsRowVectors() {
        var checked = 0
        for row in loadGraphicsVectors("document") where row["operation"] as? String == "validateRow" {
            let id = row["id"] as? String ?? "?"
            guard let json = (row["input"] as? JSONObject)?["row"] as? JSONObject, let valid = (row["expected"] as? JSONObject)?["valid"] as? Bool
            else { expect(false, "row vector \(id) malformed"); continue }
            expectEqual(GraphicsRow.validRow(json), valid, "document \(id): GraphicsPlaylistRow.IsValid")
            checked += 1
        }
        expect(checked >= 40, "every row vector replayed (\(checked))")
    }

    static func testGraphicsDocumentLoadVectors() {
        var checked = 0
        for row in loadGraphicsVectors("document") where row["operation"] as? String == "load" {
            let id = row["id"] as? String ?? "?"
            guard let input = row["input"] as? JSONObject, let expected = row["expected"] as? JSONObject
            else { expect(false, "load vector \(id) malformed"); continue }
            let path = (graphicsTempDir() as NSString).appendingPathComponent("default.json")
            defer { try? FileManager.default.removeItem(atPath: (path as NSString).deletingLastPathComponent) }
            if let primary = input["primary"] as? String { graphicsWrite(primary, path) }
            if let file = input["primaryFile"] as? String {
                try? FileManager.default.copyItem(at: repoRoot.appendingPathComponent("contracts/graphics-shell/" + file), to: URL(fileURLWithPath: path))
            }
            if let backup = input["backup"] as? String { graphicsWrite(backup, path + ".bak") }
            let load = GraphicsDocumentStore(path: path).load()
            expectEqual(load.readOnly, expected["readOnly"] as? Bool ?? false, "load \(id): readOnly")
            expectEqual(load.recoveredBackup, expected["recoveredBackup"] as? Bool ?? false, "load \(id): recovered")
            expectEqual(load.status, expected["status"] as? String ?? "", "load \(id): status")
            expectEqual(load.document.showId, expected["showId"] as? String ?? "", "load \(id): showId")
            expectEqual(load.document.authority, expected["authority"] as? String ?? "", "load \(id): authority")
            expectEqual(load.document.playlist.map(\.id), expected["playlistIds"] as? [String] ?? [], "load \(id): playlist")
            checked += 1
        }
        expect(checked >= 25, "every load vector replayed (\(checked))")
    }

    // ── behaviour ──────────────────────────────────────────────────────────

    /// A show written by the REAL Windows store loads, re-saves, and the saved
    /// JSON equals the original: cues, template pins, columns, layouts and an
    /// unknown future section all survive a Mac save (plan D3).
    static func testGraphicsWindowsShowRoundTrips() {
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("default.json")
        let source = repoRoot.appendingPathComponent("contracts/graphics-shell/documents/windows-show.json")
        try? FileManager.default.copyItem(at: source, to: URL(fileURLWithPath: path))
        let original = graphicsReadJSON(path)
        let store = GraphicsDocumentStore(path: path)
        let load = store.load()
        expect(!load.readOnly, "the Windows show loads writable")
        expectEqual(load.document.templates.map(\.templateId), ["centered-title"], "template pin read")
        do { try store.save(load.document) } catch { expect(false, "save failed: \(error)") }
        expect(jsonEqual(graphicsReadJSON(path), original), "re-saved document equals the Windows original, unknown keys included")
        expect((graphicsReadJSON(path)?["futureSection"] as? JSONObject)?["keep"] as? Bool == true, "the future section is kept")
        // Edit one row: everything else is still untouched.
        var edited = load.document
        var rows = edited.playlist
        rows[1].name = "Tonight"
        edited.playlist = rows
        do { try store.save(edited) } catch { expect(false, "edit save failed: \(error)") }
        var expectedJSON = original ?? [:]
        var expectedRows = expectedJSON["playlist"] as? [JSONObject] ?? []
        expectedRows[1]["name"] = "Tonight"
        expectedJSON["playlist"] = expectedRows
        expect(jsonEqual(graphicsReadJSON(path), expectedJSON), "an edit changes only that row's field")
        expect(jsonEqual(graphicsReadJSON(path + ".bak"), original), "the previous save is the .bak")
    }

    static func testGraphicsDocumentSaveKeepsBackup() {
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("nested/default.json")
        let store = GraphicsDocumentStore(path: path)
        let fresh = store.load()
        expectEqual(fresh.status, "New graphics workspace; automation is unarmed.", "missing file is a new document")
        var document = fresh.document
        document.playlist = [GraphicsRow(json: ["id": "a", "name": "A"])]
        do { try store.save(document) } catch { expect(false, "first save: \(error)") }
        expect(FileManager.default.fileExists(atPath: path), "first save creates the directory and file")
        expect(!FileManager.default.fileExists(atPath: path + ".bak"), "no backup of nothing")
        document.playlist = [GraphicsRow(json: ["id": "b", "name": "B"])]
        do { try store.save(document) } catch { expect(false, "second save: \(error)") }
        expectEqual((graphicsReadJSON(path)?["playlist"] as? [JSONObject])?.first?["id"] as? String, "b", "primary replaced")
        expectEqual((graphicsReadJSON(path + ".bak")?["playlist"] as? [JSONObject])?.first?["id"] as? String, "a", "previous primary kept as .bak")
        let leftovers = (try? FileManager.default.contentsOfDirectory(atPath: (path as NSString).deletingLastPathComponent))?.filter { $0.hasSuffix(".tmp") } ?? []
        expect(leftovers.isEmpty, "no temporary file left behind")
    }

    static func testGraphicsDocumentReadOnlyRefusesSave() {
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("default.json")
        let newer = #"{"schemaVersion":4,"playlist":[],"x":1}"#
        graphicsWrite(newer, path)
        let store = GraphicsDocumentStore(path: path)
        expect(store.load().readOnly, "schema 4 is read-only")
        do { try store.save(GraphicsDocument()); expect(false, "a read-only save must throw") }
        catch { expectEqual(GraphicsDocumentStore.message(error), "Graphics document is read-only; original retained.", "refusal text") }
        expectEqual(String(data: FileManager.default.contents(atPath: path) ?? Data(), encoding: .utf8), newer, "the newer original is byte-identical")
    }

    static func testGraphicsDocumentExternalChangeRefusesSave() {
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("default.json")
        graphicsWrite(#"{"schemaVersion":3}"#, path)
        let store = GraphicsDocumentStore(path: path)
        let load = store.load()
        graphicsWrite(#"{"schemaVersion":3,"showId":"other"}"#, path)   // Windows (or a sync tool) wrote meanwhile
        do { try store.save(load.document); expect(false, "save over an external change must throw") }
        catch {
            expectEqual(GraphicsDocumentStore.message(error), "Graphics file changed outside this workspace. Reload before saving.", "refusal text")
        }
        expectEqual(graphicsReadJSON(path)?["showId"] as? String, "other", "the external write survives")
        _ = store.load()
        do { try store.save(load.document) } catch { expect(false, "after a reload the save succeeds: \(error)") }
    }

    static func testGraphicsDocumentV2KeepsV2Backup() {
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("default.json")
        let v2 = #"{"schemaVersion":2,"playlist":[{"id":"a","name":"A"}]}"#
        graphicsWrite(v2, path)
        let store = GraphicsDocumentStore(path: path)
        let load = store.load()
        expectEqual(load.document.schemaVersion, 3, "v2 loads as 3")
        do { try store.save(load.document) } catch { expect(false, "save: \(error)") }
        expectEqual(String(data: FileManager.default.contents(atPath: path + ".v2.bak") ?? Data(), encoding: .utf8), v2, "first save keeps the v2 file once")
        expectEqual(graphicsReadJSON(path)?["schemaVersion"] as? Int, 3, "primary is schema 3")
        graphicsWrite("replaced", path + ".v2.bak")
        do { try store.save(load.document) } catch { expect(false, "second save: \(error)") }
        expectEqual(String(data: FileManager.default.contents(atPath: path + ".v2.bak") ?? Data(), encoding: .utf8), "replaced",
                    "a later save never touches the .v2.bak")
    }

    static func testGraphicsDocumentRecoveredKeepsCorrupt() {
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("default.json")
        let corrupt = #"{"schemaVersion":3,"playlist":[{"id":"a","name":""}]}"#
        graphicsWrite(corrupt, path)
        graphicsWrite(#"{"schemaVersion":3,"playlist":[{"id":"b","name":"B"}]}"#, path + ".bak")
        let store = GraphicsDocumentStore(path: path)
        let load = store.load()
        expect(load.recoveredBackup, "recovered from .bak")
        do { try store.save(load.document) } catch { expect(false, "save: \(error)") }
        expectEqual(String(data: FileManager.default.contents(atPath: path + ".corrupt") ?? Data(), encoding: .utf8), corrupt,
                    "the unreadable primary is kept as .corrupt")
        expectEqual((graphicsReadJSON(path + ".bak")?["playlist"] as? [JSONObject])?.first?["id"] as? String, "b",
                    "and never becomes the backup")
    }

    static func testGraphicsDocumentOversize() {
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("default.json")
        graphicsWrite("{\"schemaVersion\":3,\"pad\":\"" + String(repeating: "x", count: 2 * 1024 * 1024) + "\"}", path)
        let load = GraphicsDocumentStore(path: path).load()
        expect(load.readOnly, "an oversized primary is read-only")
        expectEqual(load.status, "Graphics document unavailable: Graphics document exceeds the 2 MiB limit.", "as Windows says")
    }

    static func testGraphicsDocumentInvalidRowNeverSaved() {
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("default.json")
        let store = GraphicsDocumentStore(path: path)
        var document = store.load().document
        document.playlist = [GraphicsRow(json: ["id": "a", "name": "A"]), GraphicsRow(json: ["id": "a", "name": "B"])]
        do { try store.save(document); expect(false, "duplicate ids must not save") }
        catch { expectEqual(GraphicsDocumentStore.message(error), "Invalid saved graphics playlist.", "refusal text") }
        expect(!FileManager.default.fileExists(atPath: path), "nothing written")
    }

    static func testGraphicsUniqueKeysScanner() {
        expect(GraphicsJSON.uniqueKeys(Data(#"{"a":{"a":1},"b":[{"a":1},{"a":2}],"c":"{\"a\":1,\"a\":2}"}"#.utf8)), "same key in different objects is fine")
        expect(!GraphicsJSON.uniqueKeys(Data(#"{"x":[{"k":1,"k":2}]}"#.utf8)), "a duplicate deep in an array is caught")
        expect(!GraphicsJSON.uniqueKeys(Data(#"{"k":1,"k":2}"#.utf8)), "an escaped duplicate is caught")
    }
}
