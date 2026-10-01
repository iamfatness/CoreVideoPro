"""Run a command with a per-process committed-memory limit, so allocations really fail.

    python scripts/qa/run-with-memory-limit.py --process-mb 700 -- <command> [args...]

Windows only. Starts the command SUSPENDED, puts it in a job object with
JOB_OBJECT_LIMIT_PROCESS_MEMORY, then resumes it; every process it starts (node, the media
core, ffmpeg) inherits the job, and each one is individually capped. (Assigning this launcher
itself did not carry to its children on this machine; assigning the child explicitly does.) Past the cap, VirtualAlloc/operator new fail in that
process exactly as they do when Windows itself is out of commit, which is the #728 condition
(2026-10-01: std::bad_alloc on a decoder thread terminated the core three times).

Exit code is the command's exit code. Nothing here judges the result: read the core's own log
for `[frame-alloc] OUT OF MEMORY` / `[media-decoder] OUT OF MEMORY` and whether the core lived.
"""
import argparse
import ctypes
import subprocess
import sys
from ctypes import wintypes


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(name, ctypes.c_ulonglong) for name in (
        "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
        "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                ("SchedulingClass", wintypes.DWORD)]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION), ("IoInfo", IO_COUNTERS),
                ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]


JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100
JobObjectExtendedLimitInformation = 9


def main():
    if "--" not in sys.argv:
        print(__doc__)
        return 2
    split = sys.argv.index("--")
    parser = argparse.ArgumentParser()
    parser.add_argument("--process-mb", type=int, required=True)
    options = parser.parse_args(sys.argv[1:split])
    command = sys.argv[split + 1:]
    if not command:
        print("No command given after --.")
        return 2

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel32.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
                                                   ctypes.c_void_p]

    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY
    limits.ProcessMemoryLimit = options.process_mb * 1024 * 1024
    if not kernel32.SetInformationJobObject(job, JobObjectExtendedLimitInformation, ctypes.byref(limits),
                                            ctypes.sizeof(limits)):
        raise ctypes.WinError(ctypes.get_last_error())
    create_suspended = 0x00000004
    child = subprocess.Popen(command, creationflags=create_suspended)
    if not kernel32.AssignProcessToJobObject(job, int(child._handle)):
        error = ctypes.WinError(ctypes.get_last_error())
        child.kill()
        raise error
    ntdll = ctypes.WinDLL("ntdll")
    ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]
    ntdll.NtResumeProcess(int(child._handle))
    print(f"[memory-limit] every process is capped at {options.process_mb} MB committed", flush=True)
    code = child.wait()

    used = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    if kernel32.QueryInformationJobObject(job, JobObjectExtendedLimitInformation, ctypes.byref(used),
                                          ctypes.sizeof(used), None):
        print(f"[memory-limit] peak single process: {used.PeakProcessMemoryUsed / 1048576:.0f} MB; "
              f"peak job: {used.PeakJobMemoryUsed / 1048576:.0f} MB", flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
