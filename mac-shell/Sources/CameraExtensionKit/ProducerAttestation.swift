import Foundation

/// Who may feed the camera's sink stream (spec §4 law 6), decided ONLY by the
/// full code-signing requirement (`CameraConstants.producerRequirement`). Fails
/// closed: there is no fallback.
///
/// The extension runs as `_cmiodalassistants` under the system `cmioextension`
/// sandbox profile. `SecCodeCopyGuestWithAttributes(pid)` resolves the
/// producer's executable ON DISK, so the extension must be able to read it.
/// Live 2026-11-15 it could not: "Unable to create bundle at URL … (1)", then
/// OSStatus 100001 (POSIX EPERM, the sandbox) for the /Applications core, and
/// 100013 (EACCES) for a probe under a 0700 folder. No producer had ever been
/// authorized. The fix is a read-only sandbox exception for exactly the two
/// producer locations (`producerReadPaths`, carried in the extension's
/// entitlements), never a weaker check. `CMIOExtensionClient` exposes only
/// clientID / signingID / pid, so there is no audit token to validate instead.
public enum ProducerAttestation {
    /// All of /Applications (the shell's install preflight accepts any bundle
    /// there, so a renamed copy or a subfolder install must still be readable;
    /// read access reveals nothing and the requirement stays the only gate), and
    /// the one place the signed vcam probe may run from (scripts/macos-vcam-e2e.sh
    /// copies it there). Must match the
    /// com.apple.security.temporary-exception.files.absolute-path.read-only
    /// array in scripts/macos-camera-extension*.entitlements.
    public static let producerReadPaths = ["/Applications/", "/Users/Shared/CoreVideoPro/vcam-probe/"]

    /// Security reports a POSIX error as 100000 + errno.
    public static let posixEPERM: Int32 = 100_001
    public static let posixEACCES: Int32 = 100_013

    /// True when the extension's sandbox can read `path` (a producer executable
    /// or an app bundle path).
    public static func sandboxCanRead(_ path: String) -> Bool {
        let probe = path.hasSuffix("/") ? path : path + "/"
        return producerReadPaths.contains { probe.hasPrefix($0) }
    }

    public enum Verdict: Equatable {
        case authorized
        case denied(String)
    }

    /// `copyStatus` is SecCodeCopyGuestWithAttributes; `requirementStatus` is
    /// SecCodeCheckValidity against the requirement (nil when there is no code).
    public static func decide(copyStatus: Int32, requirementStatus: Int32?) -> Verdict {
        if copyStatus == posixEPERM || copyStatus == posixEACCES {
            return .denied("cannot read the producer's code on disk (OSStatus \(copyStatus)): run it from "
                           + producerReadPaths.joined(separator: " or "))
        }
        guard copyStatus == 0 else { return .denied("no guest code (OSStatus \(copyStatus))") }
        guard let requirementStatus else { return .denied("no guest code") }
        return requirementStatus == 0 ? .authorized : .denied("requirement not met (OSStatus \(requirementStatus))")
    }
}
