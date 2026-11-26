// GraphicsHotkeys — pure F-key routing for graphics playout (graphics G3, plan D7).
// Port of W/Services/GraphicsHotkeyRouter.cs:17-34: nothing routes while a text
// field has focus; F1-F12 unmodified play the row carrying that hotkey (a Guest
// row plays the Program subject); Shift+F12 plays everything out; Shift+F1-F11
// cue the Shot Layout with that hotkey (G4-G7 Task 16);
// any other modifier routes nowhere. In-app only (the existing key monitor,
// Studio and Graphics tabs); no global hotkeys.

import Foundation

enum GraphicsHotkeyRoute: Equatable {
    /// Play in (Show) the row; `guest` = resolve the Program subject's entry.
    case playIn(rowId: String, guest: Bool)
    case playOutAll
    case layoutCue(layoutId: String)
}

enum GraphicsHotkeys {
    /// macOS virtual key codes for F1...F12.
    static let keyCodes: [UInt16: Int] = [122: 1, 120: 2, 99: 3, 118: 4, 96: 5, 97: 6, 98: 7, 100: 8, 101: 9, 109: 10, 103: 11, 111: 12]

    static func route(function number: Int, modifiers: TransportKeyEvent.Modifiers, textFocus: Bool,
                      rows: [GraphicsRow], layoutHotkeys: [String: String] = [:]) -> GraphicsHotkeyRoute? {
        guard !textFocus, (1...12).contains(number) else { return nil }
        if modifiers == [.shift] {
            if number == 12 { return .playOutAll }
            return layoutHotkeys["Shift+F\(number)"].map { .layoutCue(layoutId: $0) }
        }
        guard modifiers.isEmpty, let row = rows.first(where: { $0.hotkey == "F\(number)" }) else { return nil }
        return .playIn(rowId: row.id, guest: row.kind == GraphicsRow.guest)
    }
}

extension GraphicsModel {
    /// "Shift+Fn" -> layout id, for the layouts that carry a hotkey.
    var layoutHotkeys: [String: String] {
        Dictionary(document.shotLayouts.filter { !$0.hotkey.isEmpty }.map { ($0.hotkey, $0.id) }, uniquingKeysWith: { a, _ in a })
    }

    static func noEntry(_ rowId: String) -> String {
        rowId.isEmpty ? "Choose a playlist row first."
            : "Playlist row '\(rowId)' has nothing to play. Add a source in Sources or choose another row."
    }

    /// GraphicsPlayoutActions.ResolveEntry: a plain row is its one entry; a Guest
    /// row resolves to the Program subject's entry, else its first entry.
    func resolveEntry(rowId: String, subjectSourceId: String?) -> GraphicsPlaylistEntry? {
        guard let row = rows.first(where: { $0.id == rowId }) else { return nil }
        if row.hasVariants {   // names list: the next line after the one on Program (StudioControlSurface:799-802)
            return GraphicsPlayout.nextVariant(row, onProgramSourceIds: strip.onProgram.filter { $0.rowId == row.id }.map(\.sourceId))
        }
        guard row.kind == GraphicsRow.guest else { return GraphicsPlayout.entry(for: row) }
        let entries = guestEntries(row)
        return entries.first { subjectSourceId != nil && $0.sourceId == subjectSourceId } ?? entries.first
    }

    /// Runs a routed hotkey. Returns false when the route does nothing here.
    @discardableResult
    func perform(_ route: GraphicsHotkeyRoute, subjectSourceId: String?) -> Bool {
        switch route {
        case .playIn(let rowId, _):
            guard let entry = resolveEntry(rowId: rowId, subjectSourceId: subjectSourceId) else {
                status = Self.noEntry(rowId)
                return true
            }
            show(entry)
            return true
        case .playOutAll:
            clearAll()
            return true
        case .layoutCue(let layoutId):
            cueLayout(layoutId)   // graphics.layout.cue only: Windows selects a layout on a button click, not a hotkey
            return true
        }
    }
}
