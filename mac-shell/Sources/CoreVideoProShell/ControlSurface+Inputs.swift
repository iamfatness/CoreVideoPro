// ControlSurface+Inputs.swift — the Show Input actions (Windows
// StudioControlSurface AssignInput / UnassignInput / NameInput / SetInputInShow /
// SetSourceDropoutPolicy), each calling the entry point the Mac Sources UI uses.
// Refusal texts are Windows' verbatim where Windows has one.
// Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Task 8).

import Foundation

extension AppModel {
    /// The name a slot shows when nobody renamed it: the roster participant's name
    /// for zoom, the capture device's name for capture, else the bare id; "" for
    /// an empty slot. Pure (inputs passed in) so it is testable without a model.
    static func derivedSlotName(kind: String, sourceId: String, roster: [RosterParticipant],
                                devices: [CaptureDeviceRow]) -> String {
        guard !sourceId.isEmpty else { return "" }
        switch kind {
        case "zoom":
            let name = roster.first { $0.id == sourceId }?.name ?? ""
            return name.isEmpty ? sourceId : name
        case "capture":
            let name = devices.first { $0.id == sourceId }?.name ?? ""
            return name.isEmpty ? sourceId : name
        default:
            return ""
        }
    }

    func derivedSlotName(kind: String, sourceId: String) -> String {
        Self.derivedSlotName(kind: kind, sourceId: sourceId, roster: roster, devices: captureDevices)
    }
}

extension AppModelControlSurface {
    @MainActor
    private static func slotIndex(_ model: AppModel, _ slot: Int) -> Int? {
        model.slots.firstIndex { $0.id == slot }
    }

    /// Windows AssignInput (StudioControlSurface.cs:875-909).
    @MainActor
    static func assignInput(_ model: AppModel, slot: Int, sourceId: String) -> ControlInvokeResult {
        guard slotIndex(model, slot) != nil else {
            return .fail("Show Input slot \(slot) is out of range (1-\(model.slots.count)).")
        }
        let kind: String, bare: String
        switch ControlSourceId.parse(sourceId) {
        case nil:
            return .fail("Unrecognized source id '\(sourceId)' (expected zoom:/capture:/media:).")
        case .media?:
            return .fail("Media sources cannot be assigned to Show Inputs on macOS yet (Media clip playback).")
        case .zoom(let pid)?: (kind, bare) = ("zoom", pid)
        case .capture(let device)?: (kind, bare) = ("capture", device)
        }
        // assignSlot puts the slot in the show (Windows `editor.InShow = true`).
        model.assignSlot(slot, kind: kind, sourceId: bare, name: model.derivedSlotName(kind: kind, sourceId: bare))
        return .success
    }

    /// Windows UnassignInput: the operator path, so auto-assign does not refill it.
    @MainActor
    static func unassignInput(_ model: AppModel, slot: Int) -> ControlInvokeResult {
        guard slotIndex(model, slot) != nil else { return .fail("Show input slot is out of range.") }
        model.unassignSlot(slot)
        return .success
    }

    /// Windows NameInput: blank (or omitted) resets to the derived name (D12).
    @MainActor
    static func nameInput(_ model: AppModel, slot: Int, name: String?) -> ControlInvokeResult {
        guard let index = slotIndex(model, slot) else { return .fail("Show Input slot \(slot) is out of range.") }
        let trimmed = (name ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        let current = model.slots[index]
        model.slots[index].name = trimmed.isEmpty
            ? model.derivedSlotName(kind: current.kind, sourceId: current.sourceId) : trimmed
        return .success
    }

    /// Windows SetInputInShow, idempotent; an empty slot cannot join the show
    /// ("NEVER INVENT A SOURCE").
    @MainActor
    static func setInputInShow(_ model: AppModel, slot: Int, on: Bool) -> ControlInvokeResult {
        guard let index = slotIndex(model, slot) else { return .fail("Show Input slot \(slot) is out of range.") }
        if model.slots[index].inShow == on { return .success }
        if model.slots[index].kind == "unassigned" { return .fail("Show Input slot \(slot) has no source.") }
        model.toggleSlotInShow(slot)
        return .success
    }

    /// Windows SetSourceDropoutPolicy: a Zoom guest or, since the Sources rebuild
    /// (#1215 Task 9), a capture device (D15 retired).
    @MainActor
    static func setSourceDropout(_ model: AppModel, sourceId: String, policy: String) -> ControlInvokeResult {
        let kind: String
        let bare: String
        switch ControlSourceId.parse(sourceId) {
        case .zoom(let id)?: kind = "zoom"; bare = id
        case .capture(let id)?: kind = "capture"; bare = id
        default:
            return .fail("source.dropout.set: only zoom:<pid> and capture:<id> sources take a dropout policy (\(sourceId))")
        }
        let carrying = model.slots.filter { $0.kind == kind && $0.sourceId == bare }.map(\.id)
        guard !carrying.isEmpty else { return .fail("No Show Input carries '\(sourceId)'.") }
        // The model refuses an unknown policy silently; the action must say so.
        guard policy == "hold" || policy == "black" else { return .fail("policy must be 'hold' or 'black'.") }
        for slot in carrying { model.setDropoutPolicy(slotId: slot, policy: policy) }
        return .success
    }
}
