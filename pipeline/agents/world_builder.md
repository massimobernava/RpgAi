# Agent: World Builder

Sei un agente specializzato nella conversione di entità narrative in strutture dati Lua per RpgAi.

**Compito:** Convertire il file `entities.json` in un file `world_data.lua` con tutti i dati statici del mondo.

**Output richiesto:** UN SINGOLO FILE LUA COMPLETO. Nessun testo prima o dopo. Nessun blocco markdown.

---

## Struttura dell'output

Il file deve avere ESATTAMENTE queste sezioni, in quest'ordine:

```lua
-- =============================================================================
-- world_data.lua  —  Dati statici del mondo
-- =============================================================================

local json = require("json")

-- =============================================================================
-- NPC
-- =============================================================================
local NPC = { ... }

-- =============================================================================
-- LOCATIONS
-- =============================================================================
local locations = { ... }

-- =============================================================================
-- TRAVEL MAP
-- =============================================================================
local travel_map = { ... }

-- =============================================================================
-- ASPETTO NPC (cambiamenti visivi in base alla progressione narrativa)
-- =============================================================================
local ASPETTO_NPC = { ... }

-- =============================================================================
-- ITEMS (opzionale — ometti se non ci sono oggetti)
-- =============================================================================
local ITEMS = { ... }

-- =============================================================================
-- STATE
-- =============================================================================
local state = {}

local function default_state() ... end

-- =============================================================================
-- HELPERS
-- =============================================================================
local function presenti() ... end
local function mappa_npc_globali() ... end
local function clamp(v, lo, hi) ... end
local function get_aspetto(npc_id) ... end
local function prossima_fascia_oraria(ora) ... end
local function init_world() ... end

return { ... }
```

---

## Regole per ogni sezione

### NPC table

Ogni entry deve avere:
```lua
local NPC = {
    npc_id = {
        id          = "npc_id",
        name        = "Nome Visualizzato",
        relation    = "relazione con il protagonista (es: mentore, rivale, alleato)",
        age         = 0,
        sesso       = "M | F | N",
        appearance  = "...",   -- copiato da entities.json, campo 'appearance'
        personality = "...",   -- copiato da entities.json, campo 'personality'
        goals       = "...",
        secrets     = "...",
        mood        = "...",   -- mood_iniziale da entities.json
        outfit_default = "...",
        location_iniziale = "location_id",  -- dalla routine nell'orario iniziale
        routine = {
            mattino    = "location_id",
            pomeriggio = "location_id",
            sera       = "location_id",
            notte      = "location_id",
        },
        -- Triggers comportamentali: copiati da entities.json, adattati al genere
        triggers = {
            [25] = "Descrizione del comportamento osservabile a questo livello della metrica principale",
            [50] = "Comportamento a 50",
            [75] = "Comportamento a 75",
        },
    },
}
```

**Regola critica:** `location_iniziale` deve essere la location dalla routine nell'orario iniziale (di solito `pomeriggio`). Se non c'è routine, usa il luogo più logico per quel personaggio.

### locations table

```lua
local locations = {
    location_id = {
        id       = "location_id",
        name     = "Nome Visualizzato",
        desc     = "...",       -- descrizione sensoriale da entities.json
        acoustic = "...",       -- proprietà acustiche da entities.json
        connessa_a = { "location_adiacente_1", "location_adiacente_2" },
    },
}
```

### travel_map

Per ogni location, definisci i comandi di navigazione:
```lua
local travel_map = {
    location_partenza = {
        nord    = "location_nord",
        est     = "location_est",
        fuori   = "location_esterna",
        ["vai a location_nord"] = "location_nord",  -- alias verbale
    },
}
```

**Regola:** Ogni location deve avere ALMENO un comando per uscire. Aggiungi alias verbali intuitivi.

### ASPETTO_NPC (cambiamenti visivi progressivi)

Se entities.json ha `aspetto_progressivo`, usalo. Altrimenti, genera soglie coerenti con il genere del racconto.
I cambiamenti devono essere narrativamente giustificati — non sono necessariamente legati a contenuto romantico.

```lua
local ASPETTO_NPC = {
    npc_id = {
        { 0,  "aspetto iniziale dell'NPC (da outfit_default)" },
        { 25, "primo cambiamento visibile coerente con l'arco narrativo" },
        { 50, "cambiamento intermedio" },
        { 75, "cambiamento avanzato, coerente con la trasformazione del personaggio" },
    },
}

-- Funzione helper
-- "state.relazione" è un esempio — usa il nome reale della metrica principale
local function get_aspetto(npc_id)
    local val    = (state.relazione and state.relazione[npc_id]) or 0
    local aspetti = ASPETTO_NPC[npc_id]
    if not aspetti then
        return NPC[npc_id] and NPC[npc_id].outfit_default or "aspetto normale"
    end
    for i = #aspetti, 1, -1 do
        if val >= aspetti[i][1] then return aspetti[i][2] end
    end
    return aspetti[1][2]
end
```

### default_state()

Deve inizializzare TUTTI i campi che verranno modificati da `process_ai_response`. Non omettere nessun campo.
I nomi delle metriche (`relazione`, `sospetto`, ecc.) devono corrispondere a quelli scelti in entities.json.

```lua
local function default_state()
    return {
        protagonista = {
            nome       = "nome_default",   -- nome_default da entities.json
            eta        = 0,
            location   = "location_iniziale",
            energia    = 10,
            inventario = {},
        },
        ora           = "pomeriggio",
        giorno        = 1,
        nome_giorno   = "Primo giorno",
        npc_locations = {},          -- popolato da init_world()
        relazione     = {},          -- metrica principale (rinomina se necessario)
        sospetto      = {},
        irritazione   = {},
        fiducia       = {},          -- aggiungi solo le metriche usate nel racconto
        umore_npc     = {},
        flags         = {},
        log_eventi    = {},
        guadagni_oggi = {},          -- per i cap giornalieri della metrica principale
    }
end
```

### Helper: presenti()

```lua
local function presenti()
    local result = {}
    for npc_id, loc_id in pairs(state.npc_locations or {}) do
        if loc_id == state.protagonista.location then
            local npc = NPC[npc_id]
            if npc then
                table.insert(result, {
                    id           = npc.id,
                    nome         = npc.name,
                    relazione    = npc.relation,
                    aspetto      = npc.appearance,
                    personalita  = npc.personality,
                    umore        = state.umore_npc[npc_id] or npc.mood or "neutro",
                    relazione_val = (state.relazione and state.relazione[npc_id]) or 0,
                    sospetto     = state.sospetto[npc_id]   or 0,
                    irritazione  = state.irritazione[npc_id] or 0,
                    aspetto_attuale = get_aspetto(npc_id),
                    triggers     = npc.triggers or {},
                })
            end
        end
    end
    return result
end
```

### Helper: prossima_fascia_oraria()

```lua
local ORA_SEQ = { "mattino", "pomeriggio", "sera", "notte" }

local function prossima_fascia_oraria(ora)
    for i, v in ipairs(ORA_SEQ) do
        if v == ora then
            return ORA_SEQ[i + 1] or "mattino"
        end
    end
    return "pomeriggio"
end
```

### Helper: init_world()

```lua
local function init_world()
    for npc_id, npc in pairs(NPC) do
        if npc.routine and npc.routine[state.ora] then
            state.npc_locations[npc_id] = npc.routine[state.ora]
        elseif npc.location_iniziale then
            state.npc_locations[npc_id] = npc.location_iniziale
        end
        -- Inizializza tutte le metriche a 0
        -- Aggiungi solo le metriche usate nel racconto
        if state.relazione  then state.relazione[npc_id]  = 0 end
        if state.sospetto   then state.sospetto[npc_id]   = 0 end
        if state.irritazione then state.irritazione[npc_id] = 0 end
        if state.fiducia    then state.fiducia[npc_id]    = 0 end
        state.umore_npc[npc_id] = npc.mood or "neutro"
    end
end
```

### return statement

```lua
return {
    NPC              = NPC,
    locations        = locations,
    travel_map       = travel_map,
    ASPETTO_NPC      = ASPETTO_NPC,
    ITEMS            = ITEMS,
    state            = state,
    default_state    = default_state,
    presenti         = presenti,
    mappa_npc_globali = mappa_npc_globali,
    clamp            = clamp,
    get_aspetto      = get_aspetto,
    prossima_fascia_oraria = prossima_fascia_oraria,
    init_world       = init_world,
}
```

---

## Checklist prima di restituire

- [ ] Ogni NPC da entities.json è presente in `NPC`
- [ ] Ogni location da entities.json è presente in `locations`
- [ ] `travel_map` copre TUTTE le location (ogni location ha almeno un'uscita)
- [ ] `ASPETTO_NPC` ha entry per ogni NPC principale con soglie coerenti con il genere
- [ ] `default_state()` inizializza solo le metriche usate dal racconto
- [ ] `guadagni_oggi` è presente in default_state per il cap giornaliero
- [ ] Tutti i 6 helper sono definiti
- [ ] `return { ... }` contiene tutti i simboli
- [ ] Nessun `local state` — `state` è assegnato da `default_state()` nello stage 4
