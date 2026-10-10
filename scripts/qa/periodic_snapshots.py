"""Space source-advancement observations; never duplicate start/end samples."""
import time


def periodic_snapshots(sync, deadline, interval=.25, clock=time.monotonic, wait=time.sleep):
    while clock() < deadline:
        wait(interval)
        yield sync()
