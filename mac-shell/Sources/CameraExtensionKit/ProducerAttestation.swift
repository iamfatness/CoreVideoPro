import Foundation

/// Who may feed the camera's sink stream (spec §4 law 6), decided from what the
/// extension can actually observe about a producer process.
///
/// The extension runs as `_cmiodalassistants` under the system `cmioextension`
/// sandbox profile, which does not let it read `/Applications` (or any home
/// folder). `SecCodeCopyGuestWithAttributes(pid)` resolves the producer's
/// executable on disk and so fails there with POSIX EPERM/EACCES (OSStatus
/// 100001 / 100013) for EVERY producer, the core included — measured live
/// 2026-11-15 ("Unable to create bundle at URL … (1)", then "no guest code").
/// No producer had ever been authorized.
///
/// So the full requirement check stays first and is authoritative whenever it
/// can run. When it is refused for file access only, the decision falls back to
/// the kernel's code-signing state for the running process (`csops`), which
/// needs no file access: the code must be valid, not ad hoc, hardened, signed
/// by our team, with one of the two producer identifiers.
public enum ProducerAttestation {
    public static let teamID = "42394L3353"
    public static let identifiers: Set<String> = ["us.iamfatness.corevideopro.core",
                                                  "us.iamfatness.corevideopro.vcam-probe"]

    // <kern/cs_blobs.h> code-signing status bits.
    public static let csValid: UInt32 = 0x0000_0001
    public static let csAdhoc: UInt32 = 0x0000_0002
    public static let csRuntime: UInt32 = 0x0001_0000

    /// OSStatus values Security returns for a POSIX error (100000 + errno).
    public static let posixEPERM: Int32 = 100_001
    public static let posixEACCES: Int32 = 100_013

    public enum Verdict: Equatable {
        case authorized(String)
        case denied(String)
    }

    /// The kernel-attested half: `statusFlags` from CS_OPS_STATUS, `teamID` from
    /// CS_OPS_TEAMID, `identifier` from CS_OPS_IDENTITY (nil when unreadable).
    public static func kernelVerdict(statusFlags: UInt32?, teamID: String?, identifier: String?) -> Verdict {
        guard let flags = statusFlags else { return .denied("code-signing status unreadable") }
        if flags & csValid == 0 { return .denied("code signature not valid") }
        if flags & csAdhoc != 0 { return .denied("ad hoc signature") }
        if flags & csRuntime == 0 { return .denied("not hardened runtime") }
        guard teamID == Self.teamID else { return .denied("team \(teamID ?? "none")") }
        guard let identifier, identifiers.contains(identifier) else {
            return .denied("identifier \(identifier ?? "none")")
        }
        return .authorized("kernel attestation \(identifier)")
    }

    /// The whole decision. `copyStatus` is SecCodeCopyGuestWithAttributes;
    /// `requirementStatus` is SecCodeCheckValidity against the requirement (only
    /// meaningful when the copy succeeded). `kernel` is evaluated only when the
    /// copy was refused for file access.
    public static func decide(copyStatus: Int32, requirementStatus: Int32?,
                              kernel: () -> Verdict) -> Verdict {
        if copyStatus == 0 {
            return requirementStatus == 0
                ? .authorized("requirement met")
                : .denied("requirement not met (OSStatus \(requirementStatus ?? -1))")
        }
        if copyStatus == posixEPERM || copyStatus == posixEACCES { return kernel() }
        return .denied("no guest code (OSStatus \(copyStatus))")
    }
}
