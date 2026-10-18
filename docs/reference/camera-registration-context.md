# Camera registration and packaged QA launchers

The October 17 #857 paired-camera failure was reproduced on October 18 with the
unchanged installed 089ece2 core. A Microsoft Store Python launcher read an older
private camera alias owned by beta-2026-10-15-26350c0 and passed that registry view
to its native child. The worker's registry reads all succeeded; they returned
that older owner and protected DLL, while external unpackaged PowerShell read the
current 089ece2 owner and b3f089 camera DLL. This was a different effective view,
not evidence of registry corruption or a cached failed read.

The Python interpreter reported package identity
PythonSoftwareFoundation.Python.3.12_3.12.2800.0_x64__qbz5n2kfra8p0. Its native child
reported APPMODEL_ERROR_NO_PACKAGE despite reading the older alias. Consequently,
checking only the child's package identity, opening fresh HKCU handles, restarting
the worker or repairing the global key does not establish a valid QA context.
Python documents this private registry/filesystem behavior in its
[Windows Store known issues](https://docs.python.org/3.12/using/windows.html#known-issues).

The correction belongs in qualification launchers. Use an unpackaged Python from
an unpackaged shell, and identify it explicitly. The shared QA driver rejects a
packaged interpreter before starting any native process. The camera reference
records that launcher observation; it still requires ownership, actual serving
module/hash and independent receiver evidence. No-package status alone is not a
proof about every ancestor or effective registry view.

Production ownership checks remain strict: the machine registration must belong
to the executing installation and point to its protected runtime. A user alias
owned by another installation is refused, including one with our product role.
No unknown/older alias is overwritten merely because external inspection appears
correct. A bounded native failure diagnostic now records the worker's expected
app/role, observed alias/owner/role, read statuses/types and PID/thread so this
class of discrepancy is attributable without guessing.

This explains the captured October 17 trial and its October 18 reproduction. It
does not retrospectively establish every historical normal-desktop failure in
#857. Earlier failed evidence and subsequent passing samples remain distinct;
startup or receiver samples do not establish physical display, a live meeting,
audio synchronization, indefinite continuity or fleet acceptance.
