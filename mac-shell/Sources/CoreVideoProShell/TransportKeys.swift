import AppKit

// Keyboard Take / FTB (audit §5, Windows #1206 Views/StudioWorkspace.TransportHotkeys.cs).
//
// Windows routes Space on PreviewKeyDown so it beats a focused button's own Space
// activation, ignores text-entry focus and any modifier, consumes Space even when
// Take is unavailable, and fires only on the initial press. Ctrl+Enter is an
// alternate Take and Ctrl+Shift+B is FTB; Studio tab only. macOS uses Cmd for
// Ctrl (owner call); Space stays Space. SwiftUI `.keyboardShortcut` would lose
// to a focused control, so an NSEvent local monitor decides first.

struct TransportKeyEvent: Equatable {
    /// `function(n)` = Fn key F1...F12 (graphics hotkeys; transport ignores them).
    enum Kind: Equatable { case space, returnKey, b, function(Int), other }
    struct Modifiers: OptionSet, Equatable {
        let rawValue: Int
        static let command = Modifiers(rawValue: 1)
        static let shift = Modifiers(rawValue: 2)
        static let option = Modifiers(rawValue: 4)
        static let control = Modifiers(rawValue: 8)
    }
    var kind: Kind
    var modifiers: Modifiers
    var isRepeat: Bool
    var isStudioTab: Bool
    var isTextEntryFocus: Bool
}

enum TransportKeyDecision: Equatable {
    /// Not ours: let the responder chain have it.
    case ignore
    /// Ours but does nothing (key repeat): consume so nothing else reacts.
    case swallow
    /// Space: Take. Consumed even when Take is unavailable.
    case take
    /// Cmd+Return: Take only if available, otherwise passes through.
    case alternateTake
    case toggleBlackout
}

enum TransportKeys {
    /// Pure routing decision (Windows precedence).
    static func decide(_ event: TransportKeyEvent) -> TransportKeyDecision {
        guard event.isStudioTab, !event.isTextEntryFocus else { return .ignore }
        let wanted: TransportKeyDecision
        switch event.kind {
        case .space:
            // Any modifier on Space belongs to someone else.
            guard event.modifiers.isEmpty else { return .ignore }
            wanted = .take
        case .returnKey:
            guard event.modifiers == [.command] else { return .ignore }
            wanted = .alternateTake
        case .b:
            guard event.modifiers == [.command, .shift] else { return .ignore }
            wanted = .toggleBlackout
        case .function, .other:
            return .ignore
        }
        // Initial press only: a held key never re-Takes or flaps FTB.
        return event.isRepeat ? .swallow : wanted
    }

    /// Text entry = an EDITABLE field, or the shared field editor behind one.
    /// Read-only selectable text keeps Space (Windows ignores text-entry controls only).
    static func isTextEntry(_ responder: NSResponder?) -> Bool {
        var current = responder
        while let item = current {
            if let text = item as? NSText, text.isEditable { return true }
            if let field = item as? NSTextField, field.isEditable { return true }
            current = item.nextResponder
        }
        return false
    }

    static func event(from nsEvent: NSEvent, isStudioTab: Bool, firstResponder: NSResponder?) -> TransportKeyEvent {
        var mods: TransportKeyEvent.Modifiers = []
        let flags = nsEvent.modifierFlags
        if flags.contains(.command) { mods.insert(.command) }
        if flags.contains(.shift) { mods.insert(.shift) }
        if flags.contains(.option) { mods.insert(.option) }
        if flags.contains(.control) { mods.insert(.control) }
        let kind: TransportKeyEvent.Kind
        switch nsEvent.keyCode {
        case 49: kind = .space
        case 36, 76: kind = .returnKey
        case let code where GraphicsHotkeys.keyCodes[code] != nil: kind = .function(GraphicsHotkeys.keyCodes[code]!)
        default:
            kind = nsEvent.charactersIgnoringModifiers?.lowercased() == "b" ? .b : .other
        }
        return TransportKeyEvent(kind: kind, modifiers: mods, isRepeat: nsEvent.isARepeat,
                                 isStudioTab: isStudioTab,
                                 isTextEntryFocus: isTextEntry(firstResponder))
    }
}

/// Installs the local key monitor only while the Studio tab is active.
@MainActor
final class TransportHotkeyMonitor {
    private var token: Any?
    private var spaceCaptured = false
    var isInstalled: Bool { token != nil }

    /// `handle` returns true when it consumed the key.
    func setActive(_ active: Bool, handle: @escaping (NSEvent) -> Bool) {
        if !active { remove(); return }
        guard token == nil else { return }
        token = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp]) { [weak self] event in
            guard let self else { return event }
            // Only the main window: pop-outs, sheets and the quit panel keep their keys.
            guard MainWindowTracker.isMain(event.window) else { return event }
            if event.type == .keyUp {
                // Windows OnTransportPreviewKeyUp: swallow the release of a captured Space.
                if event.keyCode == 49, self.spaceCaptured { self.spaceCaptured = false; return nil }
                return event
            }
            let consumed = handle(event)
            if consumed, event.keyCode == 49 { self.spaceCaptured = true }
            return consumed ? nil : event
        }
    }

    func remove() {
        if let token { NSEvent.removeMonitor(token) }
        token = nil
        spaceCaptured = false
    }
}
