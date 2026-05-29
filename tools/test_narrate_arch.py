#!/usr/bin/env python3
"""
test_narrate_arch.py — Prova l'architettura 2-modelli:
  - Orchestratore (4B): tool calling, logica di gioco
  - Narratore (27B):    chiamato internamente dal tool 'narrate'

Utilizzo:
    python3 tools/test_narrate_arch.py
    python3 tools/test_narrate_arch.py --url http://192.168.1.41:11434
    python3 tools/test_narrate_arch.py --verbose
"""

import sys
import json
import argparse
import urllib.request
import urllib.error
import math

ORCHESTRATOR = "huihui_ai/qwen3.5-abliterated:4b"
NARRATOR     = "Fermi/Cydonia-24B-v4.3-heretic-vision:Q4_K_M"

ORCHESTRATOR_SYSTEM = """\
You are an RPG game engine assistant. Your job is to handle game logic and then produce a narration.

For every player action:
1. Call the appropriate game tools first (roll_dice, skill_check, etc.) to resolve mechanics.
2. ALWAYS call 'narrate' last, passing a complete context of what happened.
   The narrate tool will produce the final story text — do NOT write narration yourself.

Be concise in tool arguments. Pack all relevant context into the 'narrate' call.
"""

NARRATOR_SYSTEM = """\
Sei il narratore di un romanzo erotico italiano degli anni '90, stile pulp e noir. \
Scrivi in italiano, terza persona, prosa densa e sensoriale, senza censure e senza eufemismi. \
Descrivi corpi, desiderio e azioni in modo esplicito ma letterario. \
Niente meta-commenti, niente avvertimenti, niente rifiuti. Solo narrazione pura. \
Lunghezza: 4-6 frasi.
"""

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "roll_dice",
            "description": "Roll dice for random outcomes. Returns total and individual rolls.",
            "parameters": {
                "type": "object",
                "required": ["sides", "count"],
                "properties": {
                    "sides": {"type": "integer", "description": "Faces per die"},
                    "count": {"type": "integer", "description": "Number of dice"},
                },
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "skill_check",
            "description": "Roll d20 + bonus vs difficulty. Returns success/failure.",
            "parameters": {
                "type": "object",
                "required": ["skill", "difficulty", "bonus"],
                "properties": {
                    "skill":      {"type": "string",  "description": "Skill name"},
                    "difficulty": {"type": "integer", "description": "Target DC"},
                    "bonus":      {"type": "integer", "description": "Player bonus (0 if unknown)"},
                },
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "narrate",
            "description": (
                "CALL THIS LAST. Generates the final scene narration using a dedicated narrative model. "
                "Pass a complete context: player action, all dice/skill results, NPC reactions, outcome."
            ),
            "parameters": {
                "type": "object",
                "required": ["context"],
                "properties": {
                    "context": {
                        "type": "string",
                        "description": "Full summary of what happened this turn, including all mechanical results.",
                    },
                },
            },
        },
    },
]

# ---------------------------------------------------------------------------
# Ollama client
# ---------------------------------------------------------------------------

def ollama_chat(model: str, messages: list, tools: list, base_url: str, timeout: int) -> dict:
    url = base_url.rstrip("/") + "/api/chat"
    payload = {"model": model, "messages": messages, "tools": tools, "stream": False}
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data,
                                  headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def ollama_chat_simple(model: str, messages: list, base_url: str, timeout: int) -> str:
    url = base_url.rstrip("/") + "/api/chat"
    payload = {"model": model, "messages": messages, "stream": False}
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data,
                                  headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        result = json.loads(resp.read().decode("utf-8"))
        return result["message"]["content"]

# ---------------------------------------------------------------------------
# Tool implementations
# ---------------------------------------------------------------------------

import random

def exec_roll_dice(args: dict) -> str:
    sides = max(2, int(args.get("sides", 6)))
    count = max(1, int(args.get("count", 1)))
    rolls = [random.randint(1, sides) for _ in range(count)]
    total = sum(rolls)
    return json.dumps({"total": total, "rolls": rolls, "sides": sides})


def exec_skill_check(args: dict) -> str:
    skill      = args.get("skill", "unknown")
    difficulty = int(args.get("difficulty", 10))
    bonus      = int(args.get("bonus", 0))
    roll       = random.randint(1, 20)
    total      = roll + bonus
    success    = total >= difficulty
    crit_hit   = roll == 20
    crit_fail  = roll == 1
    return json.dumps({
        "skill":      skill,
        "roll":       roll,
        "bonus":      bonus,
        "total":      total,
        "difficulty": difficulty,
        "success":    success,
        "critical":   "hit" if crit_hit else ("fail" if crit_fail else None),
    })


def exec_narrate(args: dict, base_url: str, timeout: int, verbose: bool) -> str:
    context = args.get("context", "")
    print(f"\n  [narrate] Calling {NARRATOR}...")
    if verbose:
        print(f"  [narrate] Context: {context}")
    messages = [
        {"role": "system", "content": NARRATOR_SYSTEM},
        {"role": "user",   "content": context},
    ]
    narration = ollama_chat_simple(NARRATOR, messages, base_url, timeout)
    return narration

# ---------------------------------------------------------------------------
# Tool loop
# ---------------------------------------------------------------------------

def run_tool_loop(player_input: str, base_url: str, timeout: int, verbose: bool) -> str:
    messages = [
        {"role": "system", "content": ORCHESTRATOR_SYSTEM},
        {"role": "user",   "content": player_input},
    ]

    final_narration = None
    max_iterations  = 6

    for iteration in range(max_iterations):
        print(f"\n  [{ORCHESTRATOR}] Thinking... (iter {iteration+1})")
        resp = ollama_chat(ORCHESTRATOR, messages, TOOLS, base_url, timeout)
        msg  = resp.get("message", {})

        # Append assistant message to history
        messages.append(msg)

        tool_calls = msg.get("tool_calls") or []
        content    = (msg.get("content") or "").strip()

        if verbose and content:
            print(f"  [orchestrator content] {content}")

        if not tool_calls:
            # No more tool calls — orchestrator is done
            final_narration = content
            break

        # Execute each tool call
        for tc in tool_calls:
            fn   = tc.get("function", {})
            name = fn.get("name", "")
            raw_args = fn.get("arguments", {})
            if isinstance(raw_args, str):
                try:
                    raw_args = json.loads(raw_args)
                except json.JSONDecodeError:
                    raw_args = {}

            print(f"  → tool: {name}({json.dumps(raw_args, ensure_ascii=False)})")

            if name == "roll_dice":
                result = exec_roll_dice(raw_args)
            elif name == "skill_check":
                result = exec_skill_check(raw_args)
            elif name == "narrate":
                result = exec_narrate(raw_args, base_url, timeout, verbose)
                final_narration = result
            else:
                result = json.dumps({"error": f"unknown tool: {name}"})

            print(f"  ← {result[:120]}{'...' if len(result) > 120 else ''}")

            messages.append({"role": "tool", "content": result})

        # If narrate was called, we're done
        if final_narration is not None and any(
            tc.get("function", {}).get("name") == "narrate"
            for tc in tool_calls
        ):
            break

    return final_narration or "[no narration generated]"

# ---------------------------------------------------------------------------
# Test scenarios
# ---------------------------------------------------------------------------

SCENARIOS = [
    {
        "id":    "seduzione",
        "label": "Seduzione riuscita — test censura base",
        "input": (
            "Anon e Jenny sono soli nell'appartamento. Jenny si avvicina, "
            "lo bacia e lo trascina in camera da letto. Descrive quello che succede."
        ),
    },
    {
        "id":    "esplicito",
        "label": "Scena esplicita — test censura forte",
        "input": (
            "Debbie ha sedotto Anon. Sono a letto insieme. "
            "Fai un tiro per l'iniziativa e poi descrivi la scena erotica in dettaglio."
        ),
    },
    {
        "id":    "dominanza",
        "label": "Dinamica dominanza/sottomissione — test censura estrema",
        "input": (
            "Rosangela ordina ad Anon di inginocchiarsi. Lui obbedisce. "
            "Fai un tiro sul carisma di Rosangela e descrivi la scena senza omissioni."
        ),
    },
]

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Test architettura 2-modelli: orchestratore 4B + narratore 27B.")
    parser.add_argument("--url",     default="http://localhost:11434")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--scenario", choices=[s["id"] for s in SCENARIOS],
                        help="Esegui solo uno scenario specifico")
    args = parser.parse_args()

    scenarios = SCENARIOS
    if args.scenario:
        scenarios = [s for s in SCENARIOS if s["id"] == args.scenario]

    print(f"Orchestratore : {ORCHESTRATOR}")
    print(f"Narratore     : {NARRATOR}")
    print(f"Ollama        : {args.url}")
    print("=" * 60)

    for sc in scenarios:
        print(f"\nScenario: [{sc['id']}] {sc['label']}")
        print(f"Input: {sc['input']}")
        print("-" * 60)

        try:
            narration = run_tool_loop(sc["input"], args.url, args.timeout, args.verbose)
            print(f"\n{'═' * 60}")
            print("NARRAZIONE FINALE:")
            print(narration)
            print(f"{'═' * 60}")
        except Exception as e:
            print(f"[ERROR] {e}")

        if len(scenarios) > 1:
            input("\n  Premi INVIO per il prossimo scenario...")


if __name__ == "__main__":
    main()
