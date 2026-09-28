// Local runtime acceptance. Uses the actual C++ Wasm game and a local user disc.
// NODE_PATH must resolve Playwright; nothing downloads or uploads disc content.
const fs=require('node:fs');
const path=require('node:path');
const assert=require('node:assert/strict');
const {chromium}=require('playwright');
const [url,disc,outDir='work/web-port/browser-smoke']=process.argv.slice(2);
const scenario=process.env.GT2_WEB_SCENARIO || 'title';
if(!url || !disc) throw Error('Usage: node tests/web_game_smoke.cjs http://127.0.0.1:8080/ <disc.bin> [output-directory]');
(async()=>{
  fs.mkdirSync(outDir,{recursive:true});
  const browser=await chromium.launch({channel:'chrome',headless:true});
  try {
    const page=await browser.newPage({viewport:{width:1280,height:960}});
    const errors=[];page.on('pageerror',e=>errors.push(String(e)));
    await page.addInitScript(scenario=>{
      window.gt2TestArguments=scenario.startsWith('race')
        ? ['--race','--track','seattle','--cars','6','--ai-player','--no-countdown','--shot','120','/tmp/web-title.png']
        : scenario==='reverse'
        ? ['--race','--drive','--track','seattle','--cars','1','--no-countdown','--shot-at','90','/tmp/web-title.png']
        : scenario==='drive'
        ? ['--race','--drive','--track','seattle','--cars','6','--no-countdown','--shot-at','180','/tmp/web-title.png']
        : scenario==='arcade'
        ? ['--arcade-frames','120','--arcade-shot','119','/tmp/web-title.png','--no-sound']
        : scenario==='career'
        ? ['--menu','--menu-frames','120','--menu-shot','119','/tmp/web-title.png','--save-out','/saves/simulation/smoke.mcd','--no-sound']
        : ['--title-frames','120','--title-shot','119','/tmp/web-title.png','--no-sound'];
      if(scenario==='race-modern') window.gt2TestArguments.push('--modern','--msaa','4','--render-scale','75');
      window.gt2AudioProbe={callbacks:0,peak:0};
      const original=AudioContext.prototype.createScriptProcessor;
      AudioContext.prototype.createScriptProcessor=function(...args) {
        const node=original.apply(this,args);
        // Register after SDL has assigned onaudioprocess; read its filled output.
        queueMicrotask(()=>node.addEventListener('audioprocess',event=>{
          ++window.gt2AudioProbe.callbacks;
          for(let c=0;c<event.outputBuffer.numberOfChannels;++c)
            for(const value of event.outputBuffer.getChannelData(c)) window.gt2AudioProbe.peak=Math.max(window.gt2AudioProbe.peak,Math.abs(value));
        }));return node;
      };
    },scenario);
    await page.goto(url);await page.waitForFunction(()=>window.gt2Engine,undefined,{timeout:120000});
    assert.equal(await page.locator('html').getAttribute('lang'),'en');
    assert.doesNotMatch(await page.locator('body').innerText(),/[\u0400-\u04ff]/);
    // Exercise persistent storage with a binary sentinel, reload, and compare bytes.
    await page.evaluate(async()=>{
      const m=window.gt2Engine;
      m.FS.mkdirTree('/saves/smoke');m.FS.writeFile('/saves/smoke/probe.bin',new Uint8Array([0,1,127,128,255]));
      await new Promise((resolve,reject)=>m.FS.syncfs(false,e=>e?reject(e):resolve()));
    });
    await page.reload();await page.waitForFunction(()=>window.gt2Engine,undefined,{timeout:120000});
    assert.deepEqual(await page.evaluate(()=>Array.from(window.gt2Engine.FS.readFile('/saves/smoke/probe.bin'))),[0,1,127,128,255]);
    await page.evaluate(async()=>{
      const m=window.gt2Engine;m.FS.unlink('/saves/smoke/probe.bin');m.FS.rmdir('/saves/smoke');
      await new Promise((resolve,reject)=>m.FS.syncfs(false,e=>e?reject(e):resolve()));
    });
    if(scenario==='reverse') await page.evaluate(()=>{
      const fs=window.gt2Engine.FS;fs.mkdirTree('/saves/simulation');
      // CLI graphics flags deliberately lock settings; use the ordinary saved preferences instead.
      fs.writeFile('/saves/simulation/settings.txt','frame_rate=original\nrender_scale=125\n');
    });
    await page.locator('#disc').setInputFiles(path.resolve(disc));
    await page.locator('#start').click();
    if(scenario==='reverse') {
      await page.waitForFunction(()=>/sound: engine/.test(document.getElementById('log').textContent),undefined,{timeout:120000});
      await page.locator('#canvas').focus();
      const speed=()=>{const m=document.title.match(/ - (-?\d+) km\/h/);return m?Number(m[1]):null;};
      const waitSpeed=async predicate=>{
        await page.waitForFunction(({source,predicate})=>{const value=eval('('+source+')')();return value!==null && eval('('+predicate+')')(value);},
          {source:speed.toString(),predicate:predicate.toString()},{timeout:30000});
        return await page.title();
      };
      const trace=[];
      await page.keyboard.down('ArrowUp');trace.push(await waitSpeed(v=>v>=20));await page.keyboard.up('ArrowUp');
      await page.keyboard.down('ArrowDown');trace.push(await waitSpeed(v=>v<=-5));
      assert.match(await page.title(),/gear 0/);await page.keyboard.up('ArrowDown');
      await page.keyboard.down('ArrowUp');trace.push(await waitSpeed(v=>v>=5));await page.keyboard.up('ArrowUp');
      assert.doesNotMatch(await page.title(),/gear 0/);
      fs.writeFileSync(path.join(outDir,'direction-trace.json'),JSON.stringify(trace,null,2));
      // Exercise the actual C++ settings menu: 125% -> 130% with one Right press.
      await page.keyboard.press('Shift+Q');await page.waitForTimeout(300);
      await page.keyboard.press('Enter');await page.waitForTimeout(300);
      await page.keyboard.press('ArrowRight');await page.waitForTimeout(300);
      await page.keyboard.press('Escape');await page.waitForTimeout(200);
      await page.keyboard.press('Shift+Q');await page.waitForTimeout(300);
      const settings=await page.evaluate(()=>window.gt2Engine.FS.readFile('/saves/simulation/settings.txt.overlay',{encoding:'utf8'}));
      fs.writeFileSync(path.join(outDir,'settings.txt'),settings);
      assert.match(settings,/^render_scale=130$/m);
      await page.locator('#stop').click();
    }
    if(scenario==='drive') {
      await page.waitForFunction(()=>/sound: engine/.test(document.getElementById('log').textContent),undefined,{timeout:120000});
      await page.locator('#canvas').focus();await page.keyboard.down('ArrowUp');
      await page.waitForFunction(()=>document.getElementById('log').textContent.includes('frame 180'),undefined,{timeout:120000});
      await page.keyboard.up('ArrowUp');await page.locator('#stop').click();
    }
    await page.waitForFunction(()=>/Game exited/.test(document.getElementById('status').textContent)
      || document.getElementById('status').classList.contains('error'),undefined,{timeout:240000});
    const status=await page.locator('#status').textContent(),log=await page.locator('#log').textContent();
    fs.writeFileSync(path.join(outDir,'game.log'),log);
    assert.deepEqual(errors,[]);assert.match(status,/code 0/);assert.doesNotMatch(log,/Aborted|RuntimeError|WebGL error|Startup failed/);
    const capture=await page.evaluate(()=>{
      const bytes=window.gt2Engine.FS.readFile('/tmp/web-title.png');let text='';
      for(let i=0;i<bytes.length;i+=8192) text+=String.fromCharCode(...bytes.subarray(i,i+8192));return btoa(text);
    });
    fs.writeFileSync(path.join(outDir,scenario+'.png'),Buffer.from(capture,'base64'));
    // A screenshot is evidence for visual review, not a substitute for a visible frame.
    const visible=await page.evaluate(async capture=>{
      const image=new Image();image.src='data:image/png;base64,'+capture;await image.decode();
      const copy=document.createElement('canvas');copy.width=image.width;copy.height=image.height;
      const ctx=copy.getContext('2d');ctx.drawImage(image,0,0);const data=ctx.getImageData(0,0,copy.width,copy.height).data;
      let lit=0;for(let i=0;i<data.length;i+=4) if(data[i]+data[i+1]+data[i+2]>40) ++lit;return lit;
    },capture);
    assert.ok(visible>100,'Title frame is black');
    const audio=await page.evaluate(()=>window.gt2AudioProbe);
    if(scenario==='drive') {assert.ok(audio.callbacks>0,'No Web Audio callbacks');assert.ok(audio.peak>0,'Audio output is silent');}
    if(scenario==='career') {
      assert.match(log,/CRC ok, reloaded state identical/);
      const card=await page.evaluate(async()=>{
        const m=window.gt2Engine;
        await new Promise((resolve,reject)=>m.FS.syncfs(false,e=>e?reject(e):resolve()));
        return Array.from(m.FS.readFile('/saves/simulation/smoke.mcd'));
      });
      assert.equal(card.length,128*1024);
      const downloadPromise=page.waitForEvent('download');await page.locator('#export').click();
      const download=await downloadPromise,backup=path.join(outDir,'save-export.json');await download.saveAs(backup);
      const archive=JSON.parse(fs.readFileSync(backup,'utf8'));
      assert.deepEqual([...Buffer.from(archive.files['simulation/smoke.mcd'],'base64')],card);
      await page.evaluate(async()=>{
        const m=window.gt2Engine;m.FS.unlink('/saves/simulation/smoke.mcd');
        await new Promise((resolve,reject)=>m.FS.syncfs(false,e=>e?reject(e):resolve()));
      });
      await page.reload();await page.waitForFunction(()=>window.gt2Engine,undefined,{timeout:120000});
      page.once('dialog',dialog=>dialog.accept());await page.locator('#save-file').setInputFiles(path.resolve(backup));
      await page.waitForFunction(()=>document.getElementById('status').textContent==='Saves imported.',undefined,{timeout:30000});
      await page.reload();await page.waitForFunction(()=>window.gt2Engine,undefined,{timeout:120000});
      assert.deepEqual(await page.evaluate(()=>Array.from(window.gt2Engine.FS.readFile('/saves/simulation/smoke.mcd'))),card);
      await page.evaluate(()=>{window.gt2TestArguments=['--menu','--career','/saves/simulation/smoke.mcd','--menu-frames','30','--no-sound'];});
      await page.locator('#disc').setInputFiles(path.resolve(disc));await page.locator('#start').click();
      await page.waitForFunction(()=>/Game exited/.test(document.getElementById('status').textContent),undefined,{timeout:120000});
      const reloadLog=await page.locator('#log').textContent();
      fs.writeFileSync(path.join(outDir,'career-reload.log'),reloadLog);
      assert.match(reloadLog,/career .*smoke.mcd: CRC ok, day 1, money 10000/);
    }
    fs.writeFileSync(path.join(outDir,'result.json'),JSON.stringify({status:'PASS',scenario,persistentStorage:true,visiblePixels:visible,audio,errors},null,2));
    console.log('PASS: actual Wasm '+scenario+' loop, WebGL frame, IndexedDB reload');
  } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
