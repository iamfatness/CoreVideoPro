// NDI output UI (Phase 4 N1b): the footer flyout and the Settings card share
// one editor, laid out as the Windows NDI card
// (ProductionSettingsWindow.xaml:451-494): the "NDI output" switch, the status
// line, then a two-column row Program name | Group. The graphics-key rows are
// replaced by one sentence (the key is out of scope on Mac).

import SwiftUI

struct NdiOutputEditor: View {
    @EnvironmentObject var model: AppModel
    @State private var nameDraft = ""
    @State private var groupDraft = ""
    @FocusState private var focused: Field?
    private enum Field { case name, group }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Toggle("NDI output", isOn: Binding(
                get: { model.ndiOutput.enabled },
                set: { model.setNdiOutput($0) }))
                .toggleStyle(StudioToggleStyle())
                .font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary)
                .accessibilityLabel("NDI output")
            Text(model.ndiOutputStatus)
                .font(.grotesk(12, .semibold)).foregroundStyle(Studio.textPrimary)
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityLabel("NDI output status")
                .accessibilityValue(model.ndiOutputStatus)
            HStack(alignment: .top, spacing: 10) {
                field("Program name", placeholder: "CoreVideo Pro Program", text: $nameDraft, which: .name)
                field("Group", placeholder: "public", text: $groupDraft, which: .group)
            }
            FooterMutedText(NdiOutputPolicy.graphicsKeyUnavailable)
        }
        .onAppear { refreshDrafts() }
        // The other editor (footer flyout vs Settings card) or the control API can
        // change the settings: refresh, but never over a field being typed in.
        .onChange(of: model.ndiOutput) { _ in if focused == nil { refreshDrafts() } }
        // A popover dismissed with a field focused never delivers the focus loss.
        .onDisappear { commit() }
    }

    // Committed on Return or focus loss, never per keystroke (Windows OnNdiTextBoxKeyDown).
    private func field(_ title: String, placeholder: String, text: Binding<String>, which: Field) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.grotesk(11)).foregroundStyle(Studio.secondary)
            TextField(placeholder, text: text)
                .textFieldStyle(StudioFieldStyle())
                .focused($focused, equals: which)
                .onSubmit { commit() }
                .onChange(of: focused) { now in if now != which { commit() } }
                .accessibilityLabel("NDI \(title.lowercased())")
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func refreshDrafts() {
        nameDraft = model.ndiOutput.programName
        groupDraft = model.ndiOutput.groupName
    }

    private func commit() {
        model.commitNdiSettings(programName: nameDraft, groupName: groupDraft)
    }
}
