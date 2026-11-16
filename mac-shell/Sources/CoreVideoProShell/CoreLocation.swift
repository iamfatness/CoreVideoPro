import CameraExtensionKit
import Foundation

/// Which copy of the core the shell launches.
///
/// The virtual camera admits a producer only after reading its code on disk,
/// and the camera extension's sandbox cannot read /Applications (see
/// ProducerAttestation). So the .pkg installs a byte-identical copy of the core
/// at ProducerAttestation.installedCorePath, and the shell runs THAT copy, but
/// only while it is the very binary inside this app. Anything else (an older
/// install, a dev build, a deleted copy) runs the app's own core, which works
/// for everything except feeding the camera: `cameraEligible` says so, and
/// Diagnostics shows it.
enum CoreLocation {
    struct Choice: Equatable {
        var path: String?
        var cameraEligible: Bool
    }

    static func choose(bundled: String?, installed: String, installedExists: Bool, identical: Bool) -> Choice {
        guard let bundled else { return Choice(path: nil, cameraEligible: false) }
        if installedExists && identical { return Choice(path: installed, cameraEligible: true) }
        return Choice(path: bundled, cameraEligible: false)
    }

    /// Byte equality, read in chunks. A missing or unreadable file is never identical.
    static func filesIdentical(_ a: String, _ b: String) -> Bool {
        let fm = FileManager.default
        guard let sa = try? fm.attributesOfItem(atPath: a)[.size] as? NSNumber,
              let sb = try? fm.attributesOfItem(atPath: b)[.size] as? NSNumber,
              sa == sb,
              let ha = FileHandle(forReadingAtPath: a), let hb = FileHandle(forReadingAtPath: b)
        else { return false }
        defer { try? ha.close(); try? hb.close() }
        while true {
            let ca = ha.readData(ofLength: 1 << 20), cb = hb.readData(ofLength: 1 << 20)
            if ca != cb { return false }
            if ca.isEmpty { return true }
        }
    }

    /// The live decision for an app whose own core is at `bundledCore`.
    static func resolve(bundledCore: String?) -> Choice {
        let installed = ProducerAttestation.installedCorePath
        let exists = FileManager.default.isExecutableFile(atPath: installed)
        return choose(bundled: bundledCore, installed: installed, installedExists: exists,
                      identical: exists && bundledCore.map { filesIdentical($0, installed) } == true)
    }
}
