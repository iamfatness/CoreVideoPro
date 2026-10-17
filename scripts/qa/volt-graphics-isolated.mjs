/** Isolated Volt native QA. Never attaches to the installed app or OS camera.
 * A supplied, explicitly named synthetic helper is mandatory; no real meeting. */
import {spawn,execFileSync} from 'node:child_process';
import {resolve,dirname,basename,join} from 'node:path';
import {existsSync,mkdirSync,writeFileSync,readFileSync,createWriteStream} from 'node:fs';
import {createHash} from 'node:crypto';
import {parseArgs} from 'node:util';
const {values:v}=parseArgs({options:{core:{type:'string'},engine:{type:'string'},out:{type:'string'},seconds:{type:'string',default:'60'}}});
for(const name of ['core','engine','out']) if(!v[name]) throw Error(`Missing --${name}`);
const core=resolve(v.core),engine=resolve(v.engine),out=resolve(v.out),seconds=Number(v.seconds);
if(basename(engine).toLowerCase()!=='corevideo-zoom-engine-fake.exe' || !existsSync(engine) || !existsSync(core)) throw Error('Explicit synthetic helper and built core are required');
if(!Number.isFinite(seconds)||seconds<10||seconds>1800) throw Error('Duration must be 10–1800 seconds');
mkdirSync(out,{recursive:true});const report={complete:false,failures:[],scope:'Separate native process, synthetic 1080p60 input, native graphics admission/draw and Program buffer counters. No OS camera, physical display, receiver continuity or real Zoom acceptance.',coreSha256:createHash('sha256').update(readFileSync(core)).digest('hex'),seconds};
const persist=()=>writeFileSync(join(out,'evidence.json'),JSON.stringify(report,null,2));persist();
const child=spawn(core,[],{cwd:dirname(core),windowsHide:true,stdio:['pipe','pipe','pipe'],env:{...process.env,COREVIDEO_ZOOM_ENGINE_PATH:engine,COREVIDEO_FAKE_ENGINE_PARTICIPANTS:'8',COREVIDEO_FAKE_ENGINE_RES:'2',COREVIDEO_FAKE_ENGINE_FPS:'60',COREVIDEO_FAKE_NO_CHURN:'1',COREVIDEO_PROGRAM_BUFFER_FRAMES:'3'}});
const log=createWriteStream(join(out,'native.log'));child.stderr.pipe(log);
let next=0,buffer='',handshake,ended=false;const pending=new Map(),sleep=ms=>new Promise(r=>setTimeout(r,ms));
child.stdout.on('data',chunk=>{buffer+=chunk;let i;while((i=buffer.indexOf('\n'))>=0){let msg;try{msg=JSON.parse(buffer.slice(0,i));}catch{}buffer=buffer.slice(i+1);if(!msg)continue;if(msg.type==='handshake'||msg.profile){handshake??=msg;continue;}const p=pending.get(msg.id);if(p){clearTimeout(p.timer);pending.delete(msg.id);p.resolve(msg);}}});
child.on('exit',()=>{ended=true;for(const p of pending.values()){clearTimeout(p.timer);p.reject(Error('Private core exited'));}pending.clear();});child.stdin.on('error',()=>{});
const send=(type,body={})=>new Promise((resolve,reject)=>{const id=++next,timer=setTimeout(()=>{pending.delete(id);reject(Error(type+' timed out'));},20000);pending.set(id,{resolve,reject,timer});child.stdin.write(JSON.stringify({id,type,...body})+'\n');});
const started=Date.now();const sync=async(commands=[])=>{const r=await send('media-core-sync',{elapsedMs:Date.now()-started,commands});if(r.ok===false) throw Error('Native sync failed');return r.snapshot;};
const check=(value,reason)=>{if(!value)throw Error(reason);};let state;
const wait=async(predicate,reason,ms=15000)=>{const until=Date.now()+ms;do{state=await sync();if(predicate(state)) return state;await sleep(100);}while(Date.now()<until);throw Error(reason);};
const operation=async(type,instanceId,extra={})=>{
 const body={...(type==='graphics-automation-control'&&['arm','enable-automatic'].includes(extra.action)?{expectedAutomationRevision:state.graphics.automation.revision,expectedActivationRevision:state.graphics.automation.activationRevision,mode:state.graphics.automation.mode}:{}),schemaVersion:1,coreEpoch:state.graphics.coreEpoch,clientId:'volt-isolated-qa',requestId:'qa-'+(++next),...(instanceId?{instanceId,expectedRevision:state.graphics.instances.find(i=>i.instanceId===instanceId)?.revision??1}:{}),...extra};
 state=await sync([{type,graphics:body}]);const result=state.graphics.operations?.find(r=>r.requestId===body.requestId)??state.graphics.lastResult;check(result?.accepted,`${type}: ${result?.reason??'uncorrelated'}`);return result;
};
try{
 for(let i=0;i<100&&!handshake;i++)await sleep(100);check(handshake,'No handshake');
 await send('zoom-join',{payload:{meetingUrl:'https://zoom.us/j/00000000000?pwd=synthetic',displayName:'Volt isolated synthetic QA'}});
 await wait(s=>(s.zoom?.participants??s.participants??[]).filter(p=>p.hasVideo??p.videoOn).length===8,'Synthetic roster unavailable');
 const roster=(state.zoom?.participants??state.participants??[]).filter(p=>p.hasVideo??p.videoOn),first=roster[0],raw=String(first.sdkUserId??first.userId??first.participantId??first.id);
 report.syntheticParticipants=roster.length;report.subject=raw;
 const sources=roster.map((person,slot)=>{const participantId=String(person.sdkUserId??person.userId??person.participantId??person.id);return {sourceId:'zoom:'+participantId,kind:'zoom',participantId,slot,label:'CAM '+(slot+1)};});
 state=await sync([{type:'configure-multiviewer',layoutMode:'pgmPvwTop',tileCount:10},{type:'set-multiview-layout',canvasWidth:1920,canvasHeight:1080,sources}]);
 state=await sync([{type:'set-output-profile',width:1920,height:1080,fps:60},{type:'set-verbose-diagnostics',enabled:true},{type:'load-scene-graph',sceneId:'volt-isolated',routes:[{routeId:'featured',mode:'fixed',participantId:raw,audioRole:'mix'}]},{type:'set-preview-scene',sceneId:'volt-private',routes:[{routeId:'featured',mode:'fixed',participantId:raw,audioRole:'mix'}]}]);
 await wait(s=>s.graphics?.sources?.some(r=>r.videoOn),'Qualified synthetic graphics sources unavailable');
 await operation('graphics-instance-create','guest',{templateId:'compact-solid',templateVersion:1,scope:'global',fields:{name:'Draft placeholder',title:''}});
 await operation('graphics-instance-create','logo',{templateId:'corner-logo',templateVersion:1,scope:'global'});
 await operation('graphics-instance-trigger','logo',{action:'show'});
 await operation('graphics-instance-create','clock',{templateId:'clock-panel',templateVersion:1,scope:'global'});
 await wait(s=>s.graphics.instances.find(i=>i.instanceId==='clock')?.preparation!=='waiting-clock','Clock did not prepare');
 await operation('graphics-instance-trigger','clock',{action:'show'});
 await operation('graphics-automation-configure','',{expectedAutomationRevision:state.graphics.automation.revision,configuration:{rules:[{id:'intro',instanceId:'guest',sourceId:'zoom:'+raw,dwellMs:1500,durationMs:8000}],sourceTexts:{}}});
 await operation('graphics-automation-control','',{action:'enable-automatic',mode:'automatic'});
 await wait(s=>s.graphics.automation.trace.some(t=>t.instanceId==='guest'&&t.state==='draw-submitted'),'Automatic guest never reached a native draw',20000);
 report.automaticTrace=state.graphics.automation.trace;report.programInstances=state.graphics.instances.map(i=>({id:i.instanceId,revision:i.programRevision,firstDrawSubmittedFrame:i.firstDrawSubmittedFrame,visibility:i.visibility}));
 check(state.virtualCamera?.enabled!==true,'Private test unexpectedly enabled webcam');
 state=await sync([{type:'start-program-output',destinations:['recording'],isoParticipantIds:[]},
 {type:'set-recording-targets',targetFolder:out,filenamePrefix:'volt-isolated',format:'mp4',quality:'high',isoParticipantIds:[]},
 {type:'start-recording-session',sessionId:'volt-qa-'+Date.now(),targetFolder:out,filenamePrefix:'volt-isolated',format:'mp4',quality:'high',isoParticipantIds:[]}]);
 await wait(s=>s.recording?.lifecycle?.state==='producing','Isolated recording did not become producing');
 report.recordingSession=state.recording.lifecycle.sessionId;
 await sleep(3000);state=await sync();const before=state;
 check(before.outputProfile?.width===1920 && before.outputProfile?.height===1080 && before.outputProfile?.fps===60,'1080p60 output profile not confirmed');
 check(before.programBuffer?.status==='running' && before.programBuffer.delivered>0,'Program buffer did not start');
 const keys=['underruns','overflows','deadlineMisses','gpuNotReady','outputSequenceGaps'];
 const samples=[];const begin=Date.now();while(Date.now()-begin<seconds*1000){state=await sync();samples.push({seconds:(Date.now()-begin)/1000,programBuffer:state.programBuffer,compositor:state.compositor,graphics:state.graphics.resourceBudgetAtStatePublication});check(!ended,'Core exited');await sleep(500);}
 state=await sync();report.measuredSeconds=(Date.now()-begin)/1000;report.programBufferDeltas=Object.fromEntries(keys.map(k=>{check(Number.isFinite(state.programBuffer?.[k])&&Number.isFinite(before.programBuffer?.[k]),'Missing measured Program buffer counter '+k);return [k,state.programBuffer[k]-before.programBuffer[k]];}));
 report.renderDeltas=Object.fromEntries(['renderDeadlineMisses','droppedFrameCount','degradedFrameCount'].map(k=>[k,(state.compositor?.[k]??0)-(before.compositor?.[k]??0)]));
 report.programFrames=state.compositor.programFrameCount-before.compositor.programFrameCount;
 report.bufferDelivered=state.programBuffer.delivered-before.programBuffer.delivered;
 report.counterInterval={before:before.programBuffer,after:state.programBuffer};report.outputProfile=state.outputProfile;
 report.bufferMeasurementAvailable=!!state.programBuffer;report.resources=state.graphics.resourceBudgetAtStatePublication;report.outputProfile=state.outputProfile;
 writeFileSync(join(out,'samples.json'),JSON.stringify(samples,null,2));
 state=await sync([{type:'stop-recording-session',reason:'Bounded isolated Volt validation complete'}]);
 await wait(s=>s.recording?.lifecycle?.state==='completed','Recording finalization not confirmed',20000);
 check(state.recording.lifecycle.finalized && state.recording.lifecycle.sessionId===report.recordingSession,'Recording completion identity mismatch');
 const artifact=resolve(dirname(core),state.recording.artifactPath??'');check(artifact.toLowerCase().startsWith(out.toLowerCase()+'\\')||artifact.startsWith(out+'/'),'Recording escaped isolated directory');
 report.recording=state.recording;
 report.recordingProbe=JSON.parse(execFileSync('ffprobe',['-v','error','-count_frames','-show_entries','stream=codec_type,codec_name,width,height,r_frame_rate,avg_frame_rate,nb_read_frames,duration','-of','json',artifact],{windowsHide:true,encoding:'utf8',timeout:120000}));
 const video=report.recordingProbe.streams.find(s=>s.codec_type==='video');check(video?.width===1920&&video?.height===1080&&video?.r_frame_rate==='60/1'&&Number(video.nb_read_frames)>0,'Finalized recording is not decoded 1080p60');
 const decoded=JSON.parse(execFileSync('ffprobe',['-v','error','-select_streams','v:0','-show_entries','frame=best_effort_timestamp_time','-of','json',artifact],{windowsHide:true,encoding:'utf8',timeout:120000,maxBuffer:8*1024*1024}));
 const pts=decoded.frames.map(f=>Number(f.best_effort_timestamp_time));const intervals=pts.slice(1).map((value,index)=>value-pts[index]);
 check(intervals.length>0&&intervals.every(value=>Number.isFinite(value)&&value>0&&value<=1/60+.000002),'Decoded recording has non-advancing timestamps or a missed video slot');
 check(intervals.slice(0,-1).every(value=>Math.abs(value-1/60)<=.000002),'Decoded recording has an irregular interior video slot');
 report.recordingCadence={decodedFrames:pts.length,firstPts:pts[0],lastPts:pts.at(-1),maximumIntervalMs:Math.max(...intervals)*1000,minimumInteriorIntervalMs:Math.min(...intervals.slice(0,-1))*1000,finalIntervalMs:intervals.at(-1)*1000,finalPartialFrame:intervals.at(-1)<1/60-.000002};
 report.finalAutomationTrace=state.graphics.automation.trace;
 await operation('graphics-program-clear','',{channel:'all'});await sleep(500);state=await sync();check(state.graphics.instances.every(i=>i.programRevision===0&&!i.showPending),'Clear left managed graphics pending');
 report.clearInhibited=state.graphics.automation.inhibited;check(report.clearInhibited,'Clear did not inhibit rules');
 const oldEpoch=state.graphics.sources[0]?.identity?.processEpoch;
 await send('zoom-leave');await send('zoom-join',{payload:{meetingUrl:'https://zoom.us/j/00000000000?pwd=synthetic',displayName:'Volt isolated rejoin QA'}});
 await wait(s=>s.graphics.sources.some(r=>r.videoOn&&r.identity.processEpoch!==oldEpoch),'Synthetic reconnect did not qualify a new epoch');
 report.reconnectDisarmed=!state.graphics.automation.armed;check(report.reconnectDisarmed && state.graphics.instances.every(i=>i.programRevision===0&&!i.showPending),'Reconnect replayed a graphic');
 report.complete=true;report.passedFunctional=true;persist();console.log(JSON.stringify(report,null,2));
}catch(error){report.failures.push(error.message);persist();console.error(error.message);process.exitCode=1;}
finally{if(state)writeFileSync(join(out,'last-snapshot.json'),JSON.stringify(state,null,2));try{await send('zoom-leave');}catch{}child.stdin.end();for(let i=0;i<30&&!ended;i++)await sleep(100);if(!ended)child.kill();log.end();}
