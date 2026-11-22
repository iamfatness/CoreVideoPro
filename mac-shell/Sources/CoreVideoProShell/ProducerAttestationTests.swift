// ProducerAttestation (CameraExtensionKit): the camera extension's producer
// admission. The requirement check reads the producer from disk, and the
// cmioextension sandbox reads /Library but not /Applications or any home
// folder. A file temporary-exception cannot fix that: macOS 26.3's
// cmioextension.sb expands absolute-path.read-only to a bare (allow filter),
// which does not compile, so an extension carrying it crash-looped before main
// (installers 3 and 4, 2026-11-15). Producers therefore run from
// /Library/Application Support/CoreVideo Pro/, which the pkg installs.
// Registered in ShellTests.runAndExit.

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
                expect(why.contains("/Library/Application Support/CoreVideo Pro/"), "the refusal says where to run from")
            }
        }

        // The requirement is exactly Developer ID, our team, our two identifiers:
        // an equality pin, so a widening "or" clause cannot slip in (review M6).
        expectEqual(CameraConstants.producerRequirement,
                    #"anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate leaf[field.1.2.840.113635.100.6.1.13] exists and certificate leaf[subject.OU] = "42394L3353" and (identifier "us.iamfatness.corevideopro.core" or identifier "us.iamfatness.corevideopro.vcam-probe")"#,
                    "the producer requirement, verbatim")
        var compiled: SecRequirement?
        expectEqual(SecRequirementCreateWithString(CameraConstants.producerRequirement as CFString, [], &compiled),
                    errSecSuccess, "the requirement compiles")

        // Where producers run from.
        expectEqual(P.producerDirectory, "/Library/Application Support/CoreVideo Pro/", "the producer directory")
        expectEqual(P.installedCorePath, "/Library/Application Support/CoreVideo Pro/corevideo-native",
                    "the pkg-installed core")
        expectEqual(P.probeDirectory, "/Library/Application Support/CoreVideo Pro/vcam-probe/", "the e2e probe folder")
        expect(P.sandboxCanRead(P.installedCorePath), "the extension can read the installed core")
        expect(P.sandboxCanRead(P.probeDirectory + "corevideo-vcam-feed"), "the extension can read the e2e probe")
        expect(!P.sandboxCanRead("/Applications/CoreVideo Pro.app/Contents/Resources/corevideo-native"),
               "the app's own copy is NOT readable by the extension (why the core runs from /Library)")
        expect(!P.sandboxCanRead("/Users/Shared/CoreVideoPro/vcam-probe/corevideo-vcam-feed"),
               "the old /Users/Shared probe folder is gone")
        expect(!P.sandboxCanRead("/Library/Application Support/CoreVideo Pro Evil/corevideo-native"),
               "a sibling folder sharing the prefix is not a producer location")

        // Platform assumption, checked against the real profile on this Mac: the
        // cmioextension sandbox reads all of /Library and denies none of the
        // subpaths that would cover the producer directory.
        let profilePath = "/System/Library/Sandbox/Profiles/cmioextension.sb"
        if let profile = try? String(contentsOfFile: profilePath, encoding: .utf8) {
            expect(profile.contains(#"(allow file-read* (subpath "/Library"))"#),
                   "cmioextension.sb allows reading /Library")
            // Every (subpath "/Library/...") the profile names, allow or deny, must
            // not be a strict ancestor of the producer directory other than /Library
            // itself; a new deny on an ancestor would fail here, not in the field.
            let ancestors = profile.components(separatedBy: "(subpath \"").dropFirst()
                .compactMap { $0.components(separatedBy: "\"").first }
                .filter { $0.hasPrefix("/Library/") && P.producerDirectory.hasPrefix($0 + "/") }
            expect(ancestors.isEmpty, "no profile rule names an ancestor of the producer directory (found: \(ancestors))")
        } else {
            expect(false, "cannot read \(profilePath): the producer-location assumption is unchecked")
        }

        // The extension entitlements carry NO file temporary-exception: the
        // cmioextension profile cannot compile one (macOS 26.3), and the
        // extension then dies before main with "failed to compile sandbox profile".
        for name in ["macos-camera-extension.entitlements", "macos-camera-extension-adhoc.entitlements"] {
            let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
                .appendingPathComponent("../../../scripts/\(name)").standardizedFileURL
            guard let data = try? Data(contentsOf: url),
                  let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any]
            else { expect(false, "cannot read scripts/\(name) from the source tree"); continue }
            let fileExceptions = plist.keys.filter { $0.hasPrefix("com.apple.security.temporary-exception.files.") }
            expect(fileExceptions.isEmpty, "\(name) has no file temporary-exception (found: \(fileExceptions))")
            expectEqual(plist["com.apple.security.app-sandbox"] as? Bool, true, "\(name) stays sandboxed")
        }

        // The shell runs the installed core only when it is the same binary as
        // the app's own copy; anything else falls back to the app's copy and
        // says the camera cannot accept it.
        typealias C = CoreLocation
        expectEqual(C.choose(bundled: "/A/core", installed: "/L/core", installedExists: true, identical: true),
                    C.Choice(path: "/L/core", cameraEligible: true), "an identical installed copy runs")
        expectEqual(C.choose(bundled: "/A/core", installed: "/L/core", installedExists: true, identical: false),
                    C.Choice(path: "/A/core", cameraEligible: false), "a stale installed copy never runs")
        expectEqual(C.choose(bundled: "/A/core", installed: "/L/core", installedExists: false, identical: false),
                    C.Choice(path: "/A/core", cameraEligible: false), "no installed copy: the app's copy runs")
        expectEqual(C.choose(bundled: nil, installed: "/L/core", installedExists: true, identical: false),
                    C.Choice(path: nil, cameraEligible: false), "no bundled core: nothing to compare, nothing runs")
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("cvp-coreloc-\(getpid())")
        try? FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        let a = tmp.appendingPathComponent("a"), b = tmp.appendingPathComponent("b"), c = tmp.appendingPathComponent("c")
        try? Data("core-bytes".utf8).write(to: a); try? Data("core-bytes".utf8).write(to: b)
        try? Data("core-bytez".utf8).write(to: c)
        expect(C.filesIdentical(a.path, b.path), "identical bytes compare equal")
        expect(!C.filesIdentical(a.path, c.path), "different bytes compare unequal")
        expect(!C.filesIdentical(a.path, tmp.appendingPathComponent("missing").path), "a missing file is never identical")
        expect(!C.rootOwnedAndLocked(a.path), "a file this user owns is not a trusted installed core")
        expect(!C.rootOwnedAndLocked(tmp.appendingPathComponent("missing").path), "a missing path is not trusted")
        expect(C.rootOwnedAndLocked("/usr/bin/true"), "a root-owned system binary is trusted")
        expect(!C.rootOwnedAndLocked("/tmp"), "a symlink is not trusted, whatever it points at")
        try? FileManager.default.removeItem(at: tmp)
    }
}
