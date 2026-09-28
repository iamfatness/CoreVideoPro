# #538 RTMP listener receive review

This slice lets the native core accept one RTMP publisher per configured source.
The decoded video and embedded audio use the existing capture source path, so
the same source can feed Preview, Program, recording, and the mixer. It is a
listener: an encoder publishes to the configured `rtmp://host:port/app/stream`
URL. The stream path is hidden from capture-device names and FFmpeg stderr.

## Repeatable local proof

On a Release native build with `COREVIDEO_WITH_RTMP_INGEST=ON` and staged FFmpeg:

```powershell
node scripts/validate-srt-ingest.mjs --transport rtmp --seconds 24 --source-size 1920x1080 --source-fps 30 --keep
```

The script configures the listener through `configure-rtmp-ingest-sources`,
publishes H.264/AAC from an independent FFmpeg process, routes the source to
Program and the audio buses, and records Program. It fails if decoded capture
video or PCM stays at zero, Program is black or silent, the recording loses
frames, the signal remains live after publisher exit, or a restarted publisher
does not resume both video and audio on the same source. It prints the retained
recording path with `--keep`.

The existing SRT test still uses the same script without `--transport rtmp`:

```powershell
node scripts/validate-srt-ingest.mjs --seconds 8 --source-size 640x360 --source-fps 30
```

## Current operator limit

The native command and capture device are available for controlled workflows.
This slice does not add an RTMP URL editor or persistence to the WinUI Sources
page. An installed UI setup and external encoder check remain for a later
operator slice. Do not present this as finished RTMP ingest in the Sources UI.

RTMP is accepted only on a plain `rtmp://` listener URL with host, application,
and stream name. Credentials and query strings are refused. Each source uses
one FFmpeg decoder child. The capability is omitted in stub and gate-off builds.
