// Comms (Zoom talkback) wire format between the shell and the media core:
// the Swift twin of native-shell/CoreVideoPro.MediaCore/Services/ZoomCommsProtocol.cs.
// Requests are ordinary core requests whose `type` is one of `verbs` with the
// arguments in `payload`; state arrives unsolicited as
// {"type":"zoom-comms-state","comms":{...}|null}.
//
// The parse rules are the C# ones exactly (ZoomCommsProtocol.cs:22-77): an
// `id` means a response, never a fact; `comms` must be an object or null;
// absent or wrongly typed fields become false / 0 / nil / empty; `peakDbfs`
// stays nil when absent; `participantId` is clamped at 0; a roster entry's
// `channels` keeps numbers only. contracts/comms/zoom-comms-state.fixtures.json
// is read by BOTH parsers' tests, which is the drift guard.

import Foundation

struct CommsChannel: Equatable {
    var slot: Int
    var label: String?
    var listeners: Int
    var ready: Bool
    var keyed: Bool
    var latched: Bool
    var room: String?
    var reachOk: Int
    var reachDark: Int
}

struct CommsPerson: Equatable {
    var participantId: UInt32
    var name: String
    var talkback: Bool
    var room: String?
    var channels: [Int]
}

struct CommsMic: Equatable {
    var device: String?
    var devices: [String] = []
    var peakDbfs: Double?
    var gainDb: Int = 0
    var sidetone: Bool = false
    var sidetoneDevice: String?
    var sidetoneDevices: [String] = []
    var tone: Bool = false
    /// The engine's last device-open failure or the macOS microphone-permission
    /// text (owner decision N1), verbatim; nil when none. The core relays the
    /// engine's mic object, so this is absent on the Windows wire.
    var error: String?
}

struct CommsState: Equatable {
    var available: Bool
    var reason: String?
    var lampLink: Bool
    var lampMeetingMic: Bool
    var lampChannel: Bool
    var lampTx: Bool
    var room: String?
    var channels: [CommsChannel]
    var roster: [CommsPerson]
    var mic: CommsMic
    var framesSent: Int64
    var channelSends: Int64
    var underruns: Int64
    var sendErrors: Int64
    var log: [String]
    var isPrivate: Bool
}

enum ZoomCommsProtocol {
    static let verbs: Set<String> = [
        "comms-key", "comms-latch", "comms-assign", "comms-mic",
        "comms-sidetone-out", "comms-sidetone", "comms-tone", "comms-gain", "comms-release-all", "comms-private",
    ]

    /// nil = not a comms fact (another event, or a response).
    /// .some(nil) = "comms":null, the report was dropped.
    static func parseFact(_ event: JSONObject) -> CommsState?? {
        guard event["id"] == nil, event["type"] as? String == "zoom-comms-state",
              let comms = event["comms"] else { return nil }
        if comms is NSNull { return .some(nil) }
        guard let object = comms as? JSONObject else { return nil }
        return .some(parseState(object))
    }

    static func parseState(_ cm: JSONObject) -> CommsState {
        let lamps = cm["lamps"] as? JSONObject
        let mic = cm["mic"] as? JSONObject
        let stats = cm["stats"] as? JSONObject
        let channels = (cm["channels"] as? [Any] ?? []).compactMap { $0 as? JSONObject }.map { c in
            CommsChannel(slot: Int(long(c, "slot")), label: str(c, "label"), listeners: Int(long(c, "listeners")),
                         ready: bool(c, "ready"), keyed: bool(c, "keyed"), latched: bool(c, "latched"),
                         room: str(c, "room"), reachOk: Int(long(c, "reachOk")), reachDark: Int(long(c, "reachDark")))
        }
        let roster = (cm["roster"] as? [Any] ?? []).compactMap { $0 as? JSONObject }.map { r in
            CommsPerson(participantId: UInt32(clamping: max(0, long(r, "participantId"))), name: str(r, "name") ?? "",
                        talkback: bool(r, "talkback"), room: str(r, "room"),
                        channels: (r["channels"] as? [Any] ?? []).compactMap { number($0) }.map { Int(saturating($0)) })
        }
        return CommsState(
            available: bool(cm, "available"), reason: str(cm, "reason"),
            lampLink: bool(lamps, "link"), lampMeetingMic: bool(lamps, "meetingMic"),
            lampChannel: bool(lamps, "channel"), lampTx: bool(lamps, "tx"),
            room: str(cm, "room"), channels: channels, roster: roster,
            mic: CommsMic(device: str(mic, "device"), devices: strings(mic, "devices"),
                          peakDbfs: mic.flatMap { number($0["peakDbfs"]) }, gainDb: Int(long(mic, "gainDb")),
                          sidetone: bool(mic, "sidetone"), sidetoneDevice: str(mic, "sidetoneDevice"),
                          sidetoneDevices: strings(mic, "sidetoneDevices"), tone: bool(mic, "tone"),
                          error: str(mic, "error").flatMap { $0.isEmpty ? nil : $0 }),
            framesSent: long(stats, "framesSent"), channelSends: long(stats, "channelSends"),
            underruns: long(stats, "underruns"), sendErrors: long(stats, "sendErrors"),
            log: strings(cm, "log"), isPrivate: bool(cm, "private"))
    }

    // ── C#'s B / L / DN / SN / Strings ──────────────────────────────────────

    private static func isBool(_ n: NSNumber) -> Bool { CFGetTypeID(n) == CFBooleanGetTypeID() }

    private static func bool(_ o: JSONObject?, _ key: String) -> Bool {
        guard let n = o?[key] as? NSNumber, isBool(n) else { return false }
        return n.boolValue
    }

    private static func number(_ v: Any?) -> Double? {
        guard let n = v as? NSNumber, !isBool(n) else { return nil }
        return n.doubleValue
    }

    private static func long(_ o: JSONObject?, _ key: String) -> Int64 {
        guard let d = number(o?[key]) else { return 0 }
        return saturating(d)
    }

    /// Total Double -> Int64 (truncating, saturating; NaN = 0): a hostile or
    /// malformed number must never trap the shell. C# casts never throw.
    private static func saturating(_ d: Double) -> Int64 {
        guard d.isFinite else { return d.isNaN ? 0 : (d < 0 ? Int64.min : Int64.max) }
        return Int64(exactly: d.rounded(.towardZero)) ?? (d < 0 ? Int64.min : Int64.max)
    }

    private static func str(_ o: JSONObject?, _ key: String) -> String? { o?[key] as? String }

    private static func strings(_ o: JSONObject?, _ key: String) -> [String] {
        (o?[key] as? [Any] ?? []).compactMap { $0 as? String }
    }
}
