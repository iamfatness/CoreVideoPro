// ProducerAttestation (CameraExtensionKit): the camera extension's producer
// admission. Live 2026-11-15: the extension's sandbox refused the file read
// SecCodeCopyGuestWithAttributes needs (OSStatus 100001/100013) for every
// producer, so nothing was ever authorized. Registered in ShellTests.runAndExit.

import CameraExtensionKit
import Foundation

extension ShellTests {
    static func testProducerAttestation() {
        typealias P = ProducerAttestation
        // Flags measured on this Mac (csops CS_OPS_STATUS) for the installed core
        // and the Developer ID vcam-probe.
        let core: UInt32 = 0x2201_1301, probe: UInt32 = 0x2201_1311
        expectEqual(P.kernelVerdict(statusFlags: core, teamID: "42394L3353",
                                    identifier: "us.iamfatness.corevideopro.core"),
                    .authorized("kernel attestation us.iamfatness.corevideopro.core"), "installed core admitted")
        expectEqual(P.kernelVerdict(statusFlags: probe, teamID: "42394L3353",
                                    identifier: "us.iamfatness.corevideopro.vcam-probe"),
                    .authorized("kernel attestation us.iamfatness.corevideopro.vcam-probe"), "probe admitted")
        expectEqual(P.kernelVerdict(statusFlags: core, teamID: "AAAAAAAAAA",
                                    identifier: "us.iamfatness.corevideopro.core"),
                    .denied("team AAAAAAAAAA"), "another team is refused")
        expectEqual(P.kernelVerdict(statusFlags: core, teamID: nil, identifier: "us.iamfatness.corevideopro.core"),
                    .denied("team none"), "no team (unsigned) is refused")
        expectEqual(P.kernelVerdict(statusFlags: core, teamID: "42394L3353", identifier: "com.example.feeder"),
                    .denied("identifier com.example.feeder"), "another identifier is refused")
        expectEqual(P.kernelVerdict(statusFlags: core | P.csAdhoc, teamID: "42394L3353",
                                    identifier: "us.iamfatness.corevideopro.core"),
                    .denied("ad hoc signature"), "ad hoc is refused")
        expectEqual(P.kernelVerdict(statusFlags: core & ~P.csValid, teamID: "42394L3353",
                                    identifier: "us.iamfatness.corevideopro.core"),
                    .denied("code signature not valid"), "invalidated code is refused")
        expectEqual(P.kernelVerdict(statusFlags: core & ~P.csRuntime, teamID: "42394L3353",
                                    identifier: "us.iamfatness.corevideopro.core"),
                    .denied("not hardened runtime"), "non-hardened code is refused")
        expectEqual(P.kernelVerdict(statusFlags: nil, teamID: "42394L3353",
                                    identifier: "us.iamfatness.corevideopro.core"),
                    .denied("code-signing status unreadable"), "unreadable status is refused")

        let admit = P.Verdict.authorized("kernel")
        var kernelAsked = false
        expectEqual(P.decide(copyStatus: 0, requirementStatus: 0) { kernelAsked = true; return admit },
                    .authorized("requirement met"), "a readable producer meeting the requirement")
        expectEqual(P.decide(copyStatus: 0, requirementStatus: -67050) { kernelAsked = true; return admit },
                    .denied("requirement not met (OSStatus -67050)"),
                    "a readable producer failing the requirement is NOT rescued by the kernel check")
        expect(!kernelAsked, "the kernel check runs only when the requirement check cannot")
        expectEqual(P.decide(copyStatus: 100_001, requirementStatus: nil) { admit }, admit,
                    "sandbox EPERM (live, /Applications core) falls back to the kernel")
        expectEqual(P.decide(copyStatus: 100_013, requirementStatus: nil) { admit }, admit,
                    "EACCES (live, producer under a 0700 folder) falls back to the kernel")
        expectEqual(P.decide(copyStatus: -67065, requirementStatus: nil) { admit },
                    .denied("no guest code (OSStatus -67065)"), "any other failure fails closed")

        // The two admission rules must name the same team and producers.
        let requirement = CameraConstants.producerRequirement
        expect(requirement.contains("\"\(P.teamID)\""), "requirement and attestation share the team")
        for id in P.identifiers {
            expect(requirement.contains("identifier \"\(id)\""), "requirement names \(id)")
        }
    }
}
