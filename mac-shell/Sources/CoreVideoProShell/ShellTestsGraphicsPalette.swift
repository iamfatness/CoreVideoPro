// Graphics styles slice 1 — the show palette model. Vectors:
// contracts/graphics-shell/palette.vectors.json, written by
// GraphicsShellVectorTests.PaletteVectors from the real C# GraphicsPalette.

import Foundation

extension ShellTests {
    static var graphicsPaletteCases: [(String, () -> Void)] {
        [
            ("graphics-palette/vectors", testPaletteVectors),
            ("graphics-palette/document-round-trip", testPaletteDocumentRoundTrip),
            ("graphics-palette/pre-palette-show-resolves-like-windows", testPrePaletteShowResolvesLikeWindows),
        ]
    }

    static func testPaletteVectors() {
        var counts: [String: Int] = [:]
        for vector in loadGraphicsVectors("palette") {
            let id = vector["id"] as? String ?? "?", operation = vector["operation"] as? String ?? ""
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            counts[operation, default: 0] += 1
            switch operation {
            case "resolve":
                let palette = GraphicsPalette(json: input["palette"] as? JSONObject ?? [:])
                let overrides = (input["rowOverrides"] as? JSONObject)?.compactMapValues { $0 as? String }
                expectEqual(palette.resolve(overrides), expected.compactMapValues { $0 as? String }, "\(id)")
            case "validate":
                expectEqual(GraphicsPalette(json: input["palette"] as? JSONObject ?? [:]).isValid, expected["valid"] as? Bool, "\(id)")
            case "normalize":
                expectEqual(GraphicsPalette.normalize(input["text"] as? String ?? ""), expected["value"] as? String, "\(id)")
            case "contrast":
                let ratio = GraphicsPalette.contrast(input["a"] as? String ?? "", input["b"] as? String ?? "")
                let want = (expected["ratio"] as? NSNumber)?.doubleValue ?? -1
                expect(abs(ratio - want) < 1e-5, "\(id): \(ratio) vs \(want)")
            case "fromBrandKit":
                let palette = GraphicsPaletteMigration.fromBrandKit(brandColor: input["brandColor"] as? String ?? "", accentColor: input["accentColor"] as? String ?? "",
                                                                    backgroundColor: input["backgroundColor"] as? String ?? "", stock: input["stock"] as? Bool ?? false)
                expectEqual(palette.colors, (expected["colors"] as? JSONObject ?? [:]).compactMapValues { $0 as? String }, "\(id)")
            case "migrateRow":
                let overrides = GraphicsPaletteMigration.migratedOverrides(templateId: input["templateId"] as? String ?? "", style: input["style"] as? JSONObject,
                                                                           brandOverrides: nil, palette: GraphicsPalette(json: input["palette"] as? JSONObject ?? [:]))
                expectEqual(overrides, (expected["brandOverrides"] as? JSONObject)?.compactMapValues { $0 as? String }, "\(id)")
            default:
                expect(false, "\(id): unknown operation \(operation)")
            }
        }
        for (operation, minimum) in ["resolve": 5, "validate": 7, "normalize": 8, "contrast": 3, "fromBrandKit": 2, "migrateRow": 4] {
            expect((counts[operation] ?? 0) >= minimum, "palette \(operation) vectors replayed (\(counts[operation] ?? 0))")
        }
    }

    static func testPaletteDocumentRoundTrip() {
        var document = GraphicsDocument()
        expect(document.palette == nil, "a new document has no palette section")
        document.palette = GraphicsPalette.stock.with(GraphicsPalette.teamA, "#C8102EFF")
        paletteExpectNoThrow({ try GraphicsDocument.validate(document.json) }, "a valid palette passes document validation")
        var row = GraphicsRow(json: ["id": "title", "kind": "title", "name": "Title", "templateId": "centered-title", "templateVersion": 1])
        row.brandOverrides = [GraphicsPalette.accent: "#E63946FF"]
        expect(row.isValid, "a row with valid overrides is valid")
        document.playlist = [row]
        expectEqual(document.resolvedPalette(for: row)[GraphicsPalette.accent], "#E63946FF", "row override wins")
        expectEqual(document.resolvedPalette(for: row)[GraphicsPalette.teamA], "#C8102EFF", "show palette team colour")
        var bad = row; bad.json["brandOverrides"] = [GraphicsPalette.accent: "#E63946"]
        expect(!bad.isValid, "a six-digit override is refused")
        document.palette = GraphicsPalette.stock.with(GraphicsPalette.primary, GraphicsPalette.auto)
        paletteExpectThrows({ try GraphicsDocument.validate(document.json) }, "Invalid graphics palette.")
        // Unknown palette keys a newer build wrote survive a round trip.
        var future = GraphicsPalette.stock; future.json["presets"] = ["navy"]
        document.palette = future
        expectEqual((document.palette?.json["presets"] as? [String]) ?? [], ["navy"], "unknown palette keys kept")
    }

    static func testPrePaletteShowResolvesLikeWindows() {
        // Windows migrates a pre-palette show at load; the Mac resolves the same answer on read.
        let stockLook: JSONObject = ["nameColor": "#FFFFFF", "titleColor": "#E1E9F2", "backgroundColor": "#172433", "accentColor": "#44C1A1"]
        var custom = stockLook; custom["accentColor"] = "#E63946"
        let document = GraphicsDocument(json: ["schemaVersion": 3, "playlist": [
            ["id": "follows", "kind": "title", "name": "Follows", "templateId": "centered-title", "templateVersion": 1, "style": stockLook],
            ["id": "custom", "kind": "title", "name": "Custom", "templateId": "centered-title", "templateVersion": 1, "style": custom]]])
        let rows = document.playlist
        expectEqual(document.resolvedPalette(for: rows[0]), GraphicsPalette.stock.resolve(), "a stock-look row follows the palette")
        expectEqual(document.resolvedPalette(for: rows[1])[GraphicsPalette.accent], "#E63946FF", "a customised row keeps its accent")
        expect(document.palette == nil, "resolving never writes a palette into the document")
    }

    private static func paletteExpectNoThrow(_ body: () throws -> Void, _ message: String) {
        do { try body() } catch { expect(false, "\(message): \(error)") }
    }
    private static func paletteExpectThrows(_ body: () throws -> Void, _ text: String) {
        do { try body(); expect(false, "expected \(text)") } catch let error as GraphicsDocumentError { expectEqual(error.message, text, "error text") }
        catch { expect(false, "unexpected \(error)") }
    }
}
