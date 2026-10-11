/** Real Meeting SDK + cold 1080p background admission; zero-loss boundary judge.
 * Meeting URL is read only from COREVIDEO_TEST_MEETING_URL, never printed.
 * Runs a supplied Release core and the real SDK helper through the native wire.
 */
import {spawn} from 'node:child_process';
import {readFileSync,writeFileSync,mkdirSync,createWriteStream,existsSync} from 'node:fs';
import {resolve,dirname,join} from 'node:path';
import {createHash} from 'node:crypto';
import {parseArgs} from 'node:util';
import {finished} from 'node:stream/promises';
import {judgeCameraPixels} from './camera-pixel-receiver.mjs';
const {values:v}=parseArgs({options:{core:{type:'string'},engine:{type:'string'},background:{type:'string'},out:{type:'string'},seconds:{type:'string',default:'1800'},loads:{type:'string',default:'20'},sources:{type:'string',default:'8'},receiver:{type:'string'}}});
for(const key of ['core','engine','background','out']) if(!v[key]) throw Error(`Missing --${key}`);
const core=resolve(v.core),engine=resolve(v.engine),background=resolve(v.background),out=resolve(v.out);
for(const f of [core,engine,background]) if(!existsSync(f)) throw Error('Required input file missing');
if(v.receiver&&!existsSync(resolve(v.receiver)))throw Error('Independent camera receiver file missing');
const seconds=Number(v.seconds),loads=Number(v.loads),wanted=Number(v.sources);
if(!Number.isFinite(seconds)||seconds<30||seconds>7200||!Number.isInteger(loads)||loads<1||loads>100||!Number.isInteger(wanted)||wanted<1||wanted>16) throw Error('Invalid duration/load/source bounds');
if(!process.env.COREVIDEO_TEST_MEETING_URL) throw Error('Protected meeting URL must be supplied in environment');
mkdirSync(out,{recursive:true});
const report={complete:false,passed:false,coreSha256:createHash('sha256').update(readFileSync(core)).digest('hex'),seconds,loads,wanted,samples:0,changes:[],failures:[],scope:'Real SDK ingestion, production defaults, cold media load delivery counters; independent OS-camera pixels when receiver supplied. Display presentation and A/V remain unverified'};
const child=spawn(core,[],{cwd:dirname(core),windowsHide:true,stdio:['pipe','pipe','pipe'],env:{...process.env,COREVIDEO_ZOOM_ENGINE_PATH:engine,COREVIDEO_PROGRAM_BUFFER_FRAMES:"2"}});
const log=createWriteStream(join(out,'core-private.log'));child.stderr.pipe(log);
let id=0,buffer='',handshake,exit=null;const pending=new Map();
child.on('exit',(code,signal)=>{exit={code,signal};for(const p of pending.values()){clearTimeout(p.timer);p.reject(Error('Core exited'));}pending.clear();});
child.stdin.on('error',()=>{});
child.stdout.on('data',data=>{buffer+=data;let i;while((i=buffer.indexOf('\n'))>=0){const line=buffer.slice(0,i);buffer=buffer.slice(i+1);let m;try{m=JSON.parse(line);}catch{continue;}if(m.type==='handshake'||m.profile){handshake??=m;continue;}const p=pending.get(m.id);if(p){clearTimeout(p.timer);pending.delete(m.id);p.resolve(m);}}});
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const send=(type,extra={},timeout=15000)=>new Promise((resolve,reject)=>{const n=++id,timer=setTimeout(()=>{pending.delete(n);reject(Error(`${type} timed out`));},timeout);pending.set(n,{resolve,reject,timer});child.stdin.write(JSON.stringify({id:n,type,...extra})+'\n');});
const harnessStarted=Date.now();
const sync=async(commands=[])=>{const r=await send('media-core-sync',{elapsedMs:Date.now()-harnessStarted,commands});if(r.ok===false)throw Error('Native sync rejected');return r.snapshot;};
const check=(b,m)=>{if(!b)throw Error(m);};
const counterKeys=['underruns','overflows','deadlineMisses','gpuNotReady','outputSequenceGaps'];
let receiver=null,receiverExit=null,receiverOutputDone=null,last=null,before=null,spine=null;const admittedLoads=new Set();
const persist=()=>writeFileSync(join(out,'verdict.json'),JSON.stringify(report,null,2));
persist();
try{
 for(let i=0;i<100&&!handshake;i++)await sleep(100);check(handshake,'No native handshake');
 await sync([{type:'set-verbose-diagnostics',enabled:true}]);
 const result=await send('zoom-join',{payload:{meetingUrl:process.env.COREVIDEO_TEST_MEETING_URL,displayName:'CoreVideo SDK Stability Validation'}},120000);
 check(result.ok!==false,'SDK join rejected');
 check(result.snapshot?.meetingState!=='error','SDK join failed before media qualification');
 let roster=[];const until=Date.now()+90000;
 while(Date.now()<until){last=await sync();writeFileSync(join(out,'join-private.json'),JSON.stringify(last,null,2));roster=(last.zoom?.participants??last.participants??[]).filter(p=>!p.isMe&&(p.hasVideo??p.videoOn??false));if(roster.length>=wanted)break;await sleep(1000);}
 check(roster.length>=wanted,'Insufficient real camera-on participants');
 const selected=roster.slice(0,wanted),pid=p=>String(p.sdkUserId??p.userId??p.participantId??p.id);
 const ids=selected.map(pid);
 const subscription={readiness:{status:'ready',platform:'windows',sdkVersion:'zoom-engine',checks:[],blockers:[],warnings:[]},participants:selected.map(p=>({sdkUserId:pid(p),displayName:p.displayName??'',role:'guest',videoOn:true,muted:!!p.muted,talking:!!p.talking,audioLevel:0,networkQuality:'good'})),subscriptions:[{participantId:ids[0],kind:'meeting-audio',purpose:'program',priority:0},...ids.map((participantId,i)=>({participantId,kind:'participant-video',purpose:'program',priority:10+i}))],startCapture:true,blocked:false,warnings:[],summary:'Cold background admission test'};
 const rawStarted=Date.now(),rawUntil=rawStarted+300000;let raw=0,nextCaptureRetry=rawStarted+75000;
 while(Date.now()<rawUntil){const r=await send('zoom-media-spine-sync',{spinePayload:subscription,elapsedMs:300000-(rawUntil-Date.now())});spine=r.spineSnapshot;writeFileSync(join(out,'spine-start-private.json'),JSON.stringify(spine,null,2));raw=(spine?.subscriptions??[]).filter(s=>s.kind==='participant-video').reduce((n,s)=>n+Number(s.framesReceived??0),0);if((spine?.subscriptions??[]).filter(s=>s.kind==='participant-video'&&s.framesReceived>0).length===wanted)break;if(Date.now()>=nextCaptureRetry){await send("zoom-stop-capture");report.captureRetries=(report.captureRetries??0)+1;nextCaptureRetry=Date.now()+75000;}await sleep(500);}
 check((spine?.subscriptions??[]).filter(s=>s.kind==='participant-video'&&s.framesReceived>0).length===wanted,'SDK raw video unavailable for all requested sources');
 const sources=ids.map((participantId,slot)=>({sourceId:`zoom:${participantId}`,kind:'zoom',participantId,slot,label:`CAM ${slot+1}`}));
 const scene=(type,withBackground,load)=>({type,sceneId:'admission-wall',routes:[],tiles:{layerId:'tiles:admission-wall',members:sources.map(s=>s.sourceId),style:{fillMode:'auto',backgroundColor:'#101418'}},...(withBackground?{background:{mediaAssetId:`cold-background-${load}`,mediaAssetName:'cold background',mediaAssetKind:'video',mediaAssetPath:background,playing:true}}:{})});
 await sync([{type:'configure-multiviewer',layoutMode:'pgmPvwTop',tileCount:10},{type:'set-multiview-layout',canvasWidth:1920,canvasHeight:1080,sources},scene('load-scene-graph',false,0),scene('set-preview-scene',false,0),{type:'sync-virtual-camera',on:true,mirror:false,deviceName:'CoreVideo Pro Camera'}]);
 await sleep(15000);before=last=await sync();
 check(before.programBuffer?.activeFrames===2,'Expected existing two-frame Program buffer');
 check(before.realtimeEvidence?.monitorWorker?.effectiveMode==='isolated','Production monitor isolation not active');
 check(before.programSourceAdmission?.readyOnlyRequested===true, 'Ready-only source preparation evidence missing');
 check(before.virtualCamera?.enabled===true && before.virtualCamera.framesPublished>0,'Registered webcam did not start publishing');
 writeFileSync(join(out,'camera-start-private.json'),JSON.stringify(before,null,2));
 if(v.receiver){const stdout=createWriteStream(join(out,'receiver.ndjson')),stderr=createWriteStream(join(out,'receiver.stderr'));receiverOutputDone=finished(stdout);receiverOutputDone.catch(()=>{});receiver=spawn(resolve(v.receiver),[String(Math.ceil(seconds+35))],{windowsHide:true,stdio:['ignore','pipe','pipe']});receiver.stdout.pipe(stdout);receiver.stderr.pipe(stderr);receiver.on('exit',code=>{receiverExit=code;});}
 if(receiver){for(let i=0;i<60;i++){await sleep(500);last=await sync();}before=last;}
 writeFileSync(join(out,'start-private.json'),JSON.stringify(before,null,2));
 const start=Date.now(),end=start+seconds*1000;let nextLoad=0,minute=-1;
 const samples=createWriteStream(join(out,'samples.ndjson'));
 while(Date.now()<end){
  const elapsed=(Date.now()-start)/1000;let commands=[];
  if(nextLoad<loads&&elapsed>=nextLoad*seconds/loads){commands=[scene('load-scene-graph',true,nextLoad)];report.changes.push({load:nextLoad,seconds:elapsed});nextLoad++;}
  last=await sync(commands);report.samples++;
  check(!exit&&last.zoom?.evidence?.joined===true,'SDK/core continuity lost');
  const row={utc:new Date().toISOString(),seconds:elapsed,programFrames:last.programFrameCount,programBuffer:last.programBuffer,compositor:last.compositor,virtualCamera:last.virtualCamera,programSourceAdmission:last.programSourceAdmission,systemMemory:last.systemMemory,monitor:last.realtimeEvidence?.monitorWorker};
  for(const source of last.programSourceAdmission?.sources??[]) {
   const match=/^background:cold-background-(\d+)$/.exec(source.sourceId);
   if(match && source.actualFrameId!==null && source.actualFrameId>=0) admittedLoads.add(Number(match[1]));
  }
  samples.write(JSON.stringify(row)+'\n');
  for(const key of counterKeys)if(last.programBuffer?.[key]>before.programBuffer?.[key]&&!report.failures.includes(`programBuffer.${key}`))report.failures.push(`programBuffer.${key}`);
  if(last.virtualCamera?.publicationExceptions>before.virtualCamera?.publicationExceptions&&!report.failures.includes('virtualCamera.publicationExceptions'))report.failures.push('virtualCamera.publicationExceptions');
  const m=Math.floor(elapsed/60);if(m!==minute){minute=m;console.log(JSON.stringify({seconds:Math.round(elapsed),loads:nextLoad,joined:last.zoom.evidence.joined,programFrames:last.programFrameCount,losses:Object.fromEntries(counterKeys.map(k=>[k,last.programBuffer[k]-before.programBuffer[k]])),memory:last.systemMemory}));persist();}
  await sleep(500);
 }
 samples.end();
 report.complete=true;report.actualSeconds=(Date.now()-start)/1000;
 report.counterDeltas=Object.fromEntries(counterKeys.map(k=>[k,last.programBuffer[k]-before.programBuffer[k]]));
 report.compositorDeltas=Object.fromEntries(['renderDeadlineMisses','droppedFrameCount','degradedFrameCount'].map(k=>[k,last.compositor[k]-before.compositor[k]]));
 report.webcamDeltas=Object.fromEntries(['framesPublished','pendingFramesReplaced','publicationExceptions'].map(k=>[k,last.virtualCamera[k]-before.virtualCamera[k]]));
 report.programFrames=last.programFrameCount-before.programFrameCount;
 writeFileSync(join(out,'end-private.json'),JSON.stringify(last,null,2));
 const r=await send('zoom-media-spine-sync',{spinePayload:subscription,elapsedMs:500});writeFileSync(join(out,'spine-end-private.json'),JSON.stringify(r.spineSnapshot,null,2));
 report.admittedLoads=[...admittedLoads].sort((a,b)=>a-b);
 if(admittedLoads.size!==loads)report.failures.push('Not every cold background delivered an admitted GPU image');
 report.programDeliveryPassed=counterKeys.every(k=>report.counterDeltas[k]===0)&&admittedLoads.size===loads;
 // A receiver's successful process exit is not a pixel-continuity verdict.
 // Final acceptance is computed only after its capture has been drained/judged.
 persist();
}catch(e){report.failures.push(e.message.replace(/https?:\/\/\S+/g,'<redacted>'));persist();}
finally{
 if(receiver&&report.complete){for(let i=0;i<30&&receiverExit===null;i++)await sleep(500);}
 try{await sync([{type:'sync-virtual-camera',on:false}]);await send('zoom-leave',{},10000);await send('shutdown',{},5000);}catch{}
 child.stdin.end();setTimeout(()=>{if(exit===null)child.kill();},5000).unref();
 if(receiver){for(let i=0;i<30&&receiverExit===null;i++)await sleep(500);if(receiverExit===null)receiver.kill();report.receiverExit=receiverExit;if(receiverExit!==0){report.failures.push('Independent camera receiver failed');report.passed=false;}}
 if(receiver && receiverExit!==null){
  try{
   await receiverOutputDone;
   report.receiverVerdict=judgeCameraPixels(readFileSync(join(out,'receiver.ndjson'),'utf8'),{minimumSeconds:seconds});
   writeFileSync(join(out,'receiver-verdict.json'),JSON.stringify(report.receiverVerdict,null,2));
   if(!report.receiverVerdict.osCameraContinuityVerified)report.failures.push('Independent OS camera pixel continuity failed');
  }catch(e){report.failures.push(`Camera pixel judge failed: ${e.message}`);}
 }
 report.passed=report.complete&&report.failures.length===0;
 log.end();persist();console.log(JSON.stringify(report));
}
process.exitCode=report.passed?0:1;
