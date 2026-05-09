-- =============================================================================
--  tempo_e_destino.lua  —  Tempo e Destino
--  Un dramma di viaggio nel tempo per RpgAi
--
--  Genere:   Drama / Mystery
--  Lingua:   Italiano
--  Autore:   RpgAi project
--
--  Modelli consigliati:
--    anthropic/claude-3.5-sonnet  (OpenRouter)
--    qwen/qwen3-32b               (OpenRouter)
--    llama3.2                     (Ollama, locale)
--
--  Avvio (web):
--    ./build/rpgai --web --provider openrouter --or-key sk-or-... \
--                  --path scripts/ --script tempo_e_destino.lua
--
--  Avvio (console):
--    ./build/rpgai --provider ollama --model llama3.2 \
--                  --path scripts/ --script tempo_e_destino.lua
-- =============================================================================

local json = require("json")

-- =============================================================================
-- DATI DEGLI NPC PER PERIODO
-- Ogni NPC ha una entry per ogni periodo in cui esiste.
-- Le alterazioni vengono sovrascritte in get_npc_in_periodo().
-- =============================================================================

local NPC_BASE = {
    elena_morandi = {
        periodi = {
            ["1965"] = {
                nome        = "Elena Morandi",
                eta         = "18 anni",
                aspetto     = "Capelli scuri raccolti con una forcina, occhi vivaci e irrequieti, sempre con un libro sotto il braccio. Veste in modo semplice ma curato — camicetta bianca, gonna a fiori.",
                personalita = "Idealista e brillante. Studia lettere a Bologna ma torna al paese d'estate. Ha una cotta silenziosa per Marco, ma lui è tre anni più giovane e non sembra vederla. Si nasconde nella biblioteca quando non vuole pensare.",
                location_base = "biblioteca",
                routine     = "La mattina studia in biblioteca. Il pomeriggio passeggia in piazza sperando di incrociare Marco senza sembrarlo."
            },
            ["1990"] = {
                nome        = "Elena Morandi",
                eta         = "43 anni",
                aspetto     = "Capelli scuri con filamenti grigi, occhiali nuovi con la montatura anni '80. Aria stanca ma dignitosa. Veste in modo professionale — blusa scura, gonna al ginocchio.",
                personalita = "Ha rinunciato ai sogni romantici dopo decenni di attesa silenziosa. Fa la bibliotecaria e insegna italiano al liceo. Sola, non del tutto infelice — ma spesso malinconica senza sapere perché. Parla poco di sé.",
                location_base = "biblioteca",
                routine     = "Mattina in biblioteca. Pomeriggio al liceo. La sera legge."
            },
            ["presente"] = {
                nome        = "Elena Morandi",
                eta         = "77 anni",
                aspetto     = "Capelli completamente bianchi, portati con cura. Occhi ancora lucidi e attenti dietro gli occhiali. Cammina con un bastone da quando è scivolata tre inverni fa.",
                personalita = "Saggia, ironica, nostalgica. Ha vissuto una vita piena di lavoro e poco amore. Ricorda il passato con una chiarezza quasi dolorosa — ogni stagione, ogni occasione mancata. Non si lamenta mai.",
                location_base = "casa_morandi",
                routine     = "Mattina a casa a leggere. Pomeriggio in piazza se il tempo è bello."
            }
        }
    },
    marco_ferretti = {
        periodi = {
            ["1965"] = {
                nome        = "Marco Ferretti",
                eta         = "15 anni",
                aspetto     = "Alto per la sua età, mani già callose per il lavoro in bottega. Capelli corvini, sguardo vivo e un po' ribelle. Ha sempre uno schizzo di progetto in tasca.",
                personalita = "Sognatore. Vuole diventare ingegnere, disegnare ponti. Il padre insiste che erediterà la bottega come da tradizione di famiglia. Si vergogna un po' di Elena perché è più grande di lui. Ma ci pensa.",
                location_base = "bottega_ferretti",
                routine     = "Mattina obbligata in bottega. Pomeriggio in piazza o alla fontana a disegnare."
            },
            ["1990"] = {
                nome        = "Marco Ferretti",
                eta         = "40 anni",
                aspetto     = "Invecchiato prima del tempo. Mani forti ma occhi spenti. Capelli radi con qualche ciuffo grigio. Spesso sa di grappa.",
                personalita = "Amareggiato. Ha ereditato la bottega come il padre voleva. La moglie l'ha lasciato dieci anni fa. Beve troppo. Ha una figlia, Lucia, che vede raramente. A volte, ubriaco, parla ancora di ponti.",
                location_base = "osteria",
                routine     = "Lavora poco in bottega la mattina. Pomeriggio all'osteria fino a sera."
            },
            ["presente"] = {
                nome        = "Marco Ferretti",
                eta         = "70 anni",
                aspetto     = "Vecchio e solitario. Mani tremanti, faccia segnata. La bottega è chiusa da anni. Esce di rado.",
                personalita = "Rimpianto profondo. Sa di aver sprecato la sua vita. Parla poco, ma quando parla dice cose vere. Non vede Lucia da anni — lei è andata a Milano e non torna. Ogni tanto si siede in piazza il giovedì.",
                location_base = "piazza",
                routine     = "Quasi sempre in casa. Solo il giovedì esce per la spesa e si ferma in piazza."
            }
        }
    },
    lucia_ferretti = {
        periodi = {
            ["1965"] = nil,  -- non ancora nata
            ["1990"] = {
                nome        = "Lucia Ferretti",
                eta         = "15 anni",
                aspetto     = "Occhi intelligenti come quelli del padre giovane. Capelli ricci legati con un elastico. Sempre con lo zaino pieno di libri di matematica e fisica.",
                personalita = "Brillante e determinata. Ama le scienze esatte. Ha imparato a ignorare il padre quando beve e ad andare avanti da sola. È più matura della sua età.",
                location_base = "piazza",
                routine     = "Scuola la mattina, pomeriggio in piazza o a casa di amiche."
            },
            ["presente"] = {
                nome        = "Lucia Ferretti",
                eta         = "45 anni",
                aspetto     = "Donna elegante, capelli ricci tenuti corti. Veste bene — è abituata alla vita di città. Raramente torna al paese.",
                personalita = "Ingegnera strutturale a Milano. Ha tagliato quasi completamente i ponti con il padre. Il paese le ricorda un'infanzia difficile. Ogni tanto pensa di tornare, ma trova sempre una ragione per rimandare.",
                location_base = nil  -- normalmente assente
            }
        }
    }
}

-- =============================================================================
-- DATI DELLE LOCATION PER PERIODO
-- =============================================================================

local LOC_BASE = {
    piazza = {
        nome        = "Piazza del Paese",
        connessioni = { "biblioteca", "casa_morandi", "osteria", "bottega_ferretti" },
        periodi = {
            ["1965"] = {
                descrizione = "La piazza è il cuore pulsante del paese. Bambini rincorrono i piccioni, le donne chiacchierano sotto i portici, il giovedì i banchi del mercato occupano ogni angolo. Profumo di pane fresco e gasolio.",
                sonoro      = "Voci, risate di bambini, il campanile che suona i quarti d'ora."
            },
            ["1990"] = {
                descrizione = "Qualcosa nella piazza si è esaurito. Qualche negozio ha abbassato la serranda, rimpiazzato da una sala giochi rumorosa. Le panchine sono occupate da anziani che guardano il nulla. I giovani sono andati in città.",
                sonoro      = "Il borbottio di una TV dal bar, qualche motorino che passa, silenzio tra un suono e l'altro."
            },
            ["presente"] = {
                descrizione = "La piazza sopravvive a stento. Il bar tabacchi ha chiuso l'anno scorso. Dove c'era la merceria ora c'è un B&B con un cartello in tre lingue. D'estate i turisti si fermano a fotografare il campanile; il resto dell'anno è quasi deserta.",
                sonoro      = "Vento. Qualche piccione. In lontananza, un cantiere."
            }
        }
    },
    biblioteca = {
        nome        = "Biblioteca Comunale",
        connessioni = { "piazza" },
        periodi = {
            ["1965"] = {
                descrizione = "Piccola biblioteca polverosa gestita dal vecchio signor Tosi. Gli scaffali traboccano di libri ingialliti in ordine approssimativo. D'estate è l'unico posto fresco del paese.",
                sonoro      = "Silenzio quasi assoluto. Solo lo scricchiolio del parquet e il respiro di chi legge."
            },
            ["1990"] = {
                descrizione = "Ristrutturata negli anni '80. Elena Morandi l'ha trasformata in un luogo ordinato e accogliente — nuovi scaffali metallici affiancano i vecchi in legno scuro, e c'è persino un computer per il catalogo.",
                sonoro      = "Foglio di pagine, la voce bassa di Elena ai visitatori, il ronzio del computer."
            },
            ["presente"] = {
                descrizione = "Elena tiene aperta la biblioteca tre giorni alla settimana con il sussidio del comune. Gli scaffali sono pieni di donazioni ordinate con cura. Nell'angolo, due poltrone consumate invitano alla lettura.",
                sonoro      = "Radio classica in sottofondo. Quasi sempre silenzio."
            }
        }
    },
    casa_morandi = {
        nome        = "Casa Morandi",
        connessioni = { "piazza" },
        periodi = {
            ["1965"] = {
                descrizione = "Una villa borghese con giardino ben curato e rosai in fiore. I genitori di Elena — il professor Morandi e sua moglie — sono rispettati in paese. La casa profuma di cera da parquet e carta.",
                sonoro      = "Il ticchettio regolare di una macchina da scrivere dallo studio del professore."
            },
            ["1990"] = {
                descrizione = "I genitori sono morti. Elena vive sola nella casa grande. Ha conservato tutto esattamente com'era, un po' per rispetto, un po' perché non sa da dove cominciare. Alcune stanze sono sempre chiuse.",
                sonoro      = "Silenzio pesante. Il rumore di una casa troppo grande per una persona sola."
            },
            ["presente"] = {
                descrizione = "Elena ha trasformato tre stanze in una piccola pensione per turisti estivi. Il giardino è trascurato, ma il salone conserva i mobili originali e le librerie del padre con i loro diecimila volumi.",
                sonoro      = "Scricchiolio del legno. In lontananza, la voce di Elena che parla al telefono."
            }
        }
    },
    bottega_ferretti = {
        nome        = "Bottega del Fabbro Ferretti",
        connessioni = { "piazza" },
        periodi = {
            ["1965"] = {
                descrizione = "La bottega del vecchio Ferretti è rumorosa e vitale. Il padre di Marco lavora al fuoco con ritmo preciso. Odore di ferro caldo, carbone e sudore. Marco aiuta malvolentieri — i suoi occhi guardano altrove.",
                sonoro      = "Martellate cadenzate, il soffio del mantice, il padre che urla ordini senza malevolenza."
            },
            ["1990"] = {
                descrizione = "Marco ha ereditato la bottega ma ci lavora poco. Gli attrezzi del padre sono gli stessi di venticinque anni fa. Qualche cliente viene ancora, ma la concorrenza del ferramenta in città si fa sentire sempre di più.",
                sonoro      = "Colpi di martello irregolari intervallati da lunghi silenzi. Una radio suona in sottofondo."
            },
            ["presente"] = {
                descrizione = "Un cartello scritto a mano recita 'IN VENDITA'. Attraverso il vetro sporco si vedono gli attrezzi del nonno di Marco, coperti di polvere, ancora al loro posto. Marco vive nel retro ma non apre più da anni.",
                sonoro      = "Silenzio totale. Qualche piccione sul tetto."
            }
        }
    },
    osteria = {
        nome        = "L'Osteria del Gallo",
        connessioni = { "piazza" },
        periodi = {
            ["1965"] = {
                descrizione = "Il ritrovo serale degli uomini del paese. Gianni il padrone conosce tutti i segreti. Si gioca a carte con vero trasporto, si discute di politica ad alta voce, la pasta al ragù costa poco e vale molto.",
                sonoro      = "Voci accaldate, carte che sbattono sul tavolo, qualcuno che canta una canzone che tutti conoscono."
            },
            ["1990"] = {
                descrizione = "L'osteria è diventata il posto dove Marco Ferretti passa i pomeriggi. Il figlio di Gianni ha ereditato il locale ma non il carattere del padre. L'aria è cambiata — meno allegria, più silenziosa malinconia.",
                sonoro      = "TG in TV, bicchieri che tintinnano, conversazioni a mezza voce che non vanno da nessuna parte."
            },
            ["presente"] = {
                descrizione = "Nuovi proprietari milanesi hanno ristrutturato tutto. Aperitivi il venerdì, cucina fusion la sera. L'insegna del Gallo è rimasta — tutto il resto è cambiato. In estate è piena di turisti.",
                sonoro      = "Musica di sottofondo scelta a caso da uno speaker Bluetooth. Cappuccinatore. Risate in lingue diverse."
            }
        }
    }
}

-- =============================================================================
-- STATE
-- =============================================================================

local state = {}

local function default_state()
    return {
        nome_giocatore      = "Il Viaggiatore",
        periodo             = "presente",
        location            = "piazza",
        turns_in_periodo    = 0,
        cronometro          = 0,
        -- Overlay delle alterazioni: [npc_id o loc_id][periodo] = {evento, effetto}
        alterazioni_personaggi  = {},
        alterazioni_ambientali  = {},
        -- Log ordinato per /timeline
        log_temporale       = {},
        modifiche_contatore = 0,
        paradosso_rischio   = 0,
        epilogo_disponibile = false,
        game_over           = false,
        _last_image_loc     = nil
    }
end

-- =============================================================================
-- HELPERS MOTORE TEMPORALE
-- =============================================================================

local function nome_periodo(p)
    if p == "presente" then return "Presente (2024)" end
    return p
end

local function get_npc_in_periodo(npc_id, periodo)
    local npc_base = NPC_BASE[npc_id]
    if not npc_base then return nil end
    local dati = npc_base.periodi[periodo]
    if not dati then return nil end

    local npc = {}
    for k, v in pairs(dati) do npc[k] = v end
    npc.id = npc_id

    -- Applica alterazioni se esistono
    local alt = state.alterazioni_personaggi[npc_id]
    if alt and alt[periodo] then
        local a = alt[periodo]
        npc.personalita = npc.personalita .. "\n[STORIA ALTERATA: " .. a.effetto .. "]"
        npc.alterato    = true
        npc.evento_alt  = a.evento
    end

    return npc
end

local function get_location_in_periodo(loc_id, periodo)
    local loc_base = LOC_BASE[loc_id]
    if not loc_base then return nil end
    local p = loc_base.periodi[periodo]
    if not p then return nil end

    local loc = {
        id          = loc_id,
        nome        = loc_base.nome,
        descrizione = p.descrizione,
        sonoro      = p.sonoro,
        connessioni = loc_base.connessioni
    }

    -- Applica alterazioni ambientali
    local alt = state.alterazioni_ambientali[loc_id]
    if alt and alt[periodo] then
        loc.descrizione = loc.descrizione .. "\n[LUOGO ALTERATO: " .. alt[periodo].effetto .. "]"
        loc.alterata = true
    end

    return loc
end

local function presenti_in_periodo(loc_id, periodo)
    local presenti = {}
    for npc_id, npc_base in pairs(NPC_BASE) do
        local dati = npc_base.periodi[periodo]
        if dati and dati.location_base == loc_id then
            local npc = get_npc_in_periodo(npc_id, periodo)
            if npc then table.insert(presenti, npc) end
        end
    end
    return presenti
end

-- Controlla se il finale è possibile
local function aggiorna_epilogo()
    local e = state.alterazioni_personaggi["elena_morandi"]
    local m = state.alterazioni_personaggi["marco_ferretti"]
    local elena_ok = e and (e["1965"] or e["1990"])
    local marco_ok = m and (m["1965"] or m["1990"])
    state.epilogo_disponibile = (elena_ok and marco_ok) == true
end

-- =============================================================================
-- FUNZIONI RICHIESTE DAL MOTORE
-- =============================================================================

function get_welcome_message()
    return [[
═══════════════════════════════════════════════════════════
  TEMPO E DESTINO
  Un dramma di viaggio nel tempo per RpgAi
═══════════════════════════════════════════════════════════

Nella soffitta di tua nonna hai trovato un orologio —
un Longines d'oro, antico, con le iniziali "E.M." incise
sul coperchio. Quando hai caricato la molla,
l'aria intorno a te ha smesso di muoversi.

Poi sei arrivato qui: un piccolo paese del Nord Italia.
Le stesse strade di mattoni. Le stesse pietre.
Ma il calendario sul muro del bar dice 2024.

Puoi spostarti tra il 1965, il 1990 e il presente.
Ogni tua azione nel passato cambia qualcosa nel futuro.
Quante vite puoi aggiustare prima che il tessuto si strappi?

Comandi:
  /1965      — viaggia al 1965
  /1990      — viaggia al 1990
  /presente  — torna al presente
  /timeline  — mostra le alterazioni che hai fatto
  /ispeziona — dettagli sui personaggi nella location corrente
  /epilogo   — (disponibile dopo aver alterato abbastanza)

Come ti chiami, viaggiatore?
(Premi Invio per giocare come "Il Viaggiatore")
]]
end

function set_initial_state(player_input)
    state = default_state()
    if player_input and player_input ~= "" then
        state.nome_giocatore = player_input
    end
end

function generate_initial_state()
    state = default_state()
end

function get_status_for_ai()
    local loc      = get_location_in_periodo(state.location, state.periodo)
    local presenti = presenti_in_periodo(state.location, state.periodo)

    -- Alterazioni attive in questo periodo
    local alt_attive = {}
    for npc_id, per_periodo in pairs(state.alterazioni_personaggi) do
        if per_periodo[state.periodo] then
            table.insert(alt_attive, {
                tipo    = "personaggio",
                target  = npc_id,
                effetto = per_periodo[state.periodo].effetto
            })
        end
    end
    for loc_id, per_periodo in pairs(state.alterazioni_ambientali) do
        if per_periodo[state.periodo] then
            table.insert(alt_attive, {
                tipo    = "ambiente",
                target  = loc_id,
                effetto = per_periodo[state.periodo].effetto
            })
        end
    end

    local ctx = {
        giocatore = {
            nome              = state.nome_giocatore,
            periodo           = nome_periodo(state.periodo),
            location          = loc and loc.nome or state.location,
            turni_nel_periodo = state.turns_in_periodo
        },
        location_corrente  = loc,
        npc_presenti       = presenti,
        alterazioni_attive = alt_attive,
        stato = {
            modifiche_totali    = state.modifiche_contatore,
            paradosso_rischio   = state.paradosso_rischio,
            epilogo_disponibile = state.epilogo_disponibile
        }
    }

    return json.encode(ctx)
end

function get_system_prompt()
    local loc      = get_location_in_periodo(state.location, state.periodo)
    local presenti = presenti_in_periodo(state.location, state.periodo)

    local npc_parts = {}
    for _, npc in ipairs(presenti) do
        local riga = string.format("- %s (%s): %s", npc.nome, npc.eta, npc.personalita)
        if npc.alterato then riga = riga .. "  *** STORIA GIÀ ALTERATA ***" end
        table.insert(npc_parts, riga)
    end
    local npc_desc = #npc_parts > 0 and table.concat(npc_parts, "\n") or "(nessuno)"

    local loc_nota = ""
    local alt_loc  = state.alterazioni_ambientali[state.location]
    if alt_loc and alt_loc[state.periodo] then
        loc_nota = "\n*** LUOGO ALTERATO: " .. alt_loc[state.periodo].effetto .. " ***"
    end

    local rischio_testo = "basso"
    if state.paradosso_rischio >= 4 then rischio_testo = "medio — sii cauto" end
    if state.paradosso_rischio >= 7 then rischio_testo = "ALTO — ogni azione può rompere la linea" end

    return string.format([[
Sei il narratore di un dramma italiano di viaggio nel tempo. Il protagonista è %s.

PERIODO: %s
LOCATION: %s
%s%s

PERSONAGGI PRESENTI:
%s

MECCANICA TEMPORALE:
- Il giocatore può alterare la storia parlando con i personaggi o causando eventi
- Quando un'alterazione avviene, imposta il campo "alterazione" nello schema JSON
- I ripple_effects sono le conseguenze a cascata — il narratore (tu) li decide
- Esempi di alterazione: convincere Marco a iscriversi all'università nel 1965;
  far trovare ad Elena una lettera che non ha mai scritto; far litigare due persone
  in un modo che cambia la loro relazione decennale
- Le alterazioni fisiche dirette (sparire qualcuno, cambiare eventi già accaduti)
  aumentano il rischio paradosso. Le alterazioni emotive e conversazionali no.
- Rischio paradosso attuale: %s (%d/10). Se raggiunge 10, il viaggio finisce male.

TONO:
- Italiano letterario, terza persona, tempo presente
- Dramma umano e malinconia del tempo che passa — non fantascienza
- Dettagli sensoriali: luce, odori, texture del passato e del presente
- Quando descrivi un'alterazione che si riflette in un altro periodo,
  usa flashback brevi intercalati nella narrazione
- 3-5 frasi per risposta

OBIETTIVO NARRATIVO:
Elena e Marco hanno sprecato le loro vite a non dirsi le cose.
Il viaggiatore può cambiare questo — o creare conseguenze imprevedibili.
Non c'è una risposta giusta. Solo scelte.

Rispondi SOLO con il JSON specificato nello schema. Zero testo fuori dal JSON.
]],
        state.nome_giocatore,
        nome_periodo(state.periodo),
        loc and loc.nome or state.location,
        loc and ("\n" .. loc.descrizione) or "",
        loc_nota,
        npc_desc,
        rischio_testo,
        state.paradosso_rischio
    )
end

function get_json_schema_prompt()
    return [[
Rispondi ESCLUSIVAMENTE con un oggetto JSON con questa struttura:
{
  "narration": "3-5 frasi in italiano, terza persona, tempo presente",
  "nuova_location": "id location oppure stringa vuota",
  "viaggio_temporale": "1965" oppure "1990" oppure "presente" oppure null,
  "alterazione": {
    "tipo": "evento_passato" oppure "oggetto" oppure "conversazione",
    "target_id": "id npc o location",
    "target_tipo": "personaggio" oppure "ambiente",
    "periodo_target": "1965" oppure "1990" oppure "presente",
    "evento": "breve descrizione dell'evento alterato",
    "effetto": "effetto sulla storia del personaggio o luogo",
    "ripple_effects": [
      { "target_id": "id", "target_tipo": "personaggio", "periodo": "1990", "effetto": "..." }
    ]
  },
  "paradosso_delta": 0,
  "game_over": false,
  "game_over_reason": ""
}
Imposta alterazione a null se nessuna alterazione avviene.
Ometti ripple_effects se vuoto.
Zero testo fuori dal JSON. Nessun commento. Solo JSON valido.
]]
end

function get_json_schema()
    return [[{
        "type": "object",
        "required": ["narration", "game_over"],
        "additionalProperties": false,
        "properties": {
            "narration": {
                "type": "string",
                "description": "3-5 frasi in italiano, terza persona, tempo presente"
            },
            "nuova_location": {
                "type": "string",
                "enum": ["", "piazza", "biblioteca", "casa_morandi", "bottega_ferretti", "osteria"],
                "description": "id location se il giocatore si sposta, stringa vuota altrimenti"
            },
            "viaggio_temporale": {
                "type": ["string", "null"],
                "enum": ["1965", "1990", "presente", null],
                "description": "periodo di destinazione se il giocatore chiede di viaggiare, null altrimenti"
            },
            "alterazione": {
                "type": ["object", "null"],
                "description": "null se nessuna alterazione avviene in questo turno",
                "additionalProperties": false,
                "properties": {
                    "tipo": {
                        "type": "string",
                        "enum": ["evento_passato", "oggetto", "conversazione"],
                        "description": "tipo di alterazione"
                    },
                    "target_id": {
                        "type": "string",
                        "description": "id npc o location del target principale"
                    },
                    "target_tipo": {
                        "type": "string",
                        "enum": ["personaggio", "ambiente"]
                    },
                    "periodo_target": {
                        "type": "string",
                        "enum": ["1965", "1990", "presente"]
                    },
                    "evento": {
                        "type": "string",
                        "description": "breve descrizione dell'evento alterato, max 80 caratteri"
                    },
                    "effetto": {
                        "type": "string",
                        "description": "effetto principale sull'arco narrativo del target, max 120 caratteri"
                    },
                    "ripple_effects": {
                        "type": "array",
                        "description": "conseguenze a cascata in altri periodi o su altri target",
                        "items": {
                            "type": "object",
                            "additionalProperties": false,
                            "properties": {
                                "target_id":   { "type": "string" },
                                "target_tipo": { "type": "string", "enum": ["personaggio", "ambiente"] },
                                "periodo":     { "type": "string", "enum": ["1965", "1990", "presente"] },
                                "effetto":     { "type": "string" }
                            }
                        }
                    }
                }
            },
            "paradosso_delta": {
                "type": "integer",
                "minimum": -2,
                "maximum": 3,
                "description": "variazione al rischio paradosso: 0=normale, 1-3=azione rischiosa, negativo=risolve tensione"
            },
            "game_over": { "type": "boolean" },
            "game_over_reason": {
                "type": "string",
                "description": "motivazione se game_over, stringa vuota altrimenti"
            }
        }
    }]]
end

function process_ai_response(reply)
    local ok, data = pcall(json.decode, reply)
    if not ok or type(data) ~= "table" then
        return { success=false, error="JSON non valido: " .. tostring(data) }
    end

    local narrazione = data.narration
    if not narrazione or narrazione == "" then
        return { success=false, error="Campo narration mancante" }
    end

    -- Spostamento location
    if type(data.nuova_location) == "string" and data.nuova_location ~= "" then
        if LOC_BASE[data.nuova_location] then
            state.location = data.nuova_location
        end
    end

    -- Viaggio temporale
    if type(data.viaggio_temporale) == "string" then
        local validi = { ["1965"]=true, ["1990"]=true, ["presente"]=true }
        if validi[data.viaggio_temporale] then
            state.periodo = data.viaggio_temporale
            state.turns_in_periodo = 0
        end
    end

    -- Applica alterazione
    if type(data.alterazione) == "table" then
        local alt = data.alterazione
        if type(alt.target_id) == "string" and alt.target_id ~= "" and
           type(alt.periodo_target) == "string" and
           type(alt.effetto) == "string" and alt.effetto ~= "" then

            local voce = { evento = alt.evento or "", effetto = alt.effetto }

            if alt.target_tipo == "personaggio" then
                if not state.alterazioni_personaggi[alt.target_id] then
                    state.alterazioni_personaggi[alt.target_id] = {}
                end
                -- Un'alterazione diretta ha precedenza sui ripple
                state.alterazioni_personaggi[alt.target_id][alt.periodo_target] = voce

            elseif alt.target_tipo == "ambiente" then
                if not state.alterazioni_ambientali[alt.target_id] then
                    state.alterazioni_ambientali[alt.target_id] = {}
                end
                state.alterazioni_ambientali[alt.target_id][alt.periodo_target] = voce
            end

            -- Applica ripple effects (senza sovrascrivere alterazioni dirette)
            if type(alt.ripple_effects) == "table" then
                for _, r in ipairs(alt.ripple_effects) do
                    if type(r.target_id) == "string" and r.target_id ~= "" and
                       type(r.periodo) == "string" and
                       type(r.effetto) == "string" and r.effetto ~= "" then

                        local ripple_voce = {
                            evento  = "effetto a cascata da '" .. alt.target_id .. "'",
                            effetto = r.effetto
                        }
                        if r.target_tipo == "personaggio" then
                            if not state.alterazioni_personaggi[r.target_id] then
                                state.alterazioni_personaggi[r.target_id] = {}
                            end
                            if not state.alterazioni_personaggi[r.target_id][r.periodo] then
                                state.alterazioni_personaggi[r.target_id][r.periodo] = ripple_voce
                            end
                        elseif r.target_tipo == "ambiente" then
                            if not state.alterazioni_ambientali[r.target_id] then
                                state.alterazioni_ambientali[r.target_id] = {}
                            end
                            if not state.alterazioni_ambientali[r.target_id][r.periodo] then
                                state.alterazioni_ambientali[r.target_id][r.periodo] = ripple_voce
                            end
                        end
                    end
                end
            end

            -- Registra nel log
            table.insert(state.log_temporale, {
                turno   = state.cronometro,
                periodo = alt.periodo_target,
                target  = alt.target_id,
                evento  = alt.evento or "",
                effetto = alt.effetto
            })
            state.modifiche_contatore = state.modifiche_contatore + 1
            aggiorna_epilogo()
        end
    end

    -- Aggiorna rischio paradosso
    if type(data.paradosso_delta) == "number" then
        state.paradosso_rischio = math.max(0, math.min(10,
            state.paradosso_rischio + data.paradosso_delta))
    end

    -- Paradosso temporale (sconfitta)
    if state.paradosso_rischio >= 10 then
        return {
            success          = true,
            narration        = narrazione .. "\n\nL'orologio vibra con violenza crescente. Le lancette "
                            .. "girano all'impazzata. Il tessuto del tempo, logoro per troppe "
                            .. "interferenze, si strappa — e il viaggiatore svanisce tra le pieghe "
                            .. "della storia, dimenticato da tutti, inclusa se stesso.",
            game_over        = true,
            game_over_reason = "PARADOSSO TEMPORALE — Hai alterato troppo la linea del tempo."
        }
    end

    -- Game over dall'LLM
    if data.game_over == true then
        return {
            success          = true,
            narration        = narrazione,
            game_over        = true,
            game_over_reason = data.game_over_reason or "Il viaggio finisce qui."
        }
    end

    state.cronometro = state.cronometro + 1
    state.turns_in_periodo = state.turns_in_periodo + 1

    return {
        success          = true,
        narration        = narrazione,
        game_over        = false,
        game_over_reason = ""
    }
end

function get_display_state()
    local loc      = get_location_in_periodo(state.location, state.periodo)
    local loc_nome = loc and loc.nome or state.location
    local epilogo_str = state.epilogo_disponibile and "  [/epilogo]" or ""

    return string.format(
        "[ %s | %s ]  Alterazioni: %d  Paradosso: %d/10  Turno: %d%s",
        nome_periodo(state.periodo),
        loc_nome,
        state.modifiche_contatore,
        state.paradosso_rischio,
        state.cronometro,
        epilogo_str
    )
end

function get_state_snapshot()
    return json.encode(state)
end

function restore_state(snapshot)
    local ok, data = pcall(json.decode, snapshot)
    if not ok then
        return { success=false, error="Snapshot non valido: " .. tostring(data) }
    end
    state = data
    return { success=true }
end

function process_player_input(input)
    local cmd = input:lower():match("^(/[%w_]+)")

    -- Viaggi temporali
    if cmd == "/1965" then
        state.periodo = "1965"
        state.turns_in_periodo = 0
        return { success=true, handled=true,
            output = "L'orologio trema. I colori intorno a te sbiadiscono e cambiano tono — seppia, polvere, luce dorata. Le voci tornano. Sei nel 1965." }
    end

    if cmd == "/1990" then
        state.periodo = "1990"
        state.turns_in_periodo = 0
        return { success=true, handled=true,
            output = "Le lancette si azzerano. L'aria cambia sapore — plastica, benzina senza piombo, qualcosa di malinconico nell'aria. Il 1990 ti accoglie." }
    end

    if cmd == "/presente" then
        state.periodo = "presente"
        state.turns_in_periodo = 0
        local msg = "Torni al presente. "
        if state.modifiche_contatore == 0 then
            msg = msg .. "Tutto è esattamente come lo hai lasciato."
        elseif state.modifiche_contatore <= 2 then
            msg = msg .. "C'è qualcosa di diverso nell'aria — difficile da definire, come una parola che hai quasi ma non del tutto ricordato."
        else
            msg = msg .. "Il cambiamento è palpabile. Persone, luoghi, silenzi — portano il segno delle tue interferenze."
        end
        return { success=true, handled=true, output=msg }
    end

    -- /timeline
    if cmd == "/timeline" then
        if #state.log_temporale == 0 then
            return { success=true, handled=true,
                output = "Nessuna alterazione registrata. La linea del tempo è intatta." }
        end
        local righe = { "=== ALTERAZIONI TEMPORALI ===" }
        for i, entry in ipairs(state.log_temporale) do
            table.insert(righe, string.format(
                "%d. [%s] %s — %s", i, entry.periodo, entry.target, entry.effetto))
        end
        table.insert(righe, string.format(
            "\nTotale: %d  |  Rischio paradosso: %d/10",
            #state.log_temporale, state.paradosso_rischio))
        return { success=true, handled=true, output=table.concat(righe, "\n") }
    end

    -- /ispeziona
    if cmd == "/ispeziona" then
        local presenti = presenti_in_periodo(state.location, state.periodo)
        if #presenti == 0 then
            return { success=true, handled=true,
                output = "Nessun personaggio rilevante in questa location in questo periodo." }
        end
        local righe = { string.format("=== PERSONAGGI [%s] ===", nome_periodo(state.periodo)) }
        for _, npc in ipairs(presenti) do
            table.insert(righe, string.format("\n%s (%s)", npc.nome, npc.eta))
            table.insert(righe, "Aspetto: " .. npc.aspetto)
            table.insert(righe, "Carattere: " .. npc.personalita)
            if npc.alterato then
                table.insert(righe, "[STORIA ALTERATA — evento: " .. (npc.evento_alt or "?") .. "]")
            end
        end
        return { success=true, handled=true, output=table.concat(righe, "\n") }
    end

    -- /epilogo
    if cmd == "/epilogo" then
        if not state.epilogo_disponibile then
            return { success=true, handled=true,
                output = "L'epilogo non è ancora disponibile.\nDevi alterare la storia sia di Elena che di Marco (nei periodi 1965 o 1990)." }
        end

        local righe = { "=== EPILOGO ===" }
        table.insert(righe, "")
        table.insert(righe, "L'orologio nella tua mano si ferma. Le lancette non si muovono più.")
        table.insert(righe, "")

        local e = state.alterazioni_personaggi["elena_morandi"]
        local m = state.alterazioni_personaggi["marco_ferretti"]

        if e then
            local effetto = (e["1965"] and e["1965"].effetto) or
                            (e["1990"] and e["1990"].effetto) or "storia invariata"
            table.insert(righe, "ELENA: " .. effetto)
        end
        if m then
            local effetto = (m["1965"] and m["1965"].effetto) or
                            (m["1990"] and m["1990"].effetto) or "storia invariata"
            table.insert(righe, "MARCO: " .. effetto)
        end

        local lucia = state.alterazioni_personaggi["lucia_ferretti"]
        if lucia then
            local effetto = (lucia["1990"] and lucia["1990"].effetto) or
                            (lucia["presente"] and lucia["presente"].effetto)
            if effetto then table.insert(righe, "LUCIA: " .. effetto) end
        end

        table.insert(righe, "")
        table.insert(righe, "Poi l'orologio si scalda tra le tue dita.")
        table.insert(righe, "Ricordi il suo peso — e poi svanisce.")
        table.insert(righe, "")
        table.insert(righe, "Hai fatto il tuo lavoro. O forse no.")
        table.insert(righe, "Solo il tempo dirà se era giusto.")
        table.insert(righe, "")
        table.insert(righe, string.format(
            "[Alterazioni: %d  |  Rischio paradosso finale: %d/10]",
            state.modifiche_contatore, state.paradosso_rischio))

        return { success=true, handled=true, output=table.concat(righe, "\n") }
    end

    return { success=true, handled=false }
end

-- =============================================================================
-- SISTEMA IMMAGINI (opzionale)
-- =============================================================================

local IMAGE_DIR   = "images/tempo_e_destino/"
local IMAGE_STYLE = "Italian neorealist film photography, grainy film texture, "
                 .. "sepia-warm tones for 1965, faded warm for 1990, desaturated blue-grey for 2024, "
                 .. "melancholic atmosphere, cinematic composition"

local ASSET_PATHS = {
    piazza_1965              = IMAGE_DIR .. "bg_piazza_1965.jpg",
    piazza_1990              = IMAGE_DIR .. "bg_piazza_1990.jpg",
    piazza_presente          = IMAGE_DIR .. "bg_piazza_2024.jpg",
    biblioteca_1965          = IMAGE_DIR .. "bg_biblioteca_1965.jpg",
    biblioteca_1990          = IMAGE_DIR .. "bg_biblioteca_1990.jpg",
    biblioteca_presente      = IMAGE_DIR .. "bg_biblioteca_2024.jpg",
    casa_morandi_1965        = IMAGE_DIR .. "bg_casa_1965.jpg",
    casa_morandi_1990        = IMAGE_DIR .. "bg_casa_1990.jpg",
    casa_morandi_presente    = IMAGE_DIR .. "bg_casa_2024.jpg",
    bottega_ferretti_1965    = IMAGE_DIR .. "bg_bottega_1965.jpg",
    bottega_ferretti_1990    = IMAGE_DIR .. "bg_bottega_1990.jpg",
    bottega_ferretti_presente = IMAGE_DIR .. "bg_bottega_2024.jpg",
    osteria_1965             = IMAGE_DIR .. "bg_osteria_1965.jpg",
    osteria_1990             = IMAGE_DIR .. "bg_osteria_1990.jpg",
    osteria_presente         = IMAGE_DIR .. "bg_osteria_2024.jpg",
    elena_1965               = IMAGE_DIR .. "npc_elena_1965.jpg",
    elena_1990               = IMAGE_DIR .. "npc_elena_1990.jpg",
    elena_presente           = IMAGE_DIR .. "npc_elena_2024.jpg",
    marco_1965               = IMAGE_DIR .. "npc_marco_1965.jpg",
    marco_1990               = IMAGE_DIR .. "npc_marco_1990.jpg",
    marco_presente           = IMAGE_DIR .. "npc_marco_2024.jpg",
    lucia_1990               = IMAGE_DIR .. "npc_lucia_1990.jpg",
}

local ASSET_DESC = {
    piazza_1965   = "Italian village piazza 1965, children playing near fountain, weekly market stalls, women chatting under porticos, Vespa parked nearby, warm golden afternoon light, wide shot",
    piazza_1990   = "Italian village piazza 1990, fewer people, arcade hall visible, elderly men on benches, some shops closed, slightly melancholic, warm faded tones, wide shot",
    piazza_presente = "Italian village piazza 2024, nearly deserted, B&B sign on old shop, pigeons on benches, tourist brochures, quiet decline, cool blue-grey light, wide shot",
    biblioteca_1965 = "Small dusty Italian library 1965, tall wooden bookshelves packed with yellowed books, old male librarian at oak desk, single hanging light, cool summer interior",
    biblioteca_1990 = "Italian library 1990 renovated, metal shelves mixed with old wood, female librarian organizing, early desktop computer, orderly and quiet, warm indirect light",
    biblioteca_presente = "Small Italian library 2024, reading corner with worn armchairs, donated books on shelves, classical music softly playing, elderly female librarian, intimate atmosphere",
    casa_morandi_1965 = "Bourgeois Italian villa 1965, tended garden with roses, interior parquet floors, bookshelves in every room, typewriter on desk, beeswax smell, warm domestic light",
    casa_morandi_1990 = "Italian villa 1990, unchanged 1960s furniture, single woman living alone in large house, some rooms closed, preserved nostalgia, afternoon light through shutters",
    casa_morandi_presente = "Italian villa converted to small guesthouse 2024, original furniture preserved, untended garden, warm interior, elderly woman as host, golden hour light",
    bottega_ferretti_1965 = "Italian blacksmith workshop 1965, forge glowing orange, muscular older smith working rhythmically, teenage boy reluctantly helping, hot iron smell, industrial warmth",
    bottega_ferretti_1990 = "Italian blacksmith workshop 1990, slight neglect visible, same tools as decades before, middle-aged man working halfheartedly, dusty radio playing, dim light",
    bottega_ferretti_presente = "Closed Italian blacksmith shop 2024, FOR SALE handwritten sign on door, tools visible through dirty window, profound stillness, grey light",
    osteria_1965  = "Traditional Italian osteria 1965, men playing cards loudly, owner behind bar, political arguments, pasta smell, old wine bottles on shelves, warm amber light",
    osteria_1990  = "Italian osteria 1990, TV news in background, middle-aged man drinking alone at corner table, barman watching, fewer customers, melancholic evening light",
    osteria_presente = "Renovated Italian osteria 2024, modern decor but historic wooden beams, aperitivo hour, tourists, original bar preserved, bittersweet atmosphere, warm artificial light",
    elena_1965    = "Young Italian woman 18 years old 1965, dark hair in simple updo, bright intelligent eyes, holding a book, simple cotton dress, half-body portrait, warm natural light",
    elena_1990    = "Italian woman 43 years old 1990, dark hair with grey streaks, glasses, professional tired look, blouse and skirt, half-body portrait, library interior light",
    elena_presente = "Elderly Italian woman 77 years old 2024, white hair elegantly arranged, sharp eyes behind glasses, walking cane nearby, dignified bearing, half-body portrait, soft window light",
    marco_1965    = "Italian teenage boy 15 years old 1965, tall, dark rebellious hair, callused hands smudged with soot, work clothes, dreamy eyes, half-body portrait, workshop background",
    marco_1990    = "Italian man 40 years old 1990, prematurely aged, thinning hair, rough hands, signs of drink, worn work clothes, haunted eyes, half-body portrait, dim light",
    marco_presente = "Elderly Italian man 70 years old 2024, solitary look, trembling hands, unshaven, deep regret in eyes, closed blacksmith shop behind him, half-body portrait, grey light",
    lucia_1990    = "Italian teenage girl 15 years old 1990, curly dark hair tied back, intelligent eyes, heavy school backpack, resilient expression, piazza background, afternoon light",
}

function get_scene_images()
    local loc_id  = state.location
    local periodo = state.periodo
    local bg_key  = loc_id .. "_" .. periodo
    local assets  = {}

    if ASSET_PATHS[bg_key] then
        table.insert(assets, { id=bg_key, path=ASSET_PATHS[bg_key] })
    end

    -- NPC presenti: cerca chiave nomebreve_periodo
    local presenti = presenti_in_periodo(loc_id, periodo)
    for _, npc in ipairs(presenti) do
        -- elena_morandi -> elena, marco_ferretti -> marco, lucia_ferretti -> lucia
        local nome_breve = npc.id:match("^([^_]+)")
        local npc_key    = nome_breve .. "_" .. periodo
        if ASSET_PATHS[npc_key] then
            table.insert(assets, { id=npc_key, path=ASSET_PATHS[npc_key] })
        end
    end

    local scene_key = bg_key
    local hint = nil
    if state._last_image_loc == scene_key then hint = "last" end
    state._last_image_loc = scene_key

    return { assets=assets, base_image=hint }
end

function get_asset_path(id)
    return ASSET_PATHS[id]
end

function get_asset_prompt(id)
    local path = ASSET_PATHS[id]
    if not path then return nil end

    local desc = ASSET_DESC[id]
    if not desc then return { path=path, prompt=IMAGE_STYLE } end

    local sys = "You are a professional image generation prompt engineer. "
             .. "Convert the description into a detailed txt2img prompt including subject, "
             .. "composition, lighting, style, quality tags. Max 80 words. English only. "
             .. "Output the prompt text only, no explanation."
    local ok, prompt = pcall(query_llm, sys, "[]",
        "Description: " .. desc .. "\nStyle: " .. IMAGE_STYLE, "")

    return {
        path   = path,
        prompt = ok and prompt or (desc .. ", " .. IMAGE_STYLE)
    }
end

-- =============================================================================
-- END OF SCRIPT
-- =============================================================================
