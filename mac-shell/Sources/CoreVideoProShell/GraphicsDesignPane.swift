// GraphicsDesignPane — the Graphics tab's Design surface before the Builder (G8). On top, the
// YOUR DESIGNS bar laid out as Windows' BuildDesignsBar (GraphicsBuilderControl.cs:646-690): label,
// design chips, the actions in Windows' order (only Data table… and Data crawl… live, plan D8),
// then the design status across the width. Below it the Designer note and the existing overlay
// controls drawer (W/Views/OverlaysPage.xaml:17,55-58).

import SwiftUI

struct GraphicsDesignPane: View {
    @ObservedObject var graphics: GraphicsModel
    @State private var drawerOpen = false
    @State private var picked: GraphicsPickedData?
    @State private var sheet = GraphicsDataSheetState()
    @State private var designStatus = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            designsBar
            Text("The graphics Designer arrives in a later release.")
                .font(.grotesk(12)).foregroundStyle(Studio.secondary)
            DisclosureGroup(isExpanded: $drawerOpen) {
                OverlaysPane().frame(minHeight: 480)
            } label: {
                Text("Existing overlay controls").font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary)
            }
            .accessibilityLabel("Existing overlay controls")
            Spacer(minLength: 0)
        }
        .sheet(isPresented: Binding(get: { sheet.isPresented && picked != nil }, set: { if !$0 { sheet.isPresented = false } }),
               onDismiss: { designStatus = sheet.status; picked = nil }) {
            if let picked {
                if picked.crawl, let form = GraphicsDataCrawlForm.initial(fileName: picked.fileName, data: picked.data) {
                    GraphicsDataCrawlSheet(graphics: graphics, picked: picked, form: form, sheet: $sheet)
                } else {
                    GraphicsDataTableSheet(graphics: graphics, picked: picked, sheet: $sheet)
                }
            }
        }
    }

    var designsBar: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 10) {
                Text("YOUR DESIGNS").font(.plexMono(12, .semibold)).tracking(1.9).foregroundStyle(Studio.secondary)
                Group {
                    if graphics.document.designs.isEmpty {
                        Text(GraphicsDesignsBar.emptyText).font(.grotesk(13)).foregroundStyle(Studio.secondary).lineLimit(2)
                    } else {
                        ScrollView(.horizontal, showsIndicators: false) {
                            HStack(spacing: 6) {
                                ForEach(Array(graphics.document.designs.enumerated()), id: \.offset) { _, design in
                                    Text(design["name"] as? String ?? "").font(.grotesk(12, .semibold)).foregroundStyle(Studio.textPrimary)
                                        .padding(.horizontal, 10).padding(.vertical, 4)
                                        .background(Capsule().fill(Studio.surfaceRaised))
                                        .overlay(Capsule().stroke(Studio.border, lineWidth: 1))
                                }
                            }
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                HStack(spacing: 6) {
                    ForEach(GraphicsDesignsBar.buttons, id: \.title) { action in
                        button(action)
                    }
                }
            }
            if !designStatus.isEmpty {
                Text(designStatus).font(.grotesk(13)).foregroundStyle(Studio.textPrimary).fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.horizontal, 10).padding(.vertical, 8)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
    }

    @ViewBuilder
    func button(_ action: GraphicsDesignsBar.Action) -> some View {
        let tap = { open(crawl: action.title == "Data crawl…") }
        Group {
            if action.accent {
                Button(action.title, action: tap).buttonStyle(AccentButtonStyle())
            } else {
                Button(action.title, action: tap).buttonStyle(GhostButtonStyle())
            }
        }
        .lineLimit(1).fixedSize()
        .disabled(!action.enabled || graphics.documentReadOnly)
        .opacity(action.enabled && !graphics.documentReadOnly ? 1 : 0.45)   // the ghost/accent styles do not dim themselves
        .help(action.help)
        .accessibilityLabel(action.title)
    }

    func open(crawl: Bool) {
        guard let result = GraphicsDataFilePicker.pick() else { return }
        switch result {
        case .success(let (name, data)):
            if crawl && GraphicsDataCrawlForm.initial(fileName: name, data: data) == nil { designStatus = "The crawl needs a text column."; return }
            sheet = GraphicsDataSheetState(isPresented: true)
            picked = GraphicsPickedData(fileName: name, data: data, crawl: crawl)
        case .failure(let error): designStatus = error.message
        }
    }
}
