# Skill: Player Command Handler

## Quando usare
Quando implementi `process_player_input()`. Gestisce i comandi speciali prima che l'input arrivi all'LLM.

## Pattern completo

```lua
function process_player_input(input)
    if not input or input == "" then
        return { success = false, handled = false }
    end

    local cmd  = input:lower():match("^%s*(.-)%s*$")  -- trim + lowercase
    local args = cmd:match("^/[a-z]+ (.+)$")          -- argomenti dopo il comando

    -- ── /stato — metriche, flag ───────────────────────────────────────────
    -- I nomi delle metriche (relazione, sospetto, ecc.) dipendono dal racconto
    if cmd == "/stato" then
        local lines = { "═══ STATO ═══" }

        -- Metrica principale (adatta il nome e l'etichetta al racconto)
        table.insert(lines, "[ Relazione ]")
        for id, val in pairs(state.relazione or {}) do
            local npc = NPC[id]
            if npc and val > 0 then
                local bar = string.rep("█", math.floor(val / 10))
                         .. string.rep("░", 10 - math.floor(val / 10))
                table.insert(lines, string.format("  %-12s %s %d%%", npc.name, bar, val))
            end
        end

        if next(state.sospetto or {}) then
            table.insert(lines, "[ Sospetto ]")
            for id, val in pairs(state.sospetto) do
                local npc = NPC[id]
                if npc and val > 0 then
                    table.insert(lines, string.format("  %-12s %d%%", npc.name, val))
                end
            end
        end

        local flag_attivi = {}
        for k, v in pairs(state.flags) do
            if v then table.insert(flag_attivi, k) end
        end
        if #flag_attivi > 0 then
            table.insert(lines, "[ Flag attivi ]")
            for _, f in ipairs(flag_attivi) do
                table.insert(lines, "  • " .. f)
            end
        end

        table.insert(lines, string.format("[ Energia: %d/20 | Ora: %s | Giorno: %d ]",
            state.protagonista.energia or 10, state.ora, state.giorno))

        return { success = true, handled = true, output = table.concat(lines, "\n") }
    end

    -- ── /dove — posizioni NPC ─────────────────────────────────────────────
    if cmd == "/dove" then
        local lines = { "═══ POSIZIONI NPC ═══" }
        for npc_id, loc_id in pairs(state.npc_locations) do
            local npc = NPC[npc_id]
            local loc = locations[loc_id]
            if npc and loc then
                table.insert(lines, string.format("  %-12s → %s", npc.name, loc.name))
            end
        end
        return { success = true, handled = true, output = table.concat(lines, "\n") }
    end

    -- ── /inventario — oggetti del protagonista ───────────────────────────
    if cmd == "/inventario" then
        local inv = state.protagonista.inventario or {}
        local lines = { "═══ INVENTARIO ═══" }
        local found = false
        for item_id, _ in pairs(inv) do
            local item = ITEMS and ITEMS[item_id]
            table.insert(lines, "  • " .. (item and item.name or item_id))
            found = true
        end
        if not found then
            table.insert(lines, "  (vuoto)")
        end
        return { success = true, handled = true, output = table.concat(lines, "\n") }
    end

    -- ── /aiuto — lista comandi ────────────────────────────────────────────
    if cmd == "/aiuto" or cmd == "/help" then
        local lines = {
            "═══ COMANDI ═══",
            "  /stato      — metriche, flag attivi",
            "  /dove       — dove si trovano i personaggi",
            "  /inventario — oggetti in tuo possesso",
            "  /aiuto      — questa lista",
            "  (comando di movimento) — vai in un'area adiacente",
            "  (qualsiasi altra cosa) — passa all'LLM come azione",
        }
        return { success = true, handled = true, output = table.concat(lines, "\n") }
    end

    -- ── Movimento via travel_map ──────────────────────────────────────────
    local loc_id    = state.protagonista.location
    local travel    = travel_map[loc_id]
    if travel then
        -- Cerca corrispondenza esatta o parziale
        local dest = travel[cmd]
        if not dest then
            -- Prova match parziale (il giocatore potrebbe scrivere "torre" invece di "vai alla torre")
            for keyword, d in pairs(travel) do
                if cmd:find(keyword, 1, true) then
                    dest = d
                    break
                end
            end
        end
        if dest and locations[dest] then
            state.protagonista.location = dest
            -- Non handled=true: lascia che l'LLM descriva l'arrivo nella nuova area
            return { success = true, handled = false }
        end
    end

    -- ── Comandi non riconosciuti: passa all'LLM ───────────────────────────
    return { success = false, handled = false }
end
```

## Comandi speciali del gioco

Genera comandi specifici dagli `eventi_chiave` di entities.json quando hanno prerequisiti verificabili lato Lua. Esempi:

```lua
    -- ── /esamina — esamina l'area corrente ───────────────────────────────
    if cmd == "/esamina" then
        if not state.flags.area_esaminata then
            state.flags.area_esaminata = true
        end
        -- Lascia all'LLM descrivere cosa trova
        return { success = true, handled = false }
    end

    -- ── /parla <npc> — forza conversazione con NPC specifico ─────────────
    if cmd:match("^/parla") then
        local npc_id = cmd:match("^/parla (.+)$")
        if npc_id and NPC[npc_id] then
            if state.npc_locations[npc_id] ~= state.protagonista.location then
                return { success = true, handled = true,
                         output = NPC[npc_id].name .. " non è qui." }
            end
        end
        return { success = true, handled = false }
    end
```

## Return values

| Scenario | success | handled | output |
|----------|---------|---------|--------|
| Comando gestito con output | true | true | testo da mostrare |
| Movimento eseguito (LLM descrive arrivo) | true | false | nil |
| Comando non riconosciuto → passa a LLM | false | false | nil |
| Errore interno | false | true | messaggio errore |

## Regole

1. `handled = true` → il motore mostra `output` direttamente, NON chiama l'LLM
2. `handled = false` → il motore chiama l'LLM con l'input originale
3. `success = false, handled = false` → il motore tratta l'input come normale messaggio per l'LLM
4. Il movimento via travel_map deve usare `handled = false` così l'LLM descrive la nuova area
