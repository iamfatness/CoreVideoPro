// Audio buses (A6): the 15 fixed buses, aux/custom buses, bus sends and LISTEN.
// A port of W/ViewModels/AudioRoutingMatrixViewModel.cs (bus list :84-101,
// custom buses :121,287-350, bus sends :444-526, LISTEN :480-499). Pure.
//
// The core contract (MediaCore.cpp): isAudioRoutingBus accepts the 15 fixed
// ids plus any `bus-*` (:3233-3241); busSends are accepted only FROM aux-*/bus-*
// TO master|pgm-l|pgm-r|stream|mon (:3366-3388, the acyclic rule); monitorBusId
// picks the LISTEN bus, used only while the monitor is on (:7285-7299).
// Nothing here is persisted (Windows persists none of it, plan D3).

import Foundation

struct AudioBus: Equatable, Identifiable {
    let id: String
    var label: String
    var sendsToMaster = false
    var sendsToMonitor = false
    var sendsToStream = false
    var outputGainDb = 0.0

    var isRemovable: Bool { AudioBusPolicy.isRemovable(id) }
}

enum AudioBusPolicy {
    /// Windows order and labels.
    static let fixedBuses: [AudioBus] = [
        AudioBus(id: "master", label: "MASTER"), AudioBus(id: "pgm-l", label: "PGM L"),
        AudioBus(id: "pgm-r", label: "PGM R"),
    ] + (1...8).map { AudioBus(id: "iso-\($0)", label: "ISO \($0)") } + [
        AudioBus(id: "mon", label: "MON"), AudioBus(id: "stream", label: "STREAM"),
        AudioBus(id: "aux-1", label: "AUX 1"), AudioBus(id: "aux-2", label: "AUX 2"),
    ]

    /// The bus-send targets the UI offers (Windows offers these three; the core
    /// also accepts pgm-l / pgm-r, which the UI never offers).
    static let sendTargets = ["master", "mon", "stream"]

    static func isRemovable(_ id: String) -> Bool { id.hasPrefix("aux-") || id.hasPrefix("bus-") }
    static func isIsolated(_ id: String) -> Bool { id.lowercased().hasPrefix("iso-") }

    /// `bus-01`, `BUS 1`; the counter only rises (a removed number is never reused).
    static func addCustom(_ buses: [AudioBus], counter: Int) -> (buses: [AudioBus], counter: Int, added: AudioBus) {
        let bus = AudioBus(id: String(format: "bus-%02d", counter), label: "BUS \(counter)")
        return (buses + [bus], counter + 1, bus)
    }

    /// Removable buses only; nil for a fixed or unknown bus.
    static func remove(_ id: String, from buses: [AudioBus]) -> [AudioBus]? {
        guard isRemovable(id), buses.contains(where: { $0.id == id }) else { return nil }
        return buses.filter { $0.id != id }
    }

    /// Removable buses only, blank refused, trimmed.
    static func rename(_ id: String, to label: String, in buses: [AudioBus]) -> [AudioBus]? {
        let trimmed = label.trimmingCharacters(in: .whitespacesAndNewlines)
        guard isRemovable(id), !trimmed.isEmpty, let index = buses.firstIndex(where: { $0.id == id }) else { return nil }
        var next = buses
        next[index].label = trimmed
        return next
    }

    static func setSend(_ id: String, target: String, enabled: Bool, in buses: [AudioBus]) -> [AudioBus]? {
        guard isRemovable(id), sendTargets.contains(target),
              let index = buses.firstIndex(where: { $0.id == id }) else { return nil }
        var next = buses
        switch target {
        case "master": next[index].sendsToMaster = enabled
        case "mon": next[index].sendsToMonitor = enabled
        default: next[index].sendsToStream = enabled
        }
        return next
    }

    /// Clamped to [-60, +10]; nil for a fixed bus or a change under 0.01 dB.
    static func setOutputGain(_ id: String, gainDb: Double, in buses: [AudioBus]) -> [AudioBus]? {
        guard isRemovable(id), gainDb.isFinite, let index = buses.firstIndex(where: { $0.id == id }) else { return nil }
        let clamped = max(-60, min(10, gainDb))
        guard abs(clamped - buses[index].outputGainDb) >= 0.01 else { return nil }
        var next = buses
        next[index].outputGainDb = clamped
        return next
    }

    /// Windows BuildBusSends: bus order, then master, mon, stream.
    static func busSends(_ buses: [AudioBus]) -> [JSONObject] {
        buses.filter(\.isRemovable).flatMap { bus -> [JSONObject] in
            var sends: [JSONObject] = []
            if bus.sendsToMaster { sends.append(["fromBusId": bus.id, "toBusId": "master", "gainDb": bus.outputGainDb]) }
            if bus.sendsToMonitor { sends.append(["fromBusId": bus.id, "toBusId": "mon", "gainDb": bus.outputGainDb]) }
            if bus.sendsToStream { sends.append(["fromBusId": bus.id, "toBusId": "stream", "gainDb": bus.outputGainDb]) }
            return sends
        }
    }

    /// Windows SetListening: exclusive, any bus (fixed included); off clears it.
    static func listen(current: String?, bus: String, on: Bool) -> String? {
        on ? bus : (current == bus ? nil : current)
    }
}
