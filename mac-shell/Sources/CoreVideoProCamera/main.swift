import CoreMediaIO
import Foundation

// The provider object is the source of its CMIOExtensionProvider, and
// CMIOExtensionProvider.source is WEAK. It must live in a global for the life
// of the process, so `CameraProvider().provider` (a temporary) would not do.
let cameraProvider = CameraProvider()
CMIOExtensionProvider.startService(provider: cameraProvider.provider)
CFRunLoopRun()
