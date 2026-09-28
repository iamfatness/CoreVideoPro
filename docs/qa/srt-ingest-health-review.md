# SRT ingest health review (#536)

Use a disposable SRT contribution sender and the CoreVideo Pro candidate build. This check does not need Zoom. Do not paste a stream passphrase into a screenshot, Support Bundle description, or issue comment.

1. Start the app, turn Engine on, and add an SRT ingest source in Sources. Assign it to a Show Input. The source row should say `connecting`, show no decoded frame yet, and display `RTT unavailable (FFmpeg owns socket)`.
2. Start a 1080p30 sender to that source. Without clicking Preview or Take, confirm the row changes to `receiving`, its last-frame age moves, and the Show Input has moving picture and source audio.
3. Record Program for at least 24 seconds with that input in Program. Confirm the recording has picture and audio. The codec and packet error counters should remain zero for a clean sender.
4. Stop the sender. Confirm the source reports `stalled` after its frame ages out and keeps its assigned slot. Restart the same sender. Confirm live picture, audio, and a fresh last-frame age return without recreating the source.
5. Turn Engine off, then on. Confirm old meeting health is cleared and new health comes from the current source session.

Report the sender format, whether video and audio passed, the observed state sequence, and any nonzero counters. RTT is explicitly unavailable while FFmpeg owns the SRT socket; a numeric RTT would require a different socket owner and is outside this slice. The automated fault test injects codec and packet errors into the FFmpeg child, verifies the counters across a decoder restart, and checks that decoder text containing a passphrase is not copied into the operator warning.
