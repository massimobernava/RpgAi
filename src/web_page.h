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
                   color: var(--text); white-space: pre-wrap; position: relative; }
  .tts-btn { position: absolute; top: 8px; right: 8px; background: none; border: none;
             cursor: pointer; font-size: 14px; opacity: 0.3; transition: opacity .2s;
             padding: 2px 4px; line-height: 1; }
  .tts-btn:hover { opacity: 1; }
  .tts-btn.loading { opacity: 0.6; cursor: wait; }
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

  /* ---- Suggested action buttons ---- */
  .suggested-actions { display: flex; flex-wrap: wrap; gap: 6px;
                        padding: 4px 0 8px 0; max-width: 820px; }
  .btn-suggestion    { background: rgba(124,111,205,.12); border: 1px solid var(--accent);
                        color: var(--accent); border-radius: var(--radius);
                        padding: 5px 12px; font-size: 12px; cursor: pointer;
                        transition: background .15s; text-align: left; }
  .btn-suggestion:hover { background: rgba(124,111,205,.28); }

  /* ---- Image messages ---- */
  .msg-image { background: var(--surface); border: 1px solid var(--border);
               padding: 8px; border-radius: var(--radius); max-width: 820px; }
  .msg-image img { max-width: 100%; border-radius: calc(var(--radius) - 2px);
                   display: block; }
  .msg-image .img-caption { font-size: 11px; color: var(--dim);
                              margin-top: 6px; padding: 0 2px; }
  /* Slideshow */
  .ss-controls { display: flex; align-items: center; justify-content: center;
                 gap: 8px; margin-top: 8px; }
  .ss-btn { background: var(--surface2); border: 1px solid var(--border);
            color: var(--fg); border-radius: 6px; padding: 4px 10px;
            cursor: pointer; font-size: 14px; line-height: 1; }
  .ss-btn:hover { background: var(--primary); color: #000; }
  .ss-btn:disabled { opacity: 0.3; cursor: default; }
  .ss-dots { display: flex; gap: 5px; align-items: center; }
  .ss-dot  { width: 7px; height: 7px; border-radius: 50%;
             background: var(--border); cursor: pointer; transition: background .2s; }
  .ss-dot.active { background: var(--primary); }
  .ss-counter { font-size: 11px; color: var(--dim); min-width: 36px; text-align: right; }
  .ss-play { min-width: 34px; }
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

  /* ---- Settings Modal ---- */
  #settings-overlay { display: none; position: fixed; inset: 0; background: rgba(0,0,0,.72);
                      z-index: 1000; align-items: center; justify-content: center; }
  #settings-overlay.open { display: flex; }
  #settings-modal { background: var(--surface); border: 1px solid var(--border);
                    border-radius: var(--radius); width: 680px; max-width: 96vw;
                    max-height: 88vh; display: flex; flex-direction: column; overflow: hidden; }
  #settings-header { display: flex; align-items: center; justify-content: space-between;
                     padding: 14px 18px; border-bottom: 1px solid var(--border); flex-shrink: 0; }
  #settings-header h3 { color: var(--accent); font-size: 14px; margin: 0; letter-spacing: .04em; }
  #settings-close { background: none; border: none; color: var(--dim); font-size: 18px;
                    cursor: pointer; padding: 2px 8px; border-radius: var(--radius); }
  #settings-close:hover { color: var(--text); background: rgba(255,255,255,.05); }

  .settings-tabs { display: flex; border-bottom: 1px solid var(--border); flex-shrink: 0;
                   padding: 0 8px; }
  .settings-tab  { padding: 10px 14px; font-size: 12px; font-family: var(--mono);
                   color: var(--dim); cursor: pointer; border-bottom: 2px solid transparent;
                   transition: all .15s; }
  .settings-tab:hover  { color: var(--text); }
  .settings-tab.active { color: var(--accent); border-bottom-color: var(--accent); }

  .settings-body   { flex: 1; overflow-y: auto; padding: 18px; }
  .settings-panel  { display: none; flex-direction: column; gap: 14px; }
  .settings-panel.active { display: flex; }

  .sfield { display: flex; flex-direction: column; gap: 4px; }
  .sfield label { font-size: 11px; color: var(--dim); text-transform: uppercase;
                  letter-spacing: .05em; }
  .sfield input, .sfield select {
    background: var(--bg); border: 1px solid var(--border);
    border-radius: var(--radius); color: var(--text); font-family: var(--mono);
    font-size: 13px; padding: 7px 10px; width: 100%; }
  .sfield input:focus, .sfield select:focus { outline: none; border-color: var(--accent); }
  .sfield input[type="number"] { width: 110px; }
  .sfield input[type="range"]  { width: 100%; cursor: pointer; accent-color: var(--accent);
                                  padding: 0; margin-top: 4px; }

  .srow { display: flex; gap: 12px; }
  .srow .sfield { flex: 1; min-width: 0; }

  .server-card { background: var(--bg); border: 1px solid var(--border);
                 border-radius: var(--radius); padding: 14px;
                 display: flex; flex-direction: column; gap: 10px; }
  .server-card h4 { font-size: 13px; color: var(--text); margin: 0; }
  .server-status  { display: inline-flex; align-items: center; gap: 6px;
                    font-size: 12px; color: var(--dim); }
  .server-dot     { width: 8px; height: 8px; border-radius: 50%;
                    background: var(--border); flex-shrink: 0; }
  .server-dot.up   { background: var(--accent2); }
  .server-dot.down { background: var(--danger); }
  .server-actions  { display: flex; gap: 8px; flex-wrap: wrap; }
  .server-actions .btn { width: auto; padding: 6px 14px; font-size: 11px; }

  #settings-footer { padding: 12px 18px; border-top: 1px solid var(--border);
                     display: flex; gap: 10px; justify-content: flex-end; flex-shrink: 0; }
  #settings-footer .btn { width: auto; padding: 8px 22px; }

  .btn-settings-sb { background: transparent; color: var(--accent);
                     border: 1px solid var(--accent); }
  .btn-settings-sb:hover { background: rgba(124,111,205,.12); }

  /* Script list items with download button */
  .item-list li { display: flex; align-items: center; gap: 4px; }
  .item-list li .script-name { flex: 1; overflow: hidden; text-overflow: ellipsis;
                                white-space: nowrap; cursor: pointer; }
  .script-dl-btn { flex-shrink: 0; background: none; border: none; color: var(--border);
                   cursor: pointer; padding: 1px 4px; border-radius: 3px; font-size: 13px;
                   line-height: 1; transition: color .15s, background .15s; }
  .script-dl-btn:hover { color: var(--accent2); background: rgba(79,195,161,.15); }

  /* Upload button */
  .btn-upload { background: transparent; color: var(--accent2);
                border: 1px solid var(--accent2); font-size: 11px; padding: 6px; }
  .btn-upload:hover { background: rgba(79,195,161,.1); }
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
      <input type="file" id="script-file-input" accept=".lua" style="display:none">
      <button id="btn-upload-script" class="btn btn-upload">⬆ Upload .lua</button>

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
    <hr class="sb-sep">
    <button id="btn-settings" class="btn btn-settings-sb">⚙ Settings</button>
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

  <!-- ===== SETTINGS MODAL ===== -->
  <div id="settings-overlay">
    <div id="settings-modal">
      <div id="settings-header">
        <h3>⚙ Settings</h3>
        <button id="settings-close">✕</button>
      </div>
      <div class="settings-tabs">
        <div class="settings-tab active" data-tab="llm">LLM</div>
        <div class="settings-tab" data-tab="img-t2i">Image T2I</div>
        <div class="settings-tab" data-tab="img-i2i">Image I2I</div>
        <div class="settings-tab" data-tab="session">Session</div>
        <div class="settings-tab" data-tab="servers">Local Servers</div>
      </div>
      <div class="settings-body">

        <!-- TAB: LLM -->
        <div class="settings-panel active" id="tab-llm">
          <div class="sfield">
            <label>Provider</label>
            <select id="s-provider">
              <option value="ollama">Ollama (local)</option>
              <option value="openrouter">OpenRouter</option>
              <option value="openai">OpenAI</option>
              <option value="claude">Claude</option>
              <option value="gemini">Gemini</option>
            </select>
          </div>
          <div id="s-sec-ollama" class="provider-section">
            <div class="srow">
              <div class="sfield"><label>Model</label><input id="s-ollama-model" type="text" placeholder="dolphin3:latest"></div>
              <div class="sfield"><label>URL</label><input id="s-ollama-url" type="text" placeholder="http://localhost:11434"></div>
            </div>
          </div>
          <div id="s-sec-openrouter" class="provider-section" style="display:none">
            <div class="srow">
              <div class="sfield"><label>Model</label><input id="s-or-model" type="text" placeholder="qwen/qwen3-32b"></div>
              <div class="sfield"><label>API Key</label><input id="s-or-key" type="password" placeholder="sk-or-..."></div>
            </div>
          </div>
          <div id="s-sec-openai" class="provider-section" style="display:none">
            <div class="srow">
              <div class="sfield"><label>Model</label><input id="s-oai-model" type="text" placeholder="gpt-4o-mini"></div>
              <div class="sfield"><label>API Key</label><input id="s-oai-key" type="password" placeholder="sk-..."></div>
            </div>
            <div class="sfield"><label>URL (optional override)</label><input id="s-oai-url" type="text"></div>
          </div>
          <div id="s-sec-claude" class="provider-section" style="display:none">
            <div class="srow">
              <div class="sfield"><label>Model</label><input id="s-claude-model" type="text" placeholder="claude-haiku-4-5-20251001"></div>
              <div class="sfield"><label>API Key</label><input id="s-claude-key" type="password" placeholder="sk-ant-..."></div>
            </div>
          </div>
          <div id="s-sec-gemini" class="provider-section" style="display:none">
            <div class="srow">
              <div class="sfield"><label>Model</label><input id="s-g-model" type="text" placeholder="gemini-flash-latest"></div>
              <div class="sfield"><label>API Key</label><input id="s-g-key" type="password"></div>
            </div>
          </div>
        </div>

        <!-- TAB: Image T2I -->
        <div class="settings-panel" id="tab-img-t2i">
          <div class="srow">
            <div class="sfield">
              <label>Provider (empty = disabled)</label>
              <select id="s-img-provider">
                <option value="">Disabled</option>
                <option value="sdcpp_local">stable-diffusion.cpp (local)</option>
                <option value="openai">OpenAI</option>
                <option value="openrouter">OpenRouter</option>
                <option value="fal">fal.ai</option>
                <option value="wavespeed">WaveSpeed</option>
                <option value="dashscope">DashScope</option>
                <option value="aimlapi">AIMLAPI</option>
              </select>
            </div>
            <div class="sfield"><label>URL</label><input id="s-img-url" type="text" placeholder="http://localhost:7860"></div>
          </div>
          <div class="srow">
            <div class="sfield"><label>API Key</label><input id="s-img-key" type="password"></div>
            <div class="sfield"><label>T2I Model</label><input id="s-img-t2i-model" type="text" placeholder="blank = provider default"></div>
          </div>
          <div class="srow">
            <div class="sfield"><label>Width</label><input id="s-img-width" type="number" min="256" max="2048" step="64"></div>
            <div class="sfield"><label>Height</label><input id="s-img-height" type="number" min="256" max="2048" step="64"></div>
            <div class="sfield"><label>Steps</label><input id="s-img-steps" type="number" min="1" max="150"></div>
          </div>
        </div>

        <!-- TAB: Image I2I -->
        <div class="settings-panel" id="tab-img-i2i">
          <div class="srow">
            <div class="sfield">
              <label>I2I Provider <small style="color:var(--muted)">(blank = same as T2I)</small></label>
              <select id="s-img-i2i-provider">
                <option value="">Same as T2I</option>
                <option value="sdcpp_local">stable-diffusion.cpp (local)</option>
                <option value="openai">OpenAI</option>
                <option value="openrouter">OpenRouter</option>
                <option value="fal">fal.ai</option>
                <option value="wavespeed">WaveSpeed</option>
                <option value="dashscope">DashScope</option>
                <option value="aimlapi">AIMLAPI</option>
                <option value="qwen_local">Qwen Local</option>
              </select>
            </div>
            <!-- hidden for providers that ignore the model field (qwen_local, sdcpp_local) -->
            <div class="sfield" id="s-img-i2i-model-row">
              <label>I2I Model <span id="s-img-i2i-hint" style="color:var(--accent2);font-size:10px"></span></label>
              <input id="s-img-i2i-model" type="text" placeholder="blank = provider default">
            </div>
          </div>
          <div class="srow">
            <div class="sfield"><label>I2I URL</label><input id="s-img-i2i-url" type="text" placeholder=""></div>
            <div class="sfield"><label>I2I Key</label><input id="s-img-i2i-key" type="password"></div>
          </div>
          <div class="srow">
            <div class="sfield">
              <label>Steps (I2I): <span id="s-img-i2i-steps-hint" style="color:var(--accent2);font-size:10px"></span></label>
              <input id="s-img-i2i-steps" type="number" min="0" max="150" placeholder="0 = same as T2I steps">
            </div>
            <div class="sfield">
              <label>Guidance Scale: <span id="s-img-guidance-scale-val">1.00</span> <span id="s-img-gs-hint" style="color:var(--accent2);font-size:10px"></span></label>
              <input id="s-img-guidance-scale" type="range" min="0.5" max="10" step="0.1" value="1.0">
            </div>
          </div>
          <div class="sfield">
            <label>Strength: <span id="s-img-strength-val">0.75</span></label>
            <input id="s-img-strength" type="range" min="0" max="1" step="0.01" value="0.75">
            <small style="color:var(--muted)">how much i2i deviates from input (0 = copy, 1 = ignore)</small>
          </div>
          <div class="srow">
            <div class="sfield"><label>LoRA <small style="color:var(--muted)">local dir or https:// URL</small></label><input id="s-img-lora" type="text" placeholder="subfolder name or CivitAI URL"></div>
            <div class="sfield">
              <label>LoRA Scale: <span id="s-img-lora-scale-val">1.00</span></label>
              <input id="s-img-lora-scale" type="range" min="0" max="2" step="0.05" value="1">
            </div>
          </div>
          <!-- LoRA Model: WaveSpeed only (selects model variant for /image lora) -->
          <div class="srow" id="s-img-lora-model-row" style="display:none">
            <div class="sfield"><label>LoRA Model <small style="color:var(--muted)">(WaveSpeed: model for /image lora)</small></label><input id="s-img-lora-model" type="text"></div>
          </div>
        </div>

        <!-- TAB: Session -->
        <div class="settings-panel" id="tab-session">
          <div class="srow">
            <div class="sfield"><label>Max History</label><input id="s-max-history" type="number" min="1" max="200"></div>
            <div class="sfield"><label>Max Retries</label><input id="s-max-retries" type="number" min="1" max="20"></div>
          </div>
          <div class="sfield">
            <label>Scripts Path (base_path)</label>
            <input id="s-base-path" type="text" placeholder="./scripts/">
          </div>
          <div class="srow">
            <div class="sfield">
              <label>Save Mode</label>
              <select id="s-save-mode">
                <option value="last">last (overwrite)</option>
                <option value="full">full (append)</option>
              </select>
            </div>
            <div class="sfield"><label>Save Path</label><input id="s-save-path" type="text" placeholder="saves/"></div>
          </div>
          <div class="srow">
            <div class="sfield"><label>RAG File</label><input id="s-rag-file" type="text"></div>
            <div class="sfield"><label>RAG Examples</label><input id="s-rag-examples" type="number" min="1" max="20"></div>
          </div>
          <div class="srow">
            <div class="sfield"><label>Embed Provider</label><input id="s-embed-provider" type="text" placeholder="ollama / openai"></div>
            <div class="sfield"><label>Embed Model</label><input id="s-embed-model" type="text" placeholder="nomic-embed-text"></div>
          </div>
          <div class="srow">
            <div class="sfield"><label>Embed URL</label><input id="s-embed-url" type="text"></div>
            <div class="sfield"><label>Embed Key</label><input id="s-embed-key" type="password"></div>
          </div>
          <div class="sfield" style="max-width:200px">
            <label>Language Code</label>
            <input id="s-lang-code" type="text" placeholder="it / en / fr">
          </div>
        </div>

        <!-- TAB: Local Servers -->
        <div class="settings-panel" id="tab-servers">

          <!-- Python environment selector (shared by all local servers) -->
          <div class="server-card">
            <h4>Python Environment</h4>
            <div class="sfield">
              <label>Type</label>
              <select id="s-py-env-type" onchange="onPyEnvChange()">
                <option value="system">System (python3 / pip3)</option>
                <option value="venv">venv</option>
                <option value="conda">conda</option>
                <option value="uv">uv</option>
              </select>
            </div>
            <div class="sfield" id="py-env-path-row" style="display:none">
              <label id="py-env-path-label">Path / Env name</label>
              <input id="s-py-env-path" type="text" placeholder="">
            </div>
          </div>

          <div class="server-card">
            <div style="display:flex;align-items:center;justify-content:space-between">
              <h4>FaceSwap Locale Server <small style="font-size:10px;color:var(--dim)">port 8001</small></h4>
              <span class="server-status">
                <span id="dot-faceswap" class="server-dot"></span>
                <span id="label-faceswap">—</span>
              </span>
            </div>
            <div class="sfield">
              <label>URL override (saved as faceswap_url)</label>
              <input id="s-faceswap-url" type="text" placeholder="http://localhost:8001">
            </div>
            <div class="server-actions">
              <button class="btn btn-secondary" onclick="serverAction('faceswap_locale','install')">⬇ Install deps</button>
              <button class="btn btn-primary"   onclick="serverAction('faceswap_locale','start')">▶ Start</button>
              <button class="btn btn-ghost"     onclick="serverAction('faceswap_locale','stop')">■ Stop</button>
            </div>
          </div>
          <div class="server-card">
            <div style="display:flex;align-items:center;justify-content:space-between">
              <h4>Qwen Locale Server <small style="font-size:10px;color:var(--dim)">port 8000</small></h4>
              <span class="server-status">
                <span id="dot-qwen" class="server-dot"></span>
                <span id="label-qwen">—</span>
              </span>
            </div>
            <div class="sfield">
              <label>Launch args</label>
              <input id="s-qwen-args" type="text" placeholder="--dtype bf16 --host 0.0.0.0 --lightning --fast --cpu-offload">
            </div>
            <div class="server-actions">
              <button class="btn btn-secondary" onclick="serverAction('qwen_locale','install')">⬇ Install deps</button>
              <button class="btn btn-primary"   onclick="serverAction('qwen_locale','start')">▶ Start</button>
              <button class="btn btn-ghost"     onclick="serverAction('qwen_locale','stop')">■ Stop</button>
            </div>
          </div>
          <div class="server-card">
            <div style="display:flex;align-items:center;justify-content:space-between">
              <h4>TTS Locale Server <small style="font-size:10px;color:var(--dim)">port 8004</small></h4>
              <span class="server-status">
                <span id="dot-tts-locale" class="server-dot"></span>
                <span id="label-tts-locale">—</span>
              </span>
            </div>
            <div class="sfield">
              <label>Server URL <small style="color:var(--muted)">(leave empty for localhost)</small></label>
              <input id="s-tts-url" type="text" placeholder="http://SERVER_IP:8004">
            </div>
            <div class="sfield">
              <label>Python env <small style="color:var(--muted)">(overrides global)</small></label>
              <select id="s-tts-locale-env-type" onchange="onTtsEnvChange()">
                <option value="">(use global)</option>
                <option value="system">system</option>
                <option value="venv">venv</option>
                <option value="conda">conda</option>
                <option value="uv">uv</option>
              </select>
            </div>
            <div class="sfield" id="tts-locale-env-path-row" style="display:none">
              <label id="tts-locale-env-path-label">Path / Env name</label>
              <input id="s-tts-locale-env-path" type="text" placeholder="rpgai_tts">
            </div>
            <div class="sfield">
              <label>Narrator voice</label>
              <select id="s-tts-narrator-voice" style="width:100%">
                <option value="">(none — click Refresh)</option>
              </select>
              <div style="display:flex;gap:6px;margin-top:4px">
                <button class="btn btn-ghost" style="flex:1" onclick="refreshTtsVoices(true)">↻ Refresh voices</button>
                <button class="btn btn-ghost" style="flex:1" onclick="testNarratorVoice()">▶ Test voice</button>
              </div>
            </div>
            <div class="sfield">
              <label>Test sentence</label>
              <input id="s-tts-test-text" type="text" value="Ciao. Sono il narratore di questa avventura." style="width:100%">
            </div>
            <div class="sfield">
              <label>Launch args</label>
              <input id="s-tts-locale-args" type="text" placeholder="--host 0.0.0.0">
            </div>
            <div class="server-actions">
              <button class="btn btn-secondary" onclick="serverAction('tts_locale','install')">⬇ Install deps</button>
              <button class="btn btn-primary"   onclick="serverAction('tts_locale','start')">▶ Start</button>
              <button class="btn btn-ghost"     onclick="serverAction('tts_locale','stop')">■ Stop</button>
            </div>
          </div>
          <div class="server-card">
            <div style="display:flex;align-items:center;justify-content:space-between">
              <h4>T2I Locale Server <small style="font-size:10px;color:var(--dim)">port 8003</small></h4>
              <span class="server-status">
                <span id="dot-t2i" class="server-dot"></span>
                <span id="label-t2i">—</span>
              </span>
            </div>
            <div class="sfield" id="t2i-model-row" style="display:none">
              <label>Model</label>
              <select id="s-t2i-model" onchange="onT2iModelChange()">
                <option value="">(auto)</option>
              </select>
              <button class="btn btn-ghost" style="margin-left:6px;padding:2px 8px;font-size:11px" onclick="fetchT2iModels()">↻</button>
            </div>
            <div class="server-actions">
              <button class="btn btn-secondary" onclick="serverAction('t2i_locale','install')">⬇ Install deps</button>
              <button class="btn btn-primary"   onclick="serverAction('t2i_locale','start')">▶ Start</button>
              <button class="btn btn-ghost"     onclick="serverAction('t2i_locale','stop')">■ Stop</button>
            </div>
          </div>
        </div>

      </div><!-- /settings-body -->
      <div id="settings-footer">
        <button class="btn btn-ghost" onclick="closeSettings()">Cancel</button>
        <button class="btn btn-primary" onclick="saveSettings()">Save</button>
      </div>
    </div><!-- /settings-modal -->
  </div><!-- /settings-overlay -->

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
let bgAudio        = null;   // current looping background audio
let activeAudios   = [];     // all playing audio instances
let ttsNarratorVoice = '';   // set from settings; empty = TTS disabled in UI
const ttsCache     = new Map(); // key: voice|text → ArrayBuffer (session-local cache)

function stopAllAudio() {
  activeAudios.forEach(a => { try { a.pause(); } catch(e){} });
  activeAudios = [];
  bgAudio = null;
}

// History input (frecce su/giu come una shell)
let inputHistory = [];
let histIdx      = -1;

/* ================================================================
   Utilities
   ================================================================ */
function addMsg(cls, text) {
  const d = document.createElement('div');
  d.className = 'msg ' + cls;
  if (cls === 'msg-narration' && ttsNarratorVoice) {
    const span = document.createElement('span');
    span.textContent = text;
    d.appendChild(span);
    const btn = document.createElement('button');
    btn.className = 'tts-btn';
    btn.title = 'Play narration';
    btn.textContent = '🔊';
    btn.onclick = () => playTTS(text, btn);
    d.appendChild(btn);
  } else {
    d.textContent = text;
  }
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

// Slideshow: slides = [{b64, mime}, ...], autoPlay = bool, intervalMs = int
function createSlideshow(slides, caption, autoPlay, intervalMs) {
  intervalMs = intervalMs || 3000;
  let current = 0;
  let timer   = null;

  const wrap = document.createElement('div');
  wrap.className = 'msg msg-image';

  const imgEl = document.createElement('img');
  imgEl.style.cssText = 'max-width:100%;border-radius:4px;display:block;';
  imgEl.src = 'data:' + (slides[0].mime||'image/png') + ';base64,' + slides[0].b64;
  wrap.appendChild(imgEl);

  // Controls (hidden if single slide)
  const controls = document.createElement('div');
  controls.className = 'ss-controls';

  const prevBtn = document.createElement('button');
  prevBtn.className = 'ss-btn';
  prevBtn.textContent = '◀';

  const dotsEl = document.createElement('div');
  dotsEl.className = 'ss-dots';
  slides.forEach((_, i) => {
    const d = document.createElement('span');
    d.className = 'ss-dot' + (i === 0 ? ' active' : '');
    d.onclick = () => { stopAuto(); goTo(i); };
    dotsEl.appendChild(d);
  });

  const nextBtn = document.createElement('button');
  nextBtn.className = 'ss-btn';
  nextBtn.textContent = '▶';

  const playBtn = document.createElement('button');
  playBtn.className = 'ss-btn ss-play';
  playBtn.textContent = '⏸';

  const counter = document.createElement('div');
  counter.className = 'ss-counter';
  counter.textContent = '1 / ' + slides.length;

  controls.append(prevBtn, dotsEl, nextBtn, playBtn, counter);

  function goTo(idx) {
    current = ((idx % slides.length) + slides.length) % slides.length;
    const s = slides[current];
    imgEl.src = 'data:' + (s.mime||'image/png') + ';base64,' + s.b64;
    counter.textContent = (current + 1) + ' / ' + slides.length;
    dotsEl.querySelectorAll('.ss-dot').forEach((d, i) =>
      d.classList.toggle('active', i === current));
    prevBtn.disabled = current === 0 && !timer;
    nextBtn.disabled = current === slides.length - 1 && !timer;
  }

  function startAuto() {
    if (timer) clearInterval(timer);
    timer = setInterval(() => {
      if (current < slides.length - 1) { goTo(current + 1); }
      else { stopAuto(); }
    }, intervalMs);
    playBtn.textContent = '⏸';
    prevBtn.disabled = false;
    nextBtn.disabled = false;
  }

  function stopAuto() {
    if (timer) { clearInterval(timer); timer = null; }
    playBtn.textContent = '⏵';
    prevBtn.disabled = current === 0;
    nextBtn.disabled = current === slides.length - 1;
  }

  prevBtn.onclick = () => { stopAuto(); goTo(current - 1); };
  nextBtn.onclick = () => { stopAuto(); goTo(current + 1); };
  playBtn.onclick = () => { if (timer) stopAuto(); else startAuto(); };

  if (slides.length > 1) {
    wrap.appendChild(controls);
    if (autoPlay) startAuto();
    else { prevBtn.disabled = true; nextBtn.disabled = false; }
  }

  if (caption) {
    const capEl = document.createElement('div');
    capEl.className = 'img-caption';
    capEl.textContent = caption;
    wrap.appendChild(capEl);
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
    // Turn timestamps: "YYYY-MM-DDTHH:MM:SSZ" (UTC ISO)
    // img.utc_at: "YYYY-MM-DDTHH:MM:SSZ" (UTC ISO, always — server normalises old entries)
    function normTs(s) { return s ? s.replace(/\D/g, '') : ''; }

    const turns      = data.turns        || [];
    // Server already filters cached_images to this session's date range.
    const cachedImgs = data.cached_images || [];

    // Map each image to the turn index after which it was generated.
    // image goes after turn[i] if turn[i].timestamp <= img.ts < turn[i+1].timestamp
    // image goes after last turn if img.ts >= last turn timestamp
    // image goes before all turns (idx = -1) if img.ts < first turn timestamp
    function assignImageToTurn(img) {
      // utc_at is always UTC ISO (server converts old local-tz entries on the fly)
      const imgTs = img.utc_at || img.generated_at;
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
      const idx = assignImageToTurn(img);
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
  stopAllAudio();
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

  // /quit and /q: same as the Quit button — stop music and return to pre-screen
  if (text === '/quit' || text === '/q') {
    inp.value = '';
    stopAllAudio();
    log.innerHTML = '';
    showPreUI();
    loadSaves();
    return;
  }

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
    const partial  = text.includes('--partial');
    const parts    = text.trim().split(/\s+/).filter(p => p !== '--partial');
    // "lora" can appear anywhere after /image: /image lora, /image lora regen, /image regen lora
    const lora     = parts.slice(1).includes('lora');
    const modeParts = parts.slice(1).filter(p => p !== 'lora');
    const modeArg  = modeParts.length > 0 ? modeParts[0] : '';
    const mode     = ['regen', 'refine', 'fix', 'compose'].includes(modeArg) ? modeArg : '';
    const instruction = (mode === 'fix' && modeParts.length > 1) ? modeParts.slice(1).join(' ') : '';
    endpoint  = '/api/image';
    fetchOpts = {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ partial, lora, mode, instruction })
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
      await processActions(data.actions);
      showSuggestions(data.suggested_actions);
    }
  } catch (e) {
    thinking.remove();
    addMsg('msg-error', 'Network error: ' + e.message);
  }

  setInputEnabled(true);
  inp.focus();
}

async function processActions(actions) {
  if (!actions || !actions.length) return;
  for (const action of actions) {
    if (action.type === 'image') {
      const path    = action.path    || '';
      const caption = action.caption || '';
      if (!path) continue;
      try {
        const r = await fetch('/api/serve_file?path=' + encodeURIComponent(path));
        const d = await r.json();
        if (d.image) addImage(d.image, caption, d.mime);
      } catch (e) { /* image load failed silently */ }

    } else if (action.type === 'slideshow') {
      const paths    = action.images   || [];
      const caption  = action.caption  || '';
      const autoPlay = action.auto !== false;
      const interval = action.interval || 3000;
      if (!paths.length) continue;
      try {
        const results = await Promise.all(paths.map(async p => {
          const r = await fetch('/api/serve_file?path=' + encodeURIComponent(p));
          const d = await r.json();
          return d.image ? { b64: d.image, mime: d.mime || 'image/png' } : null;
        }));
        const slides = results.filter(Boolean);
        if (slides.length === 1) addImage(slides[0].b64, caption, slides[0].mime);
        else if (slides.length > 1) createSlideshow(slides, caption, autoPlay, interval);
      } catch (e) { console.warn('Slideshow failed:', e); }

    } else if (action.type === 'audio') {
      const path   = action.path   || '';
      const loop   = !!action.loop;
      const volume = (action.volume !== undefined) ? Number(action.volume) : 1.0;
      // Stop all current audio before starting new track
      stopAllAudio();
      if (!path) continue;
      try {
        const r = await fetch('/api/serve_audio?path=' + encodeURIComponent(path));
        if (!r.ok) continue;
        const blob  = await r.blob();
        const url   = URL.createObjectURL(blob);
        const audio = new Audio(url);
        audio.loop   = loop;
        audio.volume = Math.max(0, Math.min(1, volume));
        audio.onended = () => {
          activeAudios = activeAudios.filter(a => a !== audio);
          URL.revokeObjectURL(url);
        };
        audio.play();
        activeAudios.push(audio);
        if (loop) bgAudio = audio;
      } catch (e) { console.warn('Audio action failed:', e); }

    } else if (action.type === 'tts') {
      const text  = action.text  || '';
      const voice = action.voice || '';
      if (!text) continue;
      try {
        const params = new URLSearchParams({ text });
        if (voice) params.append('voice', voice);
        const r = await fetch('/api/tts?' + params.toString());
        if (r.ok) {
          const blob  = await r.blob();
          const url   = URL.createObjectURL(blob);
          const audio = new Audio(url);
          audio.onended = () => URL.revokeObjectURL(url);
          audio.play();
        }
      } catch (e) { console.warn('TTS action failed:', e); }
    }
  }
}

function showSuggestions(actions) {
  // Remove previous suggestion row if present
  const old = document.querySelector('.suggested-actions');
  if (old) old.remove();
  if (!actions || !actions.length) return;
  const row = document.createElement('div');
  row.className = 'suggested-actions';
  actions.forEach(action => {
    const btn = document.createElement('button');
    btn.className = 'btn-suggestion';
    btn.textContent = action;
    btn.onclick = () => {
      inp.value = action;
      row.remove();
      sendChat();
    };
    row.appendChild(btn);
  });
  const logEl = document.getElementById('log');
  logEl.appendChild(row);
  logEl.scrollTop = logEl.scrollHeight;
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
        const li    = document.createElement('li');
        const name  = document.createElement('span');
        name.className   = 'script-name';
        name.textContent = s;
        const dlBtn = document.createElement('button');
        dlBtn.className   = 'script-dl-btn';
        dlBtn.textContent = '⬇';
        dlBtn.title       = 'Download ' + s;
        dlBtn.onclick = e => { e.stopPropagation(); downloadScript(s); };
        li.appendChild(name);
        li.appendChild(dlBtn);
        li.onclick = () => {
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

/* ================================================================
   Script upload / download
   ================================================================ */
function downloadScript(name) {
  const a = document.createElement('a');
  a.href     = '/api/scripts/download?name=' + encodeURIComponent(name);
  a.download = name;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
}

const scriptFileInput   = document.getElementById('script-file-input');
const btnUploadScript   = document.getElementById('btn-upload-script');

btnUploadScript.addEventListener('click', () => scriptFileInput.click());

scriptFileInput.addEventListener('change', async () => {
  const file = scriptFileInput.files[0];
  if (!file) return;
  scriptFileInput.value = '';
  const text = await file.text();
  try {
    const r = await fetch('/api/scripts/upload', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ name: file.name, content: text })
    });
    const d = await r.json();
    if (d.success) {
      addMsg('msg-system', '⬆ Uploaded: ' + file.name);
      initWithRetry();  // refresh script list
    } else {
      addMsg('msg-error', 'Upload failed: ' + (d.error || 'unknown'));
    }
  } catch (e) {
    addMsg('msg-error', 'Upload error: ' + e.message);
  }
});

/* ================================================================
   Settings
   ================================================================ */
const btnSettings     = document.getElementById('btn-settings');
const settingsOverlay = document.getElementById('settings-overlay');

// Fetch narrator voice at page load so TTS buttons appear without opening settings
fetch('/api/settings').then(r => r.json()).then(d => {
  if (d.success) ttsNarratorVoice = d.tts_narrator_voice || '';
}).catch(() => {});

function openSettings() {
  loadSettings();
  settingsOverlay.classList.add('open');
  if (document.querySelector('.settings-tab[data-tab="servers"].active'))
    refreshServerStatus();
}
function closeSettings() { settingsOverlay.classList.remove('open'); }

btnSettings.addEventListener('click', openSettings);
document.getElementById('settings-close').addEventListener('click', closeSettings);
settingsOverlay.addEventListener('click', e => { if (e.target === settingsOverlay) closeSettings(); });

document.querySelectorAll('.settings-tab').forEach(tab => {
  tab.addEventListener('click', () => {
    document.querySelectorAll('.settings-tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.settings-panel').forEach(p => p.classList.remove('active'));
    tab.classList.add('active');
    document.getElementById('tab-' + tab.dataset.tab).classList.add('active');
    if (tab.dataset.tab === 'servers') refreshServerStatus();
  });
});

document.getElementById('s-provider').addEventListener('change', function() {
  document.querySelectorAll('.provider-section').forEach(s => s.style.display = 'none');
  const sec = document.getElementById('s-sec-' + this.value);
  if (sec) sec.style.display = '';
});

const I2I_DEFAULTS = {
  wavespeed:    'wavespeed-ai/qwen-image/edit-2511',
  fal:          'fal-ai/qwen-image-edit-2511',
  dashscope:    'qwen-image-edit-plus',
  aimlapi:      'alibaba/qwen-image-edit',
  openai:       'gpt-image-1',
  sdcpp_local:  '(i2i built-in)',
  qwen_local:   '(local server)',
};

const LORA_MODEL_DEFAULTS = {
  wavespeed: 'wavespeed-ai/qwen-image/edit-plus-lora',
};

const I2I_URL_PLACEHOLDERS = {
  qwen_local:  'http://127.0.0.1:8000',
  sdcpp_local: 'http://localhost:7860',
};

const I2I_STEPS_HINTS = {
  qwen_local:  '4–8 rec. (Lightning: 4)',
  sdcpp_local: '20–40 rec.',
  fal:         '20–40 rec.',
};

const I2I_GS_HINTS = {
  qwen_local: 'keep 1.0 for Qwen',
};

// Providers where the model field is loaded at server startup — not settable here
const I2I_MODEL_HIDDEN = new Set(['qwen_local', 'sdcpp_local']);

// Only WaveSpeed uses lora_model (selects a different WaveSpeed model variant)
const I2I_LORA_MODEL_SHOWN = new Set(['wavespeed']);

function effectiveI2iProvider() {
  const i2i = document.getElementById('s-img-i2i-provider').value;
  return i2i || document.getElementById('s-img-provider').value;
}

function updateI2iHints() {
  const prov = effectiveI2iProvider();

  // Model hint + field visibility
  const hint = document.getElementById('s-img-i2i-hint');
  const def  = I2I_DEFAULTS[prov];
  if (hint) hint.textContent = def ? '— default: ' + def : '';
  const modelRow = document.getElementById('s-img-i2i-model-row');
  if (modelRow) modelRow.style.display = I2I_MODEL_HIDDEN.has(prov) ? 'none' : '';

  // URL placeholder
  const urlInput = document.getElementById('s-img-i2i-url');
  if (urlInput) urlInput.placeholder = I2I_URL_PLACEHOLDERS[prov] || '';

  // Steps hint
  const stepsHint = document.getElementById('s-img-i2i-steps-hint');
  if (stepsHint) stepsHint.textContent = I2I_STEPS_HINTS[prov] || '';

  // Guidance scale hint
  const gsHint = document.getElementById('s-img-gs-hint');
  if (gsHint) gsHint.textContent = I2I_GS_HINTS[prov] || '';

  // LoRA Model row: WaveSpeed only
  const loraModelRow = document.getElementById('s-img-lora-model-row');
  if (loraModelRow) loraModelRow.style.display = I2I_LORA_MODEL_SHOWN.has(prov) ? '' : 'none';
  const loraModel = document.getElementById('s-img-lora-model');
  if (loraModel) loraModel.placeholder = LORA_MODEL_DEFAULTS[prov] || '';
}

document.getElementById('s-img-provider').addEventListener('change', updateI2iHints);
document.getElementById('s-img-i2i-provider').addEventListener('change', updateI2iHints);

['s-img-strength','s-img-lora-scale','s-img-guidance-scale'].forEach(id => {
  const el = document.getElementById(id);
  const vl = document.getElementById(id + '-val');
  if (el && vl) el.addEventListener('input', () => { vl.textContent = parseFloat(el.value).toFixed(2); });
});

async function loadSettings() {
  try {
    const r = await fetch('/api/settings');
    const d = await r.json();
    if (!d.success) return;

    const prov = d.provider || 'ollama';
    document.getElementById('s-provider').value = prov;
    document.querySelectorAll('.provider-section').forEach(s => s.style.display = 'none');
    const sec = document.getElementById('s-sec-' + prov);
    if (sec) sec.style.display = '';

    const sv = (id, val) => { const el = document.getElementById(id); if (el) el.value = val ?? ''; };
    sv('s-ollama-model',     d.ollama_model);
    sv('s-ollama-url',       d.ollama_url);
    sv('s-or-model',         d.openrouter_model);
    sv('s-or-key',           d.openrouter_key);
    sv('s-oai-model',        d.openai_model);
    sv('s-oai-key',          d.openai_key);
    sv('s-oai-url',          d.openai_url);
    sv('s-claude-model',     d.claude_model);
    sv('s-claude-key',       d.claude_key);
    sv('s-g-model',          d.gemini_model);
    sv('s-g-key',            d.gemini_key);

    const imgPv = d.img_enabled ? (d.img_provider || '') : '';
    sv('s-img-provider',     imgPv);
    sv('s-img-url',          d.img_url);
    sv('s-img-key',          d.img_key);
    sv('s-img-t2i-model',    d.img_t2i_model);
    sv('s-img-i2i-provider', d.img_i2i_provider);
    sv('s-img-i2i-model',    d.img_i2i_model);
    sv('s-img-i2i-url',      d.img_i2i_url);
    sv('s-img-i2i-key',      d.img_i2i_key);
    updateI2iHints();
    sv('s-img-width',        d.img_width  || 1024);
    sv('s-img-height',       d.img_height || 1024);
    sv('s-img-steps',        d.img_steps  || 28);
    sv('s-img-i2i-steps',   d.img_i2i_steps || 0);
    const str = d.img_strength ?? 0.75;
    sv('s-img-strength', str);
    document.getElementById('s-img-strength-val').textContent = parseFloat(str).toFixed(2);
    const gs = d.img_guidance_scale ?? 1.0;
    sv('s-img-guidance-scale', gs);
    document.getElementById('s-img-guidance-scale-val').textContent = parseFloat(gs).toFixed(2);
    sv('s-img-lora', d.img_lora);
    sv('s-img-lora-model', d.img_lora_model);
    const ls = d.img_lora_scale ?? 1.0;
    sv('s-img-lora-scale', ls);
    document.getElementById('s-img-lora-scale-val').textContent = parseFloat(ls).toFixed(2);

    sv('s-base-path',      d.base_path);
    sv('s-max-history',    d.max_history  || 30);
    sv('s-max-retries',    d.max_retries  || 3);
    sv('s-save-mode',      d.save_mode    || 'last');
    sv('s-save-path',      d.save_path);
    sv('s-rag-file',       d.rag_file);
    sv('s-rag-examples',   d.rag_examples || 3);
    sv('s-embed-provider', d.embed_provider);
    sv('s-embed-model',    d.embed_model);
    sv('s-embed-url',      d.embed_url);
    sv('s-embed-key',      d.embed_key);
    sv('s-lang-code',      d.lang_code);

    sv('s-faceswap-url', d.faceswap_url);
    sv('s-py-env-type',  d.py_env_type || 'system');
    sv('s-py-env-path',  d.py_env_path || '');
    sv('s-qwen-args',           d.qwen_locale_args  || '');
    sv('s-tts-url',             d.tts_url           || '');
    ttsNarratorVoice = d.tts_narrator_voice || '';
    // narrator voice: silently populate dropdown (no alert at startup)
    refreshTtsVoices(false).then(() => {
      const sel = document.getElementById('s-tts-narrator-voice');
      if (d.tts_narrator_voice) sel.value = d.tts_narrator_voice;
    });
    sv('s-tts-locale-args',     d.tts_locale_args   || '');
    sv('s-tts-locale-env-type', d.tts_locale_env_type || '');
    sv('s-tts-locale-env-path', d.tts_locale_env_path || '');
    onTtsEnvChange();
    onPyEnvChange();

    // Pre-select t2i model from saved setting (models fetched when server is up)
    if (d.img_t2i_model) {
      const sel = document.getElementById('s-t2i-model');
      let found = false;
      for (const opt of sel.options) {
        if (opt.value === d.img_t2i_model) { opt.selected = true; found = true; break; }
      }
      if (!found) {
        const opt = document.createElement('option');
        opt.value = d.img_t2i_model; opt.textContent = d.img_t2i_model; opt.selected = true;
        sel.appendChild(opt);
      }
    }
  } catch (e) { console.error('[settings] load error:', e); }
}

function onPyEnvChange() {
  const t    = (document.getElementById('s-py-env-type') || {}).value || 'system';
  const row  = document.getElementById('py-env-path-row');
  const lbl  = document.getElementById('py-env-path-label');
  const inp  = document.getElementById('s-py-env-path');
  if (!row) return;
  if (t === 'venv') {
    row.style.display = '';
    lbl.textContent   = 'venv directory path';
    inp.placeholder   = '/home/user/.venvs/rpgai';
  } else if (t === 'conda') {
    row.style.display = '';
    lbl.textContent   = 'Conda environment name';
    inp.placeholder   = 'rpgai';
  } else {
    row.style.display = 'none';
  }
}

async function saveSettings() {
  const gv = id => { const el = document.getElementById(id); return el ? el.value : ''; };
  const imgProv = gv('s-img-provider');
  const payload = {
    provider:         gv('s-provider'),
    ollama_model:     gv('s-ollama-model'),
    ollama_url:       gv('s-ollama-url'),
    openrouter_model: gv('s-or-model'),
    openrouter_key:   gv('s-or-key'),
    openai_model:     gv('s-oai-model'),
    openai_key:       gv('s-oai-key'),
    openai_url:       gv('s-oai-url'),
    claude_model:     gv('s-claude-model'),
    claude_key:       gv('s-claude-key'),
    gemini_model:     gv('s-g-model'),
    gemini_key:       gv('s-g-key'),
    img_enabled:      imgProv !== '',
    img_provider:     imgProv,
    img_url:          gv('s-img-url'),
    img_key:          gv('s-img-key'),
    img_t2i_model:    gv('s-img-t2i-model'),
    img_i2i_model:    gv('s-img-i2i-model'),
    img_i2i_provider: gv('s-img-i2i-provider'),
    img_i2i_url:      gv('s-img-i2i-url'),
    img_i2i_key:      gv('s-img-i2i-key'),
    img_width:        parseInt(gv('s-img-width'))  || 1024,
    img_height:       parseInt(gv('s-img-height')) || 1024,
    img_steps:          parseInt(gv('s-img-steps'))            || 28,
    img_i2i_steps:      parseInt(gv('s-img-i2i-steps'))        || 0,
    img_strength:       parseFloat(gv('s-img-strength'))        || 0.75,
    img_guidance_scale: parseFloat(gv('s-img-guidance-scale')) || 1.0,
    img_lora:           gv('s-img-lora'),
    img_lora_scale:     parseFloat(gv('s-img-lora-scale'))     || 1.0,
    img_lora_model:     gv('s-img-lora-model'),
    faceswap_url:     gv('s-faceswap-url'),
    py_env_type:      gv('s-py-env-type'),
    py_env_path:      gv('s-py-env-path'),
    qwen_locale_args:      gv('s-qwen-args'),
    tts_url:               gv('s-tts-url'),
    tts_narrator_voice:    gv('s-tts-narrator-voice'),
    tts_locale_args:       gv('s-tts-locale-args'),
    tts_locale_env_type:   gv('s-tts-locale-env-type'),
    tts_locale_env_path:   gv('s-tts-locale-env-path'),
    base_path:        gv('s-base-path'),
    max_history:      parseInt(gv('s-max-history'))  || 30,
    max_retries:      parseInt(gv('s-max-retries'))  || 3,
    save_mode:        gv('s-save-mode'),
    save_path:        gv('s-save-path'),
    rag_file:         gv('s-rag-file'),
    rag_examples:     parseInt(gv('s-rag-examples')) || 3,
    embed_provider:   gv('s-embed-provider'),
    embed_model:      gv('s-embed-model'),
    embed_url:        gv('s-embed-url'),
    embed_key:        gv('s-embed-key'),
    lang_code:        gv('s-lang-code')
  };
  try {
    const r = await fetch('/api/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    const d = await r.json();
    if (d.success) {
      ttsNarratorVoice = payload.tts_narrator_voice || '';
      closeSettings(); addMsg('msg-system', '⚙ Settings saved.');
    }
    else addMsg('msg-error', 'Settings error: ' + (d.error || 'unknown'));
  } catch (e) { addMsg('msg-error', 'Settings save failed: ' + e.message); }
}

async function refreshServerStatus() {
  try {
    const r = await fetch('/api/servers/status');
    const d = await r.json();
    const set = (dotId, lblId, up) => {
      const dot = document.getElementById(dotId), lbl = document.getElementById(lblId);
      if (!dot || !lbl) return;
      dot.className  = 'server-dot ' + (up ? 'up' : 'down');
      lbl.textContent = up ? 'Running' : 'Offline';
    };
    set('dot-faceswap',    'label-faceswap',     d.faceswap_locale);
    set('dot-qwen',        'label-qwen',         d.qwen_locale);
    set('dot-tts-locale',  'label-tts-locale',   d.tts_locale);
    set('dot-t2i',         'label-t2i',          d.t2i_locale);
    if (d.t2i_locale) fetchT2iModels();
    document.getElementById('t2i-model-row').style.display = d.t2i_locale ? '' : 'none';
  } catch (_) {}
}

async function fetchT2iModels() {
  try {
    const r = await fetch('/api/servers/models/t2i_locale');
    const d = await r.json();
    if (!d.success) return;
    const sel = document.getElementById('s-t2i-model');
    const current = sel.value;
    sel.innerHTML = '<option value="">(auto)</option>';
    (d.models || []).forEach(m => {
      const opt = document.createElement('option');
      opt.value = m; opt.textContent = m;
      if (m === current || m === d.loaded) opt.selected = true;
      sel.appendChild(opt);
    });
    // If nothing selected and loaded model known, pick it
    if (!sel.value && d.loaded) sel.value = d.loaded;
  } catch (_) {}
}

function onT2iModelChange() {
  const model = document.getElementById('s-t2i-model').value;
  // Keep text input in sync so saveSettings() picks up the same value
  const textInput = document.getElementById('s-img-t2i-model');
  if (textInput) textInput.value = model;
  fetch('/api/settings', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ img_t2i_model: model })
  }).catch(() => {});
}

function splitSentences(text) {
  // Split on sentence-ending punctuation followed by whitespace or end of string
  return text.split(/(?<=[.!?])\s+/u).map(s => s.trim()).filter(Boolean);
}

async function fetchTTSBuffer(text, voice) {
  const key = voice + '|' + text;
  if (ttsCache.has(key)) return ttsCache.get(key).slice(0); // clone: decodeAudioData detaches
  const r = await fetch('/api/tts?' + new URLSearchParams({ text, voice }));
  if (!r.ok) return null;
  const buf = await r.arrayBuffer();
  ttsCache.set(key, buf);
  return buf.slice(0);
}

async function playTTS(text, btn) {
  if (!ttsNarratorVoice) return;
  const sentences = splitSentences(text);
  if (!sentences.length) return;
  if (btn) { btn.textContent = '⏳'; btn.classList.add('loading'); }
  try {
    const ctx = new (window.AudioContext || window.webkitAudioContext)();
    // Fire all sentence fetches in parallel
    const bufPromises = sentences.map(s => fetchTTSBuffer(s, ttsNarratorVoice));
    let nextStart = ctx.currentTime + 0.05;
    let first = true;
    for (const p of bufPromises) {
      const buf = await p;
      if (!buf) continue;
      const audioBuf = await ctx.decodeAudioData(buf);
      const src = ctx.createBufferSource();
      src.buffer = audioBuf;
      src.connect(ctx.destination);
      const t = Math.max(ctx.currentTime + 0.01, nextStart);
      src.start(t);
      nextStart = t + audioBuf.duration;
      if (first && btn) { btn.textContent = '🔊'; btn.classList.remove('loading'); first = false; }
    }
  } catch(e) {
    console.warn('TTS error:', e);
    if (btn) { btn.textContent = '🔊'; btn.classList.remove('loading'); }
  }
}

async function refreshTtsVoices(showAlert) {
  const sel = document.getElementById('s-tts-narrator-voice');
  const current = sel.value;
  const urlOverride = (document.getElementById('s-tts-url') || {}).value || '';
  const params = urlOverride ? '?url=' + encodeURIComponent(urlOverride) : '';
  try {
    const r = await fetch('/api/tts/voices' + params);
    if (!r.ok) {
      if (showAlert) alert('TTS server unreachable. Check URL and that the server is running.');
      return;
    }
    const d = await r.json();
    const voices = d.voices || [];
    sel.innerHTML = '<option value="">(none)</option>';
    voices.forEach(v => {
      const o = document.createElement('option');
      o.value = o.textContent = v;
      if (v === current) o.selected = true;
      sel.appendChild(o);
    });
    if (showAlert && voices.length === 0) alert('Server reachable but no voices found. Add voices via POST /voices/add.');
  } catch(e) {
    if (showAlert) alert('TTS error: ' + e.message);
  }
}

async function testNarratorVoice() {
  const voice = document.getElementById('s-tts-narrator-voice').value;
  if (!voice) { alert('Select a narrator voice first, then click Refresh.'); return; }
  const textEl = document.getElementById('s-tts-test-text');
  const text = (textEl && textEl.value.trim()) || 'Ciao. Sono il narratore di questa avventura.';
  try {
    const buf = await fetchTTSBuffer(text, voice);
    if (!buf) { alert('TTS server error or unreachable.'); return; }
    const ctx = new (window.AudioContext || window.webkitAudioContext)();
    const audioBuf = await ctx.decodeAudioData(buf);
    const src = ctx.createBufferSource();
    src.buffer = audioBuf;
    src.connect(ctx.destination);
    src.start();
  } catch(e) { alert('TTS error: ' + e.message); }
}

function onTtsEnvChange() {
  const t = document.getElementById('s-tts-locale-env-type').value;
  const row = document.getElementById('tts-locale-env-path-row');
  const lbl = document.getElementById('tts-locale-env-path-label');
  row.style.display = t && t !== 'system' ? '' : 'none';
  if (lbl) lbl.textContent = t === 'conda' ? 'Env name' : 'Path';
}

async function serverAction(server, action) {
  try {
    const argsEl = document.getElementById('s-' + server.replace('_','-') + '-args');
    const extra_args = (action === 'start' && argsEl) ? argsEl.value.trim() : '';
    // Per-server env override (e.g. TTS uses a separate conda env)
    const srvEnvTypeEl = document.getElementById('s-' + server.replace('_','-') + '-env-type');
    const srvEnvPathEl = document.getElementById('s-' + server.replace('_','-') + '-env-path');
    const server_env_type = srvEnvTypeEl ? srvEnvTypeEl.value : '';
    const server_env_path = srvEnvPathEl ? srvEnvPathEl.value : '';
    const r = await fetch('/api/servers/action', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        server, action,
        py_env_type: gv('s-py-env-type'),
        py_env_path: gv('s-py-env-path'),
        server_env_type, server_env_path,
        extra_args
      })
    });
    const d = await r.json();
    addMsg('msg-system', '⚙ ' + (d.message || d.error || action));
    setTimeout(refreshServerStatus, 2500);
  } catch (e) { addMsg('msg-error', 'Server action failed: ' + e.message); }
}
</script>
</body>
</html>)HTML";
