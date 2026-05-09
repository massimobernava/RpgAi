# Skill: Lua Table Patterns

## Quando usare
Quando generi tabelle Lua per NPC, locations, travel_map, ASPETTO_NPC, e default_state.

## Pattern tabella NPC (con tutti i campi)

```lua
local NPC = {
    capitano = {
        id             = "capitano",
        name           = "Capitano Marek",
        relation       = "comandante della guarnigione, diffidente verso il protagonista",
        age            = 48,
        sesso          = "M",
        appearance     = "Corporatura massiccia e squadrata, spalle da chi ha passato anni in mare. "
                      .. "Capelli grigi tagliati corti, cicatrice sottile sul mento sinistro. "
                      .. "Occhi azzurro-grigio che valutano tutto con calma fredda. "
                      .. "Si muove lentamente ma con autorità deliberata. "
                      .. "Porta sempre una giacca logora con il simbolo del grado cucito sul petto.",
        personality    = "In superficie brusco, diretto, intollerante alle scuse. "
                      .. "Sotto: profondamente leale a chi guadagna la sua fiducia, "
                      .. "tormentato da una decisione passata che non ha mai dimenticato. "
                      .. "Usa la durezza come scudo contro l'empatia. "
                      .. "Tic: tamburella le dita sul tavolo quando qualcosa lo preoccupa.",
        goals          = "Proteggere la città da minacce interne ed esterne. Non vuole sangue inutile.",
        secrets        = "Sa più di quanto ammette sulla corruzione nel consiglio cittadino.",
        mood           = "guardingo",
        outfit_default = "uniforme da servizio, usurata ma pulita",
        location_iniziale = "sala_guardie",
        routine = {
            mattino    = "torre_di_guardia",
            pomeriggio = "sala_guardie",
            sera       = "sala_comune",
            notte      = "quartieri_ufficiali",
        },
        triggers = {
            [25] = "Smette di ignorare le domande del protagonista. Risponde, anche se in modo conciso. "
                .. "Lo guarda negli occhi invece di voltarsi dall'altra parte.",
            [50] = "Condivide informazioni che prima teneva per sé. Difende il protagonista davanti ad altri. "
                .. "Inizia le conversazioni anziché aspettare.",
            [75] = "Lo tratta come un pari. Rivela dettagli sul suo passato. "
                .. "In situazioni di crisi, si fida del giudizio del protagonista senza discutere.",
        },
    },
}
```

## Pattern tabella locations

```lua
local locations = {
    sala_guardie = {
        id         = "sala_guardie",
        name       = "Sala delle Guardie",
        desc       = "Stanza rettangolare con tavoli robusti e panche consumate dall'uso. "
                  .. "Odore di cera, ferro e cibo freddo. Mappe appese alle pareti con segni recenti. "
                  .. "Una stufa al centro distribuisce calore disuguale.",
        acoustic   = "I rumori delle strade filtrano dalla finestra con inferriata. "
                  .. "Le conversazioni nella stanza adiacente sono udibili se le voci si alzano. "
                  .. "Una porta massiccia separa dal corridoio — riduce ma non elimina i suoni.",
        connessa_a = { "corridoio_principale", "armeria", "cortile" },
        atmosfera  = "operativa, sorvegliata, semi-pubblica",
    },
    torre_di_guardia = {
        id         = "torre_di_guardia",
        name       = "Torre di Guardia",
        desc       = "Piattaforma sopraelevata con vista sul porto e sulle mura. "
                  .. "Vento costante che porta odore di sale. Isolata, silenziosa. "
                  .. "Il posto preferito del capitano quando deve pensare.",
        acoustic   = "Praticamente isolata. Il vento copre i rumori dalla città. "
                  .. "Una conversazione qui è completamente privata.",
        connessa_a = { "scale_interne" },
        atmosfera  = "privata, esposta al vento, riflessiva",
    },
}
```

## Pattern travel_map

```lua
-- Usa comandi semplici: direzione o nome area
-- Aggiungi alias verbali per facilitare la navigazione
local travel_map = {
    ingresso_principale = {
        dentro       = "sala_comune",
        cortile      = "cortile",
        ["su"]       = "scale_interne",
    },
    sala_guardie = {
        fuori        = "corridoio_principale",
        armeria      = "armeria",
        cortile      = "cortile",
        ["esci"]     = "corridoio_principale",
    },
    torre_di_guardia = {
        giù          = "scale_interne",
        ["scendi"]   = "scale_interne",
    },
}
```

## Pattern ASPETTO_NPC

Traccia i cambiamenti di aspetto/abbigliamento/equipaggiamento degli NPC in base alla progressione narrativa.
Le soglie e le descrizioni devono essere coerenti con il genere del racconto — non sono necessariamente legate a contenuto romantico.

```lua
-- Esempio per storia di alleanze/avventura:
local ASPETTO_NPC = {
    capitano = {
        { 0,  "uniforme da servizio, usurata ma pulita, atteggiamento chiuso" },
        { 25, "stessa uniforme, ma meno rigido nel portamento. Toglie il cappello quando parla" },
        { 50, "veste in modo informale nelle aree private. Mostra la cicatrice che di solito copre" },
        { 75, "abbigliamento da combattimento — si fida abbastanza da combattere al fianco del protagonista" },
    },
}

-- Esempio per storia di mystery/deterioramento:
local ASPETTO_NPC = {
    sospettato = {
        { 0,  "vestito impeccabile, sorriso cordiale, nessuna crepa visibile" },
        { 25, "piccole imprecisioni — cravatta leggermente storta, occhi leggermente stanchi" },
        { 50, "visibilmente sotto pressione. Non sorride più. Sceglie abiti più scuri" },
        { 75, "aspetto trascurato. Non dorme. L'inganno sta diventando difficile da sostenere" },
    },
}

-- Funzione helper (adatta "state.relazione" al nome reale della metrica)
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

## Errori comuni da evitare

```lua
-- ✗ SBAGLIATO: chiave con trattino senza quotes
local NPC = {
    npc-id = { ... }  -- syntax error!
}

-- ✓ CORRETTO
local NPC = {
    npc_id = { ... }  -- snake_case senza trattini
}

-- ✗ SBAGLIATO: tabella travel_map con chiave con spazi senza quotes
travel_map = {
    sala_guardie = { vai al corridoio = "corridoio_principale" }  -- syntax error!
}

-- ✓ CORRETTO: usa quotes per chiavi con spazi
travel_map = {
    sala_guardie = { ["vai al corridoio"] = "corridoio_principale" }
}
```
