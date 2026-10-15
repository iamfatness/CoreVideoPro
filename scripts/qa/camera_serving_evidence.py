"""Read-only, bounded evidence for the camera DLL actually serving a trial."""
import hashlib
import json
import mmap
import os
import pathlib
import re
import struct
import subprocess

MAX_LOG_BYTES = 8 * 1024 * 1024
MODULE_LINE = re.compile(r"^\[\d{2}:\d{2}:\d{2}\.\d{3} pid=(\d+)\] Serving module path=(.+)$")


def file_digest(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def service_identity():
    # Query the named service, not a guessed process name or the activating core.
    script = "$s=Get-CimInstance Win32_Service -Filter \"Name='FrameServer'\" -ErrorAction Stop; " \
        "if (!$s -or $s.State -ne 'Running' -or $s.ProcessId -le 0) { throw 'FrameServer is not running' }; " \
        "$p=Get-CimInstance Win32_Process -Filter ('ProcessId='+$s.ProcessId) -ErrorAction Stop; " \
        "if (!$p.CreationDate) { throw 'FrameServer creation identity unavailable' }; " \
        "@{name=$s.Name;state=$s.State;pid=[int]$s.ProcessId;created=$p.CreationDate.ToUniversalTime().ToString('o')} | ConvertTo-Json -Compress"
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
        capture_output=True, text=True, timeout=15,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        raise RuntimeError("FrameServer identity query failed: " + result.stderr[-1000:])
    value = json.loads(result.stdout)
    if not valid_service(value):
        raise ValueError("Malformed FrameServer identity")
    return value


def valid_service(value):
    return isinstance(value, dict) and value.get("name") == "FrameServer" and \
        value.get("state") == "Running" and type(value.get("pid")) is int and value["pid"] > 0 and \
        isinstance(value.get("created"), str) and bool(value["created"])


class ServeLogCapture:
    """Preserve only trial appends; reject replacement, truncation and overflow."""
    def __init__(self, source, destination, limit=MAX_LOG_BYTES):
        self.source, self.destination = pathlib.Path(source), pathlib.Path(destination)
        self.limit, self.identity, self.offset, self.initial_offset = limit, None, 0, 0
        self.anchor, self.anchor_at, self.errors = b"", 0, []
        self.initial_anchor_sha = None
        self.sharing_retries, self.pending_sharing = 0, False
        self.captured_digest = hashlib.sha256()
        self.destination.write_bytes(b"")
        self._initial = True
        self.poll()
        self._initial = False

    def poll(self):
        if self.errors:
            return
        try:
            with self.source.open("rb") as stream:
                stat = os.fstat(stream.fileno())
                identity = [stat.st_dev, stat.st_ino]
                if not stat.st_ino:
                    raise ValueError("serve log file identity unavailable")
                if self.identity is None:
                    self.identity = identity
                    if self._initial:
                        self.initial_offset = self.offset = stat.st_size
                        if self.offset:
                            stream.seek(self.offset - 1)
                            if stream.read(1) != b"\n":
                                raise ValueError("initial serve log offset is not a complete line boundary")
                        self.anchor_at = max(0, self.offset - 4096)
                        stream.seek(self.anchor_at)
                        self.anchor = stream.read(self.offset - self.anchor_at)
                        self.initial_anchor_sha = hashlib.sha256(self.anchor).hexdigest()
                if identity != self.identity:
                    raise ValueError("serve log file replaced")
                if stat.st_size < self.offset:
                    raise ValueError("serve log truncated")
                stream.seek(self.anchor_at)
                if stream.read(len(self.anchor)) != self.anchor:
                    raise ValueError("serve log preceding bytes changed (truncate/rewrite)")
                added = stat.st_size - self.offset
                if self.offset + added - self.initial_offset > self.limit:
                    raise ValueError("serve log trial append exceeds byte bound")
                stream.seek(self.offset)
                data = stream.read(added)
                if len(data) != added:
                    raise ValueError("serve log changed during capture")
                with self.destination.open("ab") as output:
                    output.write(data)
                self.captured_digest.update(data)
                self.offset += added
                self.anchor_at = max(0, self.offset - 4096)
                stream.seek(self.anchor_at)
                self.anchor = stream.read(self.offset - self.anchor_at)
                self.pending_sharing = False
        except PermissionError as error:
            if self.identity is not None and error.filename and pathlib.Path(error.filename) == self.source:
                self.sharing_retries += 1
                self.pending_sharing = True  # Preserve offset; a later read must capture every byte.
            else:
                self.errors.append(str(error))
        except FileNotFoundError:
            if self.identity is not None:
                self.errors.append("serve log disappeared")
        except (OSError, ValueError) as error:
            self.errors.append(str(error))

    def metadata(self):
        saved_sha = None
        errors = self.errors.copy()
        if self.pending_sharing: errors.append("serve log final read is denied; append capture incomplete")
        try:
            if self.destination.stat().st_size > self.limit:
                raise ValueError("Saved serve log exceeds byte bound")
            saved_sha = file_digest(self.destination)
            if saved_sha != self.captured_digest.hexdigest():
                errors.append("Saved serve log changed after capture")
        except (OSError, ValueError) as error:
            errors.append(str(error))
        return {"path": str(self.source), "savedPath": str(self.destination), "fileIdentity": self.identity,
            "initialOffset": self.initial_offset, "finalOffset": self.offset,
            "capturedBytes": self.offset - self.initial_offset, "limitBytes": self.limit,
            "initialBoundarySha256": self.initial_anchor_sha,
            "capturedSha256": saved_sha,
            "captureComplete": not errors and self.identity is not None, "sharingRetries": self.sharing_retries, "errors": errors}


def judge_serving_identity(capture, before, after, expected_path, expected_sha, digest=file_digest):
    evidence = {"servingIdentityVerified": False, "serviceBefore": before, "serviceAfter": after,
        "expectedDllPath": str(expected_path), "expectedDllSha256": expected_sha,
        "serveLog": capture.metadata(), "observedModules": [], "errors": []}
    errors = evidence["errors"]
    if not valid_service(before) or not valid_service(after) or before != after:
        errors.append("FrameServer service instance missing or changed during trial")
    if not evidence["serveLog"]["captureComplete"]:
        errors.append("Trial serve log capture is incomplete")
    try:
        evidence["expectedDllSha256End"] = digest(expected_path)
        if evidence["expectedDllSha256End"] != expected_sha:
            errors.append("Expected installed DLL changed during trial")
        with capture.destination.open("rb") as saved:
            data = saved.read(capture.limit + 1)
        if len(data) > capture.limit:
            raise ValueError("Saved serve log exceeds byte bound")
        if data and not data.endswith(b"\n"):
            errors.append("Trial serve log ends in a partial record")
        text = data.decode("utf-8", errors="strict")
        pid = before.get("pid") if isinstance(before, dict) else None
        paths = set()
        for line in text.splitlines():
            match = MODULE_LINE.fullmatch(line)
            if match and int(match[1]) == pid:
                paths.add(match[2])
        if not paths:
            errors.append("No trial serving-module record from the actual FrameServer PID")
        for path in sorted(paths):
            observed = {"pid": pid, "path": path, "sha256": None}
            evidence["observedModules"].append(observed)
            if not pathlib.Path(path).is_absolute() or pathlib.Path(path).name.lower() != "corevideo-virtualcam.dll":
                errors.append("Serving module path is not an absolute camera DLL path")
                continue
            observed["sha256"] = digest(path)
            if observed["sha256"] != expected_sha:
                errors.append("FrameServer served a DLL that differs from the installed candidate")
    except (OSError, UnicodeError, ValueError) as error:
        errors.append(str(error))
    evidence["servingIdentityVerified"] = not errors
    evidence["limitation"] = "Trial service instance, self-reported module path and on-disk SHA256; not a memory-image digest or display qualification."
    return evidence


def read_correlation(path):
    """Sparse stable sidecar observation; never writes or creates the mapping."""
    try:
        with open(path, "rb") as source, mmap.mmap(source.fileno(), 80, access=mmap.ACCESS_READ) as view:
            first = struct.unpack_from("<I", view, 8)[0]
            data = view[:80]
            second = struct.unpack_from("<I", view, 8)[0]
        values = struct.unpack("<IIIIQQQqqqQII", data)
        names = ("magic", "version", "sequence", "pixelSequence", "epochHigh", "epochLow", "publication",
            "programSequence", "deliveredAt100ns", "publishedAt100ns", "pixelFileIdentity", "pixelVolumeIdentity", "recordBytes")
        result = dict(zip(names, values))
        result["observed"] = first == second == result["sequence"] and first % 2 == 0 and \
            result["magic"] == 0x43565431 and result["version"] == 1 and result["recordBytes"] == 80 and \
            (result["epochHigh"] != 0 or result["epochLow"] != 0) and result["programSequence"] > 0
        return result
    except (OSError, ValueError, struct.error) as error:
        return {"observed": False, "error": str(error)}
