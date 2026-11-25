// A press-and-release key for Comms (Windows TalkKey): never an NSButton or a
// SwiftUI Button, because those act on click (mouse-up) and talk keys must act
// on press and on release, and "Click and Command are never used"
// (docs/reference/comms-talkback.md).
//
// The backing NSView takes mouseDown (left button only) as the press; AppKit
// routes the matching mouseUp to the view that took the down, which is the
// pointer capture Windows uses. A key held while the view leaves its window,
// or while its window resigns key, is released at once: a held key must never
// outlive the operator's ability to see it.

import AppKit
import SwiftUI

final class TalkKeyView: NSView {
    var enabled = true
    var onPress: () -> Void = {}
    var onRelease: () -> Void = {}
    private(set) var isDown = false
    private var resignObserver: NSObjectProtocol?

    override var acceptsFirstResponder: Bool { false }   // Space/Enter go to the page
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func mouseDown(with event: NSEvent) {
        guard enabled, !isDown, event.type == .leftMouseDown else { return }
        isDown = true
        onPress()
    }

    override func mouseUp(with event: NSEvent) { releaseIfDown() }

    override func rightMouseDown(with event: NSEvent) {}

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let resignObserver { NotificationCenter.default.removeObserver(resignObserver) }
        resignObserver = nil
        guard let window else { releaseIfDown(); return }
        resignObserver = NotificationCenter.default.addObserver(
            forName: NSWindow.didResignKeyNotification, object: window, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.releaseIfDown() }
        }
    }

    func releaseIfDown() {
        guard isDown else { return }
        isDown = false
        onRelease()
    }

    deinit {
        if let resignObserver { NotificationCenter.default.removeObserver(resignObserver) }
    }
}

/// The SwiftUI face draws the label; the NSView on top takes the mouse.
struct TalkKey<Label: View>: View {
    let enabled: Bool
    let onPress: () -> Void
    let onRelease: () -> Void
    let label: Label

    init(enabled: Bool, onPress: @escaping () -> Void, onRelease: @escaping () -> Void,
         @ViewBuilder label: () -> Label) {
        self.enabled = enabled
        self.onPress = onPress
        self.onRelease = onRelease
        self.label = label()
    }

    var body: some View {
        label.overlay(TalkKeySurface(enabled: enabled, onPress: onPress, onRelease: onRelease))
    }
}

private struct TalkKeySurface: NSViewRepresentable {
    let enabled: Bool
    let onPress: () -> Void
    let onRelease: () -> Void

    func makeNSView(context: Context) -> TalkKeyView {
        let view = TalkKeyView()
        update(view)
        return view
    }

    func updateNSView(_ view: TalkKeyView, context: Context) { update(view) }

    private func update(_ view: TalkKeyView) {
        view.enabled = enabled
        view.onPress = onPress
        view.onRelease = onRelease
    }

    static func dismantleNSView(_ view: TalkKeyView, coordinator: ()) { view.releaseIfDown() }
}
