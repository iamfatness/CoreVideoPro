// Mutates the current show: run only in an authorized test meeting.
const fs=require('node:fs'),path=require('node:path'), sleep=ms=>new Promise(r=>setTimeout(r,ms)), base=process.env.COREVIDEO_TEST_API_URL||'http://127.0.0.1:8011';
const output=path.resolve(process.env.COREVIDEO_TEST_OUTPUT_DIR||'artifacts/live-operator-qa');fs.mkdirSync(output,{recursive:true});
async function state(){return (await fetch(base+'/state')).json();}
async function invoke(action,args=[]){const r=await fetch(base+'/invoke',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({action,args}),signal:AbortSignal.timeout(40000)});const x=await r.json();if(!x.ok)throw Error(action+': '+x.error);}
function check(ok,msg){if(!ok)throw Error(msg);}
(async()=>{
 if((await state()).automationOn)await invoke('automation.toggle');
 await invoke('automation.magic');let target=(await state()).previewSceneId;
 const alternate=target==='interview'?'speaker-slides':'interview';
 await invoke('scene.select',[alternate]);if((await state()).canTake)await invoke('transport.take');
 await invoke('automation.magic');target=(await state()).previewSceneId;
 const before=await state();check(before.activeSceneId!==target,'Need distinct auto target');
 await invoke('automation.autoTake.set',[true]);await invoke('automation.toggle');
 let after;for(let i=0;i<32;i++){await sleep(500);after=await state();if(after.activeSceneId===target&&after.nativeActiveSceneId===target)break;}
 check(after.activeSceneId===target&&after.nativeActiveSceneId===target,'Automatic Take did not reach native Program');
 await invoke('graphics.lowerThird.set',[false]);await sleep(2500);const lower=await state();check(lower.automationOn&&!lower.nativeLowerThirdVisible,'Automation overrode manual lower-third off');
 await invoke('scene.select',[alternate]);check(!(await state()).automationOn,'Manual cue failed to pause automation');
 await invoke('transport.take');await sleep(5000);const end=await state();check(end.activeSceneId===alternate&&end.nativeActiveSceneId===alternate&&!end.automationOn,'Manual Take was overridden');
 fs.writeFileSync(path.join(output,'automatic-take-results.json'),JSON.stringify({ok:true,target,manualScene:alternate,nativeProgram:end.nativeActiveSceneId,automation:end.automationOn},null,2));console.log('PASS automatic native Take, manual lower-third off, manual Take holds');
})().catch(e=>{fs.writeFileSync(path.join(output,'automatic-take-results.json'),JSON.stringify({ok:false,error:e.message},null,2));console.error(e);process.exitCode=1;});
