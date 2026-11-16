// ProducerAttestation (CameraExtensionKit): the camera extension's producer
// admission. Live 2026-11-15 the extension's sandbox refused the file read
// SecCodeCopyGuestWithAttributes needs (OSStatus 100001/100013) for every
// producer. The fix is a read-only sandbox exception for the producer paths,
// never a weaker check: these cases pin that nothing but a met requirement
// authorizes. Registered in ShellTests.runAndExit.

import CameraExtensionKit
import Foundation

extension ShellTests {
    static func testProducerAttestation() {
        typealias P = ProducerAttestation
        expectEqual(P.decide(copyStatus: 0, requirementStatus: 0), .authorized, "a met requirement authorizes")
        expectEqual(P.decide(copyStatus: 0, requirementStatus: -67050),
                    .denied("requirement not met (OSStatus -67050)"), "an unmet requirement is refused")
        expectEqual(P.decide(copyStatus: 0, requirementStatus: nil), .denied("no guest code"), "no code is refused")
        expectEqual(P.decide(copyStatus: -67065, requirementStatus: nil),
                    .denied("no guest code (OSStatus -67065)"), "any copy failure is refused")
        for status: Int32 in [100_001, 100_013] {
            let verdict = P.decide(copyStatus: status, requirementStatus: nil)
            expect(verdict != .authorized, "a sandbox read refusal (\(status)) is refused, never rescued")
            if case .denied(let why) = verdict {
                expect(why.contains("/Users/Shared/CoreVideoPro/vcam-probe/"), "the refusal says where to run from")
            }
        }

        // The requirement is exactly Developer ID, our team, our two identifiers:
        // an equality pin, so a widening "or" clause cannot slip in (review M6).
        expectEqual(CameraConstants.producerRequirement,
                    #"anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate leaf[field.1.2.840.113635.100.6.1.13] exists and certificate leaf[subject.OU] = "42394L3353" and (identifier "us.iamfatness.corevideopro.core" or identifier "us.iamfatness.corevideopro.vcam-probe")"#,
                    "the producer requirement, verbatim")
        let requirement = CameraConstants.producerRequirement

        // Review I1: every bundle the install preflight accepts must be readable
        // by the extension's sandbox, or the camera installs and then refuses
        // every producer. Finder's "CoreVideo Pro 2.app" and subfolders included.
        for bundle in ["/Applications/CoreVideo Pro.app", "/Applications/CoreVideo Pro 2.app",
                       "/Applications/Video/CoreVideo Pro.app", "/Applications/Renamed.app"] {
            expect(CameraExtensionPolicy.preflight(bundlePath: bundle, hasEmbeddedProfile: true) == nil,
                   "preflight accepts \(bundle)")
            expect(P.sandboxCanRead(bundle + "/Contents/Resources/corevideo-native"),
                   "the sandbox can read the core inside \(bundle)")
        }
        expect(CameraExtensionPolicy.preflight(bundlePath: "/Users/x/Downloads/CoreVideo Pro.app",
                                               hasEmbeddedProfile: true) == .notInApplications,
               "outside /Applications the preflight stops first")
        expect(P.sandboxCanRead("/Users/Shared/CoreVideoPro/vcam-probe/corevideo-vcam-feed"), "the e2e probe")
        expect(!P.sandboxCanRead("/Users/Shared/elsewhere/corevideo-vcam-feed"), "nothing else under /Users/Shared")
        var compiled: SecRequirement?
        expectEqual(SecRequirementCreateWithString(requirement as CFString, [], &compiled), errSecSuccess,
                    "the requirement compiles")

        // The sandbox exception in the shipped entitlements is exactly the read paths.
        for name in ["macos-camera-extension.entitlements", "macos-camera-extension-adhoc.entitlements"] {
            let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
                .appendingPathComponent("../../../scripts/\(name)").standardizedFileURL
            guard let data = try? Data(contentsOf: url),
                  let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any]
            else { expect(false, "cannot read scripts/\(name) from the source tree"); continue }
            let paths = plist["com.apple.security.temporary-exception.files.absolute-path.read-only"] as? [String]
            expectEqual(paths ?? [], P.producerReadPaths, "\(name) grants exactly the producer read paths")
        }
    }
}
