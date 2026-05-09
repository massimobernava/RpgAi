# Skill: NPC Extraction

## Quando usare
Quando estrai personaggi da un racconto. Applica queste regole per ogni NPC identificato.

## Regola: appearance deve essere filmabile

L'`appearance` deve permettere a un regista di descrivere la scena senza leggere il racconto. Minimo 3 elementi: corporatura/fisico, viso/espressione, modo di muoversi/vestire.

**Troppo vago:**
```
"appearance": "uomo di mezza età, imponente"
```

**Corretto:**
```
"appearance": "corporatura massiccia e squadrata, spalle larghe da chi ha passato anni in mare. Capelli grigi tagliati corti, cicatrice sottile sul mento. Occhi azzurro-grigio che valutano tutto con calma fredda. Si muove lentamente ma con autorità deliberata, come chi non ha bisogno di affrettarsi. Porta sempre una giacca logora che non toglierebbe mai."
```

## Regola: personality deve avere contraddizioni

Un personaggio interessante ha desideri che confliggono con il suo comportamento esteriore.

**Troppo piatto:**
```
"personality": "comandante severo, rispettato dai suoi uomini"
```

**Corretto:**
```
"personality": "In superficie: brusco, diretto, intollerante alle scuse. Sotto: profondamente leale a chi guadagna la sua fiducia, tormentato da una decisione passata che non ha mai dimenticato. Usa la durezza come scudo contro l'empatia che teme di mostrare. Diventa più umano quando parla di chi ha perso. Tic: tamburella le dita sul tavolo quando qualcosa lo preoccupa e non vuole ammetterlo."
```

## Regola: routine deve essere coerente con il personaggio

Se il racconto non specifica le routine, inferiscile dalla logica del personaggio:
- Un comandante/capo → mattino sul campo/ufficio, pomeriggio riunioni, sera sala comune
- Un mercante → mattino magazzino, pomeriggio mercato, sera taverna
- Un ricercatore/studioso → mattino biblioteca, pomeriggio studio, sera studio o sala comune

**Esempio:**
```json
"routine": {
    "mattino":    "ponte_di_comando",
    "pomeriggio": "sala_riunioni",
    "sera":       "sala_comune",
    "notte":      "cabina_capitano"
}
```

## Regola: triggers_comportamentali descrivono il cambiamento visibile

Non descrivere stati interni generici ma comportamenti osservabili che l'LLM può scrivere.
I trigger si attivano quando la metrica principale dell'NPC raggiunge quella soglia.

**Troppo interno:**
```json
"triggers": { "25": "inizia a fidarsi un po' di più" }
```

**Corretto (storia di alleanza/fiducia):**
```json
"triggers": {
    "25": "Smette di ignorare le domande del protagonista. Risponde, anche se in modo conciso. Lo guarda negli occhi invece di voltarsi dall'altra parte.",
    "50": "Condivide informazioni che prima teneva per sé. Inizia le conversazioni anziché aspettare. Difende il protagonista davanti ad altri.",
    "75": "Lo tratta come un pari. Rivela dettagli sul suo passato. In situazioni di crisi, si fida del giudizio del protagonista senza discutere."
}
```

**Corretto (storia di mystery/sospetto):**
```json
"triggers": {
    "25": "Nota inconsistenze nelle storie del protagonista. Fa domande più precise, osserva senza commentare.",
    "50": "Testa apertamente il protagonista con informazioni false per vedere come reagisce. Aumenta la sorveglianza.",
    "75": "Ha quasi certezza. Decide se agire o aspettare ancora. Il linguaggio del corpo è teso, le risposte diventano monosillabiche."
}
```

## Inferire campi mancanti

Se il racconto è vago su un campo, usa questa gerarchia:
1. Cerca indizi diretti nel testo
2. Inferisci dalla personalità e dal ruolo del personaggio
3. Usa valori plausibili per il genere narrativo
4. Segnala in `note_creative` che il campo è stato inferito

Non lasciare mai un campo vuoto o con un placeholder come "da definire".
