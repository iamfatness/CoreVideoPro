// A7: the ordered insert chain (Windows `PluginInserts`), shared by channel
// strips (here) and the processing targets of Task 3. Pure policy: names,
// normalisation and the visible slot list. The chain itself lives on
// `ChannelInserts.chain` (AppModel.swift).
//
// The core runs a chain IN LIST ORDER and matches each name by lowercase
// substring (AudioDsp.h applyChannelInsertChain: gate/noise, high-pass/
// highpass/low-cut/hpf, eq/voice, compressor, limiter, first match wins; a
// bus runs only limiter then compressor, applyBusInsertChain). Before either
// matcher runs, any name containing "vst" or "host" (case-insensitive) is
// consumed by the external plug-in host hook (PluginHostScan.h:72
// isHostHandledInsertName; MediaCore.cpp processExternalInsert), live or not.
// Every helper below follows those rules so the shell never reports a slot the
// core does not process.

import Foundation

/// One visible slot of an insert chain (Windows `InsertSlotItem`).
struct InsertSlot: Equatable, Identifiable {
    let name: String        // the canonical chain value: removal uses it as identity
    let slotLabel: String
    let isBuiltIn: Bool
    let statusLabel: String
    let tooltip: String
    var id: String { name }
}

enum InsertChainPolicy {
    enum AddOutcome: Equatable {
        case added(String)
        case duplicate(String)
        /// A blank (empty or whitespace-only) name: nothing is added and
        /// nothing is said (SVM:5522, `string.IsNullOrWhiteSpace` early return).
        case rejectedBlank
    }

    /// Channel slots read "1", "2" (SVM:2321 `SlotLabel = $"{index + 1}"`);
    /// processing-target slots read "01", "02" (SVM:2690 `$"{index + 1:00}"`).
    enum LabelStyle {
        case channel
        case target
    }

    /// What the core does with a name (AudioDsp.h applyChannelInsertChain
    /// matcher, same branch order).
    enum CoreKind: Equatable {
        case gate, highpass, eq, compressor, limiter
    }

    /// SVM:5560-5567 (NormalizeAudioInsertName).
    static func normalize(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        switch trimmed.lowercased() {
        case "eq", "built-in eq": return "Built-in EQ"
        case "compressor": return "Compressor"
        case "vst", "vst3", "vst3 bridge": return "VST3 Bridge Slot"
        default: return trimmed
        }
    }

    /// SVM:2252-2258 (IsBuiltInInsert, over BuiltInInsertNames at SVM:2250).
    /// Windows-facing display only: the core decides processing by coreKind.
    static func isBuiltIn(_ name: String) -> Bool {
        let lowered = name.lowercased()
        guard !lowered.hasPrefix("vst:") else { return false }
        return ChannelInserts.builtInNames.contains { $0.caseInsensitiveCompare(name) == .orderedSame }
            || lowered.contains("gate") || lowered.contains("eq")
            || lowered.contains("compressor") || lowered.contains("limiter")
    }

    /// SVM:2260-2262 (IsVstInsert). Windows-facing: the VST picker's notion of
    /// "the plug-in on this strip". NARROWER than what the core hands to the
    /// plug-in host; for wire and status decisions use isHostHandled.
    static func isVst(_ name: String) -> Bool {
        name.lowercased().hasPrefix("vst:") || name.range(of: "VST3", options: .caseInsensitive) != nil
    }

    /// The core's isHostHandledInsertName (PluginHostScan.h:72): any name that
    /// contains "vst" or "host", case-insensitive ("Host Test", "vst pro").
    /// The core routes such a name to the external plug-in host on channels
    /// AND buses, and bypasses it there when the host cannot run it; it never
    /// reaches the built-in matchers.
    static func isHostHandled(_ name: String) -> Bool {
        let lowered = name.lowercased()
        return lowered.contains("vst") || lowered.contains("host")
    }

    /// The core matcher's verdict for a channel insert, or nil for a name it
    /// passes through. A host-handled name never reaches that matcher (the
    /// ordered external-host hook consumes it first), so it has no built-in kind.
    static func coreKind(_ name: String) -> CoreKind? {
        guard !isHostHandled(name) else { return nil }
        let lowered = name.lowercased()
        if lowered.contains("gate") || lowered.contains("noise") { return .gate }
        if lowered.contains("high-pass") || lowered.contains("highpass")
            || lowered.contains("low-cut") || lowered.contains("hpf") { return .highpass }
        if lowered.contains("eq") || lowered.contains("voice") { return .eq }
        if lowered.contains("compressor") { return .compressor }
        if lowered.contains("limiter") { return .limiter }
        return nil
    }

    /// The bus matcher's verdict (AudioDsp.h:1319-1342 applyBusInsertChain):
    /// "limiter" is checked first, then "compressor"; everything else passes
    /// through. So "Voice Compressor" compresses on a bus (on a channel the
    /// "voice" branch makes it an EQ) and "Noise Limiter" limits on a bus.
    static func busKind(_ name: String) -> CoreKind? {
        guard !isHostHandled(name) else { return nil }
        let lowered = name.lowercased()
        if lowered.contains("limiter") { return .limiter }
        if lowered.contains("compressor") { return .compressor }
        return nil
    }

    /// True when `a` and `b` name the same insert (Windows compares
    /// OrdinalIgnoreCase everywhere in the chain).
    static func sameName(_ a: String, _ b: String) -> Bool {
        a.caseInsensitiveCompare(b) == .orderedSame
    }

    /// The visible slot list. Status:
    /// - a host-handled name (isHostHandled, the core's rule) while the host is
    ///   not live: `BYPASS` with the host status line (never LIVE or PASS);
    ///   with a live host, Windows' `START` text (SVM:2315) until V2 brings a
    ///   processing verdict;
    /// - on a channel, a name coreKind matches: `LIVE`, "Processing active."
    ///   (SVM:2308);
    /// - on a bus, a name busKind matches (limiter, compressor): `LIVE`; a
    ///   gate, high-pass or EQ: `PASS` with the reason (Windows shows LIVE
    ///   here, OD-1);
    /// - any other name: `PASS` (the core ignores unknown names).
    static func slots(chain: [String], onBus: Bool, vstLive: Bool, vstStatusLine: String,
                      labelStyle: LabelStyle) -> [InsertSlot] {
        chain.enumerated().map { index, name in
            let label: String
            switch labelStyle {
            case .channel: label = "\(index + 1)"
            case .target: label = String(format: "%02d", index + 1)
            }
            let status: String
            let tooltip: String
            if isHostHandled(name) {
                if vstLive {
                    status = "START"
                    // SVM:2315 (channel). The target-style text at SVM:2686
                    // appends " Click to open its controls."; the Mac omits it
                    // DELIBERATELY for both styles because the Mac cannot open
                    // a plug-in's controls yet (no editor window on this port),
                    // so the sentence would promise a click that does nothing.
                    tooltip = "Plug-in starting; audio continues unchanged."
                } else {
                    status = "BYPASS"
                    tooltip = vstStatusLine
                }
            } else if onBus ? busKind(name) != nil : coreKind(name) != nil {
                status = "LIVE"
                tooltip = "Processing active."   // SVM:2308 / SVM:2679
            } else if onBus, coreKind(name) != nil {
                status = "PASS"
                let builtIn = name.lowercased().hasPrefix("built-in") ? name : "Built-in \(name)"
                tooltip = "\(builtIn) does not process on a bus; audio passes through unchanged."
            } else {
                status = "PASS"
                tooltip = "\(name) is not a built-in insert; audio passes through unchanged."
            }
            return InsertSlot(name: name, slotLabel: label, isBuiltIn: isBuiltIn(name),
                              statusLabel: status, tooltip: tooltip)
        }
    }
}

// ── A7 Task 3: processing targets ───────────────────────────────────────────
//
// Windows' SETUP "Processing target" picker (SVM RefreshAudioProcessingTargets,
// SVM:11506-11603): one target per channel strip ("channel:<id>"), then
// "bus:master", then every other bus in header order. A channel target edits
// that strip's own chain (ChannelInserts.chain); a bus target edits a
// session-only chain that rides EVERY send to that bus
// (AudioConsolePolicy.baseRoutingSends, SVM:9221-9229).

/// One entry of the processing-target picker (Windows AudioProcessingTargetOption).
struct AudioProcessingTarget: Identifiable, Equatable {
    let id: String
    let label: String
    let kind: String
    let detail: String
    let insertLabel: String
}

enum AudioProcessingTargetPolicy {
    /// SVM:508 `_selectedAudioProcessingTargetId = "bus:master"`.
    static let defaultTargetId = "bus:master"

    /// SVM FormatChannelProcessingTargetId.
    static func channelId(_ stripId: String) -> String { "channel:\(stripId)" }

    /// SVM FormatBusProcessingTargetId.
    static func busId(_ busId: String) -> String { "bus:\(busId)" }

    /// The strip or bus a target id names (SVM TryParseChannelProcessingTargetId:
    /// a case-insensitive "channel:" prefix and a non-empty id). An id that is
    /// neither yields (nil, nil).
    static func parse(_ id: String) -> (channel: String?, bus: String?) {
        let trimmed = id.trimmingCharacters(in: .whitespacesAndNewlines)
        let lowered = trimmed.lowercased()
        if lowered.hasPrefix("channel:") {
            let rest = String(trimmed.dropFirst("channel:".count))
            return (rest.isEmpty ? nil : rest, nil)
        }
        if lowered.hasPrefix("bus:") {
            let rest = String(trimmed.dropFirst("bus:".count))
            return (nil, rest.isEmpty ? nil : rest)
        }
        return (nil, nil)
    }

    /// SVM ResolveBusProcessingKind (SVM:11614-11637), verbatim.
    static func kind(busId: String) -> String {
        let lowered = busId.lowercased()
        if lowered == "master" { return "Master bus" }
        if lowered.hasPrefix("aux-") { return "Aux bus" }
        if lowered.hasPrefix("iso-") { return "ISO bus" }
        if lowered.hasPrefix("bus-") { return "Custom bus" }
        return "Bus"
    }

    /// SVM ResolveBusProcessingDetail (SVM:11639-11649), verbatim. The fixed
    /// names compare ordinally, the prefixes case-insensitively, as on Windows.
    static func detail(busId: String) -> String {
        switch busId {
        case "master": return "Final program mix processing before output."
        case "stream": return "Streaming output bus processing."
        case "mon": return "Control-room monitor bus processing."
        case "pgm-l": return "Left program bus processing."
        case "pgm-r": return "Right program bus processing."
        default:
            if busId.lowercased().hasPrefix("aux-") { return "Aux send processing." }
            if busId.lowercased().hasPrefix("iso-") { return "Isolated recording bus processing." }
            return "Custom bus processing."
        }
    }

    /// SVM FormatInsertLabel (SVM:11611-11612).
    static func insertLabel(_ chain: [String]) -> String {
        chain.isEmpty ? "No inserts" : chain.joined(separator: " + ")
    }

    /// OD-1: the shared core's bus chain (AudioDsp.h applyBusInsertChain) runs
    /// only the compressor and the limiter; a gate, high-pass or EQ on a bus
    /// passes audio through unchanged (on Windows too, where the slot still
    /// reads LIVE). The Mac refuses such an add with this reason, and the SETUP
    /// button is dimmed with the same text. nil = the name may go on a bus.
    static func busRefusal(_ raw: String) -> String? {
        let name = InsertChainPolicy.normalize(raw)
        // The bus matcher wins: "Voice Compressor" compresses on a bus.
        guard InsertChainPolicy.busKind(name) == nil else { return nil }
        switch InsertChainPolicy.coreKind(name) {
        case .gate?, .highpass?, .eq?:
            return "\(name) does not process on a bus yet; add a compressor or limiter."
        default:
            return nil
        }
    }

    /// The picker's targets, in Windows order: channels in strip order, then
    /// bus:master (always present, SVM:11541-11552), then every other bus in
    /// `buses` order (AudioBusPolicy order: the Windows BusHeaders).
    /// The channel detail is "Channel - <primary bus label>": Windows prefixes
    /// a role ("<Role> channel - ", "Native PCM channel - ", SVM:11521/11536);
    /// the Mac strip has no role label, so the plan takes the bare form.
    static func targets(strips: [(id: String, label: String, primaryBus: String)], buses: [AudioBus],
                        channelChains: [String: [String]], busChains: [String: [String]]) -> [AudioProcessingTarget] {
        var targets = strips.map { strip in
            AudioProcessingTarget(id: channelId(strip.id), label: strip.label, kind: "Channel",
                                  detail: "Channel - \(strip.primaryBus)",
                                  insertLabel: insertLabel(channelChains[strip.id] ?? []))
        }
        func busTarget(_ id: String, _ label: String) -> AudioProcessingTarget {
            AudioProcessingTarget(id: busId(id), label: label, kind: kind(busId: id), detail: detail(busId: id),
                                  insertLabel: insertLabel(busChains[id] ?? []))
        }
        let master = buses.first { $0.id.caseInsensitiveCompare("master") == .orderedSame }
        targets.append(busTarget("master", master?.label ?? "MASTER"))
        for bus in buses where bus.id.caseInsensitiveCompare("master") != .orderedSame {
            targets.append(busTarget(bus.id, bus.label))
        }
        return targets
    }
}
