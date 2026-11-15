import CameraExtensionKit
import Foundation
import os.log
import Security

/// Spec §4 law 6: only our core (or the vcam probe) may feed the sink. Fails
/// closed: any error on the way is a denial. Both outcomes are logged.
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
        guard copyStatus == errSecSuccess, let code else {
            log.error("producer pid \(pid, privacy: .public) denied: no guest code (OSStatus \(copyStatus, privacy: .public))")
            return false
        }
        let status = SecCodeCheckValidity(code, [], requirement)
        if status == errSecSuccess {
            log.info("producer pid \(pid, privacy: .public) authorized")
            return true
        }
        log.error("producer pid \(pid, privacy: .public) denied: requirement not met (OSStatus \(status, privacy: .public))")
        return false
    }
}
