// GraphicsDataDesigns — data designs before the Builder (G4-G7 Task 8, plan D4/D6):
// design template ids (port of MC/Models/GraphicsDesign.cs GraphicsDesignIds) and
// the saved-design check the document store applies (GraphicsDocumentStore.ValidDesign).

import Foundation

enum GraphicsDesignIds {
    static let maxLength = 64, maxDesigns = 32, maxNameLength = 80
    private static let prefix = "design-"
    private static let slugBudget = maxLength - 7 - 7   // "design-" + slug + "-" + 6 hex

    /// The native id rule: [a-z0-9-], leading letter, at most 64 characters.
    static func isNativeId(_ id: String) -> Bool {
        guard (1...maxLength).contains(id.utf16.count), let first = id.unicodeScalars.first, ("a"..."z").contains(first) else { return false }
        return id.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "-" }
    }

    /// ^design-[a-z0-9]+(-[a-z0-9]+)*-[0-9a-f]{6}$, at most 64 characters.
    static func isDesignTemplateId(_ id: String) -> Bool {
        guard id.utf16.count <= maxLength, id.hasPrefix(prefix) else { return false }
        let parts = id.dropFirst(prefix.count).split(separator: "-", omittingEmptySubsequences: false)
        guard parts.count >= 2, let last = parts.last, last.count == 6,
              last.unicodeScalars.allSatisfy({ ("0"..."9").contains($0) || ("a"..."f").contains($0) }) else { return false }
        return parts.dropLast().allSatisfy { part in
            !part.isEmpty && part.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) }
        }
    }

    static func slug(_ name: String) -> String {
        var out = ""
        for scalar in GraphicsNamesList.lowerInvariant(name).unicodeScalars {
            if ("a"..."z").contains(scalar) || ("0"..."9").contains(scalar) { out.unicodeScalars.append(scalar) }
            else if !out.isEmpty && !out.hasSuffix("-") { out += "-" }
        }
        if out.count > slugBudget { out = String(out.prefix(slugBudget)) }
        out = out.trimmingCharacters(in: CharacterSet(charactersIn: "-"))
        return out.isEmpty ? "untitled" : out
    }

    struct Exhausted: Error {}

    static func newTemplateId(name: String, existing: Set<String>, hex6: () -> String) throws -> String {
        let base = slug(name)
        for _ in 0..<1000 {
            let id = prefix + base + "-" + hex6()
            if isDesignTemplateId(id) && !existing.contains(id) { return id }
        }
        throw Exhausted()
    }

    /// Six random lower-case hex digits (production hex6).
    static func randomHex6() -> String { String(format: "%06x", UInt32.random(in: 0..<0x1000000)) }

    /// GraphicsDocumentStore.ValidDesign.
    static func validDesign(_ d: JSONObject) -> Bool {
        typealias V = GraphicsValue
        guard let designId = d["designId"] as? String, (1...64).contains(designId.utf16.count),
              let name = d["name"] as? String, (1...maxNameLength).contains(name.utf16.count),
              let family = d["family"] as? String, (1...32).contains(family.utf16.count),
              let templateId = d["templateId"] as? String, isDesignTemplateId(templateId),
              let version = V.int(d, "templateVersion", 0), version >= 1,
              let definition = d["definition"] as? JSONObject,
              let bytes = try? JSONSerialization.data(withJSONObject: definition), bytes.count <= 256 * 1024,
              let assets = V.array(d, "assets", []), assets.count <= 32 else { return false }
        var seen = Set<String>()
        for item in assets {
            guard let asset = item as? JSONObject, let assetId = asset["assetId"] as? String, isNativeId(assetId), seen.insert(assetId).inserted,
                  let sha = asset["sha256"] as? String, sha.utf16.count == 64, sha.allSatisfy(\.isHexDigit),
                  let path = asset["relativePath"] as? String, (1...260).contains(path.utf16.count), !path.hasPrefix("/"),
                  path.split(separator: "/", omittingEmptySubsequences: false).flatMap({ $0.split(separator: "\\", omittingEmptySubsequences: false) })
                    .allSatisfy({ !$0.isEmpty && $0 != ".." && !$0.contains("\0") && !$0.contains(":") })
            else { return false }
        }
        return true
    }
}
