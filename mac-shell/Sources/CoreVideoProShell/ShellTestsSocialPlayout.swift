// SocialPlayout, plan 2026-11-22 Task 13: a port of
// native-shell/CoreVideoPro.MediaCore.Tests/GraphicsSocialPlayoutTests.cs.
// The fixture is the C# Fixture: every command must pass the gateway's own
// pre-check (GraphicsCommandRequest.build), the core's instance transitions
// are scripted, and the first 50 ms sleep (preparation) parks until the test
// finishes preparation.

import Foundation

extension ShellTests {
    static var socialPlayoutCases: [(String, () -> Void)] {
        [
            ("social/playout-approval-alone-is-private-and-show-uses-exact-revision-youtube", { MainActor.assumeIsolated { testSocialApprovalAloneIsPrivate("YouTube") } }),
            ("social/playout-approval-alone-is-private-and-show-uses-exact-revision-twitch", { MainActor.assumeIsolated { testSocialApprovalAloneIsPrivate("Twitch") } }),
            ("social/playout-provider-replacement-cannot-reuse-approval", testSocialProviderReplacement),
            ("social/playout-twitch-clear-invalidates-preparation-or-retires", testSocialTwitchClearMatrix),
            ("social/playout-invalidation-while-preparing-never-submits-show", testSocialInvalidationWhilePreparing),
            ("social/playout-moderation-retires-on-air-card-and-rejects-same-batch-replay", testSocialModerationRetires),
            ("social/playout-replacement-during-preparation-starts-pending", testSocialReplacementDuringPreparation),
            ("social/gateway-public-path-still-refuses-social-client", testSocialGatewaySymmetric),
            ("social/templates-are-byte-copies-of-contracts", testSocialTemplatesAreByteCopies),
            ("social/restart-reinstalls-template-on-next-show", testSocialRestartReinstallsTemplate),
            ("social/playout-uses-the-created-revision", testSocialUsesCreatedRevision),
        ]
    }

    // ── fixture ──────────────────────────────────────────────────────────

    @MainActor
    final class SocialFixture {
        var commands: [(action: String, body: JSONObject)] = []
        var state = GraphicsState()
        var preparing = false
        var ready = false
        private var waiters: [CheckedContinuation<Void, Never>] = []
        var installed = true   // false: the core lists no templates until an install lands
        var autoPrepare = false   // the cue lands prepared at once (no parked preparation)
        var createRevision: Int64 = 1   // the core numbers revisions across instances

        init(provider: String = "YouTube") {
            state.schemaVersion = 1
            state.coreEpoch = "epoch"
            state.templates = [Self.template(provider)]
        }

        static func template(_ provider: String) -> GraphicsTemplate {
            var t = GraphicsTemplate()
            t.templateId = SocialCardTemplates.templateId(provider: provider)
            t.templateVersion = 1
            t.runtime = "declarative"
            t.definition = SocialCardTemplates.definition(provider: provider)
            var author = GraphicsTemplateField(); author.id = "author"; author.type = "string"; author.maxLength = 80; author.required = true
            var message = GraphicsTemplateField(); message.id = "message"; message.type = "string"; message.maxLength = 192; message.required = true
            t.fields = [author, message]
            return t
        }

        func make() -> SocialPlayout {
            SocialPlayout(invoke: { [unowned self] action, body in self.invoke(action, body) },
                          observe: { [unowned self] in self.state },
                          sleep: { [unowned self] _ in await self.delay() },
                          now: { socialNow }, newId: { UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "") })
        }

        private func delay() async {
            preparing = true
            if ready { await Task.yield(); return }
            await withCheckedContinuation { waiters.append($0) }
        }

        private func invoke(_ action: String, _ body: JSONObject) -> GraphicsOperationResult {
            if case .failure(let refusal) = GraphicsCommandRequest.build(action: action, request: GraphicsJSON.serialize(body),
                                                                         observed: state) {
                ShellTests.expect(false, "\(action) passes the gateway pre-check: \(refusal.error)")
                return GraphicsOperationResult(reason: refusal.error)
            }
            commands.append((action, body))
            let id = body["instanceId"] as? String ?? ""
            var current = state.instances.first
            switch (action, body["action"] as? String ?? "") {
            case ("graphics.instance.create", _):
                var made = GraphicsInstance(); made.instanceId = id; made.revision = createRevision; current = made
            case ("graphics.instance.cue", _):
                if (body["expectedRevision"] as? NSNumber)?.int64Value != current?.revision {
                    return GraphicsOperationResult(instanceId: id, currentRevision: current?.revision ?? 0, reason: "revision-conflict")
                }
                let rev = current?.revision ?? 1
                if autoPrepare { current?.cueRevision = rev; current?.cuePending = false } else { current?.cuePending = true }
            case ("graphics.instance.trigger", "show"):
                current?.programRevision = 1; current?.cueRevision = 0; current?.visibility = "visible"
            case ("graphics.instance.trigger", "clear"): current?.programRevision = 0; current?.cueRevision = 0
            case ("graphics.instance.remove", _): current = nil
            case ("graphics.template.install", _):
                installed = true
                return GraphicsOperationResult(clientId: SocialPlayout.clientId, requestId: body["requestId"] as? String ?? "",
                                               accepted: true)
            case ("graphics.template.list", _):
                if installed && state.templates.isEmpty { state.templates = [Self.template("YouTube")] }
                return GraphicsOperationResult(clientId: SocialPlayout.clientId, accepted: true)
            default:
                ShellTests.expect(false, "unexpected \(action)")
            }
            state.instances = current.map { [$0] } ?? []
            return GraphicsOperationResult(instanceId: id, currentRevision: current?.revision ?? createRevision, accepted: true)
        }

        func finishPreparation() {
            if var only = state.instances.first {
                only.cueRevision = only.revision; only.cuePending = false
                state.instances = [only]
            }
            ready = true
            let parked = waiters; waiters = []
            parked.forEach { $0.resume() }
        }

        func showTriggers() -> Int {
            commands.filter { $0.action == "graphics.instance.trigger" && $0.body["action"] as? String == "show" }.count
        }
    }

    /// Starts Show and returns once it is parked in preparation.
    @MainActor static func startShow(_ playout: SocialPlayout, _ f: SocialFixture, _ key: SocialItemKey)
        -> Task<GraphicsOperationResult, Never> {
        let show = Task { await playout.show(key) }
        spin(until: { f.preparing }, limit: 5)
        return show
    }

    @MainActor static func value<T>(_ task: Task<T, Never>) -> T? { awaitMain { await task.value } }

    // ── ports ────────────────────────────────────────────────────────────

    @MainActor static func testSocialApprovalAloneIsPrivate(_ provider: String) {
        let f = SocialFixture(provider: provider)
        let p = f.make()
        let source = value(Task { await p.connect(provider: provider) })!
        let approved = value(Task { await p.receive(source: source, id: "a", author: "Host", message: "First message") })!!
        let pending = value(Task { await p.receive(source: source, id: "b", author: "Guest", message: "Second message") })!!
        expectEqual(approved.provider, provider, "provider")
        expect(p.approve(approved.key), "approve")
        expect(value(Task { await p.show(pending.key) })?.accepted == false, "a pending item cannot be shown")
        expect(f.commands.isEmpty, "approval alone sends nothing")
        let show = startShow(p, f, approved.key)
        let create = f.commands.first?.body ?? [:]
        expectEqual(create["templateId"] as? String, SocialCardTemplates.templateId(provider: provider), "provider card")
        expectEqual((create["fields"] as? JSONObject)?["message"] as? String, "First message", "the exact text")
        expectEqual(create["clientId"] as? String, "graphics-social-review", "social client")
        expect((create["instanceId"] as? String)?.hasPrefix("social-review-") == true, "instance prefix")
        expectEqual(f.commands.count > 1 ? f.commands[1].body["rideTake"] as? Bool : nil, false, "the cue never rides a Take")
        expect(f.commands.count > 1 && f.commands[1].body["cueOwner"] == nil, "no cue owner")
        expect(!f.state.instances.contains { $0.programRevision > 0 }, "nothing on Program before prepare")
        f.finishPreparation()
        expect(value(show)?.accepted == true, "shown")
        expectEqual(f.commands.last?.body["target"] as? String, "cue", "show targets the cue")
        expect(p.onProgram, "on Program")
        _ = value(Task { await p.withdraw(source: source, id: "a"); return true })
        expect(!p.onProgram, "withdrawn: off Program")
        expect(f.state.instances.isEmpty, "and removed")
    }

    @MainActor static func testSocialProviderReplacement() {
        let f = SocialFixture()
        let p = f.make()
        let source = value(Task { await p.connect(provider: "Twitch") })!
        let item = value(Task { await p.receive(source: source, id: "1234:a", author: "Host", message: "Reviewed Twitch message") })!!
        expect(p.approve(item.key), "approve")
        _ = value(Task { await p.connect(provider: "YouTube") })
        expect(value(Task { await p.receive(source: source, id: "1234:b", author: "Host", message: "Late Twitch callback") })! == nil,
               "old source's callback refused")
        expect(value(Task { await p.show(item.key) })?.accepted == false, "old approval unusable")
        expect(f.commands.isEmpty, "nothing sent")
        expect(p.items.isEmpty, "empty")
    }

    @MainActor static func testSocialTwitchClearMatrix() {
        for room in [false, true] {
            for onAir in [false, true] {
                let tag = "room=\(room) onAir=\(onAir)"
                let f = SocialFixture(provider: "Twitch")
                let p = f.make()
                let source = value(Task { await p.connect(provider: "Twitch") })!
                let message = SocialFeedEvent.text(id: "1234:a", author: "Host", message: "Reviewed message", authorId: "5678", order: 100)
                _ = value(Task { await p.apply(source: source, [message]) })
                guard let approved = p.items.first else { expect(false, "item \(tag)"); continue }
                expect(p.approve(approved.key), "approve \(tag)")
                let show = startShow(p, f, approved.key)
                if onAir {
                    f.finishPreparation()
                    expect(value(show)?.accepted == true, "shown \(tag)")
                    expect(p.onProgram, "on Program \(tag)")
                }
                let clear = Task { await p.apply(source: source, [room ? .clearRoom(order: 200) : .clearUser(authorId: "5678", order: 200)]) }
                if !onAir {
                    f.finishPreparation()
                    expect(value(show)?.accepted == false, "not shown \(tag)")
                    expectEqual(f.showTriggers(), 0, "no show trigger \(tag)")
                }
                _ = value(clear)
                expect(!p.onProgram, "off Program \(tag)")
                expect(f.state.instances.isEmpty, "removed \(tag)")
                expect(!p.approve(approved.key), "approval gone \(tag)")
                let late = SocialFeedEvent.text(id: "1234:late", author: "Host", message: "Reviewed message", authorId: "5678", order: 199)
                _ = value(Task { await p.apply(source: source, [message, late]) })
                expect(p.items.isEmpty, "replays before the clear fenced \(tag)")
                let fresh = SocialFeedEvent.text(id: "1234:new", author: "Host", message: "Reviewed message", authorId: "5678", order: 201)
                _ = value(Task { await p.apply(source: source, [fresh]) })
                expectEqual(p.items.map(\.decision), [.pending], "a later message is pending \(tag)")
            }
        }
    }

    @MainActor static func testSocialInvalidationWhilePreparing() {
        for invalidation in ["edit", "remove", "disconnect", "provider-ban"] {
            let f = SocialFixture()
            let p = f.make()
            let source = value(Task { await p.connect(provider: "YouTube") })!
            expect(value(Task { await p.apply(source: source, [.text(id: "a", author: "Host", message: "Original",
                                                                     authorId: "host-id", order: 0)]) }) == true, "applied")
            guard let item = p.items.first else { expect(false, "item"); continue }
            expect(p.approve(item.key), "approve")
            let show = startShow(p, f, item.key)
            let invalidated: Task<Void, Never>
            switch invalidation {
            case "edit": invalidated = Task { _ = await p.receive(source: source, id: "a", author: "Host", message: "Changed") }
            case "remove": invalidated = Task { await p.withdraw(source: source, id: "a") }
            case "provider-ban": invalidated = Task { _ = await p.apply(source: source, [.ban(authorId: "host-id")]) }
            default: invalidated = Task { await p.disconnect() }
            }
            f.finishPreparation()
            expect(value(show)?.accepted == false, "\(invalidation): not shown")
            _ = value(invalidated)
            expectEqual(f.showTriggers(), 0, "\(invalidation): no show trigger ever sent")
            expect(f.state.instances.isEmpty, "\(invalidation): the attempted card is gone")
        }
    }

    @MainActor static func testSocialModerationRetires() {
        for banAuthor in [false, true] {
            let f = SocialFixture()
            let p = f.make()
            let source = value(Task { await p.connect(provider: "YouTube") })!
            let reviewed = SocialFeedEvent.text(id: "a", author: "Host", message: "Reviewed message", authorId: "host-id", order: 0)
            let other = SocialFeedEvent.text(id: "b", author: "Guest", message: "Other message", authorId: "guest-id", order: 0)
            expect(value(Task { await p.apply(source: source, [reviewed, other]) }) == true, "applied")
            guard let card = p.items.first(where: { $0.key.id == "a" }), let untouched = p.items.first(where: { $0.key.id == "b" })
            else { expect(false, "items"); continue }
            expect(p.approve(card.key) && p.approve(untouched.key), "approve both")
            let show = startShow(p, f, card.key)
            f.finishPreparation()
            expect(value(show)?.accepted == true, "shown")
            expect(p.onProgram, "on Program")
            let moderation: SocialFeedEvent = banAuthor ? .ban(authorId: "host-id") : .remove(id: "a")
            let otherReplay = SocialFeedEvent.text(id: "new-id", author: "Host", message: "Reviewed message", authorId: "host-id", order: 0)
            expect(value(Task { await p.apply(source: source, [moderation, reviewed, otherReplay]) }) == true, "batch applied")
            expect(!p.onProgram, "ban=\(banAuthor): retired")
            expect(f.state.instances.isEmpty, "removed")
            expect(!p.items.contains { $0.key.id == "a" }, "same-batch replay rejected")
            expectEqual(p.items.contains { $0.key.id == "new-id" }, !banAuthor, "a new id survives a delete, not a ban")
            expectEqual(p.items.first { $0.key.id == "b" }?.decision, .approved, "the other item keeps its approval")
            let before = f.commands.count
            expect(value(Task { await p.show(card.key) })?.accepted == false, "cannot show again")
            expectEqual(f.commands.count, before, "and sends nothing")
        }
    }

    @MainActor static func testSocialReplacementDuringPreparation() {
        let f = SocialFixture()
        let p = f.make()
        let firstSource = value(Task { await p.connect(provider: "YouTube") })!
        let first = value(Task { await p.receive(source: firstSource, id: "id", author: "First host", message: "First message") })!!
        expect(p.approve(first.key), "approve")
        let show = startShow(p, f, first.key)
        let replacement = Task { await p.connect(provider: "YouTube") }
        f.finishPreparation()
        expect(value(show)?.accepted == false, "not shown")
        let secondSource = value(replacement)!
        let second = value(Task { await p.receive(source: secondSource, id: "id", author: "Second host", message: "Second message") })!!
        expect(value(Task { await p.apply(source: firstSource, [.remove(id: "id"),
            .text(id: "ghost", author: "Old host", message: "Late delivery", authorId: "old-id", order: 0)]) }) == false,
               "old source not current")
        _ = value(Task { await p.disconnect(source: firstSource); return true })
        expect(p.isConnected, "an old source cannot disconnect the new one")
        expectEqual(p.items, [second], "only the new item")
        expectEqual(second.decision, .pending, "pending")
        expect(!p.approve(first.key), "old key")
        expect(value(Task { await p.show(second.key) })?.accepted == false, "unapproved")
        expectEqual(f.showTriggers(), 0, "no show trigger")
    }

    // ── Mac-specific ─────────────────────────────────────────────────────

    @MainActor static func testSocialGatewaySymmetric() {
        var sends = 0
        let gateway = GraphicsGateway(capable: { true }, observe: { GraphicsState(node: gatewayObservedNode) },
                                      send: { body in sends += 1; return graphicsAck(body) })
        let publicResult = awaitMain { await gateway.invoke("graphics.instance.cue", cueRequest(client: GraphicsGateway.socialClientId)) }
        expectEqual(publicResult?.reason, "social-graphics-require-operator-review", "public path refuses the social client")
        let request = cueRequest(client: "graphics-playout")
        let socialResult = awaitMain { await gateway.invokeSocial("graphics.instance.cue", request) }
        expectEqual(socialResult?.reason, "social-graphics-require-operator-review", "social path refuses any other client")
        expectEqual(sends, 0, "neither sends")
    }

    static func testSocialTemplatesAreByteCopies() {
        for (name, text) in [("youtube-chat-card", SocialCardTemplates.youTube), ("twitch-chat-card", SocialCardTemplates.twitch)] {
            let url = repoRoot.appendingPathComponent("contracts/graphics-social/\(name).json")
            let file = (try? Data(contentsOf: url)) ?? Data()
            expect(!file.isEmpty, "\(name) contract readable")
            expect(Data(text.utf8) == file, "\(name) is a byte copy of the contract")
        }
    }

    @MainActor static func testSocialRestartReinstallsTemplate() {
        let f = SocialFixture()
        f.state.coreEpoch = "epoch-2"   // a restarted core
        f.state.templates = []          // lists no templates
        f.installed = false
        let p = f.make()
        f.ready = true                  // preparation is not what this case is about
        f.autoPrepare = true
        let source = value(Task { await p.connect(provider: "YouTube") })!
        let item = value(Task { await p.receive(source: source, id: "a", author: "Host", message: "Hello") })!!
        expect(p.approve(item.key), "approve")
        expect(value(Task { await p.show(item.key) })?.accepted == true, "shown on the restarted core")
        let actions = f.commands.map(\.action)
        expectEqual(Array(actions.prefix(3)), ["graphics.template.install", "graphics.template.list", "graphics.instance.create"],
                    "install, list, then create")
        expectEqual(f.commands.first?.body["packageRoot"] as? String, "", "a manifest install with no package root")
    }

    /// The core numbers revisions across instances, so a later card is created
    /// at revision > 1; Show must cue and trigger at THAT revision (live finding).
    @MainActor static func testSocialUsesCreatedRevision() {
        let f = SocialFixture()
        f.createRevision = 7
        f.ready = true; f.autoPrepare = true
        let p = f.make()
        let source = value(Task { await p.connect(provider: "YouTube") })!
        let item = value(Task { await p.receive(source: source, id: "a", author: "Host", message: "Hello") })!!
        expect(p.approve(item.key), "approve")
        let shown = value(Task { await p.show(item.key) })
        expectEqual(shown?.accepted, true, "shown at the created revision (\(shown?.reason ?? "nil"))")
        let cue = f.commands.first { $0.action == "graphics.instance.cue" }?.body
        expectEqual((cue?["expectedRevision"] as? NSNumber)?.int64Value, 7, "cue at expectedRevision 7")
    }
}
