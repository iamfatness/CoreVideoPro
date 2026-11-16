import Foundation

/// Who may feed the camera's sink stream (spec §4 law 6), decided ONLY by the
/// full code-signing requirement (`CameraConstants.producerRequirement`). Fails
/// closed: there is no fallback.
///
/// The extension runs as `_cmiodalassistants` under the system `cmioextension`
/// sandbox profile. `SecCodeCopyGuestWithAttributes(pid)` resolves the
/// producer's executable ON DISK, so the extension must be able to read it.
/// The profile reads `/Library` (minus Caches, Logs, Preferences, AppStore*)
/// but not /Applications or any home folder: live 2026-11-15 the /Applications
/// core got OSStatus 100001 (POSIX EPERM). A file temporary-exception is NOT a
/// way out: macOS 26.3's cmioextension.sb expands
/// `temporary-exception.files.absolute-path.read-only` to a bare
/// `(allow filter)`, which fails to compile, and the extension then dies in
/// `_libsecinit_appsandbox` before main ("failed to compile sandbox profile";
/// installers 3 and 4 crash-looped every 10 s). So producers RUN FROM
/// `producerDirectory`, which the .pkg installs root-owned; the shell launches
/// the core from there (CoreLocation). `CMIOExtensionClient` exposes only
/// clientID / signingID / pid, so there is no audit token to validate instead.
public enum ProducerAttestation {
    /// Root-owned, installed by scripts/make-macos-pkg.sh, readable under the
    /// cmioextension profile's `(allow file-read* (subpath "/Library"))`.
    public static let producerDirectory = "/Library/Application Support/CoreVideo Pro/"
    /// The pkg-installed copy of the core, byte-identical to the app's
    /// Contents/Resources/corevideo-native.
    public static let installedCorePath = producerDirectory + "corevideo-native"
    /// Where scripts/macos-vcam-e2e.sh puts the signed feeder (with sudo).
    public static let probeDirectory = producerDirectory + "vcam-probe/"

    /// Security reports a POSIX error as 100000 + errno.
    public static let posixEPERM: Int32 = 100_001
    public static let posixEACCES: Int32 = 100_013

    /// True when the extension's sandbox can read `path` (a producer executable
    /// or an app bundle path).
    public static func sandboxCanRead(_ path: String) -> Bool {
        path.hasPrefix(producerDirectory)
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
                           + producerDirectory)
        }
        guard copyStatus == 0 else { return .denied("no guest code (OSStatus \(copyStatus))") }
        guard let requirementStatus else { return .denied("no guest code") }
        return requirementStatus == 0 ? .authorized : .denied("requirement not met (OSStatus \(requirementStatus))")
    }
}
