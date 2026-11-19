// Undo and redo for the grade editor (grading plan 2026-11-16, Task 10). Pure, so it is tested
// without a window. One entry per gesture (a whole slider drag is one step) and 100 deep, the
// Windows rules (ColorGradeEditorViewModel.Advanced.cs RememberGrade / TrimHistory). An entry
// is the WHOLE grade, basic values and document together, so one undo restores both.

import Foundation

struct GradeHistory {
    static let depth = 100

    private var undoStack: [SourceColorGrade] = []
    private var redoStack: [SourceColorGrade] = []
    private var inGesture = false
    private var gestureRecorded = false

    var canUndo: Bool { !undoStack.isEmpty }
    var canRedo: Bool { !redoStack.isEmpty }

    mutating func beginGesture() { inGesture = true; gestureRecorded = false }
    mutating func endGesture() { inGesture = false; gestureRecorded = false }

    /// Records the grade as it was BEFORE an edit. No-op if this gesture already recorded one.
    mutating func remember(_ current: SourceColorGrade) {
        if inGesture && gestureRecorded { return }
        undoStack.append(current)
        if undoStack.count > Self.depth { undoStack.removeFirst(undoStack.count - Self.depth) }
        redoStack.removeAll()
        if inGesture { gestureRecorded = true }
    }

    mutating func undo(from current: SourceColorGrade) -> SourceColorGrade? {
        guard let previous = undoStack.popLast() else { return nil }
        redoStack.append(current)
        return previous
    }

    mutating func redo(from current: SourceColorGrade) -> SourceColorGrade? {
        guard let next = redoStack.popLast() else { return nil }
        undoStack.append(current)
        return next
    }
}
