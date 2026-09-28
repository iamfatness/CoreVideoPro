# NDI receive review (#538)

Use a local NDI sender and the CoreVideo Pro candidate build. This check does not need a Zoom meeting. The machine needs the NDI runtime (NDI 6 Tools is sufficient).

1. Start NDI Test Patterns or another sender. Confirm its picture in NDI Studio Monitor first.
2. Start CoreVideo Pro and turn Engine on. Open Sources, refresh devices, and find the sender under **NDI**. Its source ID should remain stable across refreshes.
3. Assign the NDI source to a Show Input. Confirm the Sources row and Multiview show moving video without a second capture bridge. Put the source in Preview, then Take it to Program if convenient. Confirm the image is not a slate or a frozen thumbnail.
4. Use an NDI sender with audio. Confirm its source meter moves, the monitor button gates the sound, and a short Program recording contains the same source audio. Do not infer audio success from video alone.
5. Stop the sender for at least two seconds. The source should keep its assigned slot, show signal loss/staleness, and honor the selected last-frame behavior. Restart the sender and confirm frames resume without assigning a different source.
6. Take the source offline and confirm the receiver releases it. Reconnect it and confirm video and audio return.

Report the sender name, observed format, whether video and audio passed, and any step that failed. The automated gate uses the product NDI sender as an independent source, then asserts discovery, BGRA video, float PCM, canonical `capture:<id>` delivery, and stalled status after sender stop.
