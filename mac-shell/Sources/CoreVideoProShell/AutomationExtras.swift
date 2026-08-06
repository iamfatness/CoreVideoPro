// AutomationExtras — the AUTOMATION-tab parity work the mac shell was missing
// against `native-shell/CoreVideoPro.WinUI/Views/AutomationPage.xaml`.
//
// EVERY behaviour here is a port of a Windows behaviour that was read out of the
// WinUI source, not invented. The Windows homes are named per member. Nothing in
// this file changes the C++ core; the only core traffic it adds is the existing
// `set-overlay-asset` key:lower-third command the mac shell already speaks.
//
// What the core actually owns (verified in native/src/core):
//   * The SCENE RECOMMENDATION — `MediaCore::autoProductionState()`
//     (MediaCore.cpp:1200) publishes {ruleId, recommendedSceneId, confidence,
//     rationale} from the pure kernel `recommendScene` (Director.h:62) on every
//     snapshot. `recommend-auto-production` is a documented NO-OP command
//     (MediaCore.cpp:1122) — the core never switches a scene itself.
//   * Overlays — `set-overlay-asset` (Protocol.h:45), which is how the lower
//     third is keyed.
// Everything else on the Automation page is SHELL POLICY on both platforms: the
// confidence gate, the hold timer, auto-take, and the lower-third arming rule
// all live in `MagicSceneCoordinator` / `StudioViewModel` on Windows.

import SwiftUI

// Persisted automation policy that ShellPrefs carries. Kept in one Codable
// struct so the prefs file grows by ONE optional key (a non-optional key would
// make every pre-existing shell-preferences.json fail to decode — Swift's
// synthesized Decodable throws keyNotFound rather than using the default — and
// ShellPrefs.load()'s `try?` would silently reset the operator's whole file).
struct AutomationExtrasState: Codable, Equatable {
    // REAL: arms the automatic program lower third (Windows
    // StudioViewModel.ShouldEnableProgramLowerThird, StudioViewModel.cs:12377).
    var lowerThirdsEnabled = true

    // DISPLAY-ONLY on macOS — see AutomationExtrasSection's "not wired" block.
    // On Windows these two only feed the OFFLINE fallback heuristic
    // (ProductionStateHelper.BuildAutomationRecommendation, ProductionModels.cs:851),
    // which is bypassed whenever the core publishes a recommendation
    // (ProductionModels.cs:925) — and the core publishes one on every snapshot.
    var preferScreenShare = true
    var panelParticipantThreshold = 4.0
}

// MARK: - Model

extension AppModel {

    // ── readouts (WinUI StudioViewModel.cs:1044-1077, ported verbatim) ────────

    /// StudioViewModel.MagicSceneStatus ← ProductionStateHelper.BuildMagicSceneStatus
    /// (ProductionModels.cs:966).
    var magicSceneStatus: String {
        if roster.isEmpty { return "Join a meeting to enable Magic Scene" }
        let cameras = roster.filter(\.hasVideo).count
        let sharing = roster.filter(\.sharingScreen).count
        return "Monitoring \(roster.count) participants · \(cameras) on camera · \(sharing) sharing"
    }

    /// StudioViewModel.SceneIntelligenceSummary ← BuildSceneIntelligenceSummary
    /// (ProductionModels.cs:949). Windows keys the speaker off
    /// `Participant.IsActiveSpeaker`; the mac roster's equivalent engine field is
    /// `talking` (the same signal MediaCore::deriveDirectorSignals reasons over,
    /// MediaCore.cpp:1156).
    var sceneIntelligenceSummary: String {
        if roster.isEmpty { return "No meeting activity — scene intelligence idle" }
        let speaker = roster.first(where: \.talking)?.name ?? "No active speaker"
        let share = roster.contains(where: \.sharingScreen)
            ? "screen share active" : "no screen share"
        return "\(speaker) · \(share) · \(autoDirectEnabled ? "automation on" : "manual control")"
    }

    /// The scene the director's recommendation resolves to in THIS shell's
    /// catalog. Windows can look the id up directly (its catalog uses the
    /// director's own ids); the mac catalog is fullscreen/two-up/pip, so it goes
    /// through the existing directorSceneMap.
    private var recommendedScene: SceneDef? {
        guard let mapped = Self.directorSceneMap[autoSceneId] else { return nil }
        return scenes.first { $0.id == mapped }
    }

    /// StudioViewModel.RecommendedSceneName (ProductionModels.cs:1036).
    var recommendedSceneName: String { recommendedScene?.name ?? "—" }

    /// StudioViewModel.RecommendedLayout (ProductionModels.cs:1039).
    var recommendedLayoutLabel: String { recommendedScene?.layout ?? "—" }

    /// StudioViewModel.RecommendedConfidence (StudioViewModel.cs:1054).
    var recommendedConfidenceLabel: String {
        autoConfidence > 0 ? "\(autoConfidence)%" : "—"
    }

    /// StudioViewModel.CamerasOnCount (StudioViewModel.cs:1073).
    var camerasOnCount: Int { roster.filter(\.hasVideo).count }

    /// StudioViewModel.ScreenShareLabel (StudioViewModel.cs:1075).
    var screenShareLabel: String {
        roster.contains(where: \.sharingScreen) ? "Active" : "Off"
    }

    /// StudioViewModel.AutoSwitchLabel (StudioViewModel.cs:1057).
    var autoSwitchLabel: String { autoDirectEnabled ? "Auto" : "Manual" }

    /// StudioViewModel.AutomationPolicySummary (StudioViewModel.cs:1059).
    var automationPolicySummary: String {
        (autoTakeEnabled ? "Auto-take" : "Queue preview")
            + String(format: " · %.0f%% confidence · %.0fs hold",
                     autoConfidenceThreshold, autoHoldSeconds)
    }

    /// StudioViewModel.AutomationScenePolicySummary (StudioViewModel.cs:1063).
    var automationScenePolicySummary: String {
        (autoExtras.preferScreenShare ? "Prefer screen share" : "Ignore screen share")
            + String(format: " · panel at %.0f+ sources", autoExtras.panelParticipantThreshold)
    }

    /// StudioViewModel.AutomationOverlayPolicySummary (StudioViewModel.cs:1067).
    /// The captions half is deliberately absent — see the "not wired" block.
    var automationOverlayPolicySummary: String {
        autoExtras.lowerThirdsEnabled ? "Lower thirds on" : "Lower thirds off"
    }

    /// StudioViewModel.AutomationTakeModeLabel (StudioViewModel.cs:1071).
    var automationTakeModeLabel: String {
        autoTakeEnabled ? "Take to program" : "Queue on preview"
    }

    // ── Magic Scene (MagicSceneCoordinator.RunMagicScene, :112) ───────────────

    var canRunMagicScene: Bool { !autoSceneId.isEmpty && recommendedScene != nil }

    /// Queues the director's recommendation onto PREVIEW — never program (the
    /// Windows command sets PreviewSceneId + SchedulePreviewRoutingRefresh; the
    /// mac equivalent of that refresh is the coalesced syncScenes()).
    func runMagicScene() {
        guard meetingState == "in_meeting" || !roster.isEmpty else {
            autoStatus = "Magic Scene requires an active Zoom meeting"
            return
        }
        guard let scene = recommendedScene else {
            autoStatus = "Director idle — no recommendation to apply"
            return
        }
        previewSceneId = scene.id
        syncScenes()
        evaluateAutomationLowerThird()
        autoStatus = "Magic Scene applied: \(scene.name) queued on preview"
    }

    // ── reset (MagicSceneCoordinator.ResetAutomationDefaults, :265) ───────────

    /// The Windows defaults, verbatim: auto-take on, prefer screen share on,
    /// lower thirds on, confidence 70, hold 4s, panel at 4.
    func resetAutomationDefaults() {
        autoTakeEnabled = true
        autoConfidenceThreshold = 70
        autoHoldSeconds = 4
        autoExtras = AutomationExtrasState()
        evaluateAutomationLowerThird()
        autoStatus = "Automation defaults restored"
    }

    // ── dynamic lower thirds ─────────────────────────────────────────────────
    //
    // Windows shape (StudioViewModel.ShouldEnableProgramLowerThird :12377 +
    // ResolveProgramLowerThirdSource :12401 + RefreshProgramLowerThirdKeyPosition
    // :12153): in Set & Forget with the toggle on, the canonical key:lower-third
    // is armed automatically and NAMES THE CURRENT PROGRAM SOURCE, following
    // program as it changes. Two rules are copied because they were bug fixes:
    //   * STICKY — keep the source already showing while it is still on program,
    //     otherwise co-program sources ping-pong every tick.
    //   * DETERMINISTIC TIEBREAK by sourceId, never route order.
    // And the refresh is UN-FORCED: a same-source tick re-sends NOTHING (Windows
    // 2026-07-11 "goes up and down" fix). That also keeps this off the per-tick
    // RPC path — the key is only written when the resolved source CHANGES.

    func setAutomationLowerThirds(_ enabled: Bool) {
        autoExtras.lowerThirdsEnabled = enabled
        evaluateAutomationLowerThird()
    }

    /// Which source the automatic lower third should name, or nil if none.
    func automationLowerThirdSource() -> (id: String, name: String)? {
        var candidates: [(id: String, name: String)] = []

        func label(_ slot: ShowInputSlot) -> (id: String, name: String) {
            let wireId = (slot.kind == "zoom" ? "zoom:" : "capture:") + slot.sourceId
            let name = slot.name.isEmpty
                ? (roster.first { $0.id == slot.sourceId }?.name ?? slot.sourceId)
                : slot.name
            return (wireId, name)
        }

        if programSceneId == Self.soloSceneA || programSceneId == Self.soloSceneB {
            if let slotId = soloSlotId,
               let slot = slots.first(where: { $0.id == slotId }), slot.kind != "unassigned" {
                candidates.append(label(slot))
            }
        } else if let scene = scenes.first(where: { $0.id == programSceneId }) {
            if scene.layers.isEmpty {
                // Preset layout: in-show slots fill the cells (buildRoutes' rule).
                for slot in slots where slot.kind != "unassigned" && slot.inShow && !slot.offline {
                    candidates.append(label(slot))
                }
            } else {
                // Edited canvas: the layers' slot bindings ARE the program sources.
                for layer in scene.layers {
                    guard let slotId = layer.slotId,
                          let slot = slots.first(where: { $0.id == slotId }),
                          slot.kind != "unassigned" else { continue }
                    candidates.append(label(slot))
                }
            }
        }

        guard !candidates.isEmpty else { return nil }
        if let sticky = candidates.first(where: { $0.id == autoLowerThirdSourceId }) {
            return sticky
        }
        return candidates.sorted { $0.id < $1.id }.first
    }

    /// Called from the 0.5s auto-direct tick (AppModel.evaluateAutoDirect) BEFORE
    /// its participant guards — mirroring MagicSceneCoordinator, where
    /// ApplyAutomationOverlayPolicy runs ahead of the roster/confidence checks.
    func evaluateAutomationLowerThird() {
        guard autoDirectEnabled && autoExtras.lowerThirdsEnabled else {
            retireAutomationLowerThird()
            return
        }
        guard let source = automationLowerThirdSource() else {
            retireAutomationLowerThird()
            return
        }
        // Never STEAL the key from a manual lower third: automation only takes it
        // when it already owns it or nothing is on air. (The mac shell's manual
        // showLowerThird() writes the same canonical overlayId, and two writers
        // fighting over one key is the "slides up and down forever" class.)
        guard !autoLowerThirdSourceId.isEmpty || lowerThirdPhase == "hidden" else { return }
        guard autoLowerThirdSourceId != source.id else { return }  // un-forced refresh

        autoLowerThirdSourceId = source.id
        sendOverlay([
            "type": "set-overlay-asset", "overlayId": "key:lower-third",
            "text": source.name, "position": "lower-third", "enabled": true,
            "sourceId": source.id, "sourceName": source.name,
            "title": "", "org": "",
            "keyPosition": lowerThirdPosition, "keyPhase": "building-in",
            "buildInMs": 250, "buildOutMs": 200, "keyer": "downstream",
        ])
    }

    /// Two-step retire, identical to hideLowerThird(): an enabled:false WITHOUT
    /// keyPhase:"hidden" would start a SECOND native build-out.
    private func retireAutomationLowerThird() {
        guard !autoLowerThirdSourceId.isEmpty else { return }
        let name = automationLowerThirdSource()?.name ?? ""
        autoLowerThirdSourceId = ""
        sendOverlay([
            "type": "set-overlay-asset", "overlayId": "key:lower-third",
            "text": name, "position": "lower-third", "enabled": true,
            "keyPosition": lowerThirdPosition, "keyPhase": "building-out",
            "buildInMs": 250, "buildOutMs": 200, "keyer": "downstream",
        ])
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 220_000_000)
            self?.sendOverlay([
                "type": "set-overlay-asset", "overlayId": "key:lower-third",
                "text": "", "position": "lower-third", "enabled": false,
                "keyPhase": "hidden", "buildInMs": 250, "buildOutMs": 200,
                "keyer": "downstream",
            ])
        }
    }
}

// MARK: - View

/// Appended to AutomationPane. Everything above the "NOT WIRED" rule drives real
/// behaviour; everything below it is disabled WITH the reason, because a control
/// that silently does nothing is worse than no control.
struct AutomationExtrasSection: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Divider()
            MonoLabel("MAGIC SCENE")
            HStack(spacing: 8) {
                Button("Run Magic Scene") { model.runMagicScene() }
                    .buttonStyle(GhostButtonStyle())
                    .disabled(!model.canRunMagicScene)
                Button("Reset defaults") { model.resetAutomationDefaults() }
                    .buttonStyle(GhostButtonStyle())
                Spacer()
            }
            Text(model.magicSceneStatus)
                .font(.grotesk(11))
                .foregroundStyle(Studio.secondary)

            Toggle("Dynamic lower thirds", isOn: Binding(
                get: { model.autoExtras.lowerThirdsEnabled },
                set: { model.setAutomationLowerThirds($0) }))
                .font(.grotesk(12))
                .disabled(!model.autoDirectEnabled)
            Text(model.autoDirectEnabled
                 ? model.automationOverlayPolicySummary
                    + " — keys the program source automatically."
                 : "Set & Forget off — the lower third stays manual.")
                .font(.grotesk(11))
                .foregroundStyle(Studio.secondary)

            Divider()
            MonoLabel("READOUTS")
            HStack(alignment: .top, spacing: 8) {
                AutomationReadoutTile(label: "Recommended", value: model.recommendedSceneName)
                AutomationReadoutTile(label: "Layout", value: model.recommendedLayoutLabel)
                AutomationReadoutTile(label: "Confidence", value: model.recommendedConfidenceLabel)
            }
            HStack(alignment: .top, spacing: 8) {
                AutomationReadoutTile(label: "Cameras on", value: "\(model.camerasOnCount)")
                AutomationReadoutTile(label: "Screen share", value: model.screenShareLabel)
                AutomationReadoutTile(label: "Auto-switch", value: model.autoSwitchLabel,
                                      accent: model.autoDirectEnabled)
            }
            Text(model.sceneIntelligenceSummary)
                .font(.grotesk(11))
                .foregroundStyle(Studio.secondary)
            Text(model.automationPolicySummary + " · " + model.automationTakeModeLabel)
                .font(.plexMono(10))
                .foregroundStyle(Studio.textDim)

            Divider()
            MonoLabel("NOT WIRED ON MACOS", dim: true)
            Toggle("Prefer screen share", isOn: .constant(model.autoExtras.preferScreenShare))
                .font(.grotesk(12))
                .disabled(true)
            Toggle(String(format: "Panel at %.0f+ sources",
                          model.autoExtras.panelParticipantThreshold),
                   isOn: .constant(false))
                .font(.grotesk(12))
                .disabled(true)
            Text("Windows applies both only to its offline fallback heuristic "
                 + "(ProductionStateHelper.BuildAutomationRecommendation); the core "
                 + "director publishes a recommendation on every snapshot, so on both "
                 + "platforms they never reach a live show. Screen-share priority and "
                 + "the panel threshold live in the core kernel (Director.h).")
                .font(.grotesk(10))
                .foregroundStyle(Studio.textDim)
                .fixedSize(horizontal: false, vertical: true)
            Toggle("Automation captions", isOn: .constant(false))
                .font(.grotesk(12))
                .disabled(true)
            Text("No caption SOURCE exists in the native product: the core only "
                 + "renders cues a shell pushes (push-caption-cue), and nothing — "
                 + "core, engine, or shell — generates caption text. The transcription "
                 + "broker is renderer-era TypeScript (src/engine/captionBrokerClient.ts) "
                 + "and was never wired to the native shells.")
                .font(.grotesk(10))
                .foregroundStyle(Studio.textDim)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

private struct AutomationReadoutTile: View {
    let label: String
    let value: String
    var accent = false

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label.uppercased())
                .font(.plexMono(9))
                .foregroundStyle(Studio.textDim)
            Text(value)
                .font(.grotesk(12))
                .foregroundStyle(accent ? Studio.accent : Studio.textPrimary)
                .lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.surface))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
    }
}
