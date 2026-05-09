# Skill: Progression Model Selection

## Quando usare
Quando devi scegliere il modello di progressione narrativa per un racconto.

## Albero decisionale

```
La storia ha un climax graduale e continuo (relazioni che evolvono, influenza che cresce)?
    └─ SÌ → Il progresso è limitato per "sessione" (ogni giorno/incontro)?
        ├─ SÌ → ciclo_sessione
        └─ NO → progressione_graduale
    └─ NO → La storia procede per eventi discreti (scoperte, scene chiave, decisioni)?
        ├─ SÌ → flag_binari
        └─ Combina elementi di entrambi → misto
```

## Descrizione dei modelli

### progressione_graduale

**Quando:** Storia con tensione che sale continuamente nel tempo senza limite di sessione.
**Meccanica:** Metrica 0-100 per NPC. Ogni interazione positiva aumenta il valore. Soglie sbloccano comportamenti.
**Cap:** Opzionale ma consigliato (evita escalation in 2 turni).
**Generi:** Romanzo di formazione, storia di alleanze politiche, relazione mentor-allievo, revenge story — qualsiasi storia dove un rapporto si trasforma gradualmente nel tempo.

```json
"modello": "progressione_graduale",
"metriche_per_npc": {
    "capitano": { "metriche": ["fiducia", "sospetto"], "soglie": [25, 50, 75, 90], "cap_giornaliero": 15 }
}
```

### ciclo_sessione

**Quando:** Storia ciclica con sessioni ricorrenti. Il progresso si accumula ma con limite per ciclo.
**Meccanica:** Come progressione_graduale ma con CAP_GIORNALIERI rigidi per impedire skip narrativi.
**Generi:** Addestramento (maestro-allievo), negoziati diplomatici giornalieri, interrogatori, cicli scolastici — ogni sessione è autonoma ma il progresso si accumula.

```json
"modello": "ciclo_sessione",
"metriche_per_npc": {
    "mentore": { "metriche": ["fiducia", "rispetto"], "soglie": [20, 40, 60, 80, 100], "cap_giornaliero": 10 }
}
```

### flag_binari

**Quando:** Storia con scene chiave discrete — ogni scena è un evento narrativo unico.
**Meccanica:** Flag booleani. Trigger quando flag X è vero E metrica Y > soglia.
**Generi:** Mystery, thriller, horror — ogni scoperta apre nuove possibilità. Avventura con capitoli definiti.

```json
"modello": "flag_binari",
"eventi_chiave": [
    { "id": "trova_documento", "trigger": "location=='archivio' AND flags.chiave_trovata", "descrizione": "Trova il documento segreto" },
    { "id": "prima_alleanza", "trigger": "flags.trova_documento AND fiducia_capitano >= 30", "descrizione": "Il capitano propone un'alleanza" }
]
```

### misto

**Quando:** Storia complessa con sia evoluzione graduale che eventi discreti.
**Meccanica:** Combina progressione graduale (per i valori NPC) e flag binari (per i momenti chiave).
**Generi:** Qualsiasi storia con sotto-trama — relazione che scala E ci sono scene chiave sbloccabili.

```json
"modello": "misto",
"note": "Usa progressione_graduale per capitano/mercante. Usa flag_binari per eventi di trama (segreto_scoperto, alleanza_formata, tradimento_rivelato)."
```

## Soglie standard

Se il racconto non specifica le soglie, usa:

| Modello | Soglie consigliate | Cap giornaliero |
|---------|-------------------|----------------|
| progressione_graduale | 25, 50, 75, 90 | 15 |
| ciclo_sessione | 20, 40, 60, 80, 100 | 10 |
| flag_binari | N/A | N/A |

## Metriche disponibili

Scegli le metriche più adatte al genere della storia. Non aggiungere metriche non presenti nel racconto — ogni metrica richiede campo nello schema e validazione nel codice.

**Metriche relazionali:**
- **fiducia**: Grado di fiducia dell'NPC verso il protagonista. Alta fiducia = cooperazione, rivelazione di segreti.
- **relazione**: Qualità generica della relazione (positivo = miglioramento, negativo = deterioramento).
- **rispetto**: Riconoscimento dell'NPC del valore del protagonista (utile in contesti di potere/gerarchia).

**Metriche di tensione:**
- **sospetto**: Attenzione/diffidenza dell'NPC verso il protagonista. Alto sospetto = rischio eventi negativi.
- **irritazione**: Tensione negativa. Alta irritazione = NPC risponde male, riduce progressione positiva.
- **paura**: L'NPC teme il protagonista o una situazione. Può sbloccare azioni disperate o obbedienza forzata.

**Metriche di stato:**
- **influenza**: Grado di controllo/persuasione del protagonista sull'NPC (utile per storie politiche).
- **indizi**: Accumulo di prove/conoscenza (utile per mystery — ogni scoperta incrementa il totale).
