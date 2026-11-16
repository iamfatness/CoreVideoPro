import CameraExtensionKit
import CoreMedia
import CoreMediaIO
import CoreVideo
import Foundation
import IOKit.audio
import os.log

/// Host time in nanoseconds: mach_absolute_time through the timebase. This is
/// the clock FrameSelector, sendSampleBuffer and notifyScheduledOutputChanged
/// all share.
enum HostClock {
    private static let timebase: mach_timebase_info_data_t = {
        var info = mach_timebase_info_data_t()
        mach_timebase_info(&info)
        return info
    }()

    static func nowNanoseconds() -> UInt64 {
        let ticks = mach_absolute_time()
        let numer = UInt64(timebase.numer), denom = UInt64(timebase.denom)
        return (ticks / denom) * numer + (ticks % denom) * numer / denom
    }
}

/// The handoff between the sink (the core feeding Program) and the source
/// (what Zoom/Teams/FaceTime read). Only the newest program buffer is kept.
final class ProgramFrameStore: @unchecked Sendable {
    private let lock = NSLock()
    private var selector = FrameSelector()
    private var latest: CMSampleBuffer?

    func producerDelivered(_ buffer: CMSampleBuffer, atNanoseconds now: UInt64) {
        lock.lock(); defer { lock.unlock() }
        latest = buffer
        selector.producerDelivered(atNanoseconds: now)
    }

    /// An explicit stop: the slate shows at once, with no hold (spec §4 law 2).
    func producerStopped() {
        lock.lock(); defer { lock.unlock() }
        latest = nil
        selector.producerStopped()
    }

    /// The program buffer to serve now, or nil for the slate.
    func programBuffer(atNanoseconds now: UInt64) -> CMSampleBuffer? {
        lock.lock(); defer { lock.unlock() }
        guard selector.choice(atNanoseconds: now) == .program else { return nil }
        return latest
    }
}

/// Builds the NV12 ('420v', BT.601 video range) format descriptions both
/// streams advertise.
enum CameraFormatFactory {
    static let pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange

    static func formatDescription(width: Int, height: Int) -> CMFormatDescription? {
        let extensions = [
            kCMFormatDescriptionExtension_YCbCrMatrix: kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4
        ] as CFDictionary
        var description: CMFormatDescription?
        let status = CMVideoFormatDescriptionCreate(
            allocator: kCFAllocatorDefault, codecType: pixelFormat,
            width: Int32(width), height: Int32(height), extensions: extensions,
            formatDescriptionOut: &description)
        return status == noErr ? description : nil
    }

    static func streamFormat(_ format: CameraFormat) -> CMIOExtensionStreamFormat? {
        guard let description = formatDescription(width: format.width, height: format.height),
              let fastest = format.fps.max(), let slowest = format.fps.min() else { return nil }
        return CMIOExtensionStreamFormat(
            formatDescription: description,
            maxFrameDuration: CMTime(value: 1, timescale: CMTimeScale(slowest)),
            minFrameDuration: CMTime(value: 1, timescale: CMTimeScale(fastest)),
            validFrameDurations: format.fps.map { CMTime(value: 1, timescale: CMTimeScale($0)) })
    }
}

/// The one virtual device. Streams are added source first, then sink.
final class CameraDevice: NSObject, CMIOExtensionDeviceSource, @unchecked Sendable {
    /// Carries the extension version (CameraConstants.deviceModel): the shell
    /// matches on it to know the device it sees is THIS copy's.
    static let deviceModel = CameraConstants.deviceModel

    private(set) var device: CMIOExtensionDevice!
    let store = ProgramFrameStore()
    private var sourceStream: SourceStream!
    private var sinkStream: SinkStream!

    init(localizedName: String) {
        super.init()
        device = CMIOExtensionDevice(
            localizedName: localizedName, deviceID: CameraConstants.deviceUUID,
            legacyDeviceID: CameraConstants.legacyDeviceID, source: self)
        sourceStream = SourceStream(localizedName: localizedName, store: store)
        sinkStream = SinkStream(store: store)
        let log = Logger(subsystem: cameraLogSubsystem, category: "source")
        for stream in [sourceStream.stream!, sinkStream.stream!] {
            do {
                try device.addStream(stream)
            } catch {
                log.error("addStream \(stream.localizedName, privacy: .public) failed: \(error.localizedDescription, privacy: .public)")
            }
        }
    }

    // MARK: CMIOExtensionDeviceSource

    var availableProperties: Set<CMIOExtensionProperty> {
        [.deviceTransportType, .deviceModel]
    }

    func deviceProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionDeviceProperties {
        let result = CMIOExtensionDeviceProperties(dictionary: [:])
        if properties.contains(.deviceTransportType) {
            result.transportType = Int(kIOAudioDeviceTransportTypeVirtual)
        }
        if properties.contains(.deviceModel) { result.model = Self.deviceModel }
        return result
    }

    func setDeviceProperties(_ deviceProperties: CMIOExtensionDeviceProperties) throws {}
}
