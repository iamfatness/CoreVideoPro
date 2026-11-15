// VcamProbe — the capture side of the macOS virtual camera's local end-to-end
// check (roadmap M9.2, Task 7; scripts/macos-vcam-e2e.sh). It opens "CoreVideo
// Pro Camera" exactly as Zoom or Photo Booth would (AVCaptureSession, '420v'),
// classifies every frame as standby slate or program with the SAME
// SlateClassifier the extension's slate is built against, and reports counts.
//
//   VcamProbe capture --seconds N [--fps 30|60] [--json] [--device-id ID]
//                     [--frames-csv PATH]
//   VcamProbe list
//
// `capture --json` prints one line, keys in this order:
//   {"frames":Int,"fps":Double,"width":Int,"height":Int,"pixelFormat":"420v",
//    "slateFrames":Int,"programFrames":Int,"centerY":Int}
// The measured window starts at the FIRST delivered frame (camera open and
// format negotiation are not counted) and lasts N seconds.
//
// Device: the AVCaptureDevice whose uniqueID is the legacy ID
// (us.iamfatness.corevideopro.camera.device) OR the device UUID string — which
// one macOS reports is a Task 3 Step 5(f) spike fact, so both are accepted.
// `--device-id` overrides it (the Step 5 mutation points it at the FaceTime
// camera to prove the classifier is not vacuous).
//
// CAMERA PERMISSION: a command-line tool inherits camera access from the app
// that launched it (Terminal, iTerm, …). The first run prompts for that app; if
// access is denied the probe says how to grant it and exits 3.
//
// Exit codes: 0 ok, 2 usage, 3 camera access denied, 4 device not found,
// 5 capture failed or no frames.

import AVFoundation
import CameraExtensionKit
import CoreMedia
import CoreVideo
import Foundation

// MARK: - Arguments

struct ProbeOptions {
    var seconds: Double = 3
    var fps: Int = 30
    var json = false
    var deviceID: String?
    var framesCSV: String?
}

func usage() -> Never {
    FileHandle.standardError.write(Data("""
    usage: VcamProbe capture --seconds N [--fps 30|60] [--json] [--device-id ID] [--frames-csv PATH]
           VcamProbe list

    """.utf8))
    exit(2)
}

func fail(_ code: Int32, _ message: String) -> Never {
    FileHandle.standardError.write(Data("VcamProbe: \(message)\n".utf8))
    exit(code)
}

func parseCapture(_ args: ArraySlice<String>) -> ProbeOptions {
    var options = ProbeOptions()
    var it = args.makeIterator()
    while let arg = it.next() {
        switch arg {
        case "--seconds":
            guard let v = it.next(), let s = Double(v), s > 0 else { usage() }
            options.seconds = s
        case "--fps":
            guard let v = it.next(), let f = Int(v), f == 30 || f == 60 else { usage() }
            options.fps = f
        case "--json":
            options.json = true
        case "--device-id":
            guard let v = it.next(), !v.isEmpty else { usage() }
            options.deviceID = v
        case "--frames-csv":
            guard let v = it.next(), !v.isEmpty else { usage() }
            options.framesCSV = v
        default:
            usage()
        }
    }
    return options
}

// MARK: - Devices

func videoDevices() -> [AVCaptureDevice] {
    var types: [AVCaptureDevice.DeviceType] = [.builtInWideAngleCamera]
    if #available(macOS 14.0, *) {
        types += [.external, .continuityCamera, .deskViewCamera]
    } else {
        types += [.externalUnknown]
    }
    return AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: .video,
                                            position: .unspecified).devices
}

/// Our camera's uniqueID is either form (spike fact pending, see header).
func isCoreVideoProCamera(_ uniqueID: String) -> Bool {
    uniqueID == CameraConstants.legacyDeviceID
        || uniqueID.caseInsensitiveCompare(CameraConstants.deviceUUID.uuidString) == .orderedSame
}

func findDevice(_ explicitID: String?) -> AVCaptureDevice? {
    let devices = videoDevices()
    if let explicitID { return devices.first { $0.uniqueID == explicitID } }
    return devices.first { isCoreVideoProCamera($0.uniqueID) }
}

// MARK: - Camera permission

/// Blocks until access is known. Prompts (for the launching terminal app) the
/// first time; never silently proceeds without access.
func ensureCameraAccess() {
    switch AVCaptureDevice.authorizationStatus(for: .video) {
    case .authorized:
        return
    case .notDetermined:
        FileHandle.standardError.write(Data("VcamProbe: requesting camera access (macOS asks once for the app running this probe)…\n".utf8))
        let done = DispatchSemaphore(value: 0)
        var granted = false
        AVCaptureDevice.requestAccess(for: .video) { ok in granted = ok; done.signal() }
        done.wait()
        if granted { return }
    case .denied, .restricted:
        break
    @unknown default:
        break
    }
    fail(3, """
    camera access denied. Grant it to the app running this probe (Terminal, iTerm, …) in \
    System Settings › Privacy & Security › Camera, then run again. \
    (Reset with: tccutil reset Camera)
    """)
}

// MARK: - Capture

final class FrameCounter: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let windowSeconds: Double
    private let lock = NSLock()
    private var firstFrameAt: Double?
    private(set) var frames = 0
    private(set) var slateFrames = 0
    private(set) var programFrames = 0
    private(set) var width = 0
    private(set) var height = 0
    private(set) var pixelFormat = ""
    private(set) var lastProgramCenterY: Int?
    private(set) var lastCenterY = 0
    private(set) var csvRows: [String] = []
    let windowDone = DispatchSemaphore(value: 0)
    private var signalled = false

    init(windowSeconds: Double) { self.windowSeconds = windowSeconds }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        let now = Date().timeIntervalSince1970
        guard let image = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        lock.lock(); defer { lock.unlock() }
        if signalled { return }
        if firstFrameAt == nil { firstFrameAt = now }
        if let first = firstFrameAt, now - first > windowSeconds {
            signalled = true
            windowDone.signal()
            return
        }

        CVPixelBufferLockBaseAddress(image, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(image, .readOnly) }
        let w = CVPixelBufferGetWidthOfPlane(image, 0)
        let h = CVPixelBufferGetHeightOfPlane(image, 0)
        let bytesPerRow = CVPixelBufferGetBytesPerRowOfPlane(image, 0)
        guard CVPixelBufferIsPlanar(image), let base = CVPixelBufferGetBaseAddressOfPlane(image, 0) else {
            return
        }
        let y = UnsafeBufferPointer(start: base.assumingMemoryBound(to: UInt8.self), count: bytesPerRow * h)
        let slate = SlateClassifier.isSlate(yPlane: y, width: w, height: h, bytesPerRow: bytesPerRow)
        let centerY = Int(y[(h / 2) * bytesPerRow + w / 2])

        frames += 1
        if slate { slateFrames += 1 } else { programFrames += 1; lastProgramCenterY = centerY }
        lastCenterY = centerY
        width = w
        height = h
        pixelFormat = fourCC(CVPixelBufferGetPixelFormatType(image))
        csvRows.append("\(Int64((now * 1000).rounded())),\(slate ? "slate" : "program"),\(centerY)")
    }

    struct Result {
        var frames, slateFrames, programFrames, width, height, centerY: Int
        var fps: Double
        var pixelFormat: String
        var csvRows: [String]
    }

    func result() -> Result {
        lock.lock(); defer { lock.unlock() }
        return Result(frames: frames, slateFrames: slateFrames, programFrames: programFrames,
                      width: width, height: height, centerY: lastProgramCenterY ?? lastCenterY,
                      fps: Double(frames) / windowSeconds, pixelFormat: pixelFormat, csvRows: csvRows)
    }
}

func fourCC(_ code: OSType) -> String {
    let bytes = [UInt8((code >> 24) & 0xff), UInt8((code >> 16) & 0xff),
                 UInt8((code >> 8) & 0xff), UInt8(code & 0xff)]
    return String(bytes: bytes, encoding: .ascii) ?? String(code)
}

/// 1920×1080 if the device offers it at `fps`, else the first format that does.
func configure(_ device: AVCaptureDevice, fps: Int) throws {
    let candidates = device.formats.filter { format in
        format.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= Double(fps) && Double(fps) <= $0.maxFrameRate }
    }
    let preferred = candidates.first { format in
        let d = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
        return d.width == Int32(CameraConstants.sinkWidth) && d.height == Int32(CameraConstants.sinkHeight)
    } ?? candidates.first
    guard let format = preferred else { return }  // keep the device default
    try device.lockForConfiguration()
    device.activeFormat = format
    let duration = CMTime(value: 1, timescale: CMTimeScale(fps))
    device.activeVideoMinFrameDuration = duration
    device.activeVideoMaxFrameDuration = duration
    device.unlockForConfiguration()
}

func runCapture(_ options: ProbeOptions) -> Never {
    // Listing needs no camera permission, so a missing device is reported
    // without prompting; opening it does.
    guard let device = findDevice(options.deviceID) else {
        let wanted = options.deviceID
            ?? "\(CameraConstants.legacyDeviceID) or \(CameraConstants.deviceUUID.uuidString)"
        fail(4, "no camera with uniqueID \(wanted). Is CoreVideo Pro Camera installed and approved? (`VcamProbe list` shows what macOS sees)")
    }
    ensureCameraAccess()
    let session = AVCaptureSession()
    session.beginConfiguration()
    let input: AVCaptureDeviceInput
    do { input = try AVCaptureDeviceInput(device: device) } catch {
        fail(5, "cannot open \(device.localizedName): \(error.localizedDescription)")
    }
    guard session.canAddInput(input) else { fail(5, "the session refused \(device.localizedName)") }
    session.addInput(input)
    let output = AVCaptureVideoDataOutput()
    output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String:
                                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
    output.alwaysDiscardsLateVideoFrames = false
    let counter = FrameCounter(windowSeconds: options.seconds)
    output.setSampleBufferDelegate(counter, queue: DispatchQueue(label: "us.iamfatness.corevideopro.vcam-probe.frames"))
    guard session.canAddOutput(output) else { fail(5, "the session refused a '420v' video output") }
    session.addOutput(output)
    session.commitConfiguration()
    do { try configure(device, fps: options.fps) } catch {
        FileHandle.standardError.write(Data("VcamProbe: keeping the device's default format (\(error.localizedDescription))\n".utf8))
    }

    session.startRunning()
    // Window = N seconds from the first frame; allow 10 s for the camera to open.
    let opened = counter.windowDone.wait(timeout: .now() + options.seconds + 10)
    session.stopRunning()
    let r = counter.result()
    if let path = options.framesCSV {
        let csv = (["epochMs,class,centerY"] + r.csvRows).joined(separator: "\n") + "\n"
        try? csv.write(toFile: path, atomically: true, encoding: .utf8)
    }
    if opened == .timedOut && r.frames == 0 {
        fail(5, "\(device.localizedName) delivered no frames within \(Int(options.seconds + 10)) s")
    }
    if options.json {
        print(String(format: "{\"frames\":%d,\"fps\":%.2f,\"width\":%d,\"height\":%d,\"pixelFormat\":\"%@\",\"slateFrames\":%d,\"programFrames\":%d,\"centerY\":%d}",
                     r.frames, r.fps, r.width, r.height, r.pixelFormat as NSString,
                     r.slateFrames, r.programFrames, r.centerY))
    } else {
        print("\(device.localizedName) [\(device.uniqueID)]: \(r.frames) frames in \(options.seconds) s "
              + "(\(String(format: "%.2f", r.fps)) fps) \(r.width)x\(r.height) \(r.pixelFormat); "
              + "slate \(r.slateFrames), program \(r.programFrames), centerY \(r.centerY)")
    }
    exit(r.frames > 0 ? 0 : 5)
}

func runList() -> Never {
    for device in videoDevices() {
        let mark = isCoreVideoProCamera(device.uniqueID) ? "  <- CoreVideo Pro Camera" : ""
        print("\(device.uniqueID)\t\(device.localizedName)\(mark)")
    }
    exit(0)
}

// MARK: - Entry

let arguments = CommandLine.arguments.dropFirst()
switch arguments.first {
case "capture": runCapture(parseCapture(arguments.dropFirst()))
case "list": runList()
default: usage()
}
