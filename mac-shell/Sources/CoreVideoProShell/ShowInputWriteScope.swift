// ShowInputWriteScope — every slot-assignment write names its writer
// (Windows Models/ShowInputModels.cs:40-77 and :165-190).
//
// "EVERY assignment write in the app funnels through these setters … Log each
// real change with the ambient ShowInputWriteScope reason so the NEXT mystery
// writer … is identifiable from launch.log alone." On the Mac the choke point
// is a `didSet` on `AppModel.slots` (SwiftUI bindings and the drag-swap write
// the array directly, so "one function every write goes through" cannot be
// enforced). A write outside any scope is LOGGED as by=UNTRACKED, never
// asserted (plan D9): an UNTRACKED line in the shell log is a bug — wrap the
// writer in a scope.
//
// Slot writes are main-actor only, like Windows' UI-thread-only rule.

import Foundation

@MainActor
enum ShowInputWriteScope {
    static let untracked = "UNTRACKED"
    private static var stack: [String] = []

    /// The ambient reason; `untracked` outside any scope. Nested scopes CHAIN
    /// ("control-api>operator-assign"), so an outer entry point stays visible
    /// through the inner setter scope.
    static var reason: String { stack.isEmpty ? untracked : stack.joined(separator: ">") }

    /// Runs `body` with `reason` pushed; restores on return AND on throw.
    static func run<T>(_ reason: String, _ body: () throws -> T) rethrows -> T {
        stack.append(reason)
        defer { stack.removeLast() }
        return try body()
    }

    /// The log lines for one slot-array change: for each index present in both arrays, in
    /// ascending index order, `kind`, then `sourceId`, then `inShow` (Windows logs exactly
    /// the assignment fields, ShowInputModels.cs:174-185). Other fields are not logged.
    nonisolated static func lines(old: [ShowInputSlot], new: [ShowInputSlot], reason: String) -> [String] {
        var out: [String] = []
        for index in 0..<min(old.count, new.count) {
            let a = old[index], b = new[index]
            let slot = "slot-write: slot\(b.id)"
            if a.kind != b.kind { out.append("\(slot) kind '\(a.kind)'->'\(b.kind)' by=\(reason)") }
            if a.sourceId != b.sourceId { out.append("\(slot) sourceId '\(a.sourceId)'->'\(b.sourceId)' by=\(reason)") }
            if a.inShow != b.inShow { out.append("\(slot) inShow '\(a.inShow)'->'\(b.inShow)' by=\(reason)") }
        }
        return out
    }
}
