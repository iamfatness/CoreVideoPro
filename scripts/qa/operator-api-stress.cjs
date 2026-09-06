// Mutates the current show: run only in an authorized test meeting.
const fs=require('node:fs');
const path=require('node:path');
const output=path.resolve(process.env.COREVIDEO_TEST_OUTPUT_DIR||'artifacts/live-operator-qa');
fs.mkdirSync(output,{recursive:true});
const base=process.env.COREVIDEO_TEST_API_URL||'http://127.0.0.1:8011', sleep=ms=>new Promise(r=>setTimeout(r,ms));
const rows=[];const startTime=new Date().toISOString();
const minMinutes=Number(process.env.COREVIDEO_TEST_MINUTES||0), cycles=Number(process.env.COREVIDEO_TEST_CYCLES||90);
if(!Number.isFinite(minMinutes)||minMinutes<0||!Number.isInteger(cycles)||cycles<1)throw Error('Invalid soak duration/cycles');
let identityObservations=0;
async function state(){
 const response=await fetch(base+'/state',{signal:AbortSignal.timeout(10000)});
 check(response.ok,'State HTTP '+response.status);const s=await response.json();
 if(s.nativeLowerThirdVisible){
  check(s.nativeLowerThirdSourceId&&s.nativeProgramVideoSources?.some(source=>source.sourceId===s.nativeLowerThirdSourceId),'Visible lower third is not bound to a rendered Program source: '+JSON.stringify({scene:s.nativeRenderedSceneId,source:s.nativeLowerThirdSourceId,sources:s.nativeProgramVideoSources}));
  identityObservations++;
 }
 return s;
}
async function invoke(action,args=[],expectOk=true){const r=await fetch(base+'/invoke',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({action,args}),signal:AbortSignal.timeout(40000)});const x=await r.json();if(expectOk&&!x.ok)throw Error(action+': '+x.error);return x;}
function check(ok,msg){if(!ok)throw Error(msg);}
async function observed(predicate,label){for(let j=0;j<40;j++){const s=await state();if(predicate(s))return s;await sleep(100);}throw Error('Native confirmation timed out: '+label);}
(async()=>{
 let initial=await state();if(initial.automationOn)await invoke('automation.toggle');
 check(initial.engineOn&&initial.zoomStatus==='Zoom Live','Requires capture and a live Zoom meeting');
 check(Array.isArray(initial.nativeProgramVideoSources),'Requires actual rendered-source diagnostics');
 await invoke('scene.select',[initial.activeSceneId]);const disabled=await invoke('transport.take',[],false);check(!disabled.ok,'disabled Take falsely returned success');
 const frameStart=(await state()).nativeProgramFrameCount;
 for(let i=0;i<cycles||Date.now()-Date.parse(startTime)<minMinutes*60000;i++){
  const before=await state(),target=['interview','panel','speaker-slides'][i%3];
  check(before.nativeActiveSceneId===before.activeSceneId,'Program drifted between actions');
  check(before.engineOn&&before.zoomStatus==='Zoom Live','Capture or meeting stopped');
  await invoke('scene.select',[target]);check((await state()).activeSceneId===before.activeSceneId,'cue changed Program');
  if(before.activeSceneId!==target){await invoke('transport.take');await observed(s=>s.nativeActiveSceneId===target&&s.nativeRenderedSceneId===target&&s.nativeProgramFrameCount>before.nativeProgramFrameCount&&s.nativePreviewSceneId===before.activeSceneId,'Take '+target);}
  if(i%10===0){await invoke('graphics.lowerThird.set',[true]);await observed(s=>s.nativeLowerThirdVisible===true&&s.nativeLowerThirdPhase==='on-air','lower third on');}
  // Carry a visible key across subsequent Takes to exercise source replacement.
  if(i%10===2){await invoke('graphics.lowerThird.set',[false]);await observed(s=>s.nativeLowerThirdVisible===false,'lower third off');}
  if(i%15===0){const pgm=(await state()).activeSceneId;await invoke('automation.magic');check((await state()).activeSceneId===pgm,'Magic changed Program');await invoke('automation.toggle');await sleep(1700);await invoke('scene.select',['interview']);check(!(await state()).automationOn,'manual override lost');}
  const s=await state();check(s.activeSceneId&&s.previewSceneId,'null scene');check(s.engineOn,'capture stopped');check(s.nativeProgramFrameCount>before.nativeProgramFrameCount,'Native frames stopped during cycle');rows.push({cycle:i+1,time:new Date().toISOString(),program:s.activeSceneId,nativeProgram:s.nativeActiveSceneId,nativePreview:s.nativePreviewSceneId,frames:s.nativeProgramFrameCount});
  if(i%10===0)console.log(JSON.stringify(rows.at(-1)));await sleep(1700);
 }
 await invoke('graphics.lowerThird.set',[false]);
 const end=await state();check(end.nativeProgramFrameCount>frameStart,'native frames stopped');
 check(identityObservations>0,'No visible lower-third identity observations');
 fs.writeFileSync(path.join(output,'native-soak-results.json'),JSON.stringify({ok:true,startTime,endTime:new Date().toISOString(),cycles:rows.length,identityObservations,frameStart,frameEnd:end.nativeProgramFrameCount,rows},null,2));console.log('PASS '+rows.length+' native-confirmed cycles');
})().catch(e=>{fs.writeFileSync(path.join(output,'native-soak-results.json'),JSON.stringify({ok:false,startTime,endTime:new Date().toISOString(),error:e.message,identityObservations,rows},null,2));console.error(e);process.exitCode=1;});
