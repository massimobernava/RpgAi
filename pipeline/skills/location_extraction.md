# Skill: Location Extraction

## Quando usare
Quando estrai luoghi da un racconto. Applica queste regole per ogni location identificata.

## Regola: desc deve essere sensoriale

Non descrivere la stanza in modo architettonico. Descrivila come la vive chi la abita.

**Troppo neutro:**
```
"desc": "Cucina con tavolo e fornelli"
```

**Corretto:**
```
"desc": "Cucina ampia e luminosa con odore persistente di caffè e spezie. Il tavolo rotondo al centro è il cuore della casa — qui si mangia, si parla, si litiga. Il sole del mattino entra dalla finestra sul cortile e scalda le piastrelle bianche. Sul bancone, sempre qualcosa in preparazione."
```

## Regola: acoustic è la proprietà più strategica

L'`acoustic` determina se una scena può essere privata o rischiosa. È fondamentale per la tensione narrativa.

**Domande da rispondere:**
- Chi può sentire cosa accade qui?
- I rumori entrano dall'esterno? Da quale altra stanza?
- Una porta chiusa garantisce privacy?

**Esempi corretti:**
```json
"acoustic": "Le voci si sentono chiaramente dal soggiorno adiacente — la porta sottile non isola affatto. I passi sul parquet risuonano in tutta la casa. Chi è nelle camere al piano di sopra sente tutto quello che succede in cucina."
```

```json
"acoustic": "Camera isolata acusticamente. La porta massiccia blocca quasi tutti i suoni. Chi è fuori nel corridoio percepisce solo rumori attutiti. Unica eccezione: il pavimento scricchiolante vicino alla finestra."
```

```json
"acoustic": "Bagno con ventola sempre accesa che copre tutti i rumori interni. Dal corridoio non si sente niente. Chi è dentro non sente i passi fuori fino a quando qualcuno busca."
```

## Regola: connessa_a definisce la mobilità

Lista gli ID delle location direttamente accessibili (senza passare per altre stanze).

```json
"connessa_a": ["corridoio", "sala_pranzo", "balcone"]
```

## Inferire acoustic dal tipo di stanza

Se il racconto non descrive l'acustica, usa questi pattern:

| Tipo di stanza | Acoustic tipica |
|----------------|----------------|
| Cucina aperta | Alta esposizione — adiacente al soggiorno, voci chiaramente udibili |
| Bagno | Alta privacy — ventola, porta pesante |
| Camera da letto | Media privacy — dipende dal piano e dai muri |
| Salotto | Bassa privacy — è il centro dell'appartamento |
| Veranda/terrazzo | Nessuna privacy — voci portate dal vento |
| Garage/cantina | Alta privacy — isolamento naturale |
| Corridoio | Trasparente — connette tutto, non isola niente |

## Proprietà opzionale: atmosfera

Aggiunge parole chiave per guidare il tono dell'LLM in questa stanza:
```json
"atmosfera": "intima, esposta, pericolosa, rifugio, nostalgica"
```
