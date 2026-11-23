// A7: the ordered insert chain (Windows `PluginInserts`), shared by channel
// strips (here) and the processing targets of Task 3. Pure policy: names,
// normalisation and the visible slot list. The chain itself lives on
// `ChannelInserts.chain` (AppModel.swift).
//
// The core runs a chain IN LIST ORDER and matches each name by lowercase
// substring (AudioDsp.h applyChannelInsertChain: gate/noise, high-pass/
// highpass/low-cut/hpf, eq/voice, compressor, limiter, first match wins; a
// bus runs only compressor and limiter, applyBusInsertChain). Every helper
// below follows that matcher so the shell never reports a slot the core does
// not process.

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
    }

    /// Channel slots read "1", "2" (SVM RefreshSelectedChannelInsertSlots,
    /// `SlotLabel = $"{index + 1}"`); processing-target slots read "01", "02"
    /// (SVM RefreshSelectedAudioProcessingSlots, `$"{index + 1:00}"`).
    enum LabelStyle {
        case channel
        case target
    }

    /// What the core does with a name (AudioDsp.h applyChannelInsertChain
    /// matcher, same branch order).
    enum CoreKind: Equatable {
        case gate, highpass, eq, compressor, limiter
    }

    /// SVM NormalizeAudioInsertName.
    static func normalize(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        switch trimmed.lowercased() {
        case "eq", "built-in eq": return "Built-in EQ"
        case "compressor": return "Compressor"
        case "vst", "vst3", "vst3 bridge": return "VST3 Bridge Slot"
        default: return trimmed
        }
    }

    /// SVM IsBuiltInInsert (BuiltInInsertNames, SVM:2250-2258).
    static func isBuiltIn(_ name: String) -> Bool {
        let lowered = name.lowercased()
        guard !lowered.hasPrefix("vst:") else { return false }
        return ChannelInserts.builtInNames.contains { $0.caseInsensitiveCompare(name) == .orderedSame }
            || lowered.contains("gate") || lowered.contains("eq")
            || lowered.contains("compressor") || lowered.contains("limiter")
    }

    /// SVM IsVstInsert.
    static func isVst(_ name: String) -> Bool {
        name.lowercased().hasPrefix("vst:") || name.range(of: "VST3", options: .caseInsensitive) != nil
    }

    /// The core matcher's verdict for a channel insert, or nil for a name it
    /// passes through. A VST name never reaches that matcher (the ordered
    /// external-host hook consumes it first), so it has no built-in kind.
    static func coreKind(_ name: String) -> CoreKind? {
        guard !isVst(name) else { return nil }
        let lowered = name.lowercased()
        if lowered.contains("gate") || lowered.contains("noise") { return .gate }
        if lowered.contains("high-pass") || lowered.contains("highpass")
            || lowered.contains("low-cut") || lowered.contains("hpf") { return .highpass }
        if lowered.contains("eq") || lowered.contains("voice") { return .eq }
        if lowered.contains("compressor") { return .compressor }
        if lowered.contains("limiter") { return .limiter }
        return nil
    }

    /// True when `a` and `b` name the same insert (Windows compares
    /// OrdinalIgnoreCase everywhere in the chain).
    static func sameName(_ a: String, _ b: String) -> Bool {
        a.caseInsensitiveCompare(b) == .orderedSame
    }

    /// The visible slot list. Status:
    /// - a built-in the core processes in this position: `LIVE`, "Processing
    ///   active." (SVM RefreshSelectedChannelInsertSlots tooltip);
    /// - on a bus, a gate, high-pass or EQ: `PASS` (applyBusInsertChain runs
    ///   only compressor and limiter; Windows shows LIVE here, OD-1);
    /// - a VST while the host is not live: `BYPASS` with the host status line;
    ///   with a live host, Windows' `START` text (SVM, "Plug-in starting; audio
    ///   continues unchanged.") until V2 brings a processing verdict;
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
            if isVst(name) {
                if vstLive {
                    status = "START"
                    tooltip = "Plug-in starting; audio continues unchanged."
                } else {
                    status = "BYPASS"
                    tooltip = vstStatusLine
                }
            } else if let kind = coreKind(name) {
                if onBus && kind != .compressor && kind != .limiter {
                    status = "PASS"
                    let builtIn = name.lowercased().hasPrefix("built-in") ? name : "Built-in \(name)"
                    tooltip = "\(builtIn) does not process on a bus; audio passes through unchanged."
                } else {
                    status = "LIVE"
                    tooltip = "Processing active."
                }
            } else {
                status = "PASS"
                tooltip = "\(name) is not a built-in insert; audio passes through unchanged."
            }
            return InsertSlot(name: name, slotLabel: label, isBuiltIn: isBuiltIn(name),
                              statusLabel: status, tooltip: tooltip)
        }
    }
}
