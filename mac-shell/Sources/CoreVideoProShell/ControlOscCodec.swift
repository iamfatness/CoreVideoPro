// ControlOscCodec.swift — OSC 1.0 encode/decode, a case-for-case port of Windows
// `CoreVideoPro.Control/Osc/OscCodec.cs`, pinned by the shared codec fixtures in
// contracts/control.fixtures.json (osc.encode / osc.decode), which both hosts run.
// Pure: no sockets. Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Task 17).

import Foundation

enum ControlOscArg: Equatable { case int(Int32), float(Float), string(String), bool(Bool), blob(Data), null }

struct ControlOscMessage: Equatable {
    var address: String
    var args: [ControlOscArg]
}

enum ControlOscCodec {
    /// A packet larger than this is dropped (Mac-only guard; a UDP datagram cannot exceed it).
    static let maxDatagram = 65_536
    private static let bundleTag = Data("#bundle\0".utf8)

    private struct Truncated: Error {}

    // ── encode ───────────────────────────────────────────────────────────────

    static func encode(_ message: ControlOscMessage) -> Data {
        var out = Data()
        writeString(&out, message.address)
        var tags = ","
        var payload = Data()
        for arg in message.args {
            switch arg {
            case .bool(let flag): tags += flag ? "T" : "F"
            case .int(let value): tags += "i"; writeInt32(&payload, value)
            case .float(let value): tags += "f"; writeInt32(&payload, Int32(bitPattern: value.bitPattern))
            case .string(let text): tags += "s"; writeString(&payload, text)
            case .blob(let data):
                tags += "b"
                writeInt32(&payload, Int32(data.count))
                payload.append(data)
                payload.append(contentsOf: [UInt8](repeating: 0, count: (4 - data.count % 4) % 4))
            case .null:
                // OSC has no null; Windows encodes an empty string so the slot is kept.
                tags += "s"; writeString(&payload, "")
            }
        }
        writeString(&out, tags)
        out.append(payload)
        return out
    }

    private static func writeInt32(_ out: inout Data, _ value: Int32) {
        withUnsafeBytes(of: value.bigEndian) { out.append(contentsOf: $0) }
    }

    /// UTF-8, then 4 - (len % 4) NULs: always at least one (Windows WriteString).
    private static func writeString(_ out: inout Data, _ text: String) {
        let bytes = Array(text.utf8)
        out.append(contentsOf: bytes)
        out.append(contentsOf: [UInt8](repeating: 0, count: 4 - bytes.count % 4))
    }

    // ── decode ───────────────────────────────────────────────────────────────

    /// One message, or a bundle's messages (nested bundles flattened). Anything
    /// malformed gives [] — and a numeric or blob read past the end empties the
    /// WHOLE packet, even messages decoded earlier in a bundle (Windows' try/catch scope).
    static func decode(_ packet: Data) -> [ControlOscMessage] {
        guard packet.count <= maxDatagram else { return [] }
        let bytes = [UInt8](packet)
        do {
            if startsWithBundle(bytes[...]) {
                var messages: [ControlOscMessage] = []
                try decodeBundle(bytes[...], into: &messages)
                return messages
            }
            var offset = 0
            return try decodeMessage(bytes[...], &offset).map { [$0] } ?? []
        } catch {
            return []
        }
    }

    private static func startsWithBundle(_ data: ArraySlice<UInt8>) -> Bool {
        data.count >= bundleTag.count && data.prefix(bundleTag.count).elementsEqual(bundleTag)
    }

    private static func decodeBundle(_ data: ArraySlice<UInt8>, into messages: inout [ControlOscMessage]) throws {
        let base = data.startIndex
        var offset = bundleTag.count + 8   // "#bundle\0" + timetag
        while offset + 4 <= data.count {
            let size = Int(readInt32Raw(data, base + offset))
            offset += 4
            // A bad size STOPS bundle parsing and keeps what was decoded.
            if size < 0 || offset + size > data.count { break }
            let element = data[(base + offset)..<(base + offset + size)]
            if startsWithBundle(element) {
                try decodeBundle(element, into: &messages)
            } else {
                var elementOffset = 0
                if let message = try decodeMessage(element, &elementOffset) { messages.append(message) }
            }
            offset += size
        }
    }

    private static func decodeMessage(_ data: ArraySlice<UInt8>, _ offset: inout Int) throws -> ControlOscMessage? {
        guard let address = readString(data, &offset), address.hasPrefix("/") else { return nil }
        // A missing tag string means no args.
        if offset >= data.count { return ControlOscMessage(address: address, args: []) }
        guard let tags = readString(data, &offset), tags.hasPrefix(",") else {
            return ControlOscMessage(address: address, args: [])
        }
        var args: [ControlOscArg] = []
        for tag in tags.dropFirst() {
            switch tag {
            case "i": args.append(.int(try readInt32(data, &offset)))
            case "f": args.append(.float(Float(bitPattern: UInt32(bitPattern: try readInt32(data, &offset)))))
            case "s", "S": args.append(readString(data, &offset).map(ControlOscArg.string) ?? .null)
            case "T": args.append(.bool(true))
            case "F": args.append(.bool(false))
            case "b": args.append(.blob(try readBlob(data, &offset)))
            case "N", "I": args.append(.null)
            default:
                // Unknown tag of unknown width: stop parsing args, keep what was read.
                return ControlOscMessage(address: address, args: args)
            }
        }
        return ControlOscMessage(address: address, args: args)
    }

    private static func readInt32Raw(_ data: ArraySlice<UInt8>, _ index: Int) -> Int32 {
        Int32(bitPattern: UInt32(data[index]) << 24 | UInt32(data[index + 1]) << 16
              | UInt32(data[index + 2]) << 8 | UInt32(data[index + 3]))
    }

    private static func readInt32(_ data: ArraySlice<UInt8>, _ offset: inout Int) throws -> Int32 {
        guard offset >= 0, offset + 4 <= data.count else { throw Truncated() }
        defer { offset += 4 }
        return readInt32Raw(data, data.startIndex + offset)
    }

    /// Windows ReadString: nil when unterminated; the offset may run past the end
    /// when the padding is missing (a later numeric read then empties the packet).
    private static func readString(_ data: ArraySlice<UInt8>, _ offset: inout Int) -> String? {
        let start = offset
        var cursor = offset
        while cursor < data.count && data[data.startIndex + cursor] != 0 { cursor += 1 }
        guard cursor < data.count else { offset = cursor; return nil }
        let length = cursor - start
        let text = String(decoding: data[(data.startIndex + start)..<(data.startIndex + cursor)], as: UTF8.self)
        offset = start + length + (4 - length % 4)
        return text
    }

    private static func readBlob(_ data: ArraySlice<UInt8>, _ offset: inout Int) throws -> Data {
        let size = Int(try readInt32(data, &offset))
        guard size >= 0, offset + size <= data.count else { throw Truncated() }
        let blob = Data(data[(data.startIndex + offset)..<(data.startIndex + offset + size)])
        offset += size + (4 - size % 4) % 4
        return blob
    }

    /// Binder transport values: int -> Int64, float -> Float (D14), string -> String,
    /// bool -> Bool, null -> nil, blob -> Data.
    static func transportValues(_ args: [ControlOscArg]) -> [Any?] {
        args.map { arg -> Any? in
            switch arg {
            case .int(let value): return Int64(value)
            case .float(let value): return value
            case .string(let text): return text
            case .bool(let flag): return flag
            case .blob(let data): return data
            case .null: return nil
            }
        }
    }
}
