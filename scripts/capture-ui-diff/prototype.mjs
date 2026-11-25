#!/usr/bin/env node
// Dependency-free Chrome DevTools Protocol transport for capture.py.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import readline from 'node:readline';
import {StringDecoder} from 'node:string_decoder';
import {spawn} from 'node:child_process';
import {pathToFileURL} from 'node:url';

const [chromePath, root, mode] = process.argv.slice(2);
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'hb-chrome-'));
const chrome = spawn(chromePath, [...(mode === 'foreground' ? [] : ['--headless=new']), '--disable-gpu', '--no-first-run',
  '--no-default-browser-check', '--disable-background-networking', '--disable-extensions', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows',
  '--remote-debugging-pipe', `--user-data-dir=${profile}`, 'about:blank'],
  {stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe']});
let sequence = 0, buffer = '', stderr = '', session;
const contexts = new Map();
const pending = new Map();
const decoder = new StringDecoder('utf8');
const emit = value => process.stdout.write(JSON.stringify(value) + '\n');
function rejectPending(error) {
  for (const p of pending.values()) { clearTimeout(p.timer); p.reject(error); }
  pending.clear();
}
chrome.stderr.on('data', d => { stderr = (stderr + d).slice(-2000); });
chrome.on('error', rejectPending);
chrome.on('exit', (code, signal) => rejectPending(Error(`Chrome exited: code=${code} signal=${signal}; ${stderr}`)));
chrome.stdio[3].on('error', rejectPending);
chrome.stdio[4].on('data', d => {
  buffer += decoder.write(d);
  let i;
  while ((i = buffer.indexOf('\0')) >= 0) {
    const message = JSON.parse(buffer.slice(0, i)); buffer = buffer.slice(i + 1);
    if(message.method==='Runtime.executionContextCreated') {
      const context=message.params.context;
      if(context.auxData?.isDefault)contexts.set(context.auxData.frameId,context.id);
    }
    if(message.method==='Runtime.executionContextDestroyed') {
      for(const [frame,id] of contexts)if(id===message.params.executionContextId)contexts.delete(frame);
    }
    if(message.method==='Runtime.executionContextsCleared')contexts.clear();
    const p = pending.get(message.id);
    if (p) { pending.delete(message.id); clearTimeout(p.timer);
      message.error ? p.reject(Error(JSON.stringify(message.error))) : p.resolve(message.result); }
  }
});
function cdp(method, params = {}, sessionId) {
  return new Promise((resolve, reject) => {
    if (chrome.exitCode !== null || chrome.signalCode !== null) {
      reject(Error(`Chrome exited before ${method}: code=${chrome.exitCode} signal=${chrome.signalCode}; ${stderr}`));return;
    }
    const id = ++sequence;
    const timer = setTimeout(() => { pending.delete(id); reject(Error(`${method}: timeout; ${stderr}`)); }, 15000);
    pending.set(id, {resolve, reject, timer});
    chrome.stdio[3].write(JSON.stringify({id, method, params, sessionId}) + '\0');
  });
}
const call = (method, params) => cdp(method, params, session);
async function evaluate(expression, contextId) {
  const result = await call('Runtime.evaluate', {expression, contextId, returnByValue: true, awaitPromise: true});
  if (result.exceptionDetails) throw Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
  return result.result?.value;
}
// Only the harness clock/randomness are controlled. All UI states still go
// through prototype.js and existing app actions. Every navigation resets them.
const clockScript = `(() => {
  let time=0, next=0, seed=42; const timers=new Map(), RealDate=Date;
  const epoch=Date.parse('2026-11-17T14:58:00Z');
  window.Date=class extends RealDate { constructor(...a){super(...(a.length?a:[epoch+time]));} static now(){return epoch+time;} };
  Math.random=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed/4294967296;};
  window.setTimeout=(fn,ms=0,...args)=>{const id=++next;timers.set(id,{fn,args,at:time+Math.max(1,ms),interval:0});return id;};
  window.setInterval=(fn,ms=1,...args)=>{const id=++next;timers.set(id,{fn,args,at:time+Math.max(1,ms),interval:Math.max(1,ms)});return id;};
  window.clearTimeout=window.clearInterval=id=>timers.delete(id);
  window.__advance=ms=>{const end=time+ms;let count=0;while(true){let best=null;for(const pair of timers)if(pair[1].at<=end&&(!best||pair[1].at<best[1].at))best=pair;
    if(!best)break;if(++count>10000)throw Error('timer overflow');const[id,t]=best;time=t.at;
    if(t.interval)t.at+=t.interval;else timers.delete(id);if(typeof t.fn==='function')t.fn(...t.args);else (0,eval)(t.fn);
  }time=end;};
})();`;
const recipes = fs.readFileSync(new URL('./prototype.js', import.meta.url), 'utf8');
const finishAnimations = `(() => {
  for (const animation of document.getAnimations()) {
    animation.pause();
    const timing = animation.effect.getComputedTiming();
    animation.currentTime = Number.isFinite(timing.endTime) ? timing.endTime : 0;
    if (animation.effect.target?.matches('.toast.out')) animation.currentTime = Math.min(150, timing.activeDuration / 2);
  }
  const style = document.createElement('style');
  style.textContent = '* { caret-color: transparent !important; }';
  document.head.appendChild(style);
})();`;
try {
  const {targetId} = await cdp('Target.createTarget', {url:'about:blank'});
  ({sessionId:session} = await cdp('Target.attachToTarget',{targetId,flatten:true}));
  await call('Page.enable'); await call('Runtime.enable');
  await call('Emulation.setDeviceMetricsOverride', {width:1400,height:900,deviceScaleFactor:1,mobile:false});
  await call('Emulation.setTimezoneOverride', {timezoneId:'Asia/Shanghai'});
  await call('Page.addScriptToEvaluateOnNewDocument',{source:clockScript});
  emit({ready:true});
  for await (const line of readline.createInterface({input:process.stdin, crlfDelay:Infinity})) {
    const request = JSON.parse(line);
    if (request.quit) break;
    try {
      await call('Page.bringToFront');
      await call('Page.navigate',{url:pathToFileURL(path.join(root,'prototype/index.html')).href});
      let ready=false;
      for(let i=0;i<100;i++) {
        try {ready=await evaluate("document.readyState==='complete'&&!!window.A");}catch{}
        if(ready)break; await new Promise(r=>setTimeout(r,50));
      }
      if(!ready)throw Error('prototype did not load');
      // A 900px iframe provides the real CSS media-query viewport for SM-80,
      // while the outer screenshot canvas remains exactly 1400x900.
      let frameContext;
      if(request.id===80) {
        const url=pathToFileURL(path.join(root,'prototype/index.html')).href;
        await evaluate(`document.body.innerHTML='<iframe id="captureFrame" style="display:block;border:0;width:900px;height:900px;margin:auto" src="${url}"></iframe>'`);
        let loaded=false;
        for(let i=0;i<100;i++) {
          const {frameTree}=await call('Page.getFrameTree');
          const child=frameTree.childFrames?.[0];
          frameContext=child && contexts.get(child.frame.id);
          if(frameContext) {
            try {loaded=await evaluate("document.readyState==='complete'&&!!window.A",frameContext);}catch{}
          }
          if(loaded)break;await new Promise(r=>setTimeout(r,50));
        }
        if(!loaded)throw Error('narrow prototype iframe did not load');
      }
      await evaluate(`(${recipes})({id:${request.id},mode:${JSON.stringify(request.mode)},advance:async ms=>{__advance(ms);await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));}})`,frameContext);
      await evaluate('document.fonts.ready',frameContext);
      await evaluate(finishAnimations,frameContext);
      const {data}=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
      fs.writeFileSync(request.output,Buffer.from(data,'base64'));
      emit({ok:true,mode:request.mode,clock:'fixed date, seeded random, stepped timers'});
    } catch(error) {emit({ok:false,error:String(error.stack || error)});}
  }
} catch(error) {emit({ready:false,error:String(error.stack || error)});process.exitCode=1;}
finally {
  chrome.kill('SIGTERM');
  await new Promise(resolve=>{if(chrome.exitCode!==null||chrome.signalCode!==null)resolve();else{
    chrome.once('exit',resolve);setTimeout(()=>{chrome.kill('SIGKILL');resolve();},2000).unref();}});
  fs.rmSync(profile,{recursive:true,force:true});
}
