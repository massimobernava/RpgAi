# Video Faceswap Pipeline

Tutti i comandi usano `./my_tools/faceswap.sh`. Il server deve girare (`./faceswap_locale/start.sh`).

## Sequenza completa

```bash
OUTDIR=saves/video_work/clip1
```

### 1. Estrai frame
```bash
./my_tools/faceswap.sh video-extract video.mp4 $OUTDIR \
    [--start 00:01:23] [--end 00:01:45] [--fps 30]
```
Crea: `$OUTDIR/frames/`, `$OUTDIR/audio.aac`, `$OUTDIR/video_info.json`.

---

### 2. Registra NPC di origine (se non già registrato)
```bash
./my_tools/faceswap.sh register foto_jenny.jpg --npc jenny
```
Salva embedding in `faceswap_locale/faces/jenny.npy`.

**Multi-angolo (consigliato)**: aggiungi altri embedding (profilo, luci diverse) con `--append`:
```bash
./my_tools/faceswap.sh register foto_jenny_profilo.jpg --npc jenny --append
```
Il matching in `video-analyze`/`video-quality` usa la similarità massima sul set →
molti meno frame `unknown` sui profili. Lo swap usa la media degli embedding (identità più stabile).

Oppure estrai un frame buono dal video e usalo:
```bash
./my_tools/faceswap.sh register $OUTDIR/frames/000042.jpg --npc jenny
```

---

### 3. Analizza — individua chi è nel video
```bash
./my_tools/faceswap.sh video-analyze $OUTDIR \
    --npc jenny \
    [--threshold 0.45] [--det-thresh 0.35] [--max-gap 3]
```
- `--npc` = chi cercare (NPC già registrato). Più NPC: `--npc jenny,marco`.
- `--threshold` = soglia similarità coseno (default 0.45).
- `--det-thresh` = soglia detection InsightFace (default 0.35, più basso = trova profili).
- `--max-gap` = frame consecutivi senza volto da riempire per adiacenza (default 3).
- Guarda `$OUTDIR/npc_review/jenny_preview.mp4` per verificare copertura.
- Crea `$OUTDIR/manifest.json`.

Se rimangono frame unknown/no-face, rilancia solo su quelli:
```bash
./my_tools/faceswap.sh video-analyze $OUTDIR --npc jenny \
    --fix-unknown --threshold 0.35
```

---

### 4. Ispeziona manifest (opzionale)
```bash
./my_tools/faceswap.sh video-inspect $OUTDIR
```
Mostra: frame no-face e unknown raggruppati per sequenze, distribuzione similarità, gap_fill vs occluded.

---

### 5. Assegna NPC di destinazione
```bash
./my_tools/faceswap.sh video-assign $OUTDIR '{"jenny":"fede"}'
```
Mappa: `npc_id` rilevato → NPC da mettere al posto. Salva `source_npc_id` nel manifest.

---

### 6. Esegui swap
```bash
./my_tools/faceswap.sh video-swap $OUTDIR
```
Frame swappati in `$OUTDIR/frames_swapped/`. Progress ogni 25 frame nel terminale del server.

Solo frame falliti (dopo patch):
```bash
./my_tools/faceswap.sh video-swap $OUTDIR --retry-only
```

---

### 7. Verifica qualità
```bash
./my_tools/faceswap.sh video-quality $OUTDIR [--sim-threshold 0.30]
```
- Confronta ogni frame swapped con l'embedding del NPC di destinazione (da manifest).
- Salta frame che erano già problematici (occluded, gap_fill, matched_by=null).
- Flag: `sim` (similarità bassa), `det` (detection scarsa), `blur`, `pose_delta`.
- `side_profile` appare solo come contesto quando la sim è già bassa.
- Aggiorna manifest: frame → `"approved"` o `"flagged"`, scrive `flagged_sequences`.
- Rieseguibile: sovrascrive risultati precedenti correttamente.

---

### 8. (Opzionale) Patcha frame problematici e ri-swappa
```bash
./my_tools/faceswap.sh video-patch $OUTDIR \
    --start 000043 --end 000051 \
    --mask-parts face,skin [--expand 3] [--blur 5]

./my_tools/faceswap.sh video-swap $OUTDIR --retry-only
./my_tools/faceswap.sh video-quality $OUTDIR
```

---

### 9. (Opzionale) Restore frame — salva permanente
```bash
./my_tools/faceswap.sh video-restore $OUTDIR \
    [--restorer codeformer|gfpgan] [--fidelity 0.7] [--upscale 0]
```
- Legge `frames_swapped/`, applica CodeFormer/GFPGAN, scrive `frames_restored/`.
- **Incrementale**: specifica solo i frame da (ri)fare con `--frames 000043,000044`.
- `video-assemble` usa `frames_restored/` automaticamente se esiste.
- `--fidelity` 0.0–1.0: 1.0 = massima fedeltà al volto originale, 0.5 = massimo miglioramento.

---

### 10. Assembla video finale
```bash
./my_tools/faceswap.sh video-assemble $OUTDIR output.mp4 [--fps 30]
```
- Se `frames_restored/` esiste: usa quelli (nessun re-processing).
- Se non esiste: usa `frames_swapped/` direttamente.
- Per applicare restore al volo senza salvare: `--restore codeformer [--fidelity 0.7]`.
- Audio rilevato automaticamente da `$OUTDIR/audio.aac`.

---

## Note

- `manifest.json`: `npc_id` = NPC di destinazione (dopo video-assign), `source_npc_id` = chi era nella scena originale.
- Frame `occluded=true`: analyze setta `occlude:true` nei swap_settings → lo swap usa
  l'occluder (mani/oggetti davanti al viso ripristinati dall'originale). Forzabile su
  un range: `video-patch --start F --end F --occlude true` + `video-swap --retry-only`.
- Frame `matched_by="gap_fill"`: identità ereditata per adiacenza temporale, non da embedding.
- **Progress nel client**: i comandi video girano async (`run_async=true`) e il client
  polla `GET /job/<id>` mostrando `[step] done/total`. `GET /jobs` = lista job recenti.
- `video-assemble` senza `--fps`: usa `source_fps` da `video_info.json` (scritto da video-extract).
- `det_thresh` usato da video-analyze è salvato nel manifest e riusato da swap/quality
  (bbox coerenti). Non inquina più gli endpoint immagine (default 0.5 per-chiamata).
- Embedding multipli per NPC (`register --append`): matching = max sul set, swap = media.
  `unregister <id>` rimuove tutto.
- CoreML (Apple Silicon): opt-in con `FACESWAP_COREML=1 ./faceswap_locale/start.sh` — testare,
  alcuni grafi insightface possono comportarsi male.
- **Swapper alternativi** (`--swapper inswapper|ghost|simswap`, su `swap` e `video-swap`):
  ghost v2 (256px) e simswap (512px) da asset VisoMaster in `models/`. Ogni swapper usa
  il SUO ArcFace → `register` salva anche embedding per-modello (`faces/<id>.ghost.npy`,
  `<id>.simswap.npy`, `--append` li accoda: multi-angolo vale per tutti gli swapper;
  swap = media del set). NPC legacy senza set per-modello: backfill automatico dal crop
  al primo swap. Identità più fedele: inswapper > ghost > simswap; risoluzione/dettaglio:
  simswap > ghost > inswapper. Provali sul tuo materiale.
- **Texture transfer (anti-plastica)**: `faceswap.sh texture <swapped> <original>
  [--amount 0.5] [--radius 2] [--parts skin,nose]` — riporta la grana/pori della pelle
  originale sul volto swappato (separazione di frequenze, stesso frame = pixel-allineati,
  mask pelle dal parser). In GUI: riga "Texture dal target", componibile prima/dopo
  codeformer (di solito meglio come ULTIMO passo: codeformer la liscerebbe).
- **Expression restore (LivePortrait)**: `faceswap.sh expression <swapped> <original>
  [--region exp|eyes|lip] [--strength 0.5]` — riporta espressione/sguardo dell'originale
  sul volto swappato; `--strength` dosa il movimento (1.0 = pieno, spesso esagerato;
  0.3-0.6 = correzione delicata).
  Worker persistente (`liveportrait_proto/expression_server.py`, :8002) avviato
  automaticamente dal server al primo uso (~15s caricamento), poi ~2-3s a immagine.
  In GUI: bottone "Migliora espressione" sotto il risultato swap. Log worker:
  `/tmp/rpgai_expression_worker.log`.
- `video-quality` rieseguibile senza problemi: sovrascrive `swap_quality`/`flagged` su ogni face entry.
