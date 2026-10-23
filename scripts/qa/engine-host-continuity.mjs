import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { createHash } from 'node:crypto';
import { validateAudioTimeline } from './av-content-analysis.mjs';
const exec = promisify(execFile);
const run = (exe, args, extra = {}) => exec(exe, args, { windowsHide: true, timeout: 120000, maxBuffer: 32 * 1024 * 1024, ...extra });
const [mode, input, output] = process.argv.slice(2);
if (!input || !['generate', 'analyze'].includes(mode)) throw new Error('Usage: node engine-host-continuity.mjs generate PATTERN.mp4 | analyze MEDIA REPORT.json');
if (mode === 'generate') {
  await mkdir(dirname(resolve(input)), { recursive: true });
  // 16-bit frame identity survives lossy encoding. A continuous tone exposes
  // audio gaps during the fault window, unlike intentionally silent flash cues.
  const filters = ['drawbox=x=0:y=1016:w=1920:h=64:color=black:t=fill'];
  for (let bit = 0; bit < 16; bit++) filters.push(`drawbox=x=${bit * 120}:y=1016:w=120:h=64:color=white:t=fill:enable='mod(floor(n/${2 ** bit}),2)'`);
  await run('ffmpeg', ['-y','-v','error','-f','lavfi','-i','testsrc2=size=1920x1080:rate=60:duration=45',
    '-f','lavfi','-i','sine=frequency=1000:sample_rate=48000:duration=45','-vf',filters.join(','),
    '-ac','2','-c:v','libx264','-preset','ultrafast','-crf','18','-pix_fmt','yuv420p','-c:a','aac','-b:a','192k',input]);
  console.log(input);
} else {
  if (!output) throw new Error('Report path required');
  const probe = JSON.parse((await run('ffprobe',['-v','error','-show_streams','-show_format','-of','json',input])).stdout);
  const frames = JSON.parse((await run('ffprobe',['-v','error','-select_streams','v:0','-show_frames','-show_entries','frame=best_effort_timestamp_time','-of','json',input])).stdout).frames;
  const pts = frames.map(f => Number(f.best_effort_timestamp_time));
  const bytes = (await run('ffmpeg',['-v','error','-xerror','-i',input,'-map','0:v:0','-vf','crop=1920:64:0:1016,scale=16:1:flags=area','-fps_mode','passthrough','-pix_fmt','gray','-f','rawvideo','pipe:1'],{encoding:'buffer'})).stdout;
  if (bytes.length !== pts.length * 16) throw new Error('Decoded barcode coverage mismatch');
  const ids = pts.map((_, i) => { let id=0; for(let b=0;b<16;b++) if(bytes[i*16+b]>=128) id+=2**b; return id; });
  // Startup and finalization are reported separately; never trim interior faults.
  const first = ids.findIndex(n => n > 3 && n < 2700);
  let last = ids.length-1; while(last > first && ids[last] === ids[last-1]) last--;
  const anomalies=[]; let maxRepeated=0, repeated=0;
  for(let i=first+1;i<=last;i++) {
    const delta=ids[i]-ids[i-1];
    repeated=delta===0?repeated+1:0; maxRepeated=Math.max(maxRepeated,repeated);
    if(delta!==1) anomalies.push({index:i,pts:pts[i],before:ids[i-1],after:ids[i],delta});
  }
  const audio=await run('ffmpeg',['-hide_banner','-nostats','-v','info','-xerror','-i',input,'-map','0:a:0','-af',
    'aformat=sample_fmts=flt:sample_rates=48000:channel_layouts=mono,asettb=1/48000,ashowinfo','-f','f32le','pipe:1'],{encoding:'buffer'});
  const log=audio.stderr.toString(), pcm=audio.stdout;
  const timeline=validateAudioTimeline(log,pcm.length/4,{allowedPtsJitterSamples:input.endsWith('.flv')?24:0});
  const silence=[];let quietStart=null;
  for(let i=0;i+480<=pcm.length/4;i+=480) {
    let sum=0;for(let j=0;j<480;j++)sum+=pcm.readFloatLE((i+j)*4)**2;
    const quiet=Math.sqrt(sum/480)<0.015;
    if(quiet && quietStart===null)quietStart=i;
    if(!quiet && quietStart!==null){silence.push({start:timeline.firstPts+quietStart/48000,end:timeline.firstPts+i/48000});quietStart=null;}
  }
  if(quietStart!==null)silence.push({start:timeline.firstPts+quietStart/48000,end:timeline.firstPts+pcm.length/4/48000});
  const interiorSilence=silence.filter(s=>s.start>pts[first]+0.1 && s.end<pts[last]-0.1);
  const report={artifact:resolve(input),sha256:createHash('sha256').update(await readFile(input)).digest('hex'),streams:probe.streams,
    decodedFrames:pts.length,firstContentPts:pts[first],lastContentPts:pts[last],firstFrameId:ids[first],lastFrameId:ids[last],
    startupFrames:first,tailRepeatedFrames:ids.length-1-last,maxRepeatedFrames:maxRepeated,anomalies,
    maxPtsGapMs:Math.max(...pts.slice(1).map((p,i)=>(p-pts[i])*1000)),audioTimeline:timeline,silence,interiorSilence,
    exactFrameContinuityPassed:first>=0 && anomalies.length===0 && ids.length-1-last === 0,interiorAudioContinuityPassed:interiorSilence.length===0,
    physicalPresentationVerified:false,framePerformancePassed:false};
  await writeFile(output,JSON.stringify(report,null,2));
  console.log(JSON.stringify({...report,streams:undefined,anomalies:anomalies.slice(0,12),silence:silence.slice(0,8),interiorSilence:interiorSilence.slice(0,8)},null,2));
  process.exitCode=report.exactFrameContinuityPassed && report.interiorAudioContinuityPassed ? 0 : 1;
}
