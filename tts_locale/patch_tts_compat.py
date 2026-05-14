"""
Patch Coqui TTS library for compatibility with modern PyTorch and transformers.

Run once after installing TTS:
  python patch_tts_compat.py

Fixes applied:
  1. stream_generator.py — BeamSearchScorer removed from transformers public API in 4.37+.
     Import it from transformers.generation instead.

  2. io.py — PyTorch 2.6+ changed torch.load default to weights_only=True.
     Coqui TTS checkpoints contain XttsConfig objects (pickle), which are rejected.
     Set weights_only=False explicitly (safe: official Coqui model files only).
"""

import pathlib
import site
import sys


def find_tts_root() -> pathlib.Path:
    for sp in site.getsitepackages() + [site.getusersitepackages()]:
        p = pathlib.Path(sp) / "TTS"
        if p.exists():
            return p
    raise RuntimeError("TTS package not found in site-packages. Is it installed?")


def patch_torch_load(path: pathlib.Path) -> bool:
    """Patch torch.load call in io.py preserving original indentation."""
    label = "io.py — torch.load weights_only=False"
    target = "return torch.load(f, map_location=map_location, **kwargs)"
    marker = "kwargs.setdefault('weights_only', False)"
    txt = path.read_text(encoding="utf-8")
    if marker in txt:
        print(f"  [skip] {label} — already patched")
        return True
    if target not in txt:
        print(f"  [WARN] {label} — pattern not found, skipping")
        return False
    # Detect indentation from the matching line so we don't break block structure
    indent = ""
    for line in txt.splitlines():
        if target in line:
            indent = line[: len(line) - len(line.lstrip())]
            break
    # str.replace keeps leading whitespace intact; only 'return ...' is replaced
    patched = txt.replace(
        target,
        f"{marker}\n{indent}return torch.load(f, map_location=map_location, **kwargs)",
        1,
    )
    path.write_text(patched, encoding="utf-8")
    print(f"  [OK]   {label}")
    return True


def patch_file(path: pathlib.Path, old: str, new: str, label: str) -> bool:
    txt = path.read_text(encoding="utf-8")
    if new in txt:
        print(f"  [skip] {label} — already patched")
        return True
    if old not in txt:
        print(f"  [WARN] {label} — pattern not found, skipping")
        print(f"         Expected: {old!r}")
        return False
    path.write_text(txt.replace(old, new, 1), encoding="utf-8")
    print(f"  [OK]   {label}")
    return True


def main():
    tts_root = find_tts_root()
    print(f"TTS found at: {tts_root}\n")

    ok = True

    # Patch 1: BeamSearchScorer import
    p1 = tts_root / "tts/layers/xtts/stream_generator.py"
    if p1.exists():
        ok &= patch_file(
            p1,
            old="from transformers import (\n    BeamSearchScorer,",
            new="from transformers.generation import BeamSearchScorer\nfrom transformers import (",
            label="stream_generator.py — BeamSearchScorer import",
        )
    else:
        print(f"  [skip] stream_generator.py not found (XTTS not installed?)")

    # Patch 2: torch.load weights_only — indentation-aware to handle any block depth
    p2 = tts_root / "utils/io.py"
    if p2.exists():
        ok &= patch_torch_load(p2)
    else:
        print(f"  [WARN] io.py not found")
        ok = False

    print()
    if ok:
        print("All patches applied. You can now start the TTS server.")
    else:
        print("Some patches failed — check warnings above.")
        sys.exit(1)


if __name__ == "__main__":
    main()
