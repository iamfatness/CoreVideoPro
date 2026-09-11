import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {validateAuthorityGoldens} from './validate-authority-goldens.mjs';
const load=()=>JSON.parse(readFileSync(new URL('./data/wave1-authority.json',import.meta.url),'utf8'));
test('35 authority scenarios are internally consistent',()=>assert.equal(validateAuthorityGoldens(load()).scenarios,35));
const mutations=[
 ['duplicate-names-are-not-identity',s=>{s.expected.video=s.state.sources[0];}],
 ['person-following-rejoin-adopts-current-token',s=>{s.expected.video.instanceId='alice-session-1';}],
 ['pinned-rejoin-remains-missing',s=>{s.expected.video=s.request.video.token;}],
 ['slot-replacement-is-authoritative',s=>{s.expected.audio.sourceId='camera-alice';}],
 ['camera-off-keeps-fresh-audio',s=>{s.expected.audio=null;}],
 ['camera-on-before-new-video-is-not-ready',s=>{s.expected.video=s.expected.audio;}],
 ['muted-audio-does-not-blank-video',s=>{s.expected.audio=s.expected.video;}],
 ['stale-audio-does-not-blank-fresh-video',s=>{s.expected.audio=s.expected.video;}],
 ['video-blank-retains-explicit-audio',s=>{s.expected.audio=null;}],
 ['video-blank-audio-explicitly-muted',s=>{s.expected.audio=s.state.sources[0];}],
 ['av-routes-may-intentionally-differ',s=>{s.expected.audio=s.expected.video;}],
 ['retired-helper-epoch-cannot-resolve-pinned-source',s=>{s.expected.video=s.expected.audio;}],
 ['tiles-editorial-order-not-map-order',s=>{s.expected.members.reverse();}],
 ['tiles-roster-additions-explicit-and-excluded',s=>{s.expected.members.push('dave');}],
 ['tiles-automatic-omits-camera-off-or-departed',s=>{s.expected.members.push('alice');}],
 ['tiles-manual-holes-never-promote',s=>{s.expected.slots.splice(1,1);}],
 ['iso-arming-not-roster-or-current-eligibility',s=>{s.expected.armed.pop();}],
 ['iso-camera-off-remains-armed-with-audio',s=>{s.expected.writers=[];}],
 ['iso-person-follows-rejoin-but-pin-does-not',s=>{s.expected.waiting=[];}],
 ['stale-client-revision',s=>{s.expected.results[0].accepted=true;}],
 ['stale-preview',s=>{s.expected.results[0].accepted=true;}],
 ['duplicate-take-replays-original',s=>{s.expected.results[0].additionalPromotions=1;}],
 ['lost-ack-does-not-promote-current-preview',s=>{s.expected.results[0].promotedPreviewRevision=11;}],
 ['authority-restart-rejects-old-operation',s=>{s.expected.results[0].accepted=true;}],
 ['changed-payload-id-conflict',s=>{s.expected.results[0].accepted=true;}],
 ['evicted-ledger-retains-rejection-tombstone',s=>{s.expected.results[0].error='stale-revision';}],
 ['concurrent-duplicate-first-submissions-serialize',s=>{s.expected.totalPromotions=2;}],
 ['concurrent-distinct-takes-conflict-after-first',s=>{s.expected.results[1].accepted=true;}],
 ['bounded-operation-ledger-rejects-new-work-at-capacity',s=>{s.expected.results[0].accepted=true;}],
 ['revision-exhaustion-rejects-new-take',s=>{s.expected.results[0].accepted=true;}],
 ['revision-exhaustion-preserves-known-replay',s=>{s.expected.results[0].additionalPromotions=1;}]
];
for(const [id,mutate] of mutations)test(`rejects contradictory ${id}`,()=>{const f=load();mutate(f.scenarios.find(s=>s.id===id));assert.throws(()=>validateAuthorityGoldens(f),/contradicts/);});

for(const [label,mutate] of [
 ['result skips a revision',old=>{old.operation.expectedRevision--;}],
 ['promotes another Preview',old=>{old.promotedPreviewRevision++;}],
 ['unsafe expected revision',old=>{old.operation.expectedRevision=Number.MAX_SAFE_INTEGER+1;}],
 ['fractional Preview revision',old=>{old.operation.previewRevision=1.5;}],
 ['empty operation id',old=>{old.operation.id='';}],
 ['historical increment past exhaustion',old=>{old.operation.expectedRevision=Number.MAX_SAFE_INTEGER;}]
])test(`rejects impossible accepted ledger: ${label}`,()=>{
 const f=load();const s=f.scenarios.find(s=>s.id==='duplicate-take-replays-original');
 mutate(s.state.ledger[0]);
 // Mirror the corrupted history in the replay request/result: this must fail
 // historical validation, not merely an expected-output comparison.
 s.operations[0]=structuredClone(s.state.ledger[0].operation);
 s.expected.results[0].resultRevision=s.state.ledger[0].resultRevision;
 s.expected.results[0].promotedPreviewRevision=s.state.ledger[0].promotedPreviewRevision;
 assert.throws(()=>validateAuthorityGoldens(f));
});
