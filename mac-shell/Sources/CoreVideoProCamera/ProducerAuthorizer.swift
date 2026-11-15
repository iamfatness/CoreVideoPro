import CameraExtensionKit
import Foundation
import os.log
import Security

/// Spec §4 law 6: only our core (or the vcam probe) may feed the sink. Fails
/// closed: any error on the way is a denial. Both outcomes are logged. The
/// decision is ProducerAttestation's (CameraExtensionKit, tested in ShellTests).
/// The requirement check reads the producer from disk, which the sandbox allows
/// only for ProducerAttestation.producerReadPaths (entitlement exception).
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
        switch ProducerAttestation.decide(copyStatus: copyStatus, requirementStatus: requirementStatus) {
        case .authorized:
            log.info("producer pid \(pid, privacy: .public) authorized")
            return true
        case .denied(let why):
            log.error("producer pid \(pid, privacy: .public) denied: \(why, privacy: .public)")
            return false
        }
    }
}
