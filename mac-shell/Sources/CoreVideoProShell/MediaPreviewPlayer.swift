// MediaPreviewPlayer — the Media tab's LOCAL preview of the selected clip
// (Windows: MediaPage.xaml's MediaPlayerElement). It is an operator's look at
// the file, never what is on air: Program is composited by the core.
//
// Muted, with no control to unmute (plan default D4): an audition must not put
// sound in the control room that the Media strip does not govern.

import AVKit
import SwiftUI

struct MediaPreviewPlayer: NSViewRepresentable {
    let url: URL
    /// Plays while true, pauses otherwise. A toggle never seeks.
    var playing = false

    final class Coordinator {
        var url: URL?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> AVPlayerView {
        let view = AVPlayerView()
        view.controlsStyle = .none
        view.videoGravity = .resizeAspect
        view.setAccessibilityLabel("Local preview of the selected clip (muted)")
        load(url, into: view, coordinator: context.coordinator)
        apply(playing, to: view)
        return view
    }

    func updateNSView(_ view: AVPlayerView, context: Context) {
        if context.coordinator.url != url {
            load(url, into: view, coordinator: context.coordinator)
        }
        apply(playing, to: view)
    }

    static func dismantleNSView(_ view: AVPlayerView, coordinator: Coordinator) {
        view.player?.pause()
        view.player = nil
    }

    private func load(_ url: URL, into view: AVPlayerView, coordinator: Coordinator) {
        let player = AVPlayer(url: url)
        player.isMuted = true
        player.actionAtItemEnd = .pause
        view.player = player
        coordinator.url = url
    }

    private func apply(_ playing: Bool, to view: AVPlayerView) {
        guard let player = view.player else { return }
        player.isMuted = true
        if playing {
            if player.rate == 0 { player.play() }
        } else if player.rate != 0 {
            player.pause()
        }
    }
}
