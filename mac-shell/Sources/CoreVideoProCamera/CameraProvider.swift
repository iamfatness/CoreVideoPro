import CameraExtensionKit
import CoreMediaIO
import Foundation
import os.log

let cameraLogSubsystem = "us.iamfatness.corevideopro.camera"

/// The extension's one provider: one device, "CoreVideo Pro Camera".
final class CameraProvider: NSObject, CMIOExtensionProviderSource, @unchecked Sendable {
    /// UserDefaults key the rename path (Task 8) persists the sanitized name under.
    static let deviceNameDefaultsKey = "deviceName"

    private(set) var provider: CMIOExtensionProvider!
    private var device: CameraDevice!

    override init() {
        super.init()
        provider = CMIOExtensionProvider(source: self, clientQueue: nil)
        device = CameraDevice(localizedName: Self.persistedDeviceName())
        do {
            try provider.addDevice(device.device)
        } catch {
            Logger(subsystem: cameraLogSubsystem, category: "source")
                .error("addDevice failed: \(error.localizedDescription, privacy: .public)")
        }
    }

    static func persistedDeviceName() -> String {
        guard let raw = UserDefaults.standard.string(forKey: deviceNameDefaultsKey) else {
            return CameraConstants.defaultDeviceName
        }
        return DeviceNamePolicy.sanitized(raw)
    }

    // MARK: CMIOExtensionProviderSource

    func connect(to client: CMIOExtensionClient) throws {}

    func disconnect(from client: CMIOExtensionClient) {}

    var availableProperties: Set<CMIOExtensionProperty> {
        [.providerName, .providerManufacturer]
    }

    func providerProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionProviderProperties {
        let result = CMIOExtensionProviderProperties(dictionary: [:])
        if properties.contains(.providerName) { result.name = "CoreVideo Pro" }
        if properties.contains(.providerManufacturer) { result.manufacturer = "CoreVideo Pro" }
        return result
    }

    func setProviderProperties(_ providerProperties: CMIOExtensionProviderProperties) throws {}
}
