# Skill: Dynamic System Prompt

## Quando usare
Quando implementi `get_system_prompt()` nello script finale. Questa funzione è chiamata ogni turno — deve essere dinamica.

## Pattern completo

```lua
-- IMPORTANTE: SYSTEM_PROMPT_TEMPLATE usa %s per string.format(), NON {}
local SYSTEM_PROMPT_TEMPLATE = [[
Sei il Game Master di un gioco di ruolo testuale in italiano.
Il protagonista è %s, %d anni.

AMBIENTAZIONE:
%s

PERSONAGGI PRESENTI IN QUESTA SCENA:
%s

LOCATION ATTUALE:
%s — %s
%s

TONO E PROGRESSIONE:
%s

REGOLE NARRATIVE:
- Terza persona, tempo presente
- Risposte di 3-5 frasi
- Descrivi sempre cosa fanno i personaggi presenti, non solo il protagonista
- Usa i dettagli sensoriali (vista, suono, tatto)
- Mai usare asterischi per enfasi
- Non anticipare eventi futuri

ISTRUZIONI SPECIALI:
%s

Rispondi SOLO con JSON valido. Zero testo fuori dal JSON.
]]

function get_system_prompt()
    local loc  = locations[state.protagonista.location]
    local here = presenti()

    -- 1. Calcola livello metrica principale massimo per il tono globale
    -- (adatta "state.relazione" al nome reale della metrica principale del racconto)
    local max_prog = 0
    for _, v in pairs(state.relazione or {}) do
        if v > max_prog then max_prog = v end
    end

    -- 2. Seleziona regola di tono
    local tono = REGOLE_TONO[0] or "Tono neutro."
    for _, soglia in ipairs({90, 75, 50, 25, 10, 0}) do
        if max_prog >= soglia and REGOLE_TONO[soglia] then
            tono = REGOLE_TONO[soglia]
            break
        end
    end

    -- 3. Descrizioni NPC dinamiche
    local npc_lines = {}
    for _, npc in ipairs(here) do
        local rel  = npc.relazione_val or 0   -- metrica principale
        local susp = npc.sospetto     or 0
        local irr  = npc.irritazione  or 0
        local ndata = NPC[npc.id]

        -- Descrizione base
        local desc = string.format(
            "%s (%s, %d anni)\nAspetto: %s\nPersonalità: %s\nEquipaggiamento/aspetto attuale: %s\nUmore: %s",
            npc.nome,
            npc.relazione,
            ndata.age or 0,
            npc.aspetto,
            npc.personalita,
            npc.aspetto_attuale,
            npc.umore
        )

        -- Trigger comportamentale dalla tabella triggers
        if ndata.triggers then
            local trigger_attivo = nil
            for _, soglia in ipairs({75, 50, 25}) do
                if rel >= soglia and ndata.triggers[soglia] then
                    trigger_attivo = ndata.triggers[soglia]
                    break
                end
            end
            if trigger_attivo then
                desc = desc .. "\n[Comportamento: " .. trigger_attivo .. "]"
            end
        end

        -- Sospetto/irritazione elevati
        if susp >= 60 then
            desc = desc .. "\n[GUARDIA: osserva il protagonista con sospetto — evita di dare pretesti]"
        elseif susp >= 30 then
            desc = desc .. "\n[ATTENZIONE: leggermente diffidente verso il protagonista]"
        end

        if irr >= 60 then
            desc = desc .. "\n[IRRITAZIONE ALTA: risponde in modo tagliente, non è dell'umore]"
        end

        table.insert(npc_lines, desc)
    end

    local npc_text = #npc_lines > 0
        and table.concat(npc_lines, "\n\n")
        or  "(Nessuno presente in questa area)"

    -- 4. Istruzioni speciali da flag narrativi
    local istr_list = {}
    if state.flags then
        for flag_id, val in pairs(state.flags) do
            if val and ISTRUZIONI_PER_FLAG and ISTRUZIONI_PER_FLAG[flag_id] then
                table.insert(istr_list, ISTRUZIONI_PER_FLAG[flag_id])
            end
        end
    end
    local istr = #istr_list > 0
        and table.concat(istr_list, "\n")
        or  "Nessuna"

    -- 5. Costruisci prompt
    return string.format(
        SYSTEM_PROMPT_TEMPLATE,
        state.protagonista.nome,                      -- %s nome
        state.protagonista.eta,                       -- %d eta
        ambientazione_base,                           -- %s ambientazione (costante)
        npc_text,                                     -- %s npc descriptions
        loc and loc.name     or "sconosciuto",        -- %s location name
        loc and loc.desc     or "",                   -- %s location desc
        loc and loc.acoustic or "",                   -- %s acoustic
        tono,                                         -- %s tono
        istr                                          -- %s istruzioni speciali
    )
end

-- Costante: descrizione dell'ambientazione (non cambia mai — generata dall'analyzer)
local ambientazione_base = "Descrizione dal campo 'ambientazione' di entities.json"
```

## REGOLE_TONO pattern

La tabella mappa soglie della metrica principale → istruzioni di tono per l'LLM.
Adatta i testi al genere del racconto: non devono essere sempre "romantici" — possono essere avventurosi, cupi, politici, ecc.

```lua
-- Esempio per storia di alleanze/avventura:
local REGOLE_TONO = {
    [0]  = "Tono neutro. Diffidenza reciproca, nessuna familiarità.",
    [25] = "Prime aperture. I personaggi iniziano a rispettarsi. Conversazioni più dirette.",
    [50] = "Fiducia parziale. Collaborazione visibile. Il protagonista riceve informazioni prima negate.",
    [75] = "Alleanza solida. I personaggi si coprono a vicenda. Rivelazioni importanti possibili.",
    [90] = "Legame profondo. Massima cooperazione. I segreti più profondi sono condivisi.",
}

-- Esempio per storia di mystery/tensione:
local REGOLE_TONO = {
    [0]  = "Atmosfera tranquilla. Nessun segnale di pericolo.",
    [25] = "Qualcosa non torna. Piccole inconsistenze. I personaggi notano ma non commentano.",
    [50] = "Tensione evidente. Domande dirette. Risposte evasive. L'aria è carica.",
    [75] = "Confronto imminente. Le maschere cadono. Le verità emergono a pezzi.",
    [90] = "Risoluzione o punto di non ritorno. La verità è quasi tutta in superficie.",
}
```

## ISTRUZIONI_PER_FLAG pattern

Tabella che mappa flag narrativi → istruzioni da aggiungere al prompt.
Genera i flag dagli `eventi_chiave` di entities.json. Esempi generici:

```lua
local ISTRUZIONI_PER_FLAG = {
    alleanza_formata = [[
NOTA NARRATIVA: Il protagonista ha stretto un'alleanza con [NPC].
Il tono tra loro è cambiato: non sono più estranei. C'è un'intesa non detta.
[NPC] può sembrare più aperto o, al contrario, consapevole della responsabilità che ha assunto.
]],
    segreto_scoperto = [[
ATTENZIONE: Il protagonista ha scoperto qualcosa di importante.
[NPC] non sa ancora che il protagonista lo sa. Questo crea una tensione latente.
Descrivi il comportamento normale di [NPC] — che non può essere completamente genuino.
]],
    tradimento_rivelato = [[
Il tradimento è emerso. I rapporti tra i personaggi sono cambiati definitivamente.
Ogni interazione è filtrata da questa nuova consapevolezza.
]],
}
```

## Errori comuni

**Errore 1: usare `{}` invece di `%s` nel template**
```lua
-- ✗ SBAGLIATO — {} non è sintassi Lua per string.format
local TEMPLATE = "Ciao {NOME}, hai {ETA} anni"

-- ✓ CORRETTO
local TEMPLATE = "Ciao %s, hai %d anni"
string.format(TEMPLATE, "Protagonista", 25)
```

**Errore 2: contare male i parametri `%s`**
Il numero di `%s/%d` in TEMPLATE deve corrispondere ESATTAMENTE al numero di argomenti a `string.format()`. Conta attentamente.

**Errore 3: NPC con metrica a 0 che ha trigger**
Il primo trigger è a 25. Non mostrare trigger se il valore < 25.
