# #538 HLS Program push review

This slice sends the live Program as H.264/AAC HLS by HTTP PUT to an origin that
accepts playlist and segment uploads. It uses the existing native FFmpeg sender
and Program taps. The origin must serve uploaded files back over HTTP to viewers.

## Automated delivery proof

From a Release native build with RTMP output enabled and a staged FFmpeg runtime:

```powershell
node scripts/validate-hls-output.mjs --seconds 24
```

The harness launches a fake Zoom meeting, routes its video and audio to Program,
and starts HLS output to a local HTTP PUT origin. It fails unless the origin
receives a bounded live playlist and at least two segments, `ffprobe` decodes
H.264 and AAC from those received files, and the decoded audio is audible. It
prints the artifact directory for inspection. This is an independent receiver
proof; a sender counter alone is insufficient.

## Installed operator check

1. Configure an HTTP origin that supports PUT and serves uploaded paths by GET.
   Use a playlist URL ending in `.m3u8`, for example
   `https://origin.example/live/program.m3u8`. The current slice does not accept
   credentials embedded in the URL or query tokens.
2. Open **Settings → Stream**, turn off RTMP if it has no configured destination,
   select **HLS push**, enter the playlist URL, and choose H.264. Leave the
   shared stream profile at the desired resolution and bitrate. Start Stream.
3. Confirm the origin receives a `.m3u8` playlist and `.ts` segments. Open that
   playlist in an independent HLS player and confirm moving Program video and
   live audio. Check Health for the `hls` sender and its received-frame count.
4. Stop Stream. Confirm segment uploads stop. Start again and confirm the origin
   receives fresh segment numbers and a playable live playlist.

This is a two-second-segment live HLS output. Receiver latency is measured at
the player; the sender's generic `latencyMs` status is not a receiver measurement.
The origin must expire old segments; the live playlist is bounded to six entries,
but an HTTP PUT origin can retain files beyond that window. This slice does not
add an HTTP origin, CDN, credentials, or HLS ingest.
