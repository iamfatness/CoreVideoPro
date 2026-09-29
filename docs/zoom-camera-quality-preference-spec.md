# Zoom camera quality preference (#681)

## Operator intent

The Sources screen offers two independent global controls for Zoom guest cameras:

| Control | Choices | Default | Enforcement |
|---|---|---|---|
| Preferred maximum resolution | 360p, 720p, 1080p | 1080p | Request to the Meeting SDK raw renderer for each guest camera |
| Maximum guest frame rate | 15, 24, 25, 30, 60 fps | 60 fps | Local CoreVideo camera playout ceiling after SDK delivery |

Both choices are saved and latched at the next Zoom join. Changing either during
a meeting shows the pending choice and does not rebuild an active renderer or
change the running camera cadence. Screen share keeps its separate policy.

## SDK boundary and visible truth

`IZoomSDKRenderer::setRawDataResolution` accepts a resolution but has no
per-subscription receive-frame-rate setter. The Meeting SDK also has a global
`SetVideoQualityPreference` with custom minimum/maximum up to 30 fps; its
documentation describes the user's video and does not establish an incoming
raw-guest limit. It cannot represent a 60 fps ceiling. The local frame-rate
choice therefore limits which incoming guest-camera frames CoreVideo offers to
Program and Preview. It does not claim to lower Zoom network traffic or SDK
decode load. A 60 fps choice bypasses the limiter.

The Sources Format cell remains the measured input `WxH@fps` before the local
limit. The status text identifies the applied local ceiling and pending changes.
Program, recording, and streaming keep their existing 60 fps output clock;
when a guest source is capped lower, they show the latest accepted guest frame
until a new one is accepted. This must never be presented as new 60 fps motion.

## Implementation and validation

The shell persists intent and sends only scalar join parameters. The native
Zoom runtime owns a per-camera token budget after input-format accounting and
before decoded frames enter local playout. Source restarts reset that budget.
This does not add pixels to the shell or another Zoom subscription.

Automated checks cover preference persistence, pending/applied wording, default
60 fps pass-through, a 30 fps source passing without loss, 60-to-30 local
limiting, and join payload plumbing. Installed meeting acceptance still needs
15/30/60 choices: check input Format, motion on Program and Preview, screen
share, source churn, persistence across restart, and output continuity.
