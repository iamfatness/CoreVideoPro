// The Comms (Zoom talkback) tab's model: a port of
// native-shell/CoreVideoPro.WinUI/ViewModels/Comms/CommsViewModel.cs. Stub
// until plan Task 7 fills in the desk.

import Foundation

@MainActor
final class CommsModel: ObservableObject {
    @Published private(set) var state: CommsState?

    func apply(_ fact: CommsState??) {
        guard let fact else { return }
        state = fact
    }
}
