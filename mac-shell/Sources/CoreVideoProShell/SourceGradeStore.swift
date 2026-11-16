// SourceGradeStore — the ONLY sender of `set-source-grade` (grading plan 2026-11-16, Task 5;
// spec §5.3). It owns the identity and revision fences the core enforces, keeps one apply in
// flight per source, and re-asserts every session grade on every core generation (CLAUDE.md /
// gotchas: "a one-shot command must be re-applied on every core generation").
//
// Fences, in order: the person (binding) the draft was authored for; the core's source epoch
// (query-source-grade, re-learned after a respawn or an epoch change); the core's revision;
// and the store's own generation counter, so an acknowledgement from a previous core process
// is never recorded.
//
// D6 overridden: a NEUTRAL grade is sent with `clear: true`, so the core forgets the source's
// own grade and its layer takes the Program Look again. The store then holds no session grade
// for it and forgets the saved key.

import Foundation

enum GradeApplyOutcome: Equatable {
    case applied(revision: Int64)
    case refused(String)       // operator text, verbatim Windows wording
    case superseded            // a newer edit replaced this one before it was sent
    case stale(String)         // the core generation changed while it was in flight
}

/// What the Sources inspector reads (spec §5.8).
struct SourceGradeSummary: Equatable {
    let line: String           // SourceColorGrade.summary
    let indicator: String?     // the Windows AdvancedIndicator once G2 lands; nil in G1
    let active: Bool           // false while sourceGrades reports active:false
    let identityChanged: Bool  // the live person no longer matches the grade's binding
}

extension CoreCommands {
    // Root-level requests (JsonRpcServer reads sourceId etc. off the request root).
    static func querySourceGrade(sourceId: String) -> JSONObject {
        ["type": "query-source-grade", "sourceId": sourceId]
    }

    /// `clear: true` (D6 override) asks the core to forget the source's own grade.
    static func setSourceGrade(sourceId: String, sourceEpoch: UInt64, expectedRevision: Int64,
                               grade: JSONObject, clear: Bool = false) -> JSONObject {
        var body: JSONObject = ["type": "set-source-grade", "sourceId": sourceId,
                                "sourceEpoch": NSNumber(value: sourceEpoch),
                                "expectedRevision": NSNumber(value: expectedRevision), "grade": grade]
        if clear { body["clear"] = true }
        return body
    }
}

@MainActor
final class SourceGradeStore: ObservableObject {
    typealias Send = (JSONObject) async throws -> JSONObject

    static let reassertInterval: TimeInterval = 0.5      // first 10 attempts per source
    static let reassertSlowInterval: TimeInterval = 2.0  // after that
    static let reassertFastAttempts = 10

    static let identityChangedText = "Source identity changed. Close and reopen its grade editor before applying."
    static let noVideoText = "Source has no video yet; grade not applied."
    static let staleText = "Source changed during apply. Grade acknowledgement is no longer current; review the draft again."
    static let draftNotice = "Saved grade loaded as a draft. Apply Live confirms this source binding."

    var identityProvider: (String) -> GradeSourceIdentity? = { _ in nil }
    var now: () -> Date = Date.init
    @Published private(set) var lastNotice = ""
    /// Bumped on every change a reader (editor, inspector) may show.
    @Published private(set) var revisionTick = 0

    private let send: Send
    private let filePath: String
    private let saveDelay: TimeInterval
    private var file: SourceGradeFile
    private var saveWork: DispatchWorkItem?
    private var generation = 0

    /// What the store knows about one source in the current core generation.
    private struct Known {
        var epoch: UInt64?                // nil: ask the core before the next set
        var revision: Int64 = 0
    }
    private struct Session {
        var grade: SourceColorGrade
        var binding: String
        var isZoom: Bool
        var persistentId: String          // "" for capture / anonymous
        var active = true
        var identityChanged = false
    }
    private struct Pending {
        var grade: SourceColorGrade
        var binding: String
        var fromSaved: Bool               // D2: a saved capture grade, not yet applied this launch
        var attempts = 0
        var lastAttempt: Date?
    }
    private struct Parked {
        let grade: SourceColorGrade
        let binding: String
        let resume: (GradeApplyOutcome) -> Void
    }

    private var known: [String: Known] = [:]
    private var sessions: [String: Session] = [:]
    private var reasserts: [String: Pending] = [:]
    private var inFlight: Set<String> = []
    private var parked: [String: Parked] = [:]
    private var appliedSavedThisLaunch: Set<String> = []

    init(send: @escaping Send, filePath: String = SourceGradeFile.path, saveDelay: TimeInterval = 0.75) {
        self.send = send
        self.filePath = filePath
        self.saveDelay = saveDelay
        let loaded = SourceGradeFile.load(from: filePath)
        file = loaded.file
        lastNotice = loaded.notices.last ?? ""
    }

    // ── apply: the editor's only path to air ─────────────────────────────────

    func apply(_ grade: SourceColorGrade, sourceId: String, authoredFor binding: String) async -> GradeApplyOutcome {
        guard identityProvider(sourceId)?.binding == binding else { return .refused(Self.identityChangedText) }
        if inFlight.contains(sourceId) {
            return await withCheckedContinuation { continuation in
                if let previous = parked[sourceId] { previous.resume(.superseded) }
                parked[sourceId] = Parked(grade: grade, binding: binding, resume: { continuation.resume(returning: $0) })
            }
        }
        return await runExclusive(grade, sourceId: sourceId, binding: binding)
    }

    /// Runs one apply, then the newest parked one for the same source, if any.
    private func runExclusive(_ grade: SourceColorGrade, sourceId: String, binding: String) async -> GradeApplyOutcome {
        inFlight.insert(sourceId)
        let outcome = await perform(grade, sourceId: sourceId, binding: binding)
        finish(sourceId)
        return outcome
    }

    /// The source's in-flight apply (an edit or a re-assert) is done: run the parked edit.
    private func finish(_ sourceId: String) {
        inFlight.remove(sourceId)
        guard let next = parked.removeValue(forKey: sourceId) else { return }
        Task { @MainActor in
            let result: GradeApplyOutcome
            if self.identityProvider(sourceId)?.binding != next.binding {
                result = .refused(Self.identityChangedText)
            } else if self.inFlight.contains(sourceId) {
                // A re-assert started in between: park again behind it.
                result = await self.apply(next.grade, sourceId: sourceId, authoredFor: next.binding)
            } else {
                result = await self.runExclusive(next.grade, sourceId: sourceId, binding: next.binding)
            }
            next.resume(result)
        }
    }

    private func perform(_ grade: SourceColorGrade, sourceId: String, binding: String) async -> GradeApplyOutcome {
        let started = generation
        let wire: SourceColorGrade
        do { wire = try grade.validated(range: SourceColorGrade.wireRange) }
        catch let error as GradeValidationError { return .refused("Grade was not applied: \(error.message)") }
        catch { return .refused("Grade was not applied: \(error.localizedDescription)") }
        let clear = wire.isNeutral
        var retried = false
        while true {
            do {
                if known[sourceId]?.epoch == nil {
                    switch try await query(sourceId) {
                    case .stale: return .stale(Self.staleText)
                    case .absent: return .refused(Self.noVideoText)
                    case .present: break
                    }
                }
                let held = known[sourceId] ?? Known()
                let body = CoreCommands.setSourceGrade(
                    sourceId: sourceId, sourceEpoch: held.epoch ?? 0, expectedRevision: held.revision,
                    grade: wire.wireGrade(documentRef: nil), clear: clear)
                let response = try await send(body)
                guard generation == started else { return .stale(Self.staleText) }
                if response["accepted"] as? Bool == true {
                    let revision = (response["revision"] as? NSNumber)?.int64Value ?? held.revision + 1
                    known[sourceId] = Known(epoch: held.epoch, revision: revision)
                    record(wire, sourceId: sourceId, binding: binding, cleared: clear)
                    return .applied(revision: revision)
                }
                let reason = response["reason"] as? String ?? "unknown"
                if !retried, reason == "source-identity-changed" || reason == "grade-revision-conflict" {
                    // D7: re-query once, re-check the person, retry once.
                    retried = true
                    known[sourceId]?.epoch = nil
                    switch try await query(sourceId) {
                    case .stale: return .stale(Self.staleText)
                    case .absent: return .refused(Self.noVideoText)
                    case .present: break
                    }
                    guard identityProvider(sourceId)?.binding == binding else { return .refused(Self.identityChangedText) }
                    continue
                }
                return .refused("Grade was not applied: \(reason). Previous live grade retained.")
            } catch {
                guard generation == started else { return .stale(Self.staleText) }
                let message = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
                return .refused("Grade was not applied: \(message)")
            }
        }
    }

    private enum QueryResult { case present, absent, stale }

    /// query-source-grade; on present, the core's epoch and revision replace the store's.
    private func query(_ sourceId: String) async throws -> QueryResult {
        let started = generation
        let response = try await send(CoreCommands.querySourceGrade(sourceId: sourceId))
        guard generation == started else { return .stale }
        let revision = (response["revision"] as? NSNumber)?.int64Value ?? 0
        guard response["present"] as? Bool == true else {
            known[sourceId] = Known(epoch: nil, revision: revision)
            return .absent
        }
        let epoch = (response["sourceEpoch"] as? NSNumber)?.uint64Value ?? 0
        known[sourceId] = Known(epoch: epoch, revision: revision)
        return .present
    }

    private func record(_ grade: SourceColorGrade, sourceId: String, binding: String, cleared: Bool) {
        let identity = identityProvider(sourceId)
        reasserts[sourceId] = nil
        if let key = identity?.stableKey { appliedSavedThisLaunch.insert(key) }
        if cleared {
            sessions[sourceId] = nil
            if let key = identity?.stableKey, file.grades[key] != nil {
                file.forget(key: key)
                scheduleSave()
            }
        } else {
            var persistentId = ""
            if case .zoom(_, let pid, _)? = identity { persistentId = pid }
            sessions[sourceId] = Session(grade: grade, binding: binding, isZoom: identity?.isZoom ?? !sourceId.hasPrefix("capture:"),
                                         persistentId: persistentId)
            if let key = identity?.stableKey {
                if let notice = file.remember(grade, key: key) { lastNotice = notice }
                scheduleSave()
            }
        }
        revisionTick &+= 1
    }

    // ── lifecycle ────────────────────────────────────────────────────────────

    /// Every core generation (launch handshake AND respawn): forget every epoch and revision,
    /// and re-assert every session grade plus every saved capture grade not yet applied (D2).
    func coreGenerationChanged() {
        generation &+= 1
        known.removeAll()
        reasserts.removeAll()
        for (sourceId, session) in sessions {
            reasserts[sourceId] = Pending(grade: session.grade, binding: session.binding, fromSaved: false)
        }
        for key in file.order where key.hasPrefix("capture:") && !appliedSavedThisLaunch.contains(key) {
            guard sessions[key] == nil, let grade = file.grades[key] else { continue }
            reasserts[key] = Pending(grade: grade, binding: key, fromSaved: true)
        }
        revisionTick &+= 1
    }

    func observe(sourceGrades rows: [JSONObject]?, sources: [GradeSourceIdentity], now: Date) {
        let present = Dictionary(sources.map { ($0.sourceId, $0) }, uniquingKeysWith: { first, _ in first })
        for row in rows ?? [] {
            guard let id = row["sourceId"] as? String, var session = sessions[id] else { continue }
            let active = row["active"] as? Bool ?? true
            let livePresent = row["livePresent"] as? Bool ?? false
            session.active = active
            if !active, livePresent {
                known[id]?.epoch = nil
                if let identity = present[id], identity.binding == session.binding {
                    session.identityChanged = false
                    if reasserts[id] == nil {
                        reasserts[id] = Pending(grade: session.grade, binding: session.binding, fromSaved: false)
                    }
                } else if present[id] != nil {
                    session.identityChanged = true
                    reasserts[id] = nil
                }
            }
            sessions[id] = session
        }
        followPeople(present: present)
        for (id, pending) in reasserts {
            guard let identity = present[id] else { continue }
            guard identity.binding == pending.binding else {
                reasserts[id] = nil
                sessions[id]?.identityChanged = true
                continue
            }
            guard !inFlight.contains(id) else { continue }
            let interval = pending.attempts < Self.reassertFastAttempts ? Self.reassertInterval : Self.reassertSlowInterval
            if let last = pending.lastAttempt, now.timeIntervalSince(last) < interval { continue }
            reasserts[id]?.attempts += 1
            reasserts[id]?.lastAttempt = now
            reassert(id, pending)
        }
        revisionTick &+= 1
    }

    /// D5: within one meeting a session grade follows its person to a new participant id once
    /// the old id has left, when exactly one present Zoom source carries the same person.
    private func followPeople(present: [String: GradeSourceIdentity]) {
        for (id, session) in sessions where session.isZoom && !session.persistentId.isEmpty && present[id] == nil {
            let candidates = present.values.filter { $0.isZoom && $0.binding == session.binding && $0.sourceId != id }
            guard candidates.count == 1, let target = candidates.first, sessions[target.sourceId] == nil else { continue }
            sessions[id] = nil
            reasserts[id] = nil
            known[id] = nil
            var moved = session
            moved.active = true
            sessions[target.sourceId] = moved
            known[target.sourceId] = nil
            reasserts[target.sourceId] = Pending(grade: session.grade, binding: session.binding, fromSaved: false)
        }
    }

    private func reassert(_ sourceId: String, _ pending: Pending) {
        let started = generation
        inFlight.insert(sourceId)
        Task { @MainActor in
            defer { self.finish(sourceId) }
            let present: Bool
            do { present = try await self.query(sourceId) == .present } catch { return }
            guard present, self.generation == started else { return }
            let outcome = await self.perform(pending.grade, sourceId: sourceId, binding: pending.binding)
            switch outcome {
            case .applied:
                if pending.fromSaved { self.appliedSavedThisLaunch.insert(sourceId) }
            case .refused(let text) where text == Self.noVideoText:
                break                                  // stays pending; paced retry
            case .refused(let text):
                if !text.contains("source-identity-changed") && !text.contains("grade-revision-conflict") {
                    self.reasserts[sourceId] = nil     // a permanent refusal: stop, say so
                    self.lastNotice = text
                }
            case .superseded, .stale:
                break
            }
        }
    }

    /// The meeting ended (a KNOWN not-in-meeting state). Zoom reuses ids per meeting, so every
    /// Zoom session grade and its pending re-assert go. Capture grades and the saved file stay.
    func meetingEnded() {
        for (id, session) in sessions where session.isZoom {
            sessions[id] = nil
            reasserts[id] = nil
            known[id] = nil
        }
        revisionTick &+= 1
    }

    /// Write the saved file now (quit).
    func flush() {
        saveWork?.cancel()
        saveWork = nil
        try? file.save(to: filePath)
    }

    private func scheduleSave() {
        saveWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.saveWork = nil
            try? self.file.save(to: self.filePath)
        }
        saveWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + saveDelay, execute: work)
    }

    // ── reads (editor seed; Sources inspector) ───────────────────────────────

    func seed(for sourceId: String) -> (grade: SourceColorGrade, draftNotice: String?) {
        let identity = identityProvider(sourceId)
        if let session = sessions[sourceId], session.binding == identity?.binding { return (session.grade, nil) }
        if let key = identity?.stableKey, let saved = file.grades[key] {
            return (saved, identity?.isZoom == true ? Self.draftNotice : nil)
        }
        return (SourceColorGrade(), nil)
    }

    func summary(for sourceId: String) -> SourceGradeSummary? {
        guard let session = sessions[sourceId] else { return nil }
        return SourceGradeSummary(line: session.grade.summary, indicator: nil, active: session.active,
                                  identityChanged: session.identityChanged)
    }

    func isGraded(_ sourceId: String) -> Bool { sessions[sourceId] != nil }
}

extension AppModel {
    /// The identity a grade for `sourceId` is bound to now: a capture id is its own binding;
    /// a Zoom id resolves through the live roster (nil when the participant is not present).
    func gradeIdentity(for sourceId: String) -> GradeSourceIdentity? {
        if sourceId.hasPrefix("capture:") {
            let deviceId = String(sourceId.dropFirst("capture:".count))
            return deviceId.isEmpty ? nil : .capture(deviceId: deviceId)
        }
        guard let participant = roster.first(where: { $0.id == sourceId }) else { return nil }
        return .zoom(participantId: participant.id, persistentId: participant.persistentId ?? "",
                     sourceGeneration: participant.sourceGeneration)
    }

    /// Every roster participant and capture device now known.
    var gradeSources: [GradeSourceIdentity] {
        roster.map { .zoom(participantId: $0.id, persistentId: $0.persistentId ?? "", sourceGeneration: $0.sourceGeneration) }
            + captureDevices.map { .capture(deviceId: $0.id) }
    }
}
