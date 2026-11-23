// The Comms (Zoom talkback) page. Plan Task 8: the rail and the explicit page
// states, with Windows' copy (CommsPage.xaml). Task 9 builds the full
// five-row desk.

import SwiftUI

struct CommsPage: View {
    @ObservedObject var model: CommsModel

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            CommsRail(model: model)
            CommsStateBlock(state: model.pageState)
                .padding(.vertical, 16)
            Spacer(minLength: 0)
        }
    }
}

struct CommsRail: View {
    @ObservedObject var model: CommsModel

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text("Comms").font(.grotesk(16, .semibold)).foregroundStyle(Studio.textPrimary)
                Text(model.railText).font(.plexMono(11)).foregroundStyle(Studio.secondary)
                    .lineLimit(1).truncationMode(.tail)
                Spacer(minLength: 8)
                HStack(spacing: 16) {
                    CommsLamp(tag: "LINK", on: model.lampLink, help: "In the meeting")
                    CommsLamp(tag: "MTG MIC", on: model.lampMic,
                              help: "This participant's meeting audio is open. Zoom delivers talkback only while it is.")
                    CommsLamp(tag: "CHANNEL", on: model.lampChannel, help: "At least one talkback channel is ready")
                    CommsLamp(tag: "TX", on: model.lampTx, help: "Sending to a channel someone is in")
                }
            }
            .padding(.bottom, 10)
            Rectangle().fill(Studio.border).frame(height: 1)
        }
    }
}

struct CommsLamp: View {
    let tag: String
    let on: Bool
    let help: String

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(on ? Studio.accent : Studio.textDim).frame(width: 8, height: 8)
            Text(tag).font(.plexMono(10, .medium)).foregroundStyle(on ? Studio.textPrimary : Studio.textDim)
        }
        .help(help)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(tag) \(on ? "on" : "off")")
    }
}

/// The three non-desk states, Windows' copy. Nothing for .desk.
struct CommsStateBlock: View {
    let state: CommsPageState

    static let notInMeetingTitle = "Not in a meeting"
    static let notInMeetingBody = "Join from the Zoom page. Talkback comes up once this account is in the meeting as host or co-host."
    static let waitingText = "Waiting for the Zoom engine's talkback report…"
    static let unavailableTitle = "Talkback is not up"
    static let hostHint = "Ask the meeting host to make this account host or co-host. Video keeps working meanwhile."

    var body: some View {
        switch state {
        case .notInMeeting:
            VStack(alignment: .leading, spacing: 6) {
                Text(Self.notInMeetingTitle).font(.grotesk(15, .semibold)).foregroundStyle(Studio.textPrimary)
                Text(Self.notInMeetingBody).font(.grotesk(12)).foregroundStyle(Studio.secondary)
            }
        case .waiting:
            Text(Self.waitingText).font(.grotesk(12)).foregroundStyle(Studio.secondary)
        case .unavailable(let text, let hostHint):
            VStack(alignment: .leading, spacing: 6) {
                Text(Self.unavailableTitle).font(.grotesk(15, .semibold)).foregroundStyle(Studio.textPrimary)
                Text(text).font(.grotesk(12)).foregroundStyle(Studio.textPrimary)
                if hostHint {
                    Text(Self.hostHint).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                }
            }
        case .desk:
            EmptyView()
        }
    }
}
