'use strict';
// Browser shell only. All gameplay and rendering run in the shared C++ product.
const $ = id => document.getElementById(id);
const status = (text, error = false) => { $('status').textContent = text; $('status').className = error ? 'error' : ''; };
const log = text => { $('log').textContent = ($('log').textContent + String(text) + '\n').slice(-32000); };
let engine, running = false, ready = false, persistence = false, syncPending = null;
function fail(error) { log(error.stack || error); status(String(error.message || error), true); }
function syncSaves() {
  if (!persistence) return Promise.reject(new Error('Browser storage is unavailable. Use Export saves to keep a backup.'));
  if (!syncPending) syncPending = new Promise((resolve, reject) => engine.FS.syncfs(false, e => e ? reject(e) : resolve()))
    .finally(() => { syncPending = null; });
  return syncPending;
}
function walk(path) {
  const files = {};
  for (const name of engine.FS.readdir(path)) {
    if (name === '.' || name === '..') continue;
    const p = path + '/' + name, info = engine.FS.stat(p);
    if (engine.FS.isDir(info.mode)) Object.assign(files, walk(p));
    else if (engine.FS.isFile(info.mode)) {
      const bytes = engine.FS.readFile(p);
      // A card is 128 KiB; bounded chunks avoid argument-stack overflow.
      let binary = '';
      for (let i = 0; i < bytes.length; i += 8192) binary += String.fromCharCode(...bytes.subarray(i, i + 8192));
      files[p.slice('/saves/'.length)] = btoa(binary);
    }
  }
  return files;
}
async function exportSaves() {
  const blob = new Blob([JSON.stringify({format:'gt2-browser-saves-v1',files:walk('/saves')})], {type:'application/json'});
  const url = URL.createObjectURL(blob), a = document.createElement('a');
  a.href = url; a.download = 'gt2-saves-' + new Date().toISOString().slice(0,10) + '.json'; a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
$('export').onclick = () => exportSaves().catch(fail);
$('sync').onclick = () => syncSaves().then(() => status('Saves written to browser storage.')).catch(fail);
$('fullscreen').onclick = () => $('canvas').requestFullscreen().catch(fail);
$('stop').onclick = () => {
  // SDL_QUIT through the event API; never tear down Wasm during an Asyncify unwind.
  engine._gt2_web_request_close(); status('Exiting game…');
};
$('choose-disc').onclick = () => $('disc').click();
$('disc').onchange = () => {
  $('disc-name').textContent = $('disc').files[0]?.name || 'No disc selected';
  $('start').disabled = !ready || !$('disc').files.length;
};
$('import').onclick = () => $('save-file').click();
$('save-file').onchange = async () => {
  try {
    if (running) throw new Error('Import saves before starting the game.');
    const file = $('save-file').files[0]; if (!file) return;
    if (file.size > 16*1024*1024) throw new Error('The save backup is too large.');
    const archive = JSON.parse(await file.text());
    if (archive.format !== 'gt2-browser-saves-v1' || !archive.files || typeof archive.files !== 'object' || Array.isArray(archive.files))
      throw new Error('Unknown save backup format.');
    const entries = Object.entries(archive.files);
    if (entries.length > 256) throw new Error('Too many files in the save backup.');
    // Validate every entry before overwriting anything. No absolute paths or traversal.
    const decoded = entries.map(([path, value]) => {
      if (!/^(?:[a-zA-Z0-9_-]+\/)*[a-zA-Z0-9_.-]+$/.test(path) || path.split('/').some(p => p === '.' || p === '..') || typeof value !== 'string')
        throw new Error('Invalid filename in the save backup.');
      return [path, Uint8Array.from(atob(value), c => c.charCodeAt(0))];
    });
    if (!confirm('Replace matching saves with files from this backup?')) return;
    for (const [path, bytes] of decoded) {
      const full = '/saves/' + path;
      engine.FS.mkdirTree(full.slice(0, full.lastIndexOf('/'))); engine.FS.writeFile(full, bytes);
    }
    await syncSaves(); status('Saves imported.');
  } catch (e) { fail(e); }
};
$('start').onclick = async () => {
  try {
    const file = $('disc').files[0];
    if (!ready || running || !file) return;
    if (!file.size || file.size % 2352 !== 0 || file.size > 1024*1024*1024)
      throw new Error('Select a full 2352-byte-sector BIN, up to 1 GiB. A 2048-byte ISO is missing XA music sectors.');
    $('start').disabled = true; $('import').disabled = true; $('disc').disabled = true; $('choose-disc').disabled = true; $('mode').disabled = true;
    status('Reading selected disc…');
    const bytes = new Uint8Array(await file.arrayBuffer());
    // MEMFS takes ownership of the JS buffer, avoiding a second full-disc copy in Wasm.
    engine.FS.createDataFile('/data','disc.bin',bytes,true,false,true);
    const mode = $('mode').value;
    engine.FS.mkdirTree('/saves/' + mode);
    const args = ['/data/disc.bin','--windowed','--card','/saves/'+mode+'/card1.mcd',
      '--settings','/saves/'+mode+'/settings.txt'];
    // Test harness sets this before launch. Nothing is read from the URL or a server.
    if (Array.isArray(window.gt2TestArguments)) args.push(...window.gt2TestArguments);
    running = true; $('stop').disabled = false; $('canvas').focus(); status('Game running.');
    engine.callMain(args);
  } catch (e) { running = false; $('stop').disabled = true; fail(e); }
};
(async () => {
  try {
    engine = await createGT2({canvas:$('canvas'),noInitialRun:true,print:log,printErr:log,
      onAbort:fail,
      onGameExit:code => { running=false; $('stop').disabled=true; status('Game exited, code '+code+'. Reload the page to start again.', code!==0); syncSaves().catch(fail); }
    });
    engine.FS.mkdirTree('/data'); engine.FS.mkdirTree('/saves');
    try {
      engine.FS.mount(engine.IDBFS,{},'/saves');
      await new Promise((resolve,reject) => engine.FS.syncfs(true,e => e ? reject(e) : resolve()));
      persistence = true;
    } catch (e) { log('Persistent storage unavailable: '+e); }
    ready = true; $('export').disabled = false; $('import').disabled = false; $('sync').disabled = !persistence;
    $('start').disabled = !$('disc').files.length;
    status(persistence ? 'Ready. Select a disc and press Start.' : 'Browser storage is unavailable. Export your saves after playing.', !persistence);
    // Exposed for local diagnostics and the automated test harness, never sent remotely.
    window.gt2Engine = engine;
    setInterval(() => { if (running && persistence) syncSaves().catch(fail); }, 15000);
    document.addEventListener('visibilitychange', () => { if(document.hidden && persistence) syncSaves().catch(fail); });
    window.addEventListener('beforeunload', event => { if(running) { event.preventDefault(); event.returnValue=''; } });
  } catch(e) { fail(e); }
})();
