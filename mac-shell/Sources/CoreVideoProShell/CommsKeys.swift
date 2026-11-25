// Comms panel keys (Windows CommsPage.xaml.cs:91-159): digits 1-9 (main row and
// keypad) key the first nine reachable cells, Space is ALL CALL. Panel-scoped:
// only on the Comms tab, only while the desk is visible, only in the main
// window, never with a modifier, never while a text field has focus. An
// auto-repeat is swallowed (it must not re-toggle a latch), and a key-up
// releases only a key whose down this set recorded. Pure, so the harness
// pins every rule; CommsKeyMonitor is the NSEvent plumbing.

import AppKit

struct CommsKeyEvent: Equatable {
    enum Kind: Hashable { case space, digit(Int), other }
    var kind: Kind
    var down: Bool
    var modifiers: TransportKeyEvent.Modifiers = []
    var isRepeat = false
    var isCommsTab = true
    var deskVisible = true
    var isTextEntryFocus = false
}

enum CommsKeyDecision: Equatable { case ignore, swallow, allPress, allRelease, press(slot: Int), release(slot: Int) }

struct CommsKeys {
    /// Keys held down, with the slot each digit pressed (released by that
    /// slot even if the cell list moved under the key meanwhile).
    private(set) var down: [CommsKeyEvent.Kind: Int] = [:]

    mutating func decide(_ e: CommsKeyEvent, cellForDigit: (Int) -> Int?) -> CommsKeyDecision {
        if !e.down {
            guard let slot = down.removeValue(forKey: e.kind) else { return .ignore }
            switch e.kind {
            case .space: return .allRelease
            case .digit: return slot >= 0 ? .release(slot: slot) : .swallow
            case .other: return .ignore
            }
        }
        guard e.isCommsTab, e.deskVisible, !e.isTextEntryFocus, e.modifiers.isEmpty else { return .ignore }
        switch e.kind {
        case .other: return .ignore
        case .space:
            if down[.space] != nil || e.isRepeat { return .swallow }
            down[.space] = -1
            return .allPress
        case .digit(let d):
            guard (1...9).contains(d) else { return .ignore }
            if down[e.kind] != nil || e.isRepeat { return .swallow }
            let slot = cellForDigit(d)
            down[e.kind] = slot ?? -1   // recorded either way, so its up is consumed too
            return slot.map { .press(slot: $0) } ?? .swallow
        }
    }

    mutating func reset() { down.removeAll() }

    static func event(from ns: NSEvent, isCommsTab: Bool, deskVisible: Bool, firstResponder: NSResponder?) -> CommsKeyEvent {
        var mods: TransportKeyEvent.Modifiers = []
        let flags = ns.modifierFlags
        if flags.contains(.command) { mods.insert(.command) }
        if flags.contains(.shift) { mods.insert(.shift) }
        if flags.contains(.option) { mods.insert(.option) }
        if flags.contains(.control) { mods.insert(.control) }
        return CommsKeyEvent(kind: kind(keyCode: ns.keyCode), down: ns.type == .keyDown, modifiers: mods,
                             isRepeat: ns.type == .keyDown && ns.isARepeat, isCommsTab: isCommsTab,
                             deskVisible: deskVisible, isTextEntryFocus: TransportKeys.isTextEntry(firstResponder))
    }

    /// Virtual key codes: main-row 1-9 and keypad 1-9 (Carbon kVK_*).
    static func kind(keyCode: UInt16) -> CommsKeyEvent.Kind {
        let mainRow: [UInt16: Int] = [18: 1, 19: 2, 20: 3, 21: 4, 23: 5, 22: 6, 26: 7, 28: 8, 25: 9]
        let keypad: [UInt16: Int] = [83: 1, 84: 2, 85: 3, 86: 4, 87: 5, 88: 6, 89: 7, 91: 8, 92: 9]
        if keyCode == 49 { return .space }
        if let d = mainRow[keyCode] ?? keypad[keyCode] { return .digit(d) }
        return .other
    }
}

/// The local key monitor, installed only while the Comms tab is selected
/// (the TransportHotkeyMonitor pattern). `handle` returns nil to consume.
@MainActor
final class CommsKeyMonitor {
    private var token: Any?
    var isInstalled: Bool { token != nil }

    func setActive(_ on: Bool, handle: @escaping (NSEvent) -> NSEvent?) {
        if !on { remove(); return }
        guard token == nil else { return }
        token = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp]) { event in
            // Only the main window: pop-outs, sheets and the quit panel keep their keys.
            guard MainWindowTracker.isMain(event.window) else { return event }
            return handle(event)
        }
    }

    func remove() {
        if let token { NSEvent.removeMonitor(token) }
        token = nil
    }
}
