import CameraExtensionKit
import CoreMedia
import CoreMediaIO
import CoreVideo
import Foundation
import os.log

/// "CoreVideo Pro Camera": what Zoom, Teams, FaceTime and OBS read. A timer at
/// the active frame duration serves the newest Program buffer while it is
/// fresh, else the standby slate (spec §4 law 2: always serve something).
final class SourceStream: NSObject, CMIOExtensionStreamSource, @unchecked Sendable {
    private(set) var stream: CMIOExtensionStream!
    private let store: ProgramFrameStore
    private let queue = DispatchQueue(label: "us.iamfatness.corevideopro.camera.source")
    private let log = Logger(subsystem: cameraLogSubsystem, category: "source")
    private let streamFormats: [CMIOExtensionStreamFormat]
    /// One prebuilt slate per advertised format, built once at launch.
    private let slates: [CVPixelBuffer?]
    /// Video-range black (Y 16, UV 128), per format: served if a slate is
    /// missing and nothing has been sent yet.
    private let blacks: [CVPixelBuffer?]

    // Owned by `queue`.
    private var activeFormatIndex = 0
    private var frameDuration: CMTime
    private var timer: DispatchSourceTimer?
    private var lastChoice: FrameChoice?
    /// The last sample actually sent: the fallback when the slate cannot be
    /// built, so the camera never goes silent (spec §4 law 2).
    private var lastSent: CMSampleBuffer?
    /// Failure states, each logged once per transition.
    private var retimeFailing = false
    private var slateFailing = false

    init(localizedName: String, store: ProgramFrameStore) {
        self.store = store
        streamFormats = CameraFormats.source.compactMap(CameraFormatFactory.streamFormat)
        slates = CameraFormats.source.map { Self.makeSlate(width: $0.width, height: $0.height) }
        blacks = CameraFormats.source.map { Self.makeBlack(width: $0.width, height: $0.height) }
        frameDuration = streamFormats.first?.minFrameDuration ?? CMTime(value: 1, timescale: 60)
        super.init()
        stream = CMIOExtensionStream(
            localizedName: localizedName, streamID: CameraConstants.sourceStreamUUID,
            direction: .source, clockType: .hostTime, source: self)
    }

    /// A CVPixelBuffer holding StandbySlate.nv12, byte for byte (bulk row copies).
    static func makeSlate(width: Int, height: Int) -> CVPixelBuffer? {
        guard let bytes = StandbySlate.nv12(width: width, height: height) else { return nil }
        return makeNV12(bytes, width: width, height: height)
    }

    static func makeBlack(width: Int, height: Int) -> CVPixelBuffer? {
        guard width > 0, height > 0, width % 2 == 0, height % 2 == 0 else { return nil }
        let bytes = [UInt8](repeating: 16, count: width * height)
            + [UInt8](repeating: 128, count: width * (height / 2))
        return makeNV12(bytes, width: width, height: height)
    }

    private static func makeNV12(_ bytes: [UInt8], width: Int, height: Int) -> CVPixelBuffer? {
        let attributes = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary] as CFDictionary
        var buffer: CVPixelBuffer?
        guard CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                                  CameraFormatFactory.pixelFormat, attributes, &buffer) == kCVReturnSuccess,
              let buffer else { return nil }
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        bytes.withUnsafeBytes { raw in
            let planes = [(offset: 0, rows: height), (offset: width * height, rows: height / 2)]
            for (plane, layout) in planes.enumerated() {
                guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { continue }
                let stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
                for row in 0..<layout.rows {
                    memcpy(base + row * stride, raw.baseAddress! + layout.offset + row * width, width)
                }
            }
        }
        CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey,
                              kCVImageBufferYCbCrMatrix_ITU_R_601_4, .shouldPropagate)
        return buffer
    }

    // MARK: CMIOExtensionStreamSource

    var formats: [CMIOExtensionStreamFormat] { streamFormats }

    var availableProperties: Set<CMIOExtensionProperty> {
        [.streamActiveFormatIndex, .streamFrameDuration]
    }

    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionStreamProperties {
        let result = CMIOExtensionStreamProperties(dictionary: [:])
        queue.sync {
            if properties.contains(.streamActiveFormatIndex) { result.activeFormatIndex = activeFormatIndex }
            if properties.contains(.streamFrameDuration) { result.frameDuration = frameDuration }
        }
        return result
    }

    func setStreamProperties(_ streamProperties: CMIOExtensionStreamProperties) throws {
        queue.sync {
            if let index = streamProperties.activeFormatIndex, streamFormats.indices.contains(index) {
                activeFormatIndex = index
                frameDuration = streamFormats[index].minFrameDuration
            }
            if let duration = streamProperties.frameDuration,
               streamFormats[activeFormatIndex].validFrameDurations?.contains(duration) ?? false {
                frameDuration = duration
            }
            if timer != nil { armTimer() }
        }
    }

    /// Any consumer may read the camera (spec §4 law 6 restricts the sink only).
    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool { true }

    func startStream() throws {
        queue.sync {
            lastChoice = nil
            armTimer()
            log.info("source stream started")
        }
    }

    func stopStream() throws {
        queue.sync {
            timer?.cancel()
            timer = nil
            log.info("source stream stopped")
        }
    }

    // MARK: Serve (all on `queue`)

    private func armTimer() {
        timer?.cancel()
        let timer = DispatchSource.makeTimerSource(flags: .strict, queue: queue)
        let nanos = Int(CMTimeGetSeconds(frameDuration) * 1_000_000_000)
        timer.schedule(deadline: .now(), repeating: .nanoseconds(nanos), leeway: .microseconds(500))
        timer.setEventHandler { [weak self] in self?.tick() }
        self.timer = timer
        timer.resume()
    }

    private func tick() {
        let hostNanos = HostClock.nowNanoseconds()
        let timing = CMSampleTimingInfo(
            duration: frameDuration,
            presentationTimeStamp: CMTime(value: CMTimeValue(hostNanos), timescale: 1_000_000_000),
            decodeTimeStamp: .invalid)
        var program: CMSampleBuffer?
        if let fresh = store.programBuffer(atNanoseconds: hostNanos) {
            program = retimed(fresh, timing)
            noteTransition(&retimeFailing, failing: program == nil,
                           "program retime failed; serving the slate until it succeeds",
                           "program retime recovered")
        }
        let choice: FrameChoice = program == nil ? .slate : .program
        if choice != lastChoice {
            log.info("source now serving \(choice == .program ? "program" : "slate", privacy: .public)")
            lastChoice = choice
        }
        var buffer = program
        if buffer == nil {
            buffer = slateSample(timing)
            noteTransition(&slateFailing, failing: buffer == nil,
                           "slate unavailable; repeating the last frame sent (black if none)",
                           "slate recovered")
            if buffer == nil, let last = lastSent { buffer = retimed(last, timing) }
            if buffer == nil { buffer = blackSample(timing) }
        }
        guard let buffer else { return }
        lastSent = buffer
        stream.send(buffer, discontinuity: [], hostTimeInNanoseconds: hostNanos)
    }

    private func noteTransition(_ state: inout Bool, failing: Bool, _ failed: String, _ recovered: String) {
        guard state != failing else { return }
        state = failing
        if failing {
            log.error("\(failed, privacy: .public)")
        } else {
            log.info("\(recovered, privacy: .public)")
        }
    }

    private func retimed(_ buffer: CMSampleBuffer, _ timing: CMSampleTimingInfo) -> CMSampleBuffer? {
        var timing = timing
        var copy: CMSampleBuffer?
        let status = CMSampleBufferCreateCopyWithNewTiming(
            allocator: kCFAllocatorDefault, sampleBuffer: buffer, sampleTimingEntryCount: 1,
            sampleTimingArray: &timing, sampleBufferOut: &copy)
        return status == noErr ? copy : nil
    }

    private func slateSample(_ timing: CMSampleTimingInfo) -> CMSampleBuffer? {
        guard slates.indices.contains(activeFormatIndex), let slate = slates[activeFormatIndex] else {
            return nil
        }
        return sample(of: slate, timing)
    }

    private func blackSample(_ timing: CMSampleTimingInfo) -> CMSampleBuffer? {
        guard blacks.indices.contains(activeFormatIndex), let black = blacks[activeFormatIndex] else {
            return nil
        }
        return sample(of: black, timing)
    }

    private func sample(of slate: CVPixelBuffer, _ timing: CMSampleTimingInfo) -> CMSampleBuffer? {
        var description: CMFormatDescription?
        guard CMVideoFormatDescriptionCreateForImageBuffer(
            allocator: kCFAllocatorDefault, imageBuffer: slate, formatDescriptionOut: &description) == noErr,
              let description else { return nil }
        var timing = timing
        var sample: CMSampleBuffer?
        let status = CMSampleBufferCreateReadyWithImageBuffer(
            allocator: kCFAllocatorDefault, imageBuffer: slate, formatDescription: description,
            sampleTiming: &timing, sampleBufferOut: &sample)
        return status == noErr ? sample : nil
    }
}
