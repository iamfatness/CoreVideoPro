#!/usr/bin/env python3
"""Run a bounded headless probe in the registered owner's directory, restoring API camera state."""
import argparse
import ctypes
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time
import urllib.request

FLAGS = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

def state(api):
    with urllib.request.urlopen(api+'/state',timeout=10) as response:
        return json.load(response)

def toggle(api,on):
    data=json.dumps(dict(action='transport.virtualcam.set',args=[on])).encode()
    request=urllib.request.Request(api+'/invoke',data=data,headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(request,timeout=15) as response:
        return json.load(response)

def restore(lease):
    # Only the distinct, previously absent QA executable can be stopped here.
    if pathlib.Path(lease['candidate']).resolve() != pathlib.Path(lease['ownerDirectory']).resolve()/'corevideo-904-validation.exe':
        raise RuntimeError('Recovery target is outside the named installation directory')
    env={**os.environ,'COREVIDEO_QA_LEASE_EXE':lease['candidate']}
    command="Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $env:COREVIDEO_QA_LEASE_EXE } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"
    subprocess.run(['powershell','-NoProfile','-Command',command],env=env,
        capture_output=True,timeout=20,creationflags=FLAGS,check=True)
    toggle(lease['api'],lease['originalCameraOn'])
    candidate=pathlib.Path(lease['candidate'])
    if candidate.is_file() and hashlib.sha256(candidate.read_bytes()).hexdigest()==lease['sha256']:
        candidate.unlink()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--watch',type=pathlib.Path)
    parser.add_argument('--core',type=pathlib.Path)
    parser.add_argument('--owner-directory',type=pathlib.Path)
    parser.add_argument('--api',default='http://127.0.0.1:8011')
    parser.add_argument('--lease',type=pathlib.Path)
    parser.add_argument('command',nargs=argparse.REMAINDER)
    args=parser.parse_args()
    if args.watch:
        lease=json.loads(args.watch.read_text())
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.OpenProcess.restype=ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_ulong]
        kernel.CloseHandle.argtypes=[ctypes.c_void_p]
        handle=kernel.OpenProcess(0x100000,False,lease['parentPid'])
        args.watch.with_suffix('.ready').touch()
        try:
            while not args.watch.with_suffix('.done').exists():
                if not handle or kernel.WaitForSingleObject(handle,1000)==0:
                    restore(lease)
                    args.watch.with_suffix('.recovered').touch()
                    return
        finally:
            if handle: kernel.CloseHandle(handle)
        return
    if not args.core or not args.owner_directory or not args.lease or not args.command:
        parser.error('core, owner-directory, lease and command required')
    if any(path.exists() for path in [args.lease,*(args.lease.with_suffix(s) for s in ('.ready','.done','.recovered'))]):
        raise RuntimeError('Lease artifacts already exist; use a fresh evidence path')
    current=state(args.api)
    if current.get('engineOn') or current.get('recording') or current.get('streaming') or current.get('zoomParticipantIds'):
        raise RuntimeError('App is active; refusing publisher switch')
    import winreg
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
        r'Software\Classes\CLSID\{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}\InprocServer32',
        0,winreg.KEY_READ|winreg.KEY_WOW64_64KEY) as key:
        owner=winreg.QueryValueEx(key,'CoreVideoOwnerAppDirectory')[0]
    if os.path.normcase(os.path.abspath(owner)) != os.path.normcase(str(args.owner_directory.resolve())):
        raise RuntimeError('Requested directory does not match registered owner')
    candidate=args.owner_directory.resolve()/'corevideo-904-validation.exe'
    if candidate.exists(): raise RuntimeError('QA executable already exists; inspect before reuse')
    lease=dict(api=args.api,originalCameraOn=current['virtualCameraOn'],candidate=str(candidate),ownerDirectory=str(args.owner_directory.resolve()),
        sha256=hashlib.sha256(args.core.read_bytes()).hexdigest(),parentPid=os.getpid())
    args.lease.parent.mkdir(parents=True,exist_ok=True)
    args.lease.write_text(json.dumps(lease,indent=2))
    watcher=subprocess.Popen([sys.executable,'-X','utf8',__file__,'--watch',str(args.lease.resolve())],
        stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=FLAGS)
    deadline=time.monotonic()+10
    while not args.lease.with_suffix('.ready').exists():
        if watcher.poll() is not None or time.monotonic()>deadline: raise RuntimeError('Recovery guard failed to start')
        time.sleep(.05)
    try:
        shutil.copy2(args.core,candidate)
        toggle(args.api,False)
        command=args.command[1:] if args.command[0]=='--' else args.command
        command=[str(candidate) if part=='{candidate}' else part for part in command]
        credential_line = None
        if '--join-credentials-stdin' in command:
            credential_line=sys.stdin.readline(65537)
            if not credential_line or len(credential_line)>65536:
                raise RuntimeError('Missing or oversized anonymous join credentials')
        child=subprocess.Popen(command,stdin=subprocess.PIPE if credential_line else subprocess.DEVNULL,
            creationflags=FLAGS)
        if credential_line:
            child.stdin.write(credential_line.encode('utf-8')); child.stdin.close()
            del credential_line
        try: return child.wait(timeout=1200)
        finally:
            if child.poll() is None: child.kill(); child.wait(timeout=10)
    finally:
        restore(lease)
        args.lease.with_suffix('.done').touch()
        watcher.wait(timeout=10)

if __name__=='__main__':
    raise SystemExit(main())
