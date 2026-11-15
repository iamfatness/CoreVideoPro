import CameraExtensionKit
import CoreMedia
import CoreMediaIO
import Foundation
import os.log

/// "CoreVideo Pro Camera Sink": the core enqueues finished 1920×1080 NV12
/// Program frames here. Only the newest consumed buffer is kept.
final class SinkStream: NSObject, CMIOExtensionStreamSource, @unchecked Sendable {
    static let consumeInterval: DispatchTimeInterval = .milliseconds(8)
    static let bufferQueueSize = 4
    static let buffersRequiredForStartup = 1

    private(set) var stream: CMIOExtensionStream!
    private let store: ProgramFrameStore
    private let queue = DispatchQueue(label: "us.iamfatness.corevideopro.camera.sink")
    private let log = Logger(subsystem: cameraLogSubsystem, category: "sink")
    private let streamFormat: CMIOExtensionStreamFormat?

    // Owned by `queue`.
    private var client: CMIOExtensionClient?
    private var timer: DispatchSourceTimer?
    private var consuming = false

    init(store: ProgramFrameStore) {
        self.store = store
        let fps = CameraFormats.source.first?.fps ?? [60, 30]
        streamFormat = CameraFormatFactory.streamFormat(CameraFormat(
            width: CameraConstants.sinkWidth, height: CameraConstants.sinkHeight, fps: fps))
        super.init()
        stream = CMIOExtensionStream(
            localizedName: CameraConstants.sinkStreamName,
            streamID: CameraConstants.sinkStreamUUID,
            direction: .sink, clockType: .hostTime, source: self)
    }

    // MARK: CMIOExtensionStreamSource

    var formats: [CMIOExtensionStreamFormat] { streamFormat.map { [$0] } ?? [] }

    var availableProperties: Set<CMIOExtensionProperty> {
        [.streamActiveFormatIndex, .streamFrameDuration, .streamSinkBufferQueueSize,
         .streamSinkBuffersRequiredForStartup, .streamSinkBufferUnderrunCount, .streamSinkEndOfData]
    }

    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionStreamProperties {
        let result = CMIOExtensionStreamProperties(dictionary: [:])
        if properties.contains(.streamActiveFormatIndex) { result.activeFormatIndex = 0 }
        if properties.contains(.streamFrameDuration), let format = streamFormat {
            result.frameDuration = format.minFrameDuration
        }
        if properties.contains(.streamSinkBufferQueueSize) {
            result.sinkBufferQueueSize = Self.bufferQueueSize
        }
        if properties.contains(.streamSinkBuffersRequiredForStartup) {
            result.sinkBuffersRequiredForStartup = Self.buffersRequiredForStartup
        }
        if properties.contains(.streamSinkBufferUnderrunCount) { result.sinkBufferUnderrunCount = 0 }
        if properties.contains(.streamSinkEndOfData) { result.sinkEndOfData = 0 }
        return result
    }

    func setStreamProperties(_ streamProperties: CMIOExtensionStreamProperties) throws {}

    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool {
        guard ProducerAuthorizer.isAuthorized(pid: client.pid) else { return false }
        queue.sync { self.client = client }
        return true
    }

    func startStream() throws {
        queue.sync {
            guard timer == nil else { return }
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now(), repeating: Self.consumeInterval, leeway: .milliseconds(1))
            timer.setEventHandler { [weak self] in self?.tick() }
            self.timer = timer
            timer.resume()
            log.info("sink stream started")
        }
    }

    func stopStream() throws {
        queue.sync {
            timer?.cancel()
            timer = nil
            client = nil
            consuming = false
            store.producerStopped()
            log.info("sink stream stopped")
        }
    }

    // MARK: Consume (all on `queue`)

    private func tick() {
        guard !consuming, let client else { return }
        consuming = true
        consume(from: client)
    }

    /// Drains the client's queue while it reports more buffers. Every consumed
    /// buffer is acknowledged and counted as a delivery; only the newest one
    /// survives in the store.
    private func consume(from client: CMIOExtensionClient) {
        stream.consumeSampleBuffer(from: client) { [weak self] buffer, sequence, _, hasMore, error in
            guard let self else { return }
            self.queue.async {
                guard self.timer != nil, self.client === client else {
                    self.consuming = false
                    return
                }
                if let buffer {
                    let now = HostClock.nowNanoseconds()
                    self.stream.notifyScheduledOutputChanged(
                        CMIOExtensionScheduledOutput(sequenceNumber: sequence, hostTimeInNanoseconds: now))
                    self.store.producerDelivered(buffer, atNanoseconds: now)
                }
                if buffer != nil, error == nil, hasMore {
                    self.consume(from: client)
                } else {
                    self.consuming = false
                }
            }
        }
    }
}
