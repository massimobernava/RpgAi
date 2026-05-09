# Pipeline: Racconto → Script RpgAi

Sistema multi-agent con loop critic e skill componibili.
Converte un racconto testuale in uno script Lua completo per RpgAi, completamente automatico.

## Architettura

```
racconto.md
    │
    ▼ Stage 1: Analyzer + [NPC Extraction + Location Extraction + Progression Model]
    │                              ↕ critic loop (max 3 retry)
    ▼ entities.json
    │
    ▼ Stage 2: World Builder + [Lua Tables]
    │                              ↕ critic loop
    ▼ world_data.lua
    │
    ▼ Stage 3: Schema Designer + [JSON Schema]
    │                              ↕ critic loop
    ▼ mechanics.lua
    │
    ▼ Stage 4: Assembler + [Dynamic Prompt + State Validation + Lua Commands]
    │                              ↕ critic loop
    ▼ scripts/nome_script.lua
```

**Skill injection:** ogni agent riceve il suo prompt base + le skill rilevanti come appendice.
**Critic loop:** dopo ogni stage, il critic verifica l'output; se fallisce, il runner riprova con feedback specifico.

## Uso rapido

```bash
# Installa dipendenza
pip install anthropic

# Esegui pipeline
export ANTHROPIC_API_KEY=sk-ant-...
cd /path/to/RpgAi_github
python pipeline/run_pipeline.py racconto.md nome_script

# Output intermedi in: pipeline_stages/
# Script finale in:    scripts/nome_script.lua
```

## Opzioni

```
python pipeline/run_pipeline.py racconto.md nome_script [opzioni]

  --work-dir     DIR    Directory output intermedi (default: pipeline_stages)
  --scripts-dir  DIR    Directory script finale    (default: scripts)
  --force               Riesegui tutti gli stage anche se il file esiste
  --from-stage   N      Parti dallo stage N (1-4), usa cache degli stage precedenti
  --model        ID     Modello Claude (default: claude-sonnet-4-6)
  --retries      N      Max retry per stage (default: 3)
  --api-key      KEY    API key (o usa env ANTHROPIC_API_KEY)
```

### Esempi

```bash
# Prima esecuzione completa
python pipeline/run_pipeline.py racconto.md villa_romana

# Rigenera solo stage 3 e 4 (es: dopo aver editato entities.json)
python pipeline/run_pipeline.py racconto.md villa_romana --from-stage 3

# Forza tutto da zero
python pipeline/run_pipeline.py racconto.md villa_romana --force

# Usa Opus 4.7 per qualità massima
python pipeline/run_pipeline.py racconto.md villa_romana --model claude-opus-4-7
```

## Flusso di lavoro consigliato

1. **Prima esecuzione:** lascia girare tutto automaticamente.
2. **Revisiona** `pipeline_stages/entities.json` — aggiungi dettagli che il racconto non specificava.
3. **Rigenera dal stage 2** se hai modificato entities.json: `--from-stage 2`
4. **Testa** lo script con Ollama prima di usare provider cloud:
   ```bash
   ./build/rpgai --provider ollama --model llama3.2 \
     --path scripts/ --script nome_script.lua
   ```
5. **Itera:** se qualcosa non funziona, edita il file intermedio e riesegui dallo stage problematico.

## Struttura della directory

```
pipeline/
├── run_pipeline.py        — Orchestratore Python
├── agents/
│   ├── analyzer.md        — Stage 1: estrae entità dal racconto
│   ├── world_builder.md   — Stage 2: genera tabelle Lua
│   ├── schema_designer.md — Stage 3: progetta JSON schema e meccaniche
│   ├── assembler.md       — Stage 4: assembla script finale
│   └── critic.md          — Validator: verifica ogni stage
└── skills/
    ├── npc_extraction.md      — Pattern per NPC dettagliati
    ├── location_extraction.md — Pattern per location con acoustic
    ├── progression_model.md   — Selezione modello di progressione
    ├── lua_tables.md          — Sintassi Lua per NPC/locations/travel_map
    ├── json_schema.md         — JSON Schema con additionalProperties
    ├── dynamic_prompt.md      — get_system_prompt() con triggers
    ├── state_validation.md    — process_ai_response() completo
    └── lua_commands.md        — process_player_input() con comandi
```

## Problemi comuni

| Problema | Causa | Soluzione |
|----------|-------|-----------|
| JSON non valido in entities.json | LLM ha aggiunto testo prima/dopo | Il critic lo cattura e riprova |
| NPC si teletrasporta | `avanza_tempo` sovrascrive `sposta_npc` | Controlla skill state_validation: sposta_npc applicato prima del routing |
| Metrica principale scala troppo veloce | CAP_GIORNALIERI troppo alto | Edita mechanics.lua: `CAP_GIORNALIERI.relazione = 10` (usa il nome reale della metrica) |
| LLM ignora la personalità NPC | appearance/personality troppo vaghi | Edita entities.json, arricchisci i campi, riesegui --from-stage 2 |
| Ollama produce JSON malformato | Ollama non supporta structured output | Lo script include `json_repair` — dovrebbe gestirlo automaticamente |
| `get_system_prompt()` crasha | Template usa `{}` invece di `%s` | Edita script: sostituisci `{NOME}` → `%s` nel TEMPLATE |
