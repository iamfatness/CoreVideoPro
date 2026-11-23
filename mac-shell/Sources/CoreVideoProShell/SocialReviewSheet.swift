// The Social review sheet: GraphicsSocialReviewDialog.cs (:37-77) as a Mac
// sheet, about 548 pt wide, its content in a ScrollView of at most 485 pt.
// Copy is Windows'. Approving is private; Show approved is a separate action.

import AppKit
import SwiftUI

struct SocialReviewSheet: View {
    @ObservedObject var model: SocialReviewModel
    var onClose: () -> Void

    @State private var video = ""
    @State private var key = ""
    @State private var channel = ""
    @State private var client = ""
    @State private var selected: SocialItemKey?
    @State private var busy = false
    @State private var disconnecting = false
    @State private var testAuthor = "Viewer"
    @State private var testMessage = ""

    private var items: [SocialItem] { _ = model.revision; return model.items }
    private var selection: SocialItem? { SocialSheetRules.preserveSelection(items, selected) }
    private var rules: SocialSheetRules {
        SocialSheetRules.evaluate(selected: selection?.decision, connected: model.isConnected, busy: busy,
                                  disconnecting: disconnecting, onProgram: model.onProgram, running: model.running,
                                  provider: model.provider, videoId: video.trimmingCharacters(in: .whitespaces),
                                  channel: channel.trimmingCharacters(in: .whitespaces),
                                  clientId: client.trimmingCharacters(in: .whitespaces))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Social review").font(.grotesk(18, .semibold)).foregroundStyle(Studio.textPrimary)
            ScrollView {
                VStack(alignment: .leading, spacing: 8) {
                    note("Review each message before approving it. Show approved is a separate action.")
                    HStack(alignment: .top, spacing: 12) {
                        VStack(alignment: .leading, spacing: 4) {
                            MonoLabel("Provider")
                            Picker("", selection: Binding(get: { model.provider }, set: { next in
                                act { await model.selectProvider(next) }
                            })) {
                                Text("YouTube").tag("YouTube")
                                Text("Twitch").tag("Twitch")
                            }
                            .labelsHidden()
                            .disabled(!rules.provider)
                        }
                        .frame(width: 160, alignment: .leading)
                        note("Selected source\n" + model.sourceLabel).frame(maxWidth: .infinity, alignment: .leading)
                    }
                    if model.provider == "Twitch" {
                        sourceRow(field("Twitch channel name", text: $channel, limit: 25),
                                  field("Twitch public app Client ID", text: $client, limit: 128,
                                        placeholder: "Leave blank to reuse the saved sign-in"))
                    } else {
                        sourceRow(field("Public YouTube video ID", text: $video, limit: 11),
                                  secureField("YouTube Data API key", text: $key, limit: 128,
                                              placeholder: "Leave blank to reuse the local key"))
                    }
                    if let prompt = model.devicePrompt {
                        VStack(alignment: .leading, spacing: 2) {
                            note("Twitch sign-in code: " + prompt.userCode)
                            Button("Open Twitch activation") { NSWorkspace.shared.open(prompt.verificationUri) }
                                .buttonStyle(.link)
                        }
                    }
                    HStack(spacing: 8) {
                        ghost("Connect", enabled: rules.connect) { connect() }
                        ghost("Disconnect", enabled: rules.disconnect) { disconnect() }
                        ghost("Clear local data", enabled: rules.clear) {
                            act {
                                await model.clearLocalData()
                                key = ""; video = ""; channel = ""; client = ""
                            }
                        }
                    }
                    if SocialReviewModel.testSourceEnabled { testSourceRow }
                    note(model.onProgram ? "On Program · " + model.status : model.status)
                    List(selection: $selected) {
                        ForEach(items, id: \.key) { item in
                            Text(Self.rowText(item)).font(.grotesk(12)).tag(item.key)
                        }
                    }
                    .frame(height: 96)
                    ScrollView {
                        Text(selection.map { $0.provider + " · " + $0.author + "\n" + $0.message } ?? "Select one message to read it.")
                            .font(.grotesk(12)).foregroundStyle(Studio.textPrimary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(minHeight: 42, maxHeight: 85)
                    HStack(spacing: 8) {
                        ghost("Approve", enabled: rules.approve) { if let s = selection { model.decide(s.key, approve: true) } }
                        ghost("Reject", enabled: rules.reject) { if let s = selection { model.decide(s.key, approve: false) } }
                        ghost("Withdraw", enabled: rules.withdraw) { if let s = selection { act { await model.withdraw(s.key) } } }
                    }
                    HStack(spacing: 8) {
                        ghost("Show approved", enabled: rules.show) { if let s = selection { act { await model.show(s.key) } } }
                        ghost("Hide social graphic", enabled: rules.hide) { act { await model.hide() } }
                    }
                }
            }
            .frame(maxHeight: 485)
            HStack {
                Spacer()
                Button("Close") { key = ""; onClose() }.buttonStyle(GhostButtonStyle()).keyboardShortcut(.cancelAction)
            }
        }
        .padding(20)
        .frame(width: 548)
        .background(Studio.panel)
        .onAppear {
            video = model.videoId; channel = model.channel; client = model.twitchClientId
            model.startExpiryTick()
        }
        // A changed revision drops the selection (the row's key no longer exists).
        .onChange(of: model.revision) { _ in if selection == nil { selected = nil } }
        .onDisappear { key = "" }
    }

    static func rowText(_ item: SocialItem) -> String {
        let f = DateFormatter()
        f.timeStyle = .short
        f.dateStyle = .none
        let decision: String
        switch item.decision {
        case .pending: decision = "Pending"
        case .approved: decision = "Approved"
        case .rejected: decision = "Rejected"
        }
        return "\(item.provider) · \(item.author) · \(decision) · \(f.string(from: item.receivedAt))"
    }

    // ── pieces ───────────────────────────────────────────────────────────

    private var testSourceRow: some View {
        VStack(alignment: .leading, spacing: 4) {
            MonoLabel("Local test source (dev only)")
            HStack(spacing: 8) {
                ghost("Connect test source", enabled: !busy) { act { await model.connectTestSource() } }
                TextField("Author", text: $testAuthor).frame(width: 110)
                TextField("Message", text: $testMessage)
                ghost("Paste test message", enabled: model.testSource != nil && !testMessage.isEmpty) {
                    let a = testAuthor, m = testMessage
                    testMessage = ""
                    act { await model.pasteTestMessage(author: a, message: m) }
                }
            }
        }
    }

    private func connect() {
        let k = key.trimmingCharacters(in: .whitespaces)
        key = ""   // the key field is cleared on Connect
        let v = video.trimmingCharacters(in: .whitespaces), c = channel.trimmingCharacters(in: .whitespaces)
        let id = client.trimmingCharacters(in: .whitespaces)
        act {
            if model.provider == "Twitch" { await model.connectTwitch(channel: c, clientId: id) }
            else { await model.connectYouTube(videoId: v, key: k) }
        }
    }

    private func disconnect() {
        guard !disconnecting else { return }
        disconnecting = true
        Task { @MainActor in await model.disconnect(); disconnecting = false }
    }

    private func act(_ body: @escaping @MainActor () async -> Void) {
        guard !busy, !disconnecting else { return }
        busy = true
        Task { @MainActor in await body(); busy = false }
    }

    private func note(_ text: String) -> some View {
        Text(text).font(.grotesk(12)).foregroundStyle(Studio.secondary).fixedSize(horizontal: false, vertical: true)
    }

    private func sourceRow<A: View, B: View>(_ a: A, _ b: B) -> some View {
        GeometryReader { geo in
            HStack(alignment: .top, spacing: 8) {
                a.frame(width: (geo.size.width - 8) / 3)
                b.frame(width: (geo.size.width - 8) * 2 / 3)
            }
        }
        .frame(height: 52)
    }

    private func field(_ header: String, text: Binding<String>, limit: Int, placeholder: String = "") -> some View {
        VStack(alignment: .leading, spacing: 4) {
            MonoLabel(header)
            TextField(placeholder, text: Binding(get: { text.wrappedValue },
                                                 set: { text.wrappedValue = String($0.prefix(limit)) }))
                .textFieldStyle(.roundedBorder)
        }
    }

    private func secureField(_ header: String, text: Binding<String>, limit: Int, placeholder: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            MonoLabel(header)
            SecureField(placeholder, text: Binding(get: { text.wrappedValue },
                                                   set: { text.wrappedValue = String($0.prefix(limit)) }))
                .textFieldStyle(.roundedBorder)
        }
    }

    private func ghost(_ title: String, enabled: Bool, _ action: @escaping () -> Void) -> some View {
        Button(title, action: action).buttonStyle(GhostButtonStyle()).disabled(!enabled).opacity(enabled ? 1 : 0.45)
    }
}
