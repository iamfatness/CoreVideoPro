#!/usr/bin/env python3
"""Headless #904 attribution probe. Counters are not presentation qualification."""
import argparse
import hashlib
import json
import math
import mmap
import pathlib
import shutil
import subprocess
import struct
import sys
import threading
import time
import uuid
from monitor_evidence import Core
from camera_serving_evidence import ServeLogCapture, file_digest, service_identity, judge_serving_identity


def fixed(route_id, participant_id):
    return dict(routeId=route_id, mode='fixed', participantId=participant_id,
        audioRole='mix', fitMode='fill', opacity=1, zIndex=0,
        rect=dict(x=0,y=0,width=1,height=1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=pathlib.Path, required=True)
    engine_group = parser.add_mutually_exclusive_group(required=True)
    engine_group.add_argument('--fake', type=pathlib.Path)
    engine_group.add_argument('--engine', type=pathlib.Path, help='Real SDK helper; requires --meeting-url-file')
    parser.add_argument('--meeting-url-file', type=pathlib.Path, help='Full authorized test URL; never copied to evidence')
    parser.add_argument('--join-credentials-stdin', action='store_true', help='Read SDK JWT/ZAK via anonymous stdin, never persist them')
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--duration', type=float, default=120)
    parser.add_argument('--depth', type=int, choices=(2, 3), default=2)
    parser.add_argument('--cpu-source-preparation', choices=('0','1'), default='1')
    parser.add_argument('--screen-device', help='Actual WGC screen:<n> input instead of synthetic BGRA')
    parser.add_argument('--program-scene', choices=('single','grid'), default='single')
    parser.add_argument('--camera-receiver', type=pathlib.Path, help='Registered OS-camera receiver; caller must lease the publisher first')
    parser.add_argument('--expected-camera-dll', type=pathlib.Path)
    args = parser.parse_args()
    if sys.platform == 'win32' and not sys.flags.utf8_mode:
        parser.error('Run with python -X utf8; native RPC is UTF-8 (existing harness issue #877)')
    if not math.isfinite(args.duration) or not 0 < args.duration <= 3600:
        parser.error('duration must be finite and in (0, 3600]')
    engine = args.fake or args.engine
    if not args.core.is_file() or not engine.is_file():
        parser.error('core and fake executables must exist')
    if args.engine and (not args.meeting_url_file or not args.meeting_url_file.is_file()):
        parser.error('real SDK probe requires a file containing the full authorized test URL')
    if args.camera_receiver and (not args.expected_camera_dll or not args.expected_camera_dll.is_file()):
        parser.error('camera receiver requires the expected installed camera DLL')
    node = shutil.which('node')
    if args.camera_receiver and not node: parser.error('camera pixel judge requires Node')
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = dict(sourceCommit=args.source_commit,
        coreSha256=hashlib.sha256(args.core.read_bytes()).hexdigest(),
        harnessSha256=hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
        depth=args.depth, requestedSeconds=args.duration, startupRetained=True,
        cpuSourcePreparation=args.cpu_source_preparation,
        screenDevice=args.screen_device or 'synthetic-shm', programScene=args.program_scene,
        workload=('8 synthetic 1080p30 Zoom feeds' if args.fake else '8 real Zoom feeds (observed sizes/cadence retained)')+
            (' + actual WGC screen' if args.screen_device else ' + 2560x1440 synthetic BGRA screen at 60 Hz')+
            '; isolated 1080p60 monitors',
        synthetic=bool(args.fake),
        scope='Program reader attribution only. No shell presentation, camera receiver or audio qualification.')
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2))
    core = None
    stop = threading.Event()
    publisher = None
    mapping = None
    sample_count = 0
    latest = {}
    latest_sources = []
    monitor_max = {k:0 for k in ("failed", "superseded")}
    initial = None
    trace_key = None
    incident_files = []
    incident_bytes = 0
    phase = 'initialization'
    before_demand = None
    expected_program_sources = set()
    ingredient_samples = 0
    ingredient_errors = set()
    failure = None
    receiver = None
    receiver_files = []
    serving = ServeLogCapture(pathlib.Path('C:/ProgramData/CoreVideoPro/vcam-serve.log'),args.output/'serve.log') if args.camera_receiver else None
    service_before = service_identity() if serving else None
    serving_result = None
    started = time.monotonic()
    try:
        core = Core(str(args.core.resolve()), {
            'COREVIDEO_ZOOM_ENGINE_PATH': str(engine.resolve()),
            'COREVIDEO_ISOLATE_MONITORS': '1',
            'COREVIDEO_PROGRAM_BUFFER_FRAMES': str(args.depth),
            'COREVIDEO_FAKE_ENGINE_PARTICIPANTS': '8',
            'COREVIDEO_FAKE_ENGINE_RES': '2', 'COREVIDEO_FAKE_ENGINE_FPS': '30',
            'COREVIDEO_FAKE_NO_CHURN': '1', 'COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE': '0',
            'COREVIDEO_GPU_CAPTURE': '0', 'COREVIDEO_CPU_SOURCE_PREPARATION': args.cpu_source_preparation,
            'COREVIDEO_QA_PROGRAM_COUNTER': '1' if args.camera_receiver else '0',
            'COREVIDEO_DELIVERY_TRACE': '1' if args.camera_receiver else '0',
        }, args.output/'native.stderr.log')
        with (args.output/'samples.jsonl').open('w', encoding='utf-8') as sample_file:
            def retain(snapshot):
                nonlocal sample_count, latest, latest_sources, initial, trace_key, incident_bytes, ingredient_samples
                if serving: serving.poll()
                worker = dict(snapshot.get('realtimeEvidence',{}).get('monitorWorker',{}))
                for key in monitor_max: monitor_max[key] = max(monitor_max[key], worker.get(key,0))
                exported_display = worker.get('programDisplay',{})
                display = dict(exported_display.get('latestReader',exported_display))
                incident = display.pop('incidentTrace', [])
                if incident:
                    key = (display.get('readerEpoch'),display.get('traceOverwritten'),incident[-1].get('observedNs'))
                    if key != trace_key:
                        if len(incident_files) >= 512: raise RuntimeError('incident artifact budget exhausted; incomplete evidence')
                        name = f'incident-{len(incident_files)+1:03}.json'
                        content = json.dumps(dict(reader=display,records=incident),indent=2)
                        content_bytes = len(content.encode('utf-8'))
                        if incident_bytes+content_bytes > 256*1024*1024:
                            raise RuntimeError('incident byte budget exhausted; incomplete evidence')
                        (args.output/name).write_text(content,encoding='utf-8')
                        incident_bytes += content_bytes
                        incident_files.append(name); trace_key = key
                    display['incidentArtifact'] = incident_files[-1]
                exported = dict(exported_display)
                exported.pop('latestReader',None); exported.pop('incidentTrace',None)
                worker['programDisplay'] = exported
                worker['programDisplayReader'] = display
                row = dict(elapsedSeconds=time.monotonic()-started, phase=phase,
                    programBuffer=snapshot.get('programBuffer'),
                    realtimeEvidence=dict(monitorWorker=worker,
                        render=snapshot.get('realtimeEvidence',{}).get('render'),
                        capturePreparation=snapshot.get('realtimeEvidence',{}).get('capturePreparation')),
                    sources=[{k:s.get(k) for k in ('sourceId','kind','hasVideo','framesIngested','width','height','health')}
                        for s in snapshot.get('sources', [])])
                frame = snapshot.get('programFrame',{})
                row['programFrame'] = {k:frame.get(k) for k in ('sceneId','deliverySequence','frameNumber',
                    'videoSources','renderer','gpuComposed','width','height','fps','health')}
                row['programSourceAdmission'] = snapshot.get('programSourceAdmission')
                row['virtualCamera'] = snapshot.get('virtualCamera')
                if phase == 'required-monitor-run' and frame.get('sceneId') == 'probe-program':
                    ingredient_samples += 1
                    bindings = {s.get('participantId') if s.get('kind') == 'participant-video' else s.get('sourceId')
                        for s in frame.get('videoSources',[])}
                    if not expected_program_sources.issubset(bindings): ingredient_errors.add('Program binding membership mismatch')
                    if (frame.get('width'),frame.get('height'),frame.get('fps'),frame.get('gpuComposed')) != (1920,1080,60,True):
                        ingredient_errors.add('Program was not GPU-composed 1080p60')
                    if args.cpu_source_preparation == '1':
                        admission = snapshot.get('programSourceAdmission',{})
                        selected = {s.get('sourceId') for s in admission.get('sources',[])
                            if s.get('state') in ('ready','held') and isinstance(s.get('actualFrameId'),(int,float))
                            and s['actualFrameId'] >= 0 and s.get('actualEpoch') == s.get('requestedEpoch')}
                        if admission.get('readyOnlyRequested') is not True or not expected_program_sources.issubset(selected):
                            ingredient_errors.add('Requested Program pixels lacked source-admission evidence')
                if sample_file.tell() > 256*1024*1024: raise RuntimeError('sample artifact budget exhausted; incomplete evidence')
                sample_file.write(json.dumps(row)+'\n'); sample_file.flush()
                sample_count += 1; latest = display
                latest_sources = row['sources']
                if initial is None: initial = row
            retain(core.request({'type':'snapshot'})['snapshot'])
            payload = dict(displayName='CoreVideo Program copy attribution',cameraMaxResolution=2,cameraMaxFps=60)
            if args.join_credentials_stdin:
                line = sys.stdin.readline(65537)
                if len(line) > 65536: raise RuntimeError('join credential input exceeded bound')
                credentials = json.loads(line)
                for key in ('sdkJwt','userZak'):
                    if isinstance(credentials.get(key),str): payload[key] = credentials[key]
                if not payload.get('sdkJwt'): raise RuntimeError('SDK credential provider did not supply a JWT')
                del credentials, line
            if args.fake: payload['meetingNumber'] = '1234567890'
            else: payload['meetingUrl'] = args.meeting_url_file.read_text(encoding='utf-8-sig').strip()
            reply = core.request({'type':'zoom-join', 'payload':payload}, timeout=30)
            if not reply.get('ok'): raise RuntimeError('test join refused')
            pids = [str(101+i) for i in range(8)] if args.fake else []
            if args.engine:
                phase = 'waiting-for-roster'
                join_deadline = time.monotonic()+45
                while time.monotonic() < join_deadline:
                    zoom = core.request({'type':'zoom-snapshot'})['snapshot']
                    retain(core.request({'type':'snapshot'})['snapshot'])
                    pids = [str(p['userId']) for p in zoom.get('participants',[])
                        if p.get('videoOn') and not p.get('isMe')][:8]
                    if len(pids) == 8: break
                    stop.wait(.5)
                if len(pids) != 8: raise RuntimeError('real meeting did not provide eight video participants')
            reply = core.request({'type':'zoom-media-spine-sync', 'elapsedMs':0, 'payload':{
                'startCapture':True, 'sourceParticipantIds':pids,
                'subscriptions':[dict(participantId=p,kind='participant-video',purpose='program',priority=i)
                    for i,p in enumerate(pids)]}})
            if not reply.get('ok'): raise RuntimeError('synthetic spine refused')
            screen = args.screen_device or 'qa-screen'
            if args.screen_device:
                devices = core.request({'type':'list-capture-devices'}).get('devices',[])
                if not any(d.get('id') == screen for d in devices): raise RuntimeError('requested WGC screen is unavailable')
                reply = core.request({'type':'connect-capture-device','payload':{'deviceId':screen}})
                if not reply.get('ok'): raise RuntimeError('WGC screen connection refused')
            else:
                width, height = 2560, 1440
                name = 'Local\\CoreVideo904_'+uuid.uuid4().hex
                mapping = mmap.mmap(-1, 16+width*height*4, tagname=name)
                struct.pack_into('<IIII', mapping, 0, 2, width, height, 0)
                mapping[16:] = bytes((45, 80, 120, 255))*(width*height)
                reply = core.request({'type':'register-capture-shm','payload':{
                    'deviceId':screen,'shmName':name,'width':width,'height':height}})
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
            program_routes = [fixed('pgm','101' if args.fake else pids[0])]
            if args.program_scene == 'grid':
                program_routes = [fixed('pgm-'+p,p) for p in pids]
                program_routes.append(dict(routeId='screen',mode='capture-input',captureDeviceId=screen,
                    audioRole='mix',fitMode='fill',opacity=1,zIndex=0,rect=dict(x=0,y=0,width=1,height=1)))
                for i,route in enumerate(program_routes):
                    route['rect'] = dict(x=(i%3)/3,y=(i//3)/3,width=1/3,height=1/3)
            expected_program_sources = set(pids if args.program_scene == 'grid' else pids[:1])
            if args.program_scene == 'grid': expected_program_sources.add('capture:'+screen)
            phase = 'before-monitor-demand'
            before_demand = core.request({'type':'snapshot'})['snapshot']
            retain(before_demand)
            if args.camera_receiver:
                response = core.sync([
                    {'type':'sync-virtual-camera','on':True,'mirror':False,'framerEnabled':False,'deviceName':'CoreVideo Pro Camera'},
                    {'type':'start-program-output','destinations':['virtual-camera'],'isoParticipantIds':[]},
                ],0)
                (args.output/'camera-setup.json').write_text(json.dumps(response,indent=2))
                if not response.get('ok'): raise RuntimeError('camera setup refused')
                ready_deadline = time.monotonic()+15
                while time.monotonic() < ready_deadline:
                    snapshot = core.request({'type':'snapshot'})['snapshot']
                    retain(snapshot)
                    camera = snapshot.get('virtualCamera',{})
                    if camera.get('status') == 'live' and camera.get('framesPublished',0) > 0: break
                    stop.wait(.1)
                else: raise RuntimeError('camera publisher did not become ready')
                receiver_files = [(args.output/'receiver.ndjson').open('wb'),(args.output/'receiver.stderr.log').open('wb')]
                receiver = subprocess.Popen([str(args.camera_receiver.resolve()),str(math.ceil(args.duration))],
                    stdin=subprocess.DEVNULL,stdout=receiver_files[0],stderr=receiver_files[1],
                    creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            phase = 'monitor-demand-ack'
            retain(core.sync([
                {'type':'set-verbose-diagnostics','enabled':False},
                {'type':'set-output-profile','width':1920,'height':1080,'fps':60},
                {'type':'load-scene-graph','sceneId':'probe-program','routes':program_routes},
                {'type':'set-preview-scene','sceneId':'probe-preview','routes':[fixed('pvw',pids[1])]},
                {'type':'set-multiview-layout','canvasWidth':1920,'canvasHeight':1080,
                    'layoutMode':'pgmPvwTop',
                    'sources':[dict(sourceId=p,kind='zoom',participantId=p,slot=i,label='Synthetic '+p)
                        for i,p in enumerate(pids)]+[dict(sourceId='capture:'+screen,kind='capture',
                            captureDeviceId=screen,slot=8,label='Screen probe')]},
            ],0)['snapshot'])
            # Readiness is content-based, not a discarded time-based warmup.
            # All pre-ready counters and pixels remain in the same capture.
            phase = 'awaiting-program-source-readiness'
            readiness_deadline = time.monotonic()+30
            while time.monotonic() < readiness_deadline:
                snapshot = core.request({'type':'snapshot'})['snapshot']
                retain(snapshot)
                admission = snapshot.get('programSourceAdmission',{})
                ready_sources = {s.get('sourceId') for s in admission.get('sources',[])
                    if s.get('state') in ('ready','held') and s.get('actualFrameId',-1) >= 0
                    and s.get('actualEpoch') == s.get('requestedEpoch')}
                if snapshot.get('programFrame',{}).get('sceneId') == 'probe-program' and expected_program_sources.issubset(ready_sources):
                    break
                stop.wait(.05)
            else: raise RuntimeError('Program did not admit all requested source pixels')
            phase = 'required-monitor-run'
            deadline = time.monotonic()+args.duration
            while time.monotonic() < deadline:
                retain(core.request({'type':'snapshot'})['snapshot'])
                stop.wait(.25)
    except Exception as exc:
        failure = type(exc).__name__+': '+str(exc)
    finally:
        stop.set()
        if publisher: publisher.join(timeout=2)
        if receiver:
            try: receiver.wait(timeout=35)
            except subprocess.TimeoutExpired:
                receiver.kill(); receiver.wait(timeout=5)
                failure = failure or 'OS camera receiver exceeded bounded deadline'
            if receiver.returncode != 0: failure = failure or 'OS camera receiver failed'
        for handle in receiver_files: handle.close()
        if core: core.close()
        if serving:
            serving.poll()
            serving_result = judge_serving_identity(serving,service_before,service_identity(),
                args.expected_camera_dll,file_digest(args.expected_camera_dll))
        if mapping: mapping.close()
    if latest.get('refusalObservationVersion') != 'program-display-refusal-v1':
        failure = failure or 'candidate did not expose refusal attribution contract'
    if not latest.get('observed') or latest.get('readerEpoch') in (None, '0'):
        failure = failure or 'GPU Program reader was not observed; invalid trial'
    if sum(s.get('kind') == 'zoom' and s.get('hasVideo') is True for s in latest_sources) != 8:
        failure = failure or 'eight producing Zoom sources were not observed; invalid trial'
    if any(s.get('health') != 'producing' for s in latest_sources if s.get('kind') == 'zoom' and s.get('hasVideo')):
        failure = failure or 'one or more video sources were not producing at the final sample; invalid trial'
    report = dict(manifest, samples=sample_count, actualSeconds=time.monotonic()-started,
        error=failure, latestDisplay=latest,
        initial=initial, incidentArtifacts=incident_files, incidentBytes=incident_bytes,
        incidentFileLimit=512,incidentByteLimit=256*1024*1024,
        latestSources=latest_sources,
        expectedProgramSources=sorted(expected_program_sources), ingredientSamples=ingredient_samples,
        ingredientErrors=sorted(ingredient_errors),
        observation='UNVERIFIED' if not latest.get('observed') else 'ATTRIBUTION_ONLY',
        presentationVerdict='UNVERIFIED', allOutputZeroMissVerdict='UNVERIFIED')
    if args.camera_receiver:
        report['cameraReceiverExitCode'] = receiver.returncode if receiver else None
        report['cameraReceiverSha256'] = hashlib.sha256(args.camera_receiver.read_bytes()).hexdigest()
        report['servingIdentity'] = serving_result
        if receiver and receiver.returncode == 0:
            code = 'import {readFileSync} from "node:fs"; const {judgeCameraPixels}=await import(process.argv[1]); console.log(JSON.stringify(judgeCameraPixels(readFileSync(process.argv[2],"utf8"),{warmupSeconds:0,minimumSeconds:30})));'
            judged = subprocess.run([node,'--input-type=module','-e',code,
                pathlib.Path(__file__).with_name('camera-pixel-receiver.mjs').resolve().as_uri(),
                str((args.output/'receiver.ndjson').resolve())],capture_output=True,text=True,timeout=30,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            if judged.returncode == 0:
                report['cameraPixels'] = json.loads(judged.stdout)
                (args.output/'camera-pixels.json').write_text(json.dumps(report['cameraPixels'],indent=2))
            else: report['cameraJudgeError'] = 'Node pixel judge failed'
    if before_demand:
        report['beforeDemandCounters'] = dict(programBuffer=before_demand.get('programBuffer'),
            monitor={k:before_demand.get('realtimeEvidence',{}).get('monitorWorker',{}).get(k)
                for k in ('submitted','completed','failed','superseded')})
    report['monitorMaximumCounters'] = monitor_max
    report['monitorWorkVerdict'] = 'FAIL' if any(monitor_max.values()) else 'PASS'
    required_reader_metrics = ('copies','completedCopies','gpuDeadlineCompletedReads','gpuReadinessDeadlineMisses',
        'refused','skipped','reordered','reused','lateSubmissions','traceCaptureFailures')
    report['workloadVerdict'] = 'INVALID' if failure else ('FAIL' if ingredient_errors else ('PASS' if ingredient_samples else 'UNVERIFIED'))
    report['readerCounterScope'] = 'Reader identities and observed GPU readiness only; physical display, audio and camera receiver unverified'
    if latest.get('copies',0) > 0 and all(k in latest for k in required_reader_metrics):
        zero = all(latest[k] == 0 for k in ('refused','skipped','reordered','reused','lateSubmissions',
            'gpuReadinessDeadlineMisses','traceCaptureFailures'))
        coverage = latest['gpuDeadlineCompletedReads'] == latest['completedCopies']
        report['readerCounterVerdict'] = ('PASS' if report['workloadVerdict'] == 'PASS' else 'UNVERIFIED') if zero and coverage else 'FAIL'
    else: report['readerCounterVerdict'] = 'UNVERIFIED'
    if report['readerCounterVerdict'] == 'FAIL' or report['workloadVerdict'] == 'FAIL' or report['monitorWorkVerdict'] == 'FAIL':
        report['allOutputZeroMissVerdict'] = 'FAIL'
    if report.get('cameraPixels',{}).get('osCameraContinuityVerified') is False:
        report['allOutputZeroMissVerdict'] = 'FAIL'
    (args.output/'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k:v for k,v in report.items() if k!='latestDisplay'},indent=2))
    if failure: raise SystemExit(1)


if __name__ == '__main__': main()
