# Skill: JSON Schema Design

## Quando usare
Quando progetti il contratto JSON tra script Lua e LLM.

## Schema template completo

I nomi dei campi (`cambia_relazione`, `cambia_sospetto`, ecc.) e i valori enum di `nuova_location` dipendono dal racconto specifico — sostituisci con i valori reali estratti da entities.json.

```json
{
    "type": "object",
    "required": ["narration"],
    "additionalProperties": false,
    "properties": {
        "narration": {
            "type": "string",
            "description": "2-4 frasi in italiano, terza persona, tempo presente. Descrive la scena, le azioni dei personaggi, le sensazioni."
        },
        "state_changes": {
            "type": "object",
            "additionalProperties": false,
            "properties": {
                "nuova_location": {
                    "type": ["string", "null"],
                    "enum": ["location_1", "location_2", "location_3", "location_4", null],
                    "description": "ID location se il protagonista si sposta, null se rimane"
                },
                "avanza_tempo": {
                    "type": "boolean",
                    "description": "true se passa la fascia oraria (es: da pomeriggio a sera)"
                },
                "avanza_giorno": {
                    "type": "boolean",
                    "description": "true solo se si va a dormire o passa una notte intera"
                },
                "cambia_relazione": {
                    "type": "object",
                    "additionalProperties": false,
                    "properties": {
                        "npc_1": { "type": "integer", "minimum": -5, "maximum": 15 },
                        "npc_2": { "type": "integer", "minimum": -5, "maximum": 15 }
                    },
                    "description": "Delta metrica principale per NPC. Solo valori non-zero. Rinomina in base alla metrica scelta (cambia_fiducia, cambia_influenza, ecc.)"
                },
                "cambia_sospetto": {
                    "type": "object",
                    "additionalProperties": false,
                    "properties": {
                        "npc_1": { "type": "integer", "minimum": -10, "maximum": 10 },
                        "npc_2": { "type": "integer", "minimum": -10, "maximum": 10 }
                    }
                },
                "cambia_irritazione": {
                    "type": "object",
                    "additionalProperties": false,
                    "properties": {
                        "npc_1": { "type": "integer", "minimum": -10, "maximum": 15 }
                    }
                },
                "sposta_npc": {
                    "type": "object",
                    "additionalProperties": {
                        "type": "string"
                    },
                    "description": "{ npc_id: location_id } — sposta NPC in una nuova area"
                },
                "set_flags": {
                    "type": "object",
                    "additionalProperties": {
                        "type": "boolean"
                    },
                    "description": "{ flag_id: true/false } — attiva o disattiva flag narrativi"
                },
                "umore_npc": {
                    "type": "object",
                    "additionalProperties": {
                        "type": "string"
                    },
                    "description": "{ npc_id: 'umore' } — aggiorna l'umore di un NPC"
                },
                "energia_delta": {
                    "type": "integer",
                    "minimum": -5,
                    "maximum": 3,
                    "description": "Cambio energia protagonista (negativo = consuma, positivo = recupera)"
                },
                "cambia_inventario": {
                    "type": "object",
                    "additionalProperties": {
                        "type": "string",
                        "enum": ["add", "remove"]
                    },
                    "description": "{ item_id: 'add'|'remove' }"
                }
            }
        },
        "game_over": {
            "type": "boolean",
            "description": "true solo se la storia è terminata (buon fine o bad ending)"
        },
        "game_over_reason": {
            "type": "string",
            "description": "Spiegazione del game over (solo se game_over è true)"
        }
    }
}
```

## Regole di progettazione

### 1. Solo narration è required

L'LLM può sempre produrre una narrazione. Gli state_changes sono opzionali — l'LLM li omette quando non ci sono cambiamenti.

### 2. additionalProperties: false su TUTTI gli oggetti

Previene che l'LLM inventi campi non gestiti dallo script.

### 3. nuova_location con enum

L'enum deve contenere TUTTI gli ID location più `null`.

### 4. Limiti integer ragionevoli

- Metrica principale (relazione/fiducia/influenza): min -5, max 15 per turno (il cap giornaliero è gestito dal codice)
- Sospetto: min -10, max 10
- Energia: min -5, max 3

### 5. Non aggiungere campi che non vengono usati

Ogni campo nello schema deve essere gestito in `process_ai_response()`. Se non lo gestisci, non metterlo.

### 6. Rinomina i campi metrica in base al racconto

`cambia_relazione` è solo un esempio. Se la metrica principale è `fiducia`, il campo si chiama `cambia_fiducia`. Se è `influenza`, `cambia_influenza`. Il nome del campo nello schema deve corrispondere esattamente al nome usato in `process_ai_response()`.

## Pattern per Ollama (schema testuale)

```lua
function get_json_schema_prompt()
    return [[
FORMATO RISPOSTA OBBLIGATORIO — rispondi SOLO con questo JSON:
{
  "narration": "2-4 frasi narrative in italiano, terza persona presente",
  "state_changes": {
    "nuova_location": "id_location oppure null se non ti sposti",
    "avanza_tempo": false,
    "avanza_giorno": false,
    "cambia_relazione": { "npc_id": delta_intero },
    "sposta_npc": { "npc_id": "location_id" },
    "set_flags": { "flag_id": true },
    "energia_delta": 0
  },
  "game_over": false
}

REGOLE:
- Zero testo fuori dal JSON
- Nessun commento dentro il JSON
- "narration" deve avere almeno 2 frasi
- Ometti campi di state_changes che non cambiano (non mettere null, ometti il campo)
]]
end
```

## Errore comune: schema nested troppo profondo

Non nidificare `cambia_relazione.properties.npc_1.properties` — è sufficiente definire il tipo:

```json
"cambia_relazione": {
    "type": "object",
    "additionalProperties": false,
    "properties": {
        "npc_1": { "type": "integer", "minimum": -5, "maximum": 15 }
    }
}
```

Non:
```json
"cambia_relazione": {
    "properties": {
        "npc_1": {
            "properties": {
                "value": { ... }
            }
        }
    }
}
```
