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

## Sources operator check

1. In **Sources → RTMP ingest**, enter a local listener URL such as
   `rtmp://0.0.0.0:1935/live/test` and leave the URL field to apply it. Each
   configured source needs a different TCP port. The stream path is encrypted
   in the local settings file and omitted from device names and status text.
2. Assign **RTMP 1** from the Show inputs source picker and bring it online.
   The core starts the listener; the source should show no video until a
   publisher connects.
3. Publish H.264/AAC from a separate encoder to the same URL. Check the tile,
   Preview, Program, monitor audio, and a short Program recording. Stop the
   publisher and confirm signal clears; restart it and confirm both video and
   audio return on the same source.
4. Restart the app. Confirm the URL remains in Sources and the same Show input
   assignment still resolves. Remove the source and confirm its slot and route
   clear rather than pointing to a stale device.

The headless native test above proves decode, record and reconnect. Installed
Sources interaction and an external encoder remain human acceptance evidence
for this UI slice; report them as unverified until run.

RTMP is accepted only on a plain `rtmp://` listener URL with host, application,
and stream name. Credentials and query strings are refused. Each source uses
one FFmpeg decoder child. The capability is omitted in stub and gate-off builds.
