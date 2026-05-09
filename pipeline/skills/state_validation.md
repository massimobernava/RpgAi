# Skill: State Validation

## Quando usare
Quando implementi `process_ai_response()`. Questa funzione è chiamata dopo ogni risposta LLM.

## Pattern helper: valida_delta_npc

Applica un delta a una metrica NPC con cap giornaliero e clamping.
I nomi delle variabili (`state.relazione`, `state.sospetto`, ecc.) dipendono dalle metriche definite in entities.json — adattali allo script specifico.

```lua
-- Applica delta a state_table[npc_id] con cap opzionale
-- Modifica state_table in-place, ritorna il delta applicato per ogni NPC
local function valida_delta_npc(deltas, state_table, guadagni_oggi, cap)
    if type(deltas) ~= "table" then return {} end
    local applied = {}
    for id, delta in pairs(deltas) do
        if NPC[id] and type(delta) == "number" then
            if delta > 0 and cap then
                local oggi   = guadagni_oggi[id] or 0
                local spazio = math.max(0, cap - oggi)
                delta = math.min(delta, spazio)
                guadagni_oggi[id] = oggi + delta
            end
            -- Clamp il delta stesso tra -10 e +20 (sicurezza extra)
            delta = math.max(-10, math.min(20, delta))
            state_table[id] = clamp((state_table[id] or 0) + delta, 0, 100)
            applied[id] = delta
        end
    end
    return applied
end
```

## Pattern completo process_ai_response

I campi di state_changes (`cambia_relazione`, `cambia_sospetto`, ecc.) devono corrispondere esattamente alle metriche scelte in entities.json e definite nel JSON schema. L'esempio usa `relazione` e `sospetto` — adatta ai nomi reali del tuo script.

```lua
function process_ai_response(reply)
    -- ── 1. Parsing JSON ────────────────────────────────────────────────────
    local ok, data = pcall(json.decode, reply)
    if not ok or type(data) ~= "table" then
        -- Tentativo repair (per Ollama e modelli che aggiungono testo extra)
        if json_repair then
            local repaired = json_repair.repair(reply)
            ok, data = pcall(json.decode, repaired)
        end
        if not ok or type(data) ~= "table" then
            return { success = false, error = "JSON non valido: " .. tostring(reply):sub(1, 50) }
        end
    end

    -- ── 2. Campi obbligatori ───────────────────────────────────────────────
    if type(data.narration) ~= "string" or #data.narration < 10 then
        return { success = false, error = "Narrazione mancante o troppo corta" }
    end

    local sc = type(data.state_changes) == "table" and data.state_changes or {}

    -- ── 3. Location protagonista ───────────────────────────────────────────
    if sc.nuova_location ~= nil and sc.nuova_location ~= json.null then
        if type(sc.nuova_location) == "string" and locations[sc.nuova_location] then
            state.protagonista.location = sc.nuova_location
        end
    end

    -- ── 4. Metrica principale (con cap) — nome dipende dal racconto ────────
    -- Esempio: "relazione". Sostituisci con il nome reale (fiducia, influenza, ecc.)
    if type(sc.cambia_relazione) == "table" then
        valida_delta_npc(
            sc.cambia_relazione,
            state.relazione,
            state.guadagni_oggi or {},
            CAP_GIORNALIERI and CAP_GIORNALIERI.relazione
        )
    end

    -- ── 5. Sospetto ────────────────────────────────────────────────────────
    if type(sc.cambia_sospetto) == "table" then
        for id, delta in pairs(sc.cambia_sospetto) do
            if NPC[id] and type(delta) == "number" then
                state.sospetto[id] = clamp((state.sospetto[id] or 0) + delta, 0, 100)
            end
        end
    end

    -- ── 6. Irritazione ─────────────────────────────────────────────────────
    if type(sc.cambia_irritazione) == "table" then
        for id, delta in pairs(sc.cambia_irritazione) do
            if NPC[id] and type(delta) == "number" then
                state.irritazione[id] = clamp((state.irritazione[id] or 0) + delta, 0, 100)
            end
        end
    end

    -- ── 7. Umore NPC ───────────────────────────────────────────────────────
    if type(sc.umore_npc) == "table" then
        for id, umore in pairs(sc.umore_npc) do
            if NPC[id] and type(umore) == "string" then
                state.umore_npc[id] = umore
            end
        end
    end

    -- ── 8. Spostamento NPC ─────────────────────────────────────────────────
    if type(sc.sposta_npc) == "table" then
        for npc_id, loc_id in pairs(sc.sposta_npc) do
            if NPC[npc_id] and type(loc_id) == "string" and locations[loc_id] then
                state.npc_locations[npc_id] = loc_id
            end
        end
    end

    -- ── 9. Flag narrativi ──────────────────────────────────────────────────
    if type(sc.set_flags) == "table" then
        for k, v in pairs(sc.set_flags) do
            if type(k) == "string" then
                state.flags[k] = v
            end
        end
    end

    -- ── 10. Energia ────────────────────────────────────────────────────────
    if type(sc.energia_delta) == "number" then
        state.protagonista.energia = clamp(
            (state.protagonista.energia or 10) + sc.energia_delta, 0, 20)
    end

    -- ── 11. Inventario ─────────────────────────────────────────────────────
    if type(sc.cambia_inventario) == "table" then
        local inv = state.protagonista.inventario or {}
        for item_id, azione in pairs(sc.cambia_inventario) do
            if azione == "add" then
                inv[item_id] = true
            elseif azione == "remove" then
                inv[item_id] = nil
            end
        end
        state.protagonista.inventario = inv
    end

    -- ── 12. Avanzamento tempo ──────────────────────────────────────────────
    -- IMPORTANTE: fai questo DOPO aver applicato tutti gli altri cambiamenti
    if sc.avanza_giorno then
        state.giorno = state.giorno + 1
        state.ora    = "mattino"
        -- Reset cap giornalieri
        state.guadagni_oggi = {}
        -- Sposta NPC secondo routine mattino
        for npc_id, npc in pairs(NPC) do
            if npc.routine and npc.routine.mattino then
                state.npc_locations[npc_id] = npc.routine.mattino
            end
        end
    elseif sc.avanza_tempo then
        local ora_prec = state.ora
        state.ora = prossima_fascia_oraria(state.ora)
        -- Sposta NPC secondo routine nuova fascia
        if ora_prec ~= state.ora then
            for npc_id, npc in pairs(NPC) do
                if npc.routine and npc.routine[state.ora] then
                    state.npc_locations[npc_id] = npc.routine[state.ora]
                end
            end
        end
    end

    -- ── 13. Game over ──────────────────────────────────────────────────────
    if data.game_over then
        return {
            success          = true,
            narration        = data.narration,
            game_over        = true,
            game_over_reason = type(data.game_over_reason) == "string"
                               and data.game_over_reason or "",
        }
    end

    return { success = true, narration = data.narration }
end
```

## Pattern json_repair (safe require)

```lua
-- json_repair è opzionale — non crashare se non è presente
local json_repair
do
    local ok, mod = pcall(require, "json_repair")
    if ok then json_repair = mod end
end
```

## Cosa NON fare in process_ai_response

- **Non crashare su input nil** — ogni campo va controllato con `type(x) == "..."` prima dell'uso
- **Non applicare delta senza NPC check** — `if NPC[id] then` prima di modificare state
- **Non applicare location senza check** — `if locations[loc_id] then` prima di aggiornare
- **Non dimenticare il reset guadagni** — `avanza_giorno` deve resettare `state.guadagni_oggi`
- **Non spostare NPC prima del check avanza_tempo** — lo spostamento via `sposta_npc` + quello per routine non devono conflittare: applica `sposta_npc` prima, poi la routine sovrascrive solo se `avanza_tempo`/`avanza_giorno`
