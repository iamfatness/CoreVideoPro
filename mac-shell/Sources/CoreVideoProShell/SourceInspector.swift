// SourceInspector — per-source options for the selected Sources row (sources
// plan Task 10; Windows SourcesWorkspaceControl.cs:156-161, SourcePolicyFlyout.cs).
//
// Names: plan Task 8 (a per-source, saved name store) is superseded by #1213 —
// a hand-typed name lasts one session and resets on a new source. So the Name
// field here edits the session-only `ShowInputSlot.name`, and "Reset to source
// name" restores `sourceName` (SlotNamePolicy). The lower-third text fields
// were Task 8 and are not drawn.

import SwiftUI

// The grading plan's contract (grading spec §5.8, slice G4). G4 replaces ONLY the body of
// SourceInspectorGradeSlot with `GradeInspectorSection(context:)` and flips
// AppModel.gradeEditorAvailable. Nothing else in this file changes for G4.
struct SourceGradeContext: Equatable {
    let slotId: Int
    let sourceKey: String            // "zoom:<pid>" / "capture:<id>"
    let gradeSourceId: String        // grading spec §5.8 item 1: Zoom participant id, or "capture:<deviceId>"
    let displayName: String
    let persistentId: String?        // grading spec §5.8 item 2: Zoom only, from the roster's "persistentId"
}

/// The inspector's identity guard (SourcePolicyFlyout.cs:34-42).
struct InspectorBinding: Equatable {
    let openedFor: String?                            // the sourceKey when the editor opened
    func isStale(current: String?) -> Bool { openedFor != current }
    static let staleNotice = "Source assignment changed. Reopen these options for the current source."
}

enum SourceAssignmentHint {
    /// ShowInputSlotViewModel.cs:360-363, verbatim; "missing" = the slot is offline.
    static func text(assigned: Bool, missing: Bool, inShow: Bool) -> String {
        if !assigned { return "Choose a source for this input." }
        if missing {
            return "Saved assignment retained. Reconnect this source or choose a replacement; "
                + "another source is never substituted automatically."
        }
        if !inShow { return "Assigned, but hidden from the show. Enable In show to include this input." }
        return "Assigned and included in the show. Unassign clears only this input; the source remains available."
    }
}

extension AppModel {
    /// G1/G4 turn it on; hides "Grade" actions until then.
    static var gradeEditorAvailable: Bool { false }

    /// nil when unassigned.
    func sourceGradeContext(slotId: Int) -> SourceGradeContext? {
        guard let slot = slots.first(where: { $0.id == slotId }), let key = slot.sourceKey else { return nil }
        let isZoom = slot.kind == "zoom"
        return SourceGradeContext(
            slotId: slotId, sourceKey: key,
            gradeSourceId: isZoom ? slot.sourceId : key,
            displayName: slot.name,
            persistentId: isZoom ? roster.first { $0.id == slot.sourceId }?.persistentId : nil)
    }

    // The key setters write the slot and re-sync the scene, exactly as the row's
    // ChromaKeyControl did (persisted by sourceKey; sent only when enabled).
    func setChromaKey(slotId: Int, enabled: Bool) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }) else { return }
        slots[index].keyEnabled = enabled
        syncScenes()
    }

    func setChromaKeyColor(slotId: Int, hex: String) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }) else { return }
        slots[index].keyColorHex = hex
        syncScenes()
    }

    func setChromaKeyParameter(slotId: Int, _ path: WritableKeyPath<ShowInputSlot, Double>, _ value: Double) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }) else { return }
        slots[index][keyPath: path] = value
        syncScenes()
    }

    /// The session-only rename (#1213). Blank resets to the source's own name.
    func setSlotName(slotId: Int, _ value: String) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }) else { return }
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        slots[index].name = trimmed.isEmpty ? slots[index].sourceName : trimmed
    }
}

/// Reserved for the grading plan (G4).
struct SourceInspectorGradeSlot: View {
    let context: SourceGradeContext?

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("Grade").font(.grotesk(13, .semibold))
            Text("Per-source colour grade arrives with the grading update.")
                .font(.grotesk(11)).foregroundStyle(Studio.secondary)
        }
    }
}

struct SourceInspector: View {
    @EnvironmentObject var model: AppModel
    let slotId: Int
    @State private var binding = InspectorBinding(openedFor: nil)
    @State private var tab = "Source"
    @State private var draftName = ""

    private var slot: ShowInputSlot? { model.slots.first { $0.id == slotId } }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Input \(slotId)").font(.grotesk(15, .semibold))
                Spacer()
                Button("Close inspector") { model.sourcesWorkspace.columns.inspectorOpen = false }
                    .buttonStyle(GhostButtonStyle())
            }
            if let slot {
                header(slot)
                let stale = binding.isStale(current: slot.sourceKey)
                if stale {
                    HStack(spacing: 8) {
                        Text(InspectorBinding.staleNotice).font(.grotesk(11)).foregroundStyle(Studio.amber)
                            .fixedSize(horizontal: false, vertical: true)
                        Button("Reopen") { reopen(slot) }.buttonStyle(GhostButtonStyle())
                    }
                }
                Picker("", selection: $tab) {
                    ForEach(["Source", "Picture", "Audio"], id: \.self) { Text($0).tag($0) }
                }
                .pickerStyle(.segmented).labelsHidden()
                Group {
                    switch tab {
                    case "Picture": pictureTab(slot)
                    case "Audio": audioTab(slot)
                    default: sourceTab(slot)
                    }
                }
                .disabled(stale || slot.kind == "unassigned")
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))
        .onAppear { if let slot { reopen(slot) } }
        .onChange(of: slotId) { _ in if let slot { reopen(slot) } }
    }

    private func reopen(_ slot: ShowInputSlot) {
        binding = InspectorBinding(openedFor: slot.sourceKey)
        draftName = slot.name
    }

    private func header(_ slot: ShowInputSlot) -> some View {
        let facts = model.sourceFacts.box(slot.id).facts
        return VStack(alignment: .leading, spacing: 6) {
            RoundedRectangle(cornerRadius: 8).fill(Studio.field)
                .overlay(Text("Preview unavailable").font(.grotesk(11)).foregroundStyle(Studio.textDim))
                .frame(width: 320, height: 180)
                .frame(maxWidth: .infinity, alignment: .leading)
            HStack(spacing: 8) {
                Text(facts.chip).font(.plexMono(10, .semibold))
                    .foregroundStyle(SlotSignalFacts.signalColor(facts.chip))
                Text(facts.format).font(.plexMono(10))
                if let age = facts.frameAgeMs {
                    Text(String(format: "%.1f s old", age / 1000)).font(.plexMono(10)).foregroundStyle(Studio.amber)
                }
            }
            Text(SourceAssignmentHint.text(assigned: slot.kind != "unassigned", missing: slot.offline,
                                           inShow: slot.inShow))
                .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    @ViewBuilder
    private func sourceTab(_ slot: ShowInputSlot) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            MonoLabel("DISPLAY NAME")
            HStack(spacing: 8) {
                TextField("Name", text: $draftName, onCommit: { model.setSlotName(slotId: slot.id, draftName) })
                    .textFieldStyle(StudioFieldStyle())
                    .accessibilityLabel("Input \(slot.id) display name")
                Button("Reset to source name") {
                    model.setSlotName(slotId: slot.id, "")
                    draftName = model.slots.first { $0.id == slot.id }?.name ?? ""
                }
                .buttonStyle(GhostButtonStyle())
            }
            Text("A typed name lasts this session and this source.")
                .font(.grotesk(10)).foregroundStyle(Studio.secondary)
            if AppModel.roleControlAvailable, slot.kind == "zoom" {
                roleControl(slot)
            }
            if slot.kind == "zoom" || slot.kind == "capture" {
                dropoutControl(slot)
            }
            Button("Unassign this input") { model.unassignSlot(slot.id) }
                .buttonStyle(GhostButtonStyle())
        }
    }

    @ViewBuilder
    private func roleControl(_ slot: ShowInputSlot) -> some View {
        let present = model.roster.contains { $0.id == slot.sourceId }
        MonoLabel("ROLE")
        Picker("", selection: Binding(
            get: { model.productionRoles.byParticipant[slot.sourceId] ?? "" },
            set: { model.setParticipantRole(slot.sourceId, role: $0.isEmpty ? nil : $0) })) {
            Text("No role").tag("")
            ForEach(ProductionRoles.roles, id: \.value) { Text($0.label).tag($0.value) }
        }
        .labelsHidden().disabled(!present)
        if !present {
            Text("Guest is unavailable. Role can be changed when they return; dropout policy is saved.")
                .font(.grotesk(10)).foregroundStyle(Studio.secondary)
        }
    }

    @ViewBuilder
    private func dropoutControl(_ slot: ShowInputSlot) -> some View {
        let row = slot.kind == "capture" ? model.sourceFacts.projection.busRow(slot.sourceKey ?? "") : nil
        let applies = slot.kind == "zoom" || CaptureDropoutStatus.applies(row)
        MonoLabel("ON DROPOUT")
        Picker("", selection: Binding(
            get: { slot.dropoutPolicy == "black" ? "black" : "hold" },
            set: { model.setDropoutPolicy(slotId: slot.id, policy: $0) })) {
            if slot.kind == "capture" {
                ForEach(CaptureDropoutStatus.policies, id: \.0) { Text($0.1).tag($0.0) }
            } else {
                Text("Hold last frame").tag("hold")
                Text("Black").tag("black")
            }
        }
        .labelsHidden().disabled(!applies)
        .accessibilityLabel("Input \(slot.id) dropout policy")
        if slot.kind == "capture" {
            let line = CaptureDropoutStatus.line(row)
            Text(applies ? line : CaptureDropoutStatus.unavailableNotice)
                .font(.grotesk(10)).foregroundStyle(Studio.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    @ViewBuilder
    private func pictureTab(_ slot: ShowInputSlot) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            MonoLabel("KEY")
            Toggle("Key this source", isOn: Binding(
                get: { slot.keyEnabled }, set: { model.setChromaKey(slotId: slot.id, enabled: $0) }))
                .font(.grotesk(12))
            if slot.keyEnabled {
                HStack(spacing: 8) {
                    Button("Green screen") { model.setChromaKeyColor(slotId: slot.id, hex: "#00ff00") }
                        .buttonStyle(GhostButtonStyle())
                    Button("Blue screen") { model.setChromaKeyColor(slotId: slot.id, hex: "#0000ff") }
                        .buttonStyle(GhostButtonStyle())
                }
                slider(slot, "Similarity", \.keySimilarity)
                slider(slot, "Smoothness", \.keySmoothness)
                slider(slot, "Spill", \.keySpill)
            }
            Divider()
            SourceInspectorGradeSlot(context: model.sourceGradeContext(slotId: slot.id))
        }
    }

    private func slider(_ slot: ShowInputSlot, _ label: String,
                        _ path: WritableKeyPath<ShowInputSlot, Double>) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("\(label) \(String(format: "%.2f", slot[keyPath: path]))").font(.plexMono(10))
            Slider(value: Binding(get: { slot[keyPath: path] },
                                  set: { model.setChromaKeyParameter(slotId: slot.id, path, $0) }), in: 0...1)
                .accessibilityLabel("Input \(slot.id) key \(label.lowercased())")
        }
    }

    @ViewBuilder
    private func audioTab(_ slot: ShowInputSlot) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            if slot.kind == "capture" {
                MonoLabel("MICROPHONE")
                Menu {
                    Button("No audio") { model.pairAudio(slotId: slot.id, device: nil) }
                    Divider()
                    ForEach(model.audioInputs) { device in
                        Button(device.name) { model.pairAudio(slotId: slot.id, device: device) }
                    }
                } label: {
                    Text(slot.audioDeviceName.isEmpty ? "Pair a microphone" : slot.audioDeviceName)
                        .font(.grotesk(12))
                }
                .menuStyle(.borderlessButton).fixedSize()
                .accessibilityLabel("Input \(slot.id) microphone")
            } else {
                Text(slot.kind == "zoom" ? "Zoom source audio" : "No source").font(.grotesk(12))
            }
            Text("Gain, mute and processing live on the Audio page.")
                .font(.grotesk(11)).foregroundStyle(Studio.secondary)
        }
    }
}
