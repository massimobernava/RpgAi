std::string main_page = R"HTML(<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>RpgAi</title>
<style>
  :root {
    --bg:      #0d0d0f;
    --surface: #16161a;
    --border:  #2a2a35;
    --accent:  #7c6fcd;
    --accent2: #4fc3a1;
    --text:    #e8e6f0;
    --dim:     #7a788a;
    --warn:    #e5a550;
    --danger:  #e55c6c;
    --hud-bg:  #111118;
    --radius:  8px;
    --mono:    'Cascadia Code','Fira Code','Consolas',monospace;
  }
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  html, body { height: 100%; background: var(--bg); color: var(--text);
               font-family: var(--mono); font-size: 14px; }

  /* ---- Layout ---- */
  #app   { display: flex; height: 100vh; overflow: hidden; }
  #main  { flex: 1; display: flex; flex-direction: column; min-width: 0; }

  /* ---- Sidebar ---- */
  #sidebar { width: 260px; min-width: 200px; background: var(--surface);
             border-right: 1px solid var(--border); display: flex;
             flex-direction: column; padding: 16px 12px; gap: 10px;
             overflow-y: auto; }
  #sidebar h2 { color: var(--accent); font-size: 13px; letter-spacing: .1em;
                text-transform: uppercase; padding-bottom: 6px;
                border-bottom: 1px solid var(--border); flex-shrink: 0; }
  .sb-label { font-size: 11px; color: var(--dim); text-transform: uppercase;
              letter-spacing: .05em; margin-top: 4px; flex-shrink: 0; }
  .sb-value { font-size: 12px; color: var(--text); word-break: break-all; }
  .sb-sep   { border: none; border-top: 1px solid var(--border);
              margin: 2px 0; flex-shrink: 0; }

  /* Sezioni sidebar */
  #section-pre  { display: flex; flex-direction: column; gap: 8px; }
  #section-game { display: none; flex-direction: column; gap: 8px; }

  /* Liste script e save */
  .item-list { list-style: none; display: flex; flex-direction: column; gap: 3px; }
  .item-list li { padding: 5px 8px; border-radius: var(--radius); cursor: pointer;
                  border: 1px solid transparent; font-size: 12px; color: var(--dim);
                  transition: all .15s; word-break: break-all; }
  .item-list li:hover        { border-color: var(--border); color: var(--text); }
  .item-list li.active-script { border-color: var(--accent);  color: var(--accent);
                                 background: rgba(124,111,205,.1); }
  .item-list li.active-save   { border-color: var(--accent2); color: var(--accent2);
                                 background: rgba(79,195,161,.1); }
  .list-empty { font-size: 11px; color: var(--dim); font-style: italic; padding: 2px 4px; }

  /* ---- Command palette ---- */
  #sb-commands-section { display: none; flex-direction: column; gap: 4px; flex-shrink: 0; }
  #sb-commands-list { list-style: none; display: flex; flex-direction: column; gap: 2px; }
  #sb-commands-list li {
    padding: 4px 8px; border-radius: var(--radius); cursor: pointer;
    font-size: 11px; font-family: var(--mono); color: var(--accent2);
    background: var(--hud-bg); border: 1px solid var(--border);
    transition: border-color .15s, color .15s; white-space: nowrap;
    overflow: hidden; text-overflow: ellipsis; }
  #sb-commands-list li:hover { border-color: var(--accent2); color: var(--text); }

  /* Pulsanti */
  .btn { width: 100%; padding: 8px; border: none; border-radius: var(--radius);
         font-family: var(--mono); font-size: 12px; font-weight: 600;
         cursor: pointer; transition: opacity .15s; flex-shrink: 0; }
  .btn:hover    { opacity: .85; }
  .btn:disabled { opacity: .35; cursor: not-allowed; }
  .btn-primary   { background: var(--accent);  color: #fff; }
  .btn-secondary { background: var(--accent2); color: #000; }
  .btn-ghost     { background: transparent; color: var(--danger);
                   border: 1px solid var(--danger); font-size: 11px; padding: 6px; }

  /* ---- HUD ---- */
  #hud { background: var(--hud-bg); border-bottom: 1px solid var(--border);
         padding: 8px 16px; font-size: 12px; color: var(--accent2);
         white-space: pre; overflow-x: auto; min-height: 36px; flex-shrink: 0; }

  /* ---- Log ---- */
  #log { flex: 1; overflow-y: auto; padding: 16px;
         display: flex; flex-direction: column; gap: 12px;
         scroll-behavior: smooth; }
  .msg { border-radius: var(--radius); padding: 12px 14px;
         line-height: 1.65; max-width: 820px; }
  .msg-narration { background: var(--surface); border-left: 3px solid var(--accent);
                   color: var(--text); white-space: pre-wrap; }
  .msg-player    { align-self: flex-end; background: rgba(124,111,205,.18);
                   border: 1px solid var(--accent); color: var(--text);
                   font-size: 13px; max-width: 60%; }
  .msg-system    { background: transparent; color: var(--dim); font-size: 12px;
                   border-left: 2px solid var(--border); padding: 4px 10px; }
  .msg-command   { background: var(--hud-bg); border: 1px solid var(--border);
                   color: var(--accent2); white-space: pre-wrap; }
  .msg-error     { background: rgba(229,92,108,.1); border: 1px solid var(--danger);
                   color: var(--danger); }
  .msg-gameover  { background: rgba(229,165,80,.08); border: 2px solid var(--warn);
                   color: var(--warn); text-align: center; font-size: 16px;
                   font-weight: 700; padding: 24px; }
  .thinking { color: var(--dim); font-style: italic; animation: pulse 1.2s infinite; }
  @keyframes pulse { 0%,100%{opacity:.4} 50%{opacity:1} }

  /* ---- Image messages ---- */
  .msg-image { background: var(--surface); border: 1px solid var(--border);
               padding: 8px; border-radius: var(--radius); max-width: 820px; }
  .msg-image img { max-width: 100%; border-radius: calc(var(--radius) - 2px);
                   display: block; }
  .msg-image .img-caption { font-size: 11px; color: var(--dim);
                              margin-top: 6px; padding: 0 2px; }
  .msg-missing { background: rgba(229,165,80,.07); border: 1px solid var(--warn);
                 color: var(--warn); }
  .msg-missing ul { margin: 6px 0 0 16px; font-size: 12px; }
  .msg-missing .hint { font-size: 11px; color: var(--dim); margin-top: 8px; }

  /* ---- Input ---- */
  #input-area { border-top: 1px solid var(--border); padding: 12px 16px;
                display: flex; gap: 8px; background: var(--surface); flex-shrink: 0; }
  #input { flex: 1; background: var(--bg); border: 1px solid var(--border);
           border-radius: var(--radius); color: var(--text);
           font-family: var(--mono); font-size: 14px; padding: 10px 12px;
           resize: none; min-height: 44px; max-height: 160px; overflow-y: auto; }
  #input:focus    { outline: none; border-color: var(--accent); }
  #input:disabled { opacity: .4; }
  #btn-send { background: var(--accent); border: none; border-radius: var(--radius);
              color: #fff; font-size: 18px; width: 44px; flex-shrink: 0;
              cursor: pointer; transition: opacity .15s; }
  #btn-send:hover    { opacity: .85; }
  #btn-send:disabled { opacity: .3; cursor: not-allowed; }

  /* ---- Scrollbar ---- */
  ::-webkit-scrollbar { width: 6px; height: 6px; }
  ::-webkit-scrollbar-track { background: transparent; }
  ::-webkit-scrollbar-thumb { background: var(--border); border-radius: 3px; }
</style>
</head>
<body>
<div id="app">

  <!-- ===== SIDEBAR ===== -->
  <div id="sidebar">
    <h2>⚔ RpgAi</h2>

    <!-- Pre-game section -->
    <div id="section-pre">
      <div class="sb-label">New Game</div>
      <ul id="script-list" class="item-list">
        <li class="list-empty">Loading...</li>
      </ul>
      <button id="btn-start" class="btn btn-primary" disabled>▶ Start</button>

      <hr class="sb-sep">
      <div class="sb-label">Load Game</div>
      <ul id="save-list" class="item-list"></ul>
      <div id="save-empty" class="list-empty" style="display:none">
        No saves found.
      </div>
      <button id="btn-load" class="btn btn-secondary" disabled>↩ Load</button>
    </div>

    <!-- In-game section -->
    <div id="section-game">
      <div class="sb-label">Script attivo</div>
      <div id="info-script" class="sb-value">—</div>
      <div class="sb-label">Save File</div>
      <div id="info-save" class="sb-value">—</div>
      <hr class="sb-sep">
      <button id="btn-manualsave" class="btn btn-secondary">💾 Save now</button>
      <hr class="sb-sep">
      <button id="btn-quit" class="btn btn-ghost">✕ Quit game</button>
      <!-- Command palette — populated by loadCommands() if script implements get_commands() -->
      <div id="sb-commands-section">
        <hr class="sb-sep">
        <div class="sb-label">Comandi</div>
        <ul id="sb-commands-list"></ul>
      </div>
    </div>
  </div>

  <!-- ===== MAIN ===== -->
  <div id="main">
    <pre id="hud">Select a script and press Start, or load a saved game.</pre>
    <div id="log"></div>
    <div id="input-area">
      <textarea id="input" rows="1" disabled
        placeholder="Select a script to begin..."></textarea>
      <button id="btn-send" disabled>➤</button>
    </div>
  </div>
</div>

<script>
/* ================================================================
   Elementi DOM
   ================================================================ */
const log        = document.getElementById('log');
const hud        = document.getElementById('hud');
const inp        = document.getElementById('input');
const btnSend    = document.getElementById('btn-send');
const btnStart   = document.getElementById('btn-start');
const btnLoad    = document.getElementById('btn-load');
const btnManSave = document.getElementById('btn-manualsave');
const btnQuit    = document.getElementById('btn-quit');
const scriptList = document.getElementById('script-list');
const saveList   = document.getElementById('save-list');
const saveEmpty  = document.getElementById('save-empty');
const secPre     = document.getElementById('section-pre');
const secGame    = document.getElementById('section-game');
const infoScript    = document.getElementById('info-script');
const infoSave      = document.getElementById('info-save');
const sbCmdSection  = document.getElementById('sb-commands-section');
const sbCmdList     = document.getElementById('sb-commands-list');

/* ================================================================
   Stato UI
   'idle'          → no active game
   'awaiting_init' → welcome shown, waiting for response/empty enter
   'playing'       → game in progress
   ================================================================ */
let gameState      = 'idle';
let selectedScript = null;
let selectedSave   = null;
let busy           = false;

// History input (frecce su/giu come una shell)
let inputHistory = [];
let histIdx      = -1;

/* ================================================================
   Utilities
   ================================================================ */
function addMsg(cls, text) {
  const d = document.createElement('div');
  d.className  = 'msg ' + cls;
  d.textContent = text;
  log.appendChild(d);
  log.scrollTop = log.scrollHeight;
  return d;
}
function addThinking() {
  return addMsg('msg-system thinking', 'Processing...');
}
function setInputEnabled(on) {
  busy             = !on;
  inp.disabled     = !on;
  btnSend.disabled = !on;
}

/* ================================================================
   Transizioni sidebar
   ================================================================ */
function showPreUI() {
  secPre.style.display  = 'flex';
  secGame.style.display = 'none';
  gameState = 'idle';
  setInputEnabled(false);
  inp.placeholder = 'Select a script to begin...';
  hud.textContent = 'Select a script and press Start, or load a saved game.';
}

function showGameUI(scriptName, saveName) {
  secPre.style.display  = 'none';
  secGame.style.display = 'flex';
  infoScript.textContent = scriptName || 'sconosciuto';
  infoSave.textContent   = saveName   || '(default)';
}

/* ================================================================
   Image utilities
   ================================================================ */

// Mostra un'immagine base64 nel log con didascalia opzionale
function addImage(b64, caption, mime, tooltip) {
  mime = mime || 'image/png';
  const wrap = document.createElement('div');
  wrap.className = 'msg msg-image';
  const img = document.createElement('img');
  img.src = 'data:' + mime + ';base64,' + b64;
  img.alt = caption || 'Scene';
  if (tooltip) img.title = tooltip;
  wrap.appendChild(img);
  if (caption) {
    const cap = document.createElement('div');
    cap.className = 'img-caption';
    cap.textContent = caption;
    wrap.appendChild(cap);
  }
  log.appendChild(wrap);
  log.scrollTop = log.scrollHeight;
  return wrap;
}

// Polling su un job immagine finché non è done/error
// onDone(b64, assetId, prompt) — onError(msg)
function pollImageJob(jobId, onDone, onError, intervalMs) {
  intervalMs = intervalMs || 2500;
  const timer = setInterval(async () => {
    try {
      const r    = await fetch('/api/image/job/' + jobId);
      const data = await r.json();
      if (data.status === 'done') {
        clearInterval(timer);
        onDone(data.image, data.asset_id || null, data.prompt || '');
      } else if (data.status === 'error') {
        clearInterval(timer);
        onError(data.error || 'Image generation error.');
      }
      // 'pending' → continua
    } catch (e) {
      clearInterval(timer);
      onError('Network error during polling: ' + e.message);
    }
  }, intervalMs);
}

// Handles the response from /api/image or /api/generate_asset
// placeholder: DOM element to replace with the image or error message
async function handleImageResponse(data, placeholder, caption) {
  if (!data.success) {
    // Asset mancanti
    if (data.missing && data.missing.length > 0) {
      const div = document.createElement('div');
      div.className = 'msg msg-missing';
      div.innerHTML = '<strong>⚠ Missing assets for this scene:</strong><ul>'
        + data.missing.map(m =>
            '<li>' + m.id + ' — <code>' + m.hint + '</code></li>'
          ).join('')
        + '</ul><div class="hint">'
        + (data.hint || 'Generate the missing assets, then try again.')
        + (data.available && data.available.length > 0
            ? '<br>Available: ' + data.available.join(', ')
            + ' — use <code>/image --partial</code> to proceed with these.'
            : '')
        + '</div>';
      if (placeholder) placeholder.replaceWith(div);
      else log.appendChild(div);
      log.scrollTop = log.scrollHeight;
    } else {
      const msg = addMsg('msg-error', '⚠ ' + (data.error || 'Image generation error.'));
      if (placeholder) placeholder.replaceWith(msg);
    }
    return;
  }

  // Job avviato — polling
  const jobId = data.job_id;
  const hint  = data.warning ? '⚠ ' + data.warning : null;

  pollImageJob(jobId,
    (b64, assetId, prompt) => {
      const finalCaption = caption || (assetId ? 'Asset: ' + assetId : 'Generated scene');
      const tooltip = (prompt ? prompt + '\n' : '') + 'ID: ' + jobId;
      const imgEl = addImage(b64, finalCaption + (hint ? '\n' + hint : ''), undefined, tooltip);
      if (placeholder) placeholder.replaceWith(imgEl);
      log.scrollTop = log.scrollHeight;
    },
    (err) => {
      const errEl = addMsg('msg-error', '⚠ ' + err);
      if (placeholder) placeholder.replaceWith(errEl);
    }
  );
}

/* ================================================================
   Load script list
   ================================================================ */
async function loadScripts() {
  try {
    const r    = await fetch('/api/scripts');
    const data = await r.json();
    scriptList.innerHTML = '';
    if (!data.scripts || !data.scripts.length) {
      scriptList.innerHTML = '<li class="list-empty">No scripts found.</li>';
      return;
    }
    data.scripts.forEach(s => {
      const li       = document.createElement('li');
      li.textContent = s;
      li.onclick     = () => {
        scriptList.querySelectorAll('li').forEach(x => x.classList.remove('active-script'));
        li.classList.add('active-script');
        selectedScript    = s;
        btnStart.disabled = false;
      };
      scriptList.appendChild(li);
    });
    scriptList.firstElementChild.click();
  } catch (e) {
    scriptList.innerHTML =
      '<li class="list-empty" style="color:var(--danger)">Load error.</li>';
  }
}

/* ================================================================
   Load save list
   ================================================================ */
async function loadSaves() {
  try {
    const r    = await fetch('/api/saves');
    const data = await r.json();
    saveList.innerHTML = '';
    selectedSave      = null;
    btnLoad.disabled  = true;

    if (!data.saves || !data.saves.length) {
      saveEmpty.style.display = 'block';
      return;
    }
    saveEmpty.style.display = 'none';
    data.saves.forEach(s => {
      const li       = document.createElement('li');
      li.textContent = s;
      li.onclick     = () => {
        saveList.querySelectorAll('li').forEach(x => x.classList.remove('active-save'));
        li.classList.add('active-save');
        selectedSave     = s;
        btnLoad.disabled = false;
      };
      saveList.appendChild(li);
    });
  } catch (e) {
    saveEmpty.style.display = 'block';
    saveEmpty.textContent   = 'Error loading saves.';
  }
}

/* ================================================================
   Command palette
   GET /api/commands — optional; hides section if not implemented
   ================================================================ */
async function loadCommands() {
  sbCmdSection.style.display = 'none';
  sbCmdList.innerHTML = '';
  try {
    const r    = await fetch('/api/commands');
    const data = await r.json();
    if (!data.success || !data.commands || !data.commands.length) return;
    data.commands.forEach(c => {
      const li    = document.createElement('li');
      li.textContent = c.label || c.cmd;
      li.title       = c.desc || c.cmd;
      li.onclick     = () => {
        if (c.exec) {
          inp.value = c.cmd;
          inp.dispatchEvent(new Event('input'));
          sendInput();
        } else {
          inp.value = c.cmd + ' ';
          inp.dispatchEvent(new Event('input'));
          inp.focus();
          inp.setSelectionRange(inp.value.length, inp.value.length);
        }
      };
      sbCmdList.appendChild(li);
    });
    sbCmdSection.style.display = 'flex';
  } catch (_) {}
}

/* ================================================================
   NEW GAME
   POST /api/start → riceve welcome, entra in awaiting_init
   ================================================================ */
btnStart.addEventListener('click', async () => {
  if (!selectedScript) return;
  btnStart.disabled = true;
  log.innerHTML     = '';
  hud.textContent   = '...';
  setInputEnabled(false);

  try {
    const r = await fetch('/api/start', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ script: selectedScript })
    });
    const data = await r.json();
    if (!data.success) {
      addMsg('msg-error', 'Start error: ' + (data.error || 'unknown'));
      btnStart.disabled = false;
      return;
    }

    addMsg('msg-narration', data.welcome);
    hud.textContent = data.display || '';
    gameState       = 'awaiting_init';
    inp.placeholder = 'Reply to the script (empty = use default)...';
    showGameUI(selectedScript, null);
    loadCommands();
    setInputEnabled(true);
    inp.focus();

  } catch (e) {
    addMsg('msg-error', 'Network error: ' + e.message);
    btnStart.disabled = false;
  }
});

/* ================================================================
   LOAD GAME
   POST /api/load → restore session, enter playing state
   ================================================================ */
btnLoad.addEventListener('click', async () => {
  if (!selectedSave) return;
  btnLoad.disabled = true;
  log.innerHTML    = '';
  hud.textContent  = '...';
  setInputEnabled(false);
  addMsg('msg-system', 'Loading: ' + selectedSave);

  try {
    const r = await fetch('/api/load', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ save: selectedSave })
    });
    const data = await r.json();
    if (!data.success) {
      addMsg('msg-error', 'Load error: ' + (data.error || 'unknown'));
      btnLoad.disabled = false;
      return;
    }

    hud.textContent = data.display || '';
    gameState       = 'playing';
    inp.placeholder = 'What do you do?';
    showGameUI(data.script || selectedSave, selectedSave);
    loadCommands();

    // Correlate images with turns by timestamp, then replay chat with images inline.
    // Turn timestamps: "YYYY-MM-DDTHH:MM:SSZ" (UTC)
    // Image generated_at: "YYYYMMDD_HHMMSS" (local) — strip non-digits for comparison.
    function normTs(s) { return s ? s.replace(/\D/g, '') : ''; }

    const turns      = data.turns        || [];
    // Server already filters cached_images to this session's date range.
    const cachedImgs = data.cached_images || [];

    // Map each image to the turn index after which it was generated.
    // image goes after turn[i] if turn[i].timestamp <= img.ts < turn[i+1].timestamp
    // image goes after last turn if img.ts >= last turn timestamp
    // image goes before all turns (idx = -1) if img.ts < first turn timestamp
    function assignImageToTurn(imgTs) {
      if (turns.length === 0) return -1;
      const nImg = normTs(imgTs);
      for (let i = turns.length - 1; i >= 0; i--) {
        if (normTs(turns[i].timestamp) <= nImg) return i;
      }
      return -1;
    }

    // Build map: turn index → [image, ...]
    const imgAfterTurn = {};
    for (const img of cachedImgs) {
      const idx = assignImageToTurn(img.generated_at);
      if (!imgAfterTurn[idx]) imgAfterTurn[idx] = [];
      imgAfterTurn[idx].push(img);
    }

    // Async image loader — appends to log in order, with tooltip (prompt + file ID)
    async function loadAndShowImages(imgList) {
      for (const img of imgList) {
        try {
          const r2 = await fetch('/api/scene_image?file=' + encodeURIComponent(img.file));
          const d2 = await r2.json();
          if (d2.success) {
            const tooltip = (img.prompt ? img.prompt + '\n' : '') + 'ID: ' + (img.cache_key || img.file);
            addImage(d2.image, img.generated_at || 'Scene', d2.mime, tooltip);
          }
        } catch (_) {}
      }
    }

    const replayAsync = async () => {
      if (turns.length > 0) {
        addMsg('msg-system', '── Storia precedente (' + turns.length + ' turni) ──');
        // Images that predate all turns
        if (imgAfterTurn[-1]) await loadAndShowImages(imgAfterTurn[-1]);
        for (let i = 0; i < turns.length; i++) {
          const t = turns[i];
          if (t.player_input) addMsg('msg-player', t.player_input);
          if (t.narration)    addMsg('msg-narration', t.narration);
          if (imgAfterTurn[i]) await loadAndShowImages(imgAfterTurn[i]);
        }
        addMsg('msg-system', '── Fine storia precedente ──');
      } else {
        addMsg('msg-system', 'Partita ripristinata.');
      }
    };
    replayAsync();

    setInputEnabled(true);
    inp.focus();

  } catch (e) {
    addMsg('msg-error', 'Network error: ' + e.message);
    btnLoad.disabled = false;
  }
});

/* ================================================================
   MANUAL SAVE
   ================================================================ */
btnManSave.addEventListener('click', async () => {
  try {
    const r    = await fetch('/api/save', { method: 'POST' });
    const data = await r.json();
    addMsg('msg-system',
      data.success ? ('Saved: ' + (data.message || 'ok'))
                   : ('Save error: ' + (data.error || '?')));
  } catch (e) {
    addMsg('msg-error', 'Network error while saving.');
  }
});

/* ================================================================
   QUIT GAME
   ================================================================ */
btnQuit.addEventListener('click', () => {
  if (!confirm('Quit the current game?\nUnsaved progress will be lost.'))
    return;
  log.innerHTML = '';
  showPreUI();
  loadSaves();
});

/* ================================================================
   INVIO INPUT
   Gestisce i tre stati: idle (bloccato), awaiting_init, playing
   ================================================================ */
async function sendInput() {
  const text = inp.value.trim();
  if (gameState === 'idle')                       return;
  if (gameState === 'playing' && !text)           return;
  if (busy)                                       return;

  inp.value        = '';
  inp.style.height = '';

  if (text) {
    inputHistory.unshift(text);
    if (inputHistory.length > 50) inputHistory.pop();
  }
  histIdx = -1;

  /* ---------- AWAITING_INIT ---------- */
  if (gameState === 'awaiting_init') {
    if (text) addMsg('msg-player', text);
    setInputEnabled(false);
    const thinking = addThinking();
    try {
      const r = await fetch('/api/init', {
        method:  'POST',
        headers: { 'Content-Type': 'application/json' },
        body:    JSON.stringify({ input: text })   // vuoto → generate_initial_state
      });
      const data = await r.json();
      thinking.remove();
      if (!data.success) {
        addMsg('msg-error', 'Init error: ' + (data.error || '?'));
        setInputEnabled(true);
        return;
      }
      hud.textContent = data.display || '';
      gameState       = 'playing';
      inp.placeholder = 'What do you do?';
    } catch (e) {
      thinking.remove();
      addMsg('msg-error', 'Network error: ' + e.message);
    }
    setInputEnabled(true);
    inp.focus();
    return;
  }

  /* ---------- PLAYING ---------- */
  addMsg('msg-player', text);
  setInputEnabled(false);
  const thinking = addThinking();

  const isCommand = text.startsWith('/');

  // /show_asset usa GET con query param; /image usa POST dedicato
  let endpoint, fetchOpts;
  if (text.startsWith('/show_asset')) {
    const id = text.split(' ').slice(1).join(' ').trim();
    endpoint  = '/api/show_asset?id=' + encodeURIComponent(id);
    fetchOpts = { method: 'GET' };
  } else if (text.startsWith('/image')) {
    const partial = text.includes('--partial');
    const parts   = text.trim().split(/\s+/);
    const modeArg = (parts.length > 1 && !parts[1].startsWith('--')) ? parts[1] : '';
    const mode    = ['regen', 'refine', 'fix', 'compose'].includes(modeArg) ? modeArg : '';
    const instruction = (mode === 'fix' && parts.length > 2) ? parts.slice(2).join(' ') : '';
    endpoint  = '/api/image';
    fetchOpts = {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ partial, mode, instruction })
    };
  } else if (text.startsWith('/swap')) {
    endpoint  = '/api/swap';
    fetchOpts = {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ input: text })
    };
  } else if (text.startsWith('/generate_asset')) {
    const id = text.split(' ').slice(1).join(' ').trim();
    endpoint  = '/api/generate_asset';
    fetchOpts = {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ id })
    };
  } else {
    endpoint  = isCommand ? '/api/command' : '/api/chat';
    fetchOpts = {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ input: text })
    };
  }

  try {
    const r = await fetch(endpoint, fetchOpts);
    const data = await r.json();
    thinking.remove();

    if (!data.success) {
      if (data.missing && data.missing.length > 0) {
        // /image with missing assets — show the list with generate hints
        await handleImageResponse(data, null, null);
      } else {
        addMsg('msg-error', ' ' + (data.error || 'Unknown error'));
      }
    } else if (isCommand) {
      // /fix responds with narration; other commands with output
      if (data.narration !== undefined) {
        addMsg('msg-narration', data.narration);
        if (data.display) hud.textContent = data.display;
        if (data.game_over) { handleGameOver(data.game_over_reason); return; }
      } else if (data.image !== undefined) {
        // /show_asset — synchronous image already ready
        addImage(data.image, 'Asset: ' + (data.asset_id || text), data.mime);
      } else if (data.job_id !== undefined) {
        // /image, /generate_asset, /swap — async, poll for result
        const ph  = addMsg('msg-system thinking', 'Generating image...');
        const cap = text.startsWith('/generate_asset')
          ? 'Asset: ' + text.split(' ').slice(1).join(' ')
          : text.startsWith('/swap') ? 'Face swap'
          : 'Scene';
        await handleImageResponse(data, ph, cap);
      } else {
        addMsg('msg-command', data.output || '(no output)');
        if (data.display) hud.textContent = data.display;
      }
    } else {
      addMsg('msg-narration', data.narration || '');
      if (data.display) hud.textContent = data.display;
      if (data.game_over) { handleGameOver(data.game_over_reason); return; }
    }
  } catch (e) {
    thinking.remove();
    addMsg('msg-error', 'Network error: ' + e.message);
  }

  setInputEnabled(true);
  inp.focus();
}

function handleGameOver(reason) {
  addMsg('msg-gameover', 'THE END\n' + (reason || ''));
  setInputEnabled(false);
  gameState = 'idle';
}

/* ================================================================
   Event listeners
   ================================================================ */
btnSend.addEventListener('click', sendInput);

inp.addEventListener('keydown', e => {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    sendInput();
    return;
  }
  if (e.key === 'ArrowUp') {
    e.preventDefault();
    if (histIdx < inputHistory.length - 1) {
      histIdx++;
      inp.value = inputHistory[histIdx];
    }
    return;
  }
  if (e.key === 'ArrowDown') {
    e.preventDefault();
    if (histIdx > 0) { histIdx--; inp.value = inputHistory[histIdx]; }
    else             { histIdx = -1; inp.value = ''; }
    return;
  }
});

inp.addEventListener('input', () => {
  inp.style.height = 'auto';
  inp.style.height = Math.min(inp.scrollHeight, 160) + 'px';
});

/* ================================================================
   Startup — with automatic retry on temporary error
   ================================================================ */
async function initWithRetry() {
  // Try loadScripts with retry (max 5 attempts, 1s interval)
  let attempts = 0;
  const tryLoad = async () => {
    attempts++;
    try {
      const r    = await fetch('/api/scripts');
      const data = await r.json();
      scriptList.innerHTML = '';
      if (!data.success) {
        // Show detailed error instead of failing silently
        const msg = data.error || 'Unknown error';
        scriptList.innerHTML =
          '<li class="list-empty" style="color:var(--danger)">Error: ' + msg + '</li>';
        console.error('[scripts] Server error:', msg);
        return;
      }
      if (!data.scripts || !data.scripts.length) {
        scriptList.innerHTML =
          '<li class="list-empty">No scripts found in: ' + (data.path || 'basePath') + '</li>';
        return;
      }
      data.scripts.forEach(s => {
        const li       = document.createElement('li');
        li.textContent = s;
        li.onclick     = () => {
          scriptList.querySelectorAll('li').forEach(x => x.classList.remove('active-script'));
          li.classList.add('active-script');
          selectedScript    = s;
          btnStart.disabled = false;
        };
        scriptList.appendChild(li);
      });
      scriptList.firstElementChild.click();
    } catch (e) {
      if (attempts < 5) {
        console.warn('[scripts] Retry', attempts, e.message);
        setTimeout(tryLoad, 1000);
      } else {
        scriptList.innerHTML =
          '<li class="list-empty" style="color:var(--danger)">Server non raggiungibile.</li>';
      }
    }
  };
  tryLoad();

  // loadSaves non richiede retry aggressivo — fallimento silenzioso è ok
  try {
    const r    = await fetch('/api/saves');
    const data = await r.json();
    saveList.innerHTML = '';
    selectedSave      = null;
    btnLoad.disabled  = true;
    if (!data.saves || !data.saves.length) {
      saveEmpty.style.display = 'block';
      return;
    }
    saveEmpty.style.display = 'none';
    data.saves.forEach(s => {
      const li       = document.createElement('li');
      li.textContent = s;
      li.onclick     = () => {
        saveList.querySelectorAll('li').forEach(x => x.classList.remove('active-save'));
        li.classList.add('active-save');
        selectedSave     = s;
        btnLoad.disabled = false;
      };
      saveList.appendChild(li);
    });
  } catch (e) {
    saveEmpty.style.display = 'block';
    saveEmpty.textContent   = 'Error loading saves.';
  }
}

initWithRetry();
</script>
</body>
</html>)HTML";
