// The private social queue and the sole command path from an approved item to
// air: a port of native-shell/CoreVideoPro.MediaCore/Services/GraphicsSocialPlayout.cs.
//
// Main-actor isolated (the Windows class uses a lock plus a semaphore): review
// mutations apply at once, as Windows' ChangeAsync does under its lock, while
// Show, Hide and card retirement are serialised by `operations` (Windows
// `_operations`). Every await is a point where a feed edit, a moderator
// delete or a disconnect may land, so Show re-checks the exact approval
// before create, during preparation and immediately before the trigger, and
// an on-air card whose approval vanished is retired after every mutation.
// Wire identity: clientId graphics-social-review, instance prefix
// social-review-, the cue never rides a Take (rideTake:false, no cueOwner),
// and Show targets the cue.

import Foundation

@MainActor
final class SocialPlayout {
    static let clientId = GraphicsGateway.socialClientId
    static let instancePrefix = "social-review-"

    typealias Invoke = @MainActor (_ action: String, _ body: JSONObject) async -> GraphicsOperationResult

    /// `revision` is what the core's create returned. Windows hard-codes 1
    /// (GraphicsSocialPlayout.Card), but the core numbers revisions across
    /// instances: the second card in one core process is created at 2, and a
    /// cue at expectedRevision 1 is refused with revision-conflict (found by the
    /// live check, 2026-11-23). The Mac uses the created revision.
    private struct Card { let key: SocialItemKey; let epoch: String; let id: String; var revision: Int64 = 1 }

    let review: SocialReview
    private let invoke: Invoke
    private let observe: () -> GraphicsState?
    private let sleep: (UInt64) async -> Void
    private let now: () -> Date
    private let newId: () -> String
    private var card: Card?
    private let operations = SocialAsyncLock()

    init(invoke: @escaping Invoke, observe: @escaping () -> GraphicsState?,
         sleep: @escaping (UInt64) async -> Void = { try? await Task.sleep(nanoseconds: $0) },
         now: @escaping () -> Date = Date.init,
         newId: @escaping () -> String = { UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "") }) {
        self.invoke = invoke
        self.observe = observe
        self.sleep = sleep
        self.now = now
        self.newId = newId
        self.review = SocialReview(now: now)
    }

    /// Production: the gateway's gated social path and its observed state.
    convenience init(gateway: GraphicsGateway) {
        self.init(invoke: { [weak gateway] action, body in
            await gateway?.invokeSocial(action, body) ?? .refused(GraphicsCommandRequest.unavailable)
        }, observe: { [weak gateway] in gateway?.observe() })
    }

    var items: [SocialItem] { review.items }
    var isConnected: Bool { review.isConnected }

    var onProgram: Bool {
        guard let card, let state = current(card.epoch) else { return false }
        return state.instances.contains { $0.instanceId == card.id && $0.isOnProgram }
    }

    // ── review mutations (each followed by the retire-if-invalid rule) ───

    @discardableResult func connect(provider: String) async -> Int64 {
        await change { self.review.connect(provider: provider) }
    }

    func disconnect() async { await change { self.review.disconnect() } }

    func disconnect(source: Int64) async {
        await change { if self.review.isCurrent(source) { self.review.disconnect() } }
    }

    func expire() async { await change { _ = self.review.items } }

    @discardableResult func apply(source: Int64, _ events: [SocialFeedEvent]) async -> Bool {
        await change {
            for event in events { self.review.apply(source: source, event) }
            return self.review.isCurrent(source)
        }
    }

    @discardableResult func receive(source: Int64, id: String, author: String, message: String) async -> SocialItem? {
        await change { self.review.receive(source: source, id: id, author: author, message: message) }
    }

    func withdraw(source: Int64, id: String) async { await change { self.review.withdraw(source: source, id: id) } }

    @discardableResult func withdraw(_ key: SocialItemKey) async -> Bool { await change { self.review.withdraw(key) } }

    func approve(_ key: SocialItemKey) -> Bool { review.approve(key) }
    func reject(_ key: SocialItemKey) -> Bool { review.reject(key) }

    // ── Show / Hide (GraphicsSocialPlayout.cs:73-155) ───────────────────

    func show(_ key: SocialItemKey) async -> GraphicsOperationResult {
        await operations.acquire()
        var attempted: Card?
        var shown = false
        let result = await showLocked(key, attempted: &attempted)
        shown = result.accepted
        if attempted != nil { _ = await retire(invalidOnly: shown) }
        operations.release()
        return result
    }

    private func showLocked(_ key: SocialItemKey, attempted: inout Card?) async -> GraphicsOperationResult {
        guard let item = review.approved(key) else { return Self.refuse("Approve this exact message before showing it.") }
        let templateId = SocialCardTemplates.templateId(provider: item.provider)
        let bundled = SocialCardTemplates.definition(provider: item.provider)
        guard await retire(invalidOnly: false) else { return Self.refuse("The previous social graphic could not be cleared.") }
        guard let state = observe(), !state.coreEpoch.isEmpty else { return Self.refuse("Native Graphics is unavailable.") }
        let epoch = state.coreEpoch
        var template = state.template(templateId, 1)
        if template == nil {
            let (installed, refusal) = await install(templateId: templateId, definition: SocialCardTemplates.text(provider: item.provider),
                                                     epoch: epoch)
            if let refusal { return refusal }
            template = installed
        }
        guard let definition = template?.definition, (definition as NSDictionary).isEqual(to: bundled) else {
            return Self.refuse("The native social graphic template could not be verified.")
        }
        guard review.approved(key) != nil else { return Self.refuse("The message changed before Show.") }
        var made = Card(key: key, epoch: epoch, id: Self.instancePrefix + newId())
        card = made
        attempted = made
        let created = await send("graphics.instance.create", epoch: epoch, [
            "instanceId": made.id, "scope": "global", "templateId": templateId, "templateVersion": 1,
            "fields": ["author": item.author, "message": item.message],
        ])
        guard created.accepted else { return created }
        let revision = created.currentRevision > 0 ? created.currentRevision
            : current(epoch)?.instance(made.id)?.revision ?? 1
        made.revision = revision
        card = made
        attempted = made
        let cued = await instanceCommand("graphics.instance.cue", made, ["rideTake": false])
        guard cued.accepted else { return cued }
        let started = now()
        while true {
            guard let observed = current(epoch) else { return Self.refuse("Native Graphics restarted.") }
            let instance = observed.instance(made.id)
            if let instance, instance.cueRevision == revision, !instance.cuePending, instance.revision == revision { break }
            if instance?.cuePreparation == "failed" || now().timeIntervalSince(started) >= 5 {
                return Self.refuse("The social graphic could not prepare.")
            }
            if review.approved(key) == nil { return Self.refuse("The message changed before Show.") }
            await sleep(50_000_000)
        }
        // The last check and the trigger are one step on the main actor: no
        // feed callback can land between them.
        guard review.approved(key) != nil, current(epoch) != nil else {
            return Self.refuse("The message changed before Show.")
        }
        return await instanceCommand("graphics.instance.trigger", made, ["action": "show", "target": "cue"])
    }

    func hide() async -> Bool {
        await operations.acquire()
        let ok = await retire(invalidOnly: false)
        operations.release()
        return ok
    }

    // ── internals ────────────────────────────────────────────────────────

    /// A review mutation applies at once; an on-air (or attempted) card whose
    /// exact approval is gone is then retired under the operations lock.
    private func change<T>(_ apply: () -> T) async -> T {
        let value = apply()
        if let card, review.approved(card.key) == nil {
            await operations.acquire()
            _ = await retire(invalidOnly: true)
            operations.release()
        }
        return value
    }

    /// Up to 3 attempts 50 ms apart: clear, then remove; instance-not-found
    /// counts as gone. A core that restarted took the card with it.
    private func retire(invalidOnly: Bool) async -> Bool {
        guard let card else { return true }
        if invalidOnly, review.approved(card.key) != nil { return true }
        var attempt = 0
        while attempt < 3, current(card.epoch) != nil {
            let cleared = await instanceCommand("graphics.instance.trigger", card, ["action": "clear"])
            if cleared.accepted || cleared.reason == "instance-not-found" {
                let removed = await instanceCommand("graphics.instance.remove", card, [:])
                if removed.accepted || removed.reason == "instance-not-found" {
                    self.card = nil
                    return true
                }
            }
            await sleep(50_000_000)
            attempt += 1
        }
        if current(card.epoch) != nil { return false }
        self.card = nil
        return true
    }

    /// GraphicsTemplateInstall.AttemptAsync over the social path.
    private func install(templateId: String, definition: String, epoch: String) async -> (GraphicsTemplate?, GraphicsOperationResult?) {
        let requestId = "install-" + newId()
        let imported = await send("graphics.template.install", epoch: epoch,
                                  ["manifest": definition, "packageRoot": "", "requestId": requestId])
        guard imported.accepted else { return (nil, imported) }
        let limit = now().addingTimeInterval(5)
        repeat {
            await sleep(50_000_000)
            let listed = await send("graphics.template.list", epoch: epoch, [:])
            guard listed.accepted else { return (nil, listed) }
            guard let state = current(epoch) else { return (nil, Self.refuse("The social graphic could not be shown.")) }
            if let template = state.template(templateId, 1) { return (template, nil) }
            if let failed = state.templateInstalls.first(where: { $0.requestId == requestId && $0.state == "failed" }) {
                let reason = failed.reason == "template-library-capacity"
                    ? "The graphics library is full. Delete designs you no longer use, or remove graphics from the playlist."
                    : "The template could not be installed (\(failed.reason))."
                return (nil, GraphicsOperationResult(requestId: requestId, accepted: false, reason: reason))
            }
        } while now() < limit
        return (nil, nil)
    }

    private func current(_ epoch: String) -> GraphicsState? {
        guard let state = observe(), state.coreEpoch == epoch else { return nil }
        return state
    }

    private func instanceCommand(_ action: String, _ card: Card, _ body: JSONObject) async -> GraphicsOperationResult {
        var body = body
        body["instanceId"] = card.id
        body["expectedRevision"] = card.revision
        return await send(action, epoch: card.epoch, body)
    }

    private func send(_ action: String, epoch: String, _ body: JSONObject) async -> GraphicsOperationResult {
        guard current(epoch) != nil else { return Self.refuse("Native Graphics restarted.") }
        var body = body
        body["schemaVersion"] = 1
        body["clientId"] = Self.clientId
        if body["requestId"] == nil { body["requestId"] = "social-" + newId() }
        body["coreEpoch"] = epoch
        return await invoke(action, body)
    }

    private static func refuse(_ reason: String) -> GraphicsOperationResult { GraphicsOperationResult(reason: reason) }
}

/// A FIFO async mutex for the main actor (Windows SemaphoreSlim(1,1)).
@MainActor
final class SocialAsyncLock {
    private var held = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    func acquire() async {
        if !held { held = true; return }
        await withCheckedContinuation { waiters.append($0) }
    }

    func release() {
        if waiters.isEmpty { held = false } else { waiters.removeFirst().resume() }
    }
}
