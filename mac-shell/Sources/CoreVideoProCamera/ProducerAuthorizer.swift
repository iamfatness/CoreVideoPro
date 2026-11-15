import CameraExtensionKit
import Darwin
import Foundation
import os.log
import Security

// Exported by libsystem_kernel; not declared in the public SDK headers.
@_silgen_name("csops")
private func csops(_ pid: pid_t, _ ops: UInt32, _ useraddr: UnsafeMutableRawPointer?, _ usersize: Int) -> Int32

/// Spec §4 law 6: only our core (or the vcam probe) may feed the sink. Fails
/// closed: any error on the way is a denial. Both outcomes are logged. The
/// decision is ProducerAttestation's (CameraExtensionKit, tested in ShellTests);
/// this file only gathers its inputs.
enum ProducerAuthorizer {
    private static let log = Logger(subsystem: cameraLogSubsystem, category: "auth")

    private static let requirement: SecRequirement? = {
        var requirement: SecRequirement?
        let status = SecRequirementCreateWithString(
            CameraConstants.producerRequirement as CFString, [], &requirement)
        return status == errSecSuccess ? requirement : nil
    }()

    static func isAuthorized(pid: pid_t) -> Bool {
        guard let requirement else {
            log.error("producer pid \(pid, privacy: .public) denied: requirement does not compile")
            return false
        }
        var code: SecCode?
        let attributes = [kSecGuestAttributePid: NSNumber(value: pid)] as CFDictionary
        let copyStatus = SecCodeCopyGuestWithAttributes(nil, attributes, [], &code)
        let requirementStatus = code.map { SecCodeCheckValidity($0, [], requirement) }
        let verdict = ProducerAttestation.decide(copyStatus: copyStatus, requirementStatus: requirementStatus) {
            ProducerAttestation.kernelVerdict(statusFlags: statusFlags(pid),
                                              teamID: blobString(pid, op: 14 /* CS_OPS_TEAMID */),
                                              identifier: blobString(pid, op: 11 /* CS_OPS_IDENTITY */))
        }
        switch verdict {
        case .authorized(let how):
            log.info("producer pid \(pid, privacy: .public) authorized: \(how, privacy: .public)")
            return true
        case .denied(let why):
            log.error("producer pid \(pid, privacy: .public) denied: \(why, privacy: .public)")
            return false
        }
    }

    private static func statusFlags(_ pid: pid_t) -> UInt32? {
        var flags: UInt32 = 0
        return csops(pid, 0 /* CS_OPS_STATUS */, &flags, MemoryLayout<UInt32>.size) == 0 ? flags : nil
    }

    /// CS_OPS_TEAMID / CS_OPS_IDENTITY fill an 8-byte blob header, then a
    /// NUL-terminated string.
    private static func blobString(_ pid: pid_t, op: UInt32) -> String? {
        var buffer = [UInt8](repeating: 0, count: 8 + 256)
        let status = buffer.withUnsafeMutableBytes { csops(pid, op, $0.baseAddress, $0.count) }
        guard status == 0 else { return nil }
        let bytes = buffer[8...].prefix { $0 != 0 }
        return bytes.isEmpty ? nil : String(decoding: bytes, as: UTF8.self)
    }
}
