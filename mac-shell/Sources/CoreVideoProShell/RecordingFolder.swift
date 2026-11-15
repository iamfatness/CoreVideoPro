// RecordingFolder — where recordings land (Tier 0.11; Windows
// ProductionSettingsWindow "Target folder": Browse… / Open folder,
// ProductionSettingsWindow.xaml:758-771, .xaml.cs:447-492).
//
// The Mac used to record to a fixed ~/Movies/CoreVideoPro with no way to pick
// an external or faster drive. The operator's choice is now persisted, checked
// for writability when chosen, and re-checked at every Record: a folder that is
// gone (an unplugged external drive) or not writable falls back to the default
// folder LOUDLY — a warning in the strip and a line under the setting — never
// a silent failure to record.

import AppKit
import Foundation

enum RecordingFolderPolicy {
    /// The built-in folder (the old fixed location).
    static func defaultFolder(fileManager: FileManager = .default) -> String {
        (fileManager.urls(for: .moviesDirectory, in: .userDomainMask).first?.path
            ?? NSTemporaryDirectory()) + "/CoreVideoPro"
    }

    struct Resolution: Equatable {
        let folder: String
        /// Non-nil when the operator's folder could not be used.
        let fallbackWarning: String?
    }

    /// Why a folder cannot take recordings, or nil when it can. Creates the
    /// folder when its parent exists (Windows Open folder creates it too), but
    /// never under a missing /Volumes/<drive>: that is an unplugged drive, and
    /// mkdir there would at best fail and at worst record onto the boot disk.
    static func problem(with path: String, fileManager: FileManager = .default) -> String? {
        let standard = (path as NSString).standardizingPath
        guard standard.hasPrefix("/") else { return "not an absolute path" }
        let parts = standard.split(separator: "/", omittingEmptySubsequences: true)
        if parts.count >= 2, parts[0] == "Volumes" {
            let volume = "/Volumes/" + parts[1]
            var isDir: ObjCBool = false
            if !fileManager.fileExists(atPath: volume, isDirectory: &isDir) || !isDir.boolValue {
                return "the drive \(parts[1]) is not connected"
            }
        }
        var isDir: ObjCBool = false
        if fileManager.fileExists(atPath: standard, isDirectory: &isDir) {
            guard isDir.boolValue else { return "it is a file, not a folder" }
        } else {
            do {
                try fileManager.createDirectory(atPath: standard, withIntermediateDirectories: true)
            } catch {
                return "it does not exist and could not be created"
            }
        }
        return fileManager.isWritableFile(atPath: standard) ? nil : "it is not writable"
    }

    /// The folder a recording should use right now. `chosen` empty = default.
    static func resolve(chosen: String, defaultFolder: String,
                        fileManager: FileManager = .default) -> Resolution {
        guard !chosen.isEmpty else { return Resolution(folder: defaultFolder, fallbackWarning: nil) }
        guard let problem = problem(with: chosen, fileManager: fileManager) else {
            return Resolution(folder: (chosen as NSString).standardizingPath, fallbackWarning: nil)
        }
        return Resolution(folder: defaultFolder, fallbackWarning: fallbackWarning(
            chosen: chosen, problem: problem, defaultFolder: defaultFolder))
    }

    static func fallbackWarning(chosen: String, problem: String, defaultFolder: String) -> String {
        "Recording folder unavailable (\(problem)): \(chosen). "
            + "Recording to \(defaultFolder) instead."
    }

    /// The Windows note's first sentence. Its second ("Files are named
    /// <prefix>-program-<timestamp>.mp4") is not true of the Mac writer, which
    /// names sessions differently, so it is not copied.
    static let note = "Recordings save here."
}

extension AppModel {
    /// What Settings shows: the operator's folder, or the default.
    var recordingFolderDisplay: String {
        recordingFolderChoice.isEmpty ? RecordingFolderPolicy.defaultFolder() : recordingFolderChoice
    }

    /// The Record flyout's folder: the core's own target folder while a
    /// recording is live (where the files are actually going, fallback
    /// included), else where the next Record will go.
    var recordFlyoutFolder: String {
        if recordingStatus == "recording" || recordingStatus == "warning",
           let core = (lastSnapshot["recording"] as? JSONObject)?["targetFolder"] as? String,
           !core.isEmpty {
            return core
        }
        return recordingFolderDisplay
    }

    /// The folder this Record uses. A folder that cannot take recordings falls
    /// back to the default LOUDLY (warning strip + the Settings line); the
    /// operator's choice is kept, so reconnecting the drive restores it.
    func recordingTargetFolder() -> String {
        // The in-binary harness never creates folders in ~/Movies.
        let fallback = ShellHarness.isActive
            ? NSTemporaryDirectory() + "corevideo-shell-harness-\(getpid())/recordings"
            : RecordingFolderPolicy.defaultFolder()
        let resolution = RecordingFolderPolicy.resolve(chosen: recordingFolderChoice,
                                                       defaultFolder: fallback)
        if let warning = resolution.fallbackWarning {
            recordingFolderWarning = warning
            pushWarning("recording: \(warning)")
        } else if !recordingFolderChoice.isEmpty {
            recordingFolderWarning = ""
        }
        return resolution.folder
    }

    /// Applies a folder the operator picked. Refused (and said so) when it
    /// cannot take recordings; the previous choice stands.
    @discardableResult
    func setRecordingFolder(_ path: String) -> Bool {
        if let problem = RecordingFolderPolicy.problem(with: path) {
            recordingFolderWarning = "Cannot record to \(path): \(problem)."
            return false
        }
        recordingFolderChoice = (path as NSString).standardizingPath
        recordingFolderWarning = ""
        return true
    }

    func useDefaultRecordingFolder() {
        recordingFolderChoice = ""
        recordingFolderWarning = ""
    }

    /// Browse… (Windows FolderPicker): directories only, New Folder allowed.
    func chooseRecordingFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose"
        panel.message = "Choose the folder recordings save to."
        panel.directoryURL = URL(fileURLWithPath: recordingFolderDisplay)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        setRecordingFolder(url.path)
    }

    /// Open folder (Windows): creates it if missing, then shows it in Finder.
    func openRecordingFolder() {
        let folder = recordingFolderDisplay
        try? FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
        NSWorkspace.shared.open(URL(fileURLWithPath: folder))
    }
}
