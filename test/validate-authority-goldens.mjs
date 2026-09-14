// Fixture consistency only; runtime adapters must produce their own decisions.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
const str=x=>typeof x==='string'&&x.length>0;
const rev=x=>Number.isSafeInteger(x)&&x>=0;
const unique=a=>Array.isArray(a)&&new Set(a).size===a.length;
const key=s=>({sourceId:s.sourceId,instanceId:s.instanceId,processEpoch:s.processEpoch,generation:s.generation});
const same=(a,b)=>['sourceId','instanceId','processEpoch','generation'].every(k=>a[k]===b[k]);
function validKey(k){assert.ok(k&&['sourceId','instanceId','processEpoch'].every(f=>str(k[f]))&&rev(k.generation)&&k.generation>0);}
function select(state,selector,medium){
 assert.ok(selector&&['person','pinned','input','blank','muted','name'].includes(selector.kind));
 if(['blank','muted'].includes(selector.kind))return {source:null,reason:selector.kind};
 if(selector.kind==='name')return {source:null,reason:(state.people??[]).filter(p=>p.name===selector.name).length>1?'ambiguous':'identity-required'};
 if(selector.kind==='pinned')validKey(selector.token);
 const person=selector.kind==='input'?state.inputs?.find(i=>i.id===selector.inputId)?.personId:selector.personId;
 const matches=state.sources.filter(s=>selector.kind==='pinned'?same(s,selector.token):s.personId===person);
 if(matches.length!==1)return {source:null,reason:matches.length?'ambiguous':'missing'};
 const s=matches[0];
 if(medium==='audio'&&s.audioMuted)return {source:null,reason:'muted'};
 if(!s[`${medium}Available`])return {source:null,reason:'unavailable'};
 if(!s[`${medium}Fresh`])return {source:null,reason:'stale'};
 return {source:key(s),reason:'resolved'};
}
function takes(s){
 const state=structuredClone(s.state),results=[];
 assert.ok(str(state.authorityEpoch)&&rev(state.revision)&&rev(state.previewRevision));
 assert.ok(unique(state.ledger.map(e=>e.operation.id))&&unique(state.expiredOperationIds));
 assert.ok(rev(state.operationCapacity)&&state.operationCapacity>0&&state.ledger.length+state.expiredOperationIds.length<=state.operationCapacity);
 for(const old of state.ledger){
  const op=old.operation;
  assert.ok(str(op.id)&&str(op.authorityEpoch)&&rev(op.expectedRevision)&&rev(op.previewRevision),'Invalid historical fingerprint');
  assert.ok(!state.expiredOperationIds.includes(op.id));
  assert.equal(op.authorityEpoch,state.authorityEpoch);
  assert.ok(rev(old.resultRevision)&&old.resultRevision<=state.revision);
  assert.ok(op.expectedRevision<Number.MAX_SAFE_INTEGER,'Historical revision exhausted');
  assert.equal(old.resultRevision,op.expectedRevision+1,'Historical result must increment once');
  assert.equal(old.promotedPreviewRevision,op.previewRevision,'Historical promotion must match requested Preview');
 }
 for(const op of s.operations){
  assert.ok(str(op.id)&&str(op.authorityEpoch)&&rev(op.expectedRevision)&&rev(op.previewRevision));
  const old=state.ledger.find(e=>e.operation.id===op.id);
  let error=null,promoted=null,resultRevision=state.revision,promotions=0;
  if(op.authorityEpoch!==state.authorityEpoch)error='authority-epoch';
  else if(state.expiredOperationIds.includes(op.id))error='operation-expired';
  else if(old){if(old.operation.expectedRevision!==op.expectedRevision||old.operation.previewRevision!==op.previewRevision)error='operation-id-conflict';else{resultRevision=old.resultRevision;promoted=old.promotedPreviewRevision;}}
  else if(state.ledger.length+state.expiredOperationIds.length>=state.operationCapacity)error='operation-capacity';
  else if(op.expectedRevision!==state.revision)error='stale-revision';
  else if(op.previewRevision!==state.previewRevision)error='stale-preview';
  else if(state.revision===Number.MAX_SAFE_INTEGER)error='revision-exhausted';
  else{promoted=state.previewRevision;resultRevision=++state.revision;promotions=1;state.ledger.push({operation:op,resultRevision,promotedPreviewRevision:promoted});}
  results.push({accepted:error===null,error,resultRevision,promotedPreviewRevision:promoted,additionalPromotions:promotions});
 }
 return {results,totalPromotions:results.reduce((n,r)=>n+r.additionalPromotions,0)};
}
export function validateAuthorityGoldens(f){
 assert.equal(f.version,'authority-goldens-v2');assert.equal(f.ledgerPolicy,'retain-rejection-tombstone-until-authority-epoch-ends');
 assert.ok(Array.isArray(f.scenarios)&&f.scenarios.length&&unique(f.scenarios.map(s=>s.id)));
 for(const s of f.scenarios)try{
  assert.ok(str(s.id));const state=s.state;
  if(s.kind!=='take'){
   assert.ok(Array.isArray(state.sources)&&unique(state.sources.map(x=>x.sourceId))&&unique(state.sources.map(x=>x.instanceId)));
   for(const source of state.sources){validKey(source);assert.ok(str(source.personId));for(const field of ['videoAvailable','videoFresh','audioAvailable','audioFresh','audioMuted'])assert.equal(typeof source[field],'boolean');}
   if(state.inputs)assert.ok(unique(state.inputs.map(x=>x.id)));
   for(const old of state.retiredSources??[]){validKey(old);assert.ok(!state.sources.some(x=>same(x,old)));}
  }
  let expected;
  if(s.kind==='route'){const v=select(state,s.request.video,'video'),a=select(state,s.request.audio,'audio');expected={video:v.source,audio:a.source,videoReason:v.reason,audioReason:a.reason};}
  else if(s.kind==='tiles'){
   assert.ok(unique(s.request.excluded));
   const resolve=selector=>{const r=select(state,selector,'video'),source=r.source&&state.sources.find(x=>same(x,r.source)),person=selector.kind==='person'?selector.personId:source?.personId;return s.request.excluded.includes(person)?{source:null,reason:'excluded'}:r;};
   if(s.request.mode==='manual'){assert.ok(unique(s.request.slots.map(x=>x.id)));expected={slots:s.request.slots.map(slot=>({id:slot.id,...resolve(slot.selector)}))};}
   else{assert.equal(s.request.mode,'automatic');assert.equal(typeof s.request.allowRosterAdditions,'boolean');const ordered=state.inputs.map(x=>x.personId);if(s.request.allowRosterAdditions)ordered.push(...state.roster);expected={members:[...new Set(ordered)].filter(personId=>resolve({kind:'person',personId}).source)};}
  }else if(s.kind==='iso'){
   assert.ok(unique(s.request.selections.map(x=>x.id)));const writers=[],waiting=[];
   for(const x of s.request.selections){const video=select(state,x.selector,'video').source,audio=select(state,x.selector,'audio').source;if(video||audio)writers.push({id:x.id,video,audio});else waiting.push(x.id);}
   expected={armed:s.request.selections.map(x=>x.id),writers,waiting};
  }else if(s.kind==='take')expected=takes(s);else assert.fail(`Unknown kind ${s.kind}`);
  assert.deepEqual(s.expected,expected,'Expected authority contradicts scenario facts');
 }catch(error){throw new Error(`${s.id}: ${error.message}`,{cause:error});}
 return {valid:true,scenarios:f.scenarios.length};
}
if(process.argv[1]&&import.meta.url===pathToFileURL(resolve(process.argv[1])).href){try{console.log(JSON.stringify(validateAuthorityGoldens(JSON.parse(readFileSync(process.argv[2]??new URL('./data/wave1-authority.json',import.meta.url),'utf8')))));}catch(error){console.error(error.message);process.exitCode=1;}}
