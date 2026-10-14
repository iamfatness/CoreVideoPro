#!/usr/bin/env python3
"""Headless #904 attribution probe. Counters are not presentation qualification."""
import argparse
import hashlib
import json
import math
import mmap
import pathlib
import struct
import threading
import time
import uuid
from monitor_evidence import Core


def fixed(route_id, participant_id):
    return dict(routeId=route_id, mode='fixed', participantId=participant_id,
        audioRole='mix', fitMode='fill', opacity=1, zIndex=0,
        rect=dict(x=0,y=0,width=1,height=1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=pathlib.Path, required=True)
    parser.add_argument('--fake', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--duration', type=float, default=120)
    parser.add_argument('--depth', type=int, choices=(2, 3), default=2)
    args = parser.parse_args()
    if not math.isfinite(args.duration) or not 0 < args.duration <= 3600:
        parser.error('duration must be finite and in (0, 3600]')
    if not args.core.is_file() or not args.fake.is_file():
        parser.error('core and fake executables must exist')
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = dict(sourceCommit=args.source_commit,
        coreSha256=hashlib.sha256(args.core.read_bytes()).hexdigest(),
        harnessSha256=hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
        depth=args.depth, requestedSeconds=args.duration, startupRetained=True,
        workload='8 synthetic 1080p30 Zoom feeds + 2560x1440 BGRA screen at 60 Hz; isolated 1080p60 monitors',
        scope='Program reader attribution only. No shell presentation, camera receiver or audio qualification.')
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2))
    core = None
    stop = threading.Event()
    publisher = None
    mapping = None
    sample_count = 0
    latest = {}
    initial = None
    trace_key = None
    incident_files = []
    failure = None
    started = time.monotonic()
    try:
        core = Core(str(args.core.resolve()), {
            'COREVIDEO_ZOOM_ENGINE_PATH': str(args.fake.resolve()),
            'COREVIDEO_ISOLATE_MONITORS': '1',
            'COREVIDEO_PROGRAM_BUFFER_FRAMES': str(args.depth),
            'COREVIDEO_FAKE_ENGINE_PARTICIPANTS': '8',
            'COREVIDEO_FAKE_ENGINE_RES': '2', 'COREVIDEO_FAKE_ENGINE_FPS': '30',
            'COREVIDEO_FAKE_NO_CHURN': '1', 'COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE': '0',
            'COREVIDEO_GPU_CAPTURE': '0', 'COREVIDEO_CPU_SOURCE_PREPARATION': '0',
        }, args.output/'native.stderr.log')
        with (args.output/'samples.jsonl').open('w', encoding='utf-8') as sample_file:
            def retain(snapshot):
                nonlocal sample_count, latest, initial, trace_key
                worker = dict(snapshot.get('realtimeEvidence',{}).get('monitorWorker',{}))
                display = dict(worker.get('programDisplay',{}))
                incident = display.pop('incidentTrace', [])
                if incident:
                    key = (display.get('readerEpoch'),display.get('traceOverwritten'),incident[-1].get('observedNs'))
                    if key != trace_key:
                        if len(incident_files) >= 64: raise RuntimeError('incident artifact budget exhausted; incomplete evidence')
                        name = f'incident-{len(incident_files)+1:03}.json'
                        (args.output/name).write_text(json.dumps(dict(reader=display,records=incident),indent=2))
                        incident_files.append(name); trace_key = key
                    display['incidentArtifact'] = incident_files[-1]
                worker['programDisplay'] = display
                row = dict(elapsedSeconds=time.monotonic()-started,
                    programBuffer=snapshot.get('programBuffer'),
                    realtimeEvidence=dict(monitorWorker=worker,
                        render=snapshot.get('realtimeEvidence',{}).get('render')),
                    sources=[{k:s.get(k) for k in ('sourceId','kind','hasVideo','framesIngested')}
                        for s in snapshot.get('sources', [])])
                if sample_file.tell() > 256*1024*1024: raise RuntimeError('sample artifact budget exhausted; incomplete evidence')
                sample_file.write(json.dumps(row)+'\n'); sample_file.flush()
                sample_count += 1; latest = display
                if initial is None: initial = row
            retain(core.request({'type':'snapshot'})['snapshot'])
            reply = core.request({'type':'zoom-join', 'payload':{
                'meetingNumber':'1234567890','displayName':'Synthetic Program copy attribution'}}, timeout=20)
            if not reply.get('ok'): raise RuntimeError('synthetic join refused')
            pids = [str(101+i) for i in range(8)]
            reply = core.request({'type':'zoom-media-spine-sync', 'elapsedMs':0, 'payload':{
                'startCapture':True, 'sourceParticipantIds':pids,
                'subscriptions':[dict(participantId=p,kind='participant-video',purpose='program',priority=i)
                    for i,p in enumerate(pids)]}})
            if not reply.get('ok'): raise RuntimeError('synthetic spine refused')
            width, height = 2560, 1440
            name = 'Local\\CoreVideo904_'+uuid.uuid4().hex
            mapping = mmap.mmap(-1, 16+width*height*4, tagname=name)
            # Capture format is the maintained BGRA SHM v1 contract.
            struct.pack_into('<IIII', mapping, 0, 2, width, height, 0)
            mapping[16:] = bytes((45, 80, 120, 255))*(width*height)
            reply = core.request({'type':'register-capture-shm','payload':{
                'deviceId':'qa-screen','shmName':name,'width':width,'height':height}})
            if not reply.get('ok'): raise RuntimeError('screen registration refused')
            def publish():
                sequence = 2
                deadline = time.monotonic()
                while not stop.is_set():
                    sequence = (sequence+2)&0xfffffffe
                    struct.pack_into('<I',mapping,0,sequence-1)
                    struct.pack_into('<I',mapping,16,sequence)
                    struct.pack_into('<I',mapping,0,sequence)
                    deadline = max(deadline+1/60,time.monotonic())
                    stop.wait(max(0,deadline-time.monotonic()))
            publisher = threading.Thread(target=publish); publisher.start()
            retain(core.sync([
                {'type':'set-verbose-diagnostics','enabled':False},
                {'type':'set-output-profile','width':1920,'height':1080,'fps':60},
                {'type':'load-scene-graph','sceneId':'probe-program','routes':[fixed('pgm','101')]},
                {'type':'set-preview-scene','sceneId':'probe-preview','routes':[fixed('pvw','102')]},
                {'type':'set-multiview-layout','canvasWidth':1920,'canvasHeight':1080,
                    'sources':[dict(sourceId=p,kind='zoom',participantId=p,slot=i,label='Synthetic '+p)
                        for i,p in enumerate(pids)]+[dict(sourceId='capture:qa-screen',kind='capture',
                            captureDeviceId='qa-screen',slot=8,label='Synthetic screen')]},
            ],0)['snapshot'])
            deadline = time.monotonic()+args.duration
            while time.monotonic() < deadline:
                retain(core.request({'type':'snapshot'})['snapshot'])
                stop.wait(.25)
    except Exception as exc:
        failure = type(exc).__name__+': '+str(exc)
    finally:
        stop.set()
        if publisher: publisher.join(timeout=2)
        if core: core.close()
        if mapping: mapping.close()
    if latest.get('refusalObservationVersion') != 'program-display-refusal-v1':
        failure = failure or 'candidate did not expose refusal attribution contract'
    report = dict(manifest, samples=sample_count, actualSeconds=time.monotonic()-started,
        error=failure, latestDisplay=latest,
        initial=initial, incidentArtifacts=incident_files,
        observation='UNVERIFIED' if not latest.get('observed') else 'ATTRIBUTION_ONLY',
        presentationVerdict='UNVERIFIED', allOutputZeroMissVerdict='UNVERIFIED')
    (args.output/'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k:v for k,v in report.items() if k!='latestDisplay'},indent=2))
    if failure: raise SystemExit(1)


if __name__ == '__main__': main()
