#!/usr/bin/env python3
"""Repeated independent camera consumer starts; producer/registration are untouched."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receiver', type=pathlib.Path, required=True)
    parser.add_argument('--dll', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--count', type=int, default=60)
    parser.add_argument('--seconds', type=int, default=1)
    args = parser.parse_args()
    if not 1 <= args.count <= 120 or not 1 <= args.seconds <= 10:
        parser.error('count must be 1..120 and seconds 1..10')
    if not args.receiver.is_file() or not args.dll.is_file() or not shutil.which('node'):
        parser.error('receiver, candidate DLL and Node are required')
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = dict(scope='Cold direct-DLL consumer pixels only; existing publisher, registration, desktop and app are not controlled',
        receiverSha256=hashlib.sha256(args.receiver.read_bytes()).hexdigest(),
        dllSha256=hashlib.sha256(args.dll.read_bytes()).hexdigest(),count=args.count,secondsPerStart=args.seconds,
        warmupSeconds=0,startedUnix=time.time())
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2))
    failures = []
    files = []
    for index in range(args.count):
        path = args.output/f'receiver-{index+1:03}.ndjson'
        with path.open('wb') as output, path.with_suffix('.stderr.log').open('wb') as error:
            child = subprocess.Popen([str(args.receiver.resolve()),str(args.seconds),'--dll',str(args.dll.resolve())],
                stdin=subprocess.DEVNULL,stdout=output,stderr=error,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            try: code = child.wait(timeout=args.seconds+30)
            except subprocess.TimeoutExpired:
                child.kill(); child.wait(timeout=5); code = -1
            if code: failures.append(dict(start=index+1,exitCode=code))
        files.append(str(path.resolve()))
    judge = pathlib.Path(__file__).with_name('camera-pixel-receiver.mjs').resolve().as_uri()
    code = ('import {readFileSync} from "node:fs"; const {judgeCameraPixels}=await import(process.argv[1]); '
            'console.log(JSON.stringify(JSON.parse(process.argv[2]).map(path=>({path,...judgeCameraPixels('
            'readFileSync(path,"utf8"),{warmupSeconds:0,minimumSeconds:.5})}))));')
    judged = subprocess.run([shutil.which('node'),'--input-type=module','-e',code,judge,json.dumps(files)],
        capture_output=True,text=True,timeout=60,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    results = json.loads(judged.stdout) if judged.returncode == 0 else []
    passed = not failures and len(results) == args.count and all(x.get('receiverPixelContinuityPassed') for x in results)
    report = dict(manifest,finishedUnix=time.time(),verdict='PASS' if passed else 'FAIL',failures=failures,results=results,
        samples=sum(x.get('samples',0) for x in results),invalid=sum(x.get('invalid',0) for x in results),
        repeated=sum(x.get('repeated',0) for x in results),missing=sum(x.get('missing',0) for x in results))
    (args.output/'report.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({k:report[k] for k in ('verdict','count','samples','invalid','repeated','missing')}))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
