// Mutates the current show: run only in an authorized test meeting.
const fs=require('node:fs');
const path=require('node:path');
const output=path.resolve('artifacts/live-operator-qa');
fs.mkdirSync(output,{recursive:true});
const base=process.env.COREVIDEO_TEST_API_URL||'http://127.0.0.1:8011', sleep=ms=>new Promise(r=>setTimeout(r,ms));
const rows=[];const startTime=new Date().toISOString();
async function state(){return (await fetch(base+'/state',{signal:AbortSignal.timeout(10000)})).json();}
async function invoke(action,args=[],expectOk=true){const r=await fetch(base+'/invoke',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({action,args}),signal:AbortSignal.timeout(40000)});const x=await r.json();if(expectOk&&!x.ok)throw Error(action+': '+x.error);return x;}
function check(ok,msg){if(!ok)throw Error(msg);}
async function observed(predicate,label){for(let j=0;j<40;j++){const s=await state();if(predicate(s))return s;await sleep(100);}throw Error('Native confirmation timed out: '+label);}
(async()=>{
 let initial=await state();if(initial.automationOn)await invoke('automation.toggle');
 await invoke('scene.select',[initial.activeSceneId]);const disabled=await invoke('transport.take',[],false);check(!disabled.ok,'disabled Take falsely returned success');
 const frameStart=(await state()).nativeProgramFrameCount;
 for(let i=0;i<90;i++){
  const before=await state(),target=['interview','panel','speaker-slides'][i%3];
  check(before.nativeActiveSceneId===before.activeSceneId,'Program drifted between actions');
  await invoke('scene.select',[target]);check((await state()).activeSceneId===before.activeSceneId,'cue changed Program');
  if(before.activeSceneId!==target){await invoke('transport.take');await observed(s=>s.nativeActiveSceneId===target&&s.nativePreviewSceneId===before.activeSceneId,'Take '+target);}
  if(i%10===0){await invoke('graphics.lowerThird.set',[true]);await observed(s=>s.nativeLowerThirdVisible===true&&s.nativeLowerThirdPhase==='on-air','lower third on');await invoke('graphics.lowerThird.set',[false]);await observed(s=>s.nativeLowerThirdVisible===false,'lower third off');}
  if(i%15===0){const pgm=(await state()).activeSceneId;await invoke('automation.magic');check((await state()).activeSceneId===pgm,'Magic changed Program');await invoke('automation.toggle');await sleep(1700);await invoke('scene.select',['interview']);check(!(await state()).automationOn,'manual override lost');}
  const s=await state();check(s.activeSceneId&&s.previewSceneId,'null scene');check(s.engineOn,'capture stopped');rows.push({cycle:i+1,program:s.activeSceneId,nativeProgram:s.nativeActiveSceneId,nativePreview:s.nativePreviewSceneId,frames:s.nativeProgramFrameCount});
  if(i%10===0)console.log(JSON.stringify(rows.at(-1)));await sleep(1700);
 }
 const end=await state();check(end.nativeProgramFrameCount>frameStart,'native frames stopped');
 fs.writeFileSync(path.join(output,'native-soak-results.json'),JSON.stringify({ok:true,startTime,endTime:new Date().toISOString(),cycles:rows.length,frameStart,frameEnd:end.nativeProgramFrameCount,rows},null,2));console.log('PASS '+rows.length+' native-confirmed cycles');
})().catch(e=>{fs.writeFileSync(path.join(output,'native-soak-results.json'),JSON.stringify({ok:false,startTime,error:e.message,rows},null,2));console.error(e);process.exitCode=1;});
