// SourcesTable — the Sources tab as the Windows ten-row table (sources plan
// Task 7; Windows Controls/SourcesWorkspaceControl.cs + SourcesTableRowControl.cs).
// Header and rows share SourcesColumnLayout through one Layout, so they can
// never disagree; the flexible Source column takes what the fixed columns leave.

import SwiftUI

/// Lays out one table line: 11 subviews in order —
/// #, thumbnail, Source, Type, Format, FPS, Signal, Audio, MV, ISO, Actions.
/// Widths come from the proposal (never a fixed row width), so the table fits
/// whatever page it is given. A hidden column is placed at zero width; its
/// cell clips, so it draws nothing.
struct SourcesColumnsLayout: Layout {
    let columns: SourcesColumnPrefs
    static let spacing: CGFloat = 8
    static let thumbnailWidth: CGFloat = 96

    func widths(for total: CGFloat) -> [CGFloat] {
        let base = SourcesColumnLayout.widths(tableWidth: total, prefs: columns)
        let thumb = SourcesColumnLayout.showsThumbnail(tableWidth: total) ? Self.thumbnailWidth : 0
        // [#, thumb, Source, Type, Format, FPS, Signal, Audio, MV, ISO, Actions]
        var out: [CGFloat] = [base[0], thumb, -1] + Array(base[2...])
        let visible = out.filter { $0 != 0 }.count
        let fixed = out.filter { $0 > 0 }.reduce(0, +)
        out[2] = max(0, total - fixed - Self.spacing * CGFloat(max(0, visible - 1)))
        return out
    }

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let total = proposal.width ?? 1000
        let w = widths(for: total)
        var height: CGFloat = 0
        for (index, view) in subviews.enumerated() where index < w.count && w[index] > 0 {
            height = max(height, view.sizeThatFits(ProposedViewSize(width: w[index], height: nil)).height)
        }
        return CGSize(width: total, height: height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let w = widths(for: bounds.width)
        var x = bounds.minX
        for (index, view) in subviews.enumerated() {
            let width = index < w.count ? w[index] : 0
            view.place(at: CGPoint(x: x, y: bounds.midY), anchor: .leading,
                       proposal: ProposedViewSize(width: width, height: bounds.height))
            if width > 0 { x += width + Self.spacing }
        }
    }
}

/// A table cell: fills its column, clips (so a zero-width column draws nothing).
private struct Cell<Content: View>: View {
    @ViewBuilder var content: Content
    var body: some View {
        content.frame(maxWidth: .infinity, alignment: .leading).clipped()
    }
}

/// The inspector sits beside the table in a 340 pt column when the page is at least
/// 1500 pt wide, else below it (SourcesWorkspaceControl.cs:156-161). Decided from the
/// PROPOSED width, so the table and inspector never overflow the page.
struct InspectorPlacementLayout: Layout {
    static let spacing: CGFloat = 12

    private func beside(_ width: CGFloat, _ subviews: Subviews) -> Bool {
        subviews.count > 1 && SourcesColumnLayout.inspectorBeside(pageWidth: width)
    }

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width = proposal.width ?? 1000
        guard let table = subviews.first else { return .zero }
        if beside(width, subviews) {
            let tableWidth = width - SourcesColumnLayout.inspectorWidth - Self.spacing
            let a = table.sizeThatFits(ProposedViewSize(width: tableWidth, height: nil))
            let b = subviews[1].sizeThatFits(ProposedViewSize(width: SourcesColumnLayout.inspectorWidth, height: nil))
            return CGSize(width: width, height: max(a.height, b.height))
        }
        var height: CGFloat = 0
        for (index, view) in subviews.enumerated() {
            height += view.sizeThatFits(ProposedViewSize(width: width, height: nil)).height
            if index > 0 { height += Self.spacing }
        }
        return CGSize(width: width, height: height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        guard let table = subviews.first else { return }
        if beside(bounds.width, subviews) {
            let tableWidth = bounds.width - SourcesColumnLayout.inspectorWidth - Self.spacing
            table.place(at: bounds.origin, proposal: ProposedViewSize(width: tableWidth, height: nil))
            subviews[1].place(at: CGPoint(x: bounds.minX + tableWidth + Self.spacing, y: bounds.minY),
                              proposal: ProposedViewSize(width: SourcesColumnLayout.inspectorWidth, height: nil))
            return
        }
        var y = bounds.minY
        for view in subviews {
            let size = view.sizeThatFits(ProposedViewSize(width: bounds.width, height: nil))
            view.place(at: CGPoint(x: bounds.minX, y: y), proposal: ProposedViewSize(width: bounds.width, height: nil))
            y += size.height + Self.spacing
        }
    }
}

struct SourcesTable: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            InspectorPlacementLayout {
                tableCard
                if let selected = model.sourcesWorkspace.selectedSlotId,
                   model.sourcesWorkspace.columns.inspectorOpen {
                    SourceInspector(slotId: selected)
                }
            }
            HStack(alignment: .top, spacing: 12) {
                ZoomFeedHealthCard()
                CaptureDevicesSection()
            }
        }
        .frame(maxWidth: TabPageLayout.maxWidth(for: .sources))
    }

    private var tableCard: some View {
            VStack(alignment: .leading, spacing: 10) {
                Text("Show inputs").font(.grotesk(15, .semibold))
                SourcesToolsRow()
                Text(SourcesSummary.text(slots: model.slots, matching: visibleSlots.count))
                    .font(.grotesk(12, .medium)).foregroundStyle(Studio.accent)
                if !model.rosterRecoveryNotice.isEmpty {
                    Text(model.rosterRecoveryNotice).font(.plexMono(10)).foregroundStyle(Studio.amber)
                }
                SourcesColumnsLayout(columns: model.sourcesWorkspace.columns) {
                    ForEach(Array(SourcesColumnLayout.headers.enumerated()), id: \.offset) { index, title in
                        if index == 1 { Cell { Text("") } }   // the thumbnail rides the Source column's left
                        Cell { MonoLabel(title) }
                    }
                }
                ForEach(visibleSlots) { slot in
                    SourcesTableRow(slot: slot, box: model.sourceFacts.box(slot.id))
                }
                if visibleSlots.isEmpty {
                    Text("No matching inputs. Choose All sources or clear the search to see empty slots.")
                        .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                }
                Text("Observed source format/FPS. A stale source is not advancing video. "
                     + "ISO selection is separate from writer state.")
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if !model.sourcesStatus.isEmpty {
                    Text(model.sourcesStatus).font(.grotesk(11)).foregroundStyle(Studio.amber)
                }
            }
            .padding(14)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))
    }

    private var visibleSlots: [ShowInputSlot] {
        model.slots.filter { slot in
            let type = SourceType.of(kind: slot.kind, sourceId: slot.sourceId,
                                     vendor: model.captureDevices.first { $0.id == slot.sourceId }?.vendor)
            return model.sourcesWorkspace.matches(slotId: slot.id, displayName: slot.name,
                                                  sourceLabel: type.pickerPrefix + slot.name, type: type)
        }
    }
}

struct SourcesToolsRow: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 10) {
            TextField("Search inputs", text: $model.sourcesWorkspace.query)
                .textFieldStyle(StudioFieldStyle())
                .frame(width: 200)
                .accessibilityLabel("Search inputs")
            Picker("", selection: $model.sourcesWorkspace.category) {
                ForEach(SourcesWorkspaceState.categories, id: \.self) { Text($0).tag($0) }
            }
            .labelsHidden()
            .frame(width: 150)
            .accessibilityLabel("Source category")
            Menu {
                Toggle("Type", isOn: $model.sourcesWorkspace.columns.showType)
                Toggle("Signal", isOn: $model.sourcesWorkspace.columns.showSignal)
                Toggle("Audio", isOn: $model.sourcesWorkspace.columns.showAudio)
            } label: {
                Text("Columns").font(.grotesk(12))
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
            AutoAssignInputsToggle()
            Spacer(minLength: 0)
            GradeSelectedButton()
            Button(model.sourcesWorkspace.columns.inspectorOpen ? "Close inspector" : "Open inspector") {
                if model.sourcesWorkspace.columns.inspectorOpen {
                    model.sourcesWorkspace.columns.inspectorOpen = false
                } else {
                    model.sourcesWorkspace.inspect(model.sourcesWorkspace.selectedSlotId ?? 1)
                }
            }
            .buttonStyle(GhostButtonStyle())
        }
    }
}

struct SourcesTableRow: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow
    let slot: ShowInputSlot
    @ObservedObject var box: SlotFactsBox

    private var assigned: Bool { slot.kind != "unassigned" }
    private var type: SourceType {
        SourceType.of(kind: slot.kind, sourceId: slot.sourceId,
                      vendor: model.captureDevices.first { $0.id == slot.sourceId }?.vendor)
    }
    private var selected: Bool { model.sourcesWorkspace.selectedSlotId == slot.id }

    var body: some View {
        SourcesColumnsLayout(columns: model.sourcesWorkspace.columns) {
            Cell {
                Text("\(slot.id)").font(.plexMono(11, .medium)).foregroundStyle(Studio.secondary)
            }
            Cell { thumbnail }
            Cell { sourceCell }
            Cell { Text(assigned ? type.rawValue : "—").font(.grotesk(11)).foregroundStyle(Studio.secondary) }
            Cell {
                VStack(alignment: .leading, spacing: 1) {
                    Text(assigned ? box.facts.format : "—").font(.plexMono(10))
                        .foregroundStyle(box.facts.formatStale ? Studio.amber : Studio.textPrimary)
                    // Windows FormatDetailLabel: frame age while stale, else the cap note.
                    if assigned, !box.facts.formatDetail.isEmpty {
                        Text(box.facts.formatDetail).font(.plexMono(9))
                            .foregroundStyle(box.facts.formatStale ? Studio.amber : Studio.secondary)
                            .lineLimit(1)
                    }
                }
            }
            Cell { Text(assigned ? box.facts.fps : "—").font(.plexMono(10)) }
            Cell {
                Text(box.facts.chip).font(.plexMono(9, .semibold))
                    .foregroundStyle(SlotSignalFacts.signalColor(box.facts.chip))
            }
            Cell { Text(audioText).font(.grotesk(11)).foregroundStyle(Studio.secondary).lineLimit(1) }
            Cell {
                Toggle("", isOn: Binding(get: { slot.inShow }, set: { _ in model.toggleSlotInShow(slot.id) }))
                    .toggleStyle(.checkbox).labelsHidden().disabled(!assigned)
                    .accessibilityLabel("Input \(slot.id) in multiview")
            }
            Cell {
                HStack(spacing: 4) {
                    Toggle("", isOn: Binding(get: { slot.iso }, set: { _ in model.toggleSlotIso(slot.id) }))
                        .toggleStyle(.checkbox).labelsHidden().disabled(!assigned)
                        .accessibilityLabel("Input \(slot.id) ISO")
                    if assigned { SlotWriterChip(box: box) }
                }
            }
            Cell { actions }
        }
        .padding(.vertical, 4).padding(.horizontal, 4)
        .background(RoundedRectangle(cornerRadius: 8)
            .fill(selected ? Studio.accent.opacity(0.08) : Color.clear))
        .contentShape(Rectangle())
        .onTapGesture { model.sourcesWorkspace.selectedSlotId = slot.id }
    }

    private var audioText: String {
        switch slot.kind {
        case "zoom": return "Zoom source audio"
        case "capture": return slot.audioDeviceName.isEmpty ? "No paired microphone" : slot.audioDeviceName
        default: return "—"
        }
    }

    /// D1: thumbnails arrive with the core source sheet (slice S6).
    private var thumbnail: some View {
        RoundedRectangle(cornerRadius: 6).fill(Studio.field)
            .overlay(Text("Preview unavailable").font(.grotesk(9)).foregroundStyle(Studio.textDim)
                .multilineTextAlignment(.center).padding(4))
            .frame(width: 96, height: 54)
            .accessibilityLabel("Input \(slot.id) preview unavailable")
    }

    private var sourceCell: some View {
        // The assignment this menu was built for (picker guard, audit row 12).
        let openedFor = slot.sourceKey
        let groups = SourcePickerCatalog.groups(roster: model.roster, devices: model.captureDevices,
                                                current: assigned ? slot : nil)
        return VStack(alignment: .leading, spacing: 2) {
            Menu {
                ForEach(Array(groups.enumerated()), id: \.offset) { _, group in
                    Section(group.title) {
                        ForEach(Array(group.options.enumerated()), id: \.offset) { _, option in
                            Button(option.label) { choose(option, openedFor: openedFor) }
                                .disabled(option.unavailable)
                        }
                    }
                }
            } label: {
                Text(assigned ? slot.name : "Select a source")
                    .font(.grotesk(12))
                    .foregroundStyle(assigned ? Studio.textPrimary : Studio.secondary)
                    .lineLimit(1)
            }
            .menuStyle(.borderlessButton)
            .accessibilityLabel("Input \(slot.id) source")
            if assigned {
                HStack(spacing: 6) {
                    Text(type.rawValue).font(.grotesk(10)).foregroundStyle(Studio.secondary)
                    if slot.offline {
                        Text("OFFLINE").font(.plexMono(9, .semibold)).foregroundStyle(Studio.amber)
                    }
                }
            }
        }
    }

    private func choose(_ option: SourcePickerOption, openedFor: String?) {
        let name = option.kind == "zoom"
            ? model.roster.first { $0.id == option.sourceId }?.name ?? option.sourceId
            : model.captureDevices.first { $0.id == option.sourceId }?.name ?? option.sourceId
        model.assignSlot(slot.id, kind: option.kind, sourceId: option.sourceId, name: name,
                         openedFor: openedFor)
        if option.kind == "capture",
           let device = model.captureDevices.first(where: { $0.id == option.sourceId }),
           device.connectionState != "connected" {
            model.connectCaptureDevice(device)
        }
    }

    /// D5: name, microphone, key and dropout live in the inspector; the row keeps
    /// Inspect, Unassign in "…", and a KEY chip.
    private var actions: some View {
        HStack(spacing: 6) {
            Button("Inspect") { model.sourcesWorkspace.inspect(slot.id) }
                .buttonStyle(.plain).font(.grotesk(11, .semibold)).foregroundStyle(Studio.accent)
                .accessibilityLabel("Inspect input \(slot.id)")
            Button("Grade") { model.openGradeWorkspace(slotId: slot.id) { openWindow(id: PopoutWindow.sourceGrade, value: $0) } }
                .buttonStyle(.plain).font(.grotesk(11, .semibold)).foregroundStyle(Studio.accent)
                .disabled(!assigned)
                .accessibilityLabel("Grade input \(slot.id)")          // SourcesTableRowControl.cs:78
            if slot.keyEnabled {
                Text("KEY").font(.plexMono(9, .semibold)).foregroundStyle(Studio.accent)
            }
            SlotGradedChip(store: model.sourceGrades, slotId: slot.id)
            moreMenu
        }
    }

    private var moreMenu: some View {
        Menu {
            Button("Unassign") { model.unassignSlot(slot.id) }.disabled(!assigned)
        } label: {
            Text("…").font(.grotesk(13, .semibold))
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .accessibilityLabel("Input \(slot.id) actions")
    }
}

/// Mac-only: who is in the meeting and whether their camera is on.
struct ZoomFeedHealthCard: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Zoom feed health").font(.grotesk(14, .semibold))
            if model.roster.isEmpty {
                Text("No participants").font(.grotesk(12)).foregroundStyle(Studio.secondary)
            }
            ForEach(model.roster) { participant in
                HStack(spacing: 6) {
                    Circle()
                        .fill(participant.hasVideo ? Studio.accent : Studio.secondary.opacity(0.4))
                        .frame(width: 7, height: 7)
                    Text(participant.name).font(.grotesk(12)).lineLimit(1)
                    Text(participant.hasVideo ? "video" : "no video")
                        .font(.plexMono(10)).foregroundStyle(Studio.secondary)
                }
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))
    }
}

/// Every capture device (no 8-device cap), grouped Cameras / Screens.
struct CaptureDevicesSection: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("Capture devices").font(.grotesk(14, .semibold))
                Spacer()
                Button("Refresh") { model.refreshCaptureDevices() }
                    .buttonStyle(GhostButtonStyle())
                    .accessibilityLabel("Refresh capture devices")
            }
            Text("\(model.captureDevices.filter { $0.connectionState == "connected" }.count)"
                 + " connected · \(model.captureDevices.count) detected")
                .font(.grotesk(12)).foregroundStyle(Studio.secondary)
            if model.captureDevices.isEmpty {
                Text("No capture devices detected. Connect a camera or allow Screen Recording, then Refresh.")
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
            }
            group("Cameras", model.captureDevices.filter { type($0).category == "Cameras" })
            group("Screens", model.captureDevices.filter { type($0).category == "Screens" })
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))
    }

    private func type(_ device: CaptureDeviceRow) -> SourceType {
        SourceType.of(kind: "capture", sourceId: device.id, vendor: device.vendor)
    }

    @ViewBuilder
    private func group(_ title: String, _ devices: [CaptureDeviceRow]) -> some View {
        if !devices.isEmpty {
            MonoLabel(title.uppercased())
            ForEach(devices) { device in
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 6) {
                        Circle()
                            .fill(device.signalPresent ? Studio.accent
                                  : device.connectionState == "error" ? Studio.red
                                  : Studio.secondary.opacity(0.4))
                            .frame(width: 7, height: 7)
                        Text(device.name).font(.grotesk(12)).lineLimit(1)
                        Spacer()
                        Button(device.connectionState == "connected" ? "Disconnect" : "Connect") {
                            model.connectCaptureDevice(device)
                        }
                        .buttonStyle(.plain).font(.grotesk(11, .semibold)).foregroundStyle(Studio.accent)
                        Button("Take offline") { model.takeCaptureDeviceOffline(device.id) }
                            .buttonStyle(.plain).font(.grotesk(11, .semibold)).foregroundStyle(Studio.amber)
                    }
                    // Z2 (Windows SourcesInputsPage.xaml:110-134): the offset and the nudges.
                    // Cameras only: a ScreenCaptureKit display carries no audio on the
                    // Mac, so a nudge there would be a control that does nothing.
                    if CaptureAudioSyncPolicy.offersNudge(category: type(device).category) {
                        HStack(spacing: 8) {
                            Text(CaptureAudioSyncPolicy.label(model.captureAudioSyncOffset(device.id)))
                                .font(.plexMono(10)).foregroundStyle(Studio.secondary)
                            Button("-20 ms") { model.nudgeCaptureAudio(deviceId: device.id, deltaMs: -CaptureAudioSyncPolicy.stepMs) }
                                .buttonStyle(.plain).font(.grotesk(10, .semibold)).foregroundStyle(Studio.accent)
                                .help("Nudge this device's audio 20 ms earlier")
                            Button("+20 ms") { model.nudgeCaptureAudio(deviceId: device.id, deltaMs: CaptureAudioSyncPolicy.stepMs) }
                                .buttonStyle(.plain).font(.grotesk(10, .semibold)).foregroundStyle(Studio.accent)
                                .help("Nudge this device's audio 20 ms later")
                        }
                    }
                    if !device.warning.isEmpty {
                        Text(device.warning).font(.grotesk(10))
                            .foregroundStyle(device.connectionState == "error" ? Studio.red : Studio.amber)
                            .fixedSize(horizontal: false, vertical: true)
                    } else if device.connectionState == "connected", !device.signalPresent {
                        Text("Connected — waiting for the first frame")
                            .font(.grotesk(10)).foregroundStyle(Studio.amber)
                    }
                }
            }
        }
    }
}
