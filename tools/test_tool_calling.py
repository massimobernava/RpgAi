#!/usr/bin/env python3
"""
test_tool_calling.py — Valuta se un modello Ollama chiama i tool correttamente.

Esegue una batteria di test case: prompt che devono (o non devono) generare
tool_calls, con validazione di nome tool, argomenti obbligatori e tipi.

Utilizzo:
    python3 tools/test_tool_calling.py --model llama3.2
    python3 tools/test_tool_calling.py --model llama3.2 mistral qwen2.5
    python3 tools/test_tool_calling.py --model llama3.2 --url http://192.168.1.10:11434
    python3 tools/test_tool_calling.py --model llama3.2 --verbose
    python3 tools/test_tool_calling.py --model llama3.2 --cases dice skill
    python3 tools/test_tool_calling.py --model llama3.2 --json

Opzioni:
    --model <nome> [nome...]  Modelli da testare (obbligatorio, può essere multiplo)
    --url   <url>             URL Ollama (default: http://localhost:11434)
    --cases <id> [id...]      Filtra test per ID (default: tutti)
    --verbose                 Stampa risposta completa Ollama per ogni test
    --json                    Output finale in JSON (utile per pipeline)
    --timeout <sec>           Timeout per chiamata Ollama (default: 60)
"""

import sys
import json
import argparse
import urllib.request
import urllib.error
from typing import Optional

# ---------------------------------------------------------------------------
# Tool definitions — Ollama format
# ---------------------------------------------------------------------------

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "roll_dice",
            "description": "Roll one or more dice. Use for combat, skill checks, random events.",
            "parameters": {
                "type": "object",
                "required": ["sides", "count"],
                "properties": {
                    "sides": {"type": "integer", "description": "Number of faces per die (e.g. 6, 10, 20)"},
                    "count": {"type": "integer", "description": "Number of dice to roll"},
                },
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "skill_check",
            "description": "Perform a skill check. Rolls d20 + skill bonus vs difficulty.",
            "parameters": {
                "type": "object",
                "required": ["skill", "difficulty"],
                "properties": {
                    "skill":      {"type": "string",  "description": "Skill name (e.g. Perception, Stealth)"},
                    "difficulty": {"type": "integer", "description": "Target number to meet or exceed"},
                },
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "get_weather",
            "description": "Get current weather for a city.",
            "parameters": {
                "type": "object",
                "required": ["city"],
                "properties": {
                    "city":    {"type": "string", "description": "City name"},
                    "country": {"type": "string", "description": "Country code (optional, e.g. IT, US)"},
                },
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "memory_write",
            "description": "Store a persistent fact about a character or entity.",
            "parameters": {
                "type": "object",
                "required": ["entity", "category", "content"],
                "properties": {
                    "entity":   {"type": "string", "description": "Entity name (e.g. Marco, Jenny)"},
                    "category": {"type": "string", "description": "Fact category (e.g. role, mood, location)"},
                    "content":  {"type": "string", "description": "The fact to store"},
                },
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "calculate",
            "description": "Evaluate a mathematical expression and return the result.",
            "parameters": {
                "type": "object",
                "required": ["expression"],
                "properties": {
                    "expression": {"type": "string", "description": "Math expression to evaluate (e.g. '47 * 89', '(12 + 5) / 3')"},
                },
            },
        },
    },
]

# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------
# expect_tool: str = must call this tool | None = must NOT call any tool
# validate:    callable(args dict) -> (bool, str) — True=pass, reason on fail

def _req(args: dict, *keys) -> tuple[bool, str]:
    for k in keys:
        if k not in args or args[k] is None:
            return False, f"missing required arg '{k}'"
    return True, "ok"

def _type_int(args: dict, key: str) -> tuple[bool, str]:
    v = args.get(key)
    if not isinstance(v, (int, float)):
        return False, f"'{key}' must be integer, got {type(v).__name__}={v!r}"
    return True, "ok"

def _type_str(args: dict, key: str) -> tuple[bool, str]:
    v = args.get(key)
    if not isinstance(v, str) or not v.strip():
        return False, f"'{key}' must be non-empty string, got {v!r}"
    return True, "ok"

def _all(*checks) -> tuple[bool, str]:
    for ok, reason in checks:
        if not ok:
            return False, reason
    return True, "ok"

TEST_CASES = [
    # ---- roll_dice ----
    {
        "id": "dice_basic",
        "description": "Tira 3d6 — deve chiamare roll_dice(sides=6, count=3)",
        "prompt": "Tira 3 dadi a 6 facce.",
        "expect_tool": "roll_dice",
        "validate": lambda a: _all(
            _req(a, "sides", "count"),
            _type_int(a, "sides"),
            _type_int(a, "count"),
            (int(a.get("sides", 0)) == 6, f"sides deve essere 6, got {a.get('sides')}"),
            (int(a.get("count", 0)) == 3, f"count deve essere 3, got {a.get('count')}"),
        ),
    },
    {
        "id": "dice_d20",
        "description": "Tira un d20 — deve chiamare roll_dice(sides=20, count=1)",
        "prompt": "Roll a d20 for me.",
        "expect_tool": "roll_dice",
        "validate": lambda a: _all(
            _req(a, "sides", "count"),
            _type_int(a, "sides"),
            (int(a.get("sides", 0)) == 20, f"sides deve essere 20, got {a.get('sides')}"),
        ),
    },
    # ---- skill_check ----
    {
        "id": "skill_perception",
        "description": "Skill check Percezione DC 15 — deve chiamare skill_check",
        "prompt": "Il giocatore cerca di percepire il nemico nascosto. Fai un controllo abilità su Perception, difficoltà 15.",
        "expect_tool": "skill_check",
        "validate": lambda a: _all(
            _req(a, "skill", "difficulty"),
            _type_str(a, "skill"),
            _type_int(a, "difficulty"),
            (int(a.get("difficulty", 0)) == 15, f"difficulty deve essere 15, got {a.get('difficulty')}"),
        ),
    },
    {
        "id": "skill_stealth",
        "description": "Skill check Stealth implicito — deve chiamare skill_check con skill e difficulty",
        "prompt": "The rogue tries to sneak past the guards. Make a Stealth check against DC 12.",
        "expect_tool": "skill_check",
        "validate": lambda a: _all(
            _req(a, "skill", "difficulty"),
            _type_int(a, "difficulty"),
        ),
    },
    # ---- get_weather ----
    {
        "id": "weather_city",
        "description": "Meteo Roma — deve chiamare get_weather(city=...)",
        "prompt": "What's the weather like in Rome today?",
        "expect_tool": "get_weather",
        "validate": lambda a: _all(
            _req(a, "city"),
            _type_str(a, "city"),
        ),
    },
    {
        "id": "weather_it",
        "description": "Meteo Milano — deve chiamare get_weather in italiano",
        "prompt": "Dimmi il meteo di Milano.",
        "expect_tool": "get_weather",
        "validate": lambda a: _all(
            _req(a, "city"),
            _type_str(a, "city"),
        ),
    },
    # ---- memory_write ----
    {
        "id": "memory_fact",
        "description": "Salva fatto su NPC — deve chiamare memory_write con entity/category/content",
        "prompt": "Remember that Marco is the thief who stole the ring.",
        "expect_tool": "memory_write",
        "validate": lambda a: _all(
            _req(a, "entity", "category", "content"),
            _type_str(a, "entity"),
            _type_str(a, "content"),
        ),
    },
    {
        "id": "memory_mood",
        "description": "Salva stato emotivo NPC — deve chiamare memory_write",
        "prompt": "Ricorda che Jenny è arrabbiata con Anon da ieri sera.",
        "expect_tool": "memory_write",
        "validate": lambda a: _all(
            _req(a, "entity", "category", "content"),
            _type_str(a, "entity"),
        ),
    },
    # ---- calculate ----
    {
        "id": "calc_mul",
        "description": "Calcolo matematico — deve chiamare calculate con expression",
        "prompt": "What is 47 multiplied by 89?",
        "expect_tool": "calculate",
        "validate": lambda a: _all(
            _req(a, "expression"),
            _type_str(a, "expression"),
        ),
    },
    # ---- false positives (must NOT call any tool) ----
    {
        "id": "no_tool_greeting",
        "description": "Saluto — NON deve chiamare tool",
        "prompt": "Hello! How are you doing today?",
        "expect_tool": None,
        "validate": None,
    },
    {
        "id": "no_tool_story",
        "description": "Narrazione libera — NON deve chiamare tool",
        "prompt": "Tell me a short fantasy story about a brave knight.",
        "expect_tool": None,
        "validate": None,
    },
    {
        "id": "no_tool_explain",
        "description": "Spiegazione concettuale — NON deve chiamare tool",
        "prompt": "Explain briefly what a skill check is in tabletop RPGs.",
        "expect_tool": None,
        "validate": None,
    },
]

# ---------------------------------------------------------------------------
# Ollama client
# ---------------------------------------------------------------------------

def ollama_chat(
    model: str,
    messages: list,
    tools: list,
    base_url: str,
    timeout: int,
) -> dict:
    url = base_url.rstrip("/") + "/api/chat"
    payload = {
        "model":    model,
        "messages": messages,
        "tools":    tools,
        "stream":   False,
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url, data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


# ---------------------------------------------------------------------------
# Evaluation
# ---------------------------------------------------------------------------

PASS  = "PASS"
FAIL  = "FAIL"
WARN  = "WARN"


def evaluate(test: dict, response: dict) -> tuple[str, str]:
    """Returns (status, detail)."""
    msg = response.get("message", {})
    tool_calls = msg.get("tool_calls") or []
    content    = (msg.get("content") or "").strip()

    expect = test["expect_tool"]

    # Case: should NOT call a tool
    if expect is None:
        if tool_calls:
            names = [tc.get("function", {}).get("name", "?") for tc in tool_calls]
            return FAIL, f"false positive — called {names} when no tool expected"
        return PASS, "no tool called (correct)"

    # Case: must call a tool
    if not tool_calls:
        # Sometimes model puts tool call in content as JSON — detect it
        if "tool_calls" in content or '"name"' in content:
            return WARN, "tool call embedded in content text (not parsed as tool_calls)"
        return FAIL, f"no tool_calls in response (content: {content[:120]!r})"

    # Check first tool call name
    first = tool_calls[0].get("function", {})
    called_name = first.get("name", "")
    if called_name != expect:
        return FAIL, f"wrong tool: expected '{expect}', got '{called_name}'"

    # Validate args
    raw_args = first.get("arguments", {})
    # Ollama may return args as dict or JSON string
    if isinstance(raw_args, str):
        try:
            raw_args = json.loads(raw_args)
        except json.JSONDecodeError:
            return FAIL, f"arguments not valid JSON: {raw_args!r}"

    if test.get("validate"):
        ok, reason = test["validate"](raw_args)
        if not ok:
            return FAIL, f"args invalid — {reason} | args={raw_args}"

    extra = ""
    if len(tool_calls) > 1:
        extra = f" (+ {len(tool_calls)-1} extra tool calls ignored)"
    return PASS, f"tool='{called_name}' args={raw_args}{extra}"


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = (
    "You are an RPG game assistant. Use the available tools whenever the user's "
    "request clearly requires one. Do not call tools for general conversation or "
    "explanations."
)


def run_model(model: str, cases: list, args: argparse.Namespace) -> dict:
    results = []
    passed = failed = warned = 0

    for tc in cases:
        messages = [
            {"role": "system",  "content": SYSTEM_PROMPT},
            {"role": "user",    "content": tc["prompt"]},
        ]

        status = detail = ""
        raw = None
        error = None

        try:
            raw = ollama_chat(model, messages, TOOLS, args.url, args.timeout)
            status, detail = evaluate(tc, raw)
        except urllib.error.URLError as e:
            status, detail, error = FAIL, "network error", str(e)
        except Exception as e:
            status, detail, error = FAIL, "exception", str(e)

        if status == PASS:  passed  += 1
        elif status == WARN: warned += 1
        else:                failed += 1

        icon = {"PASS": "✓", "FAIL": "✗", "WARN": "~"}.get(status, "?")
        print(f"  {icon} [{tc['id']}] {status}: {detail}")
        if args.verbose and raw:
            print(f"    RAW: {json.dumps(raw.get('message', {}), ensure_ascii=False)}")
        if error:
            print(f"    ERROR: {error}")

        results.append({
            "id":       tc["id"],
            "status":   status,
            "detail":   detail,
            "prompt":   tc["prompt"],
            "expect":   tc["expect_tool"],
        })

    total = len(cases)
    score = int(100 * passed / total) if total else 0
    print(f"\n  Score: {passed}/{total} passed, {warned} warn, {failed} failed — {score}%")

    return {
        "model":   model,
        "passed":  passed,
        "warned":  warned,
        "failed":  failed,
        "total":   total,
        "score":   score,
        "results": results,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Test tool-calling correctness for Ollama models.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--model",   nargs="+", required=True, metavar="MODEL",
                        help="Ollama model name(s) to test")
    parser.add_argument("--url",     default="http://localhost:11434",
                        help="Ollama base URL (default: http://localhost:11434)")
    parser.add_argument("--cases",   nargs="+", metavar="ID",
                        help="Filter test cases by ID (default: all)")
    parser.add_argument("--verbose", action="store_true",
                        help="Print raw Ollama response for each test")
    parser.add_argument("--json",    action="store_true",
                        help="Print final summary as JSON")
    parser.add_argument("--timeout", type=int, default=60,
                        help="Per-call timeout in seconds (default: 60)")
    args = parser.parse_args()

    # Filter test cases
    cases = TEST_CASES
    if args.cases:
        ids = set(args.cases)
        cases = [tc for tc in TEST_CASES if tc["id"] in ids]
        if not cases:
            print(f"[ERROR] No test cases match: {args.cases}", file=sys.stderr)
            print(f"  Available: {[tc['id'] for tc in TEST_CASES]}", file=sys.stderr)
            sys.exit(1)

    print(f"Ollama: {args.url}")
    print(f"Tests:  {len(cases)} case(s)")
    print(f"Tools:  {[t['function']['name'] for t in TOOLS]}\n")
    print("=" * 60)

    all_results = []
    for model in args.model:
        print(f"\nModel: {model}")
        print("-" * 40)
        result = run_model(model, cases, args)
        all_results.append(result)

    # Comparison table (multiple models)
    if len(args.model) > 1:
        print("\n" + "=" * 60)
        print("COMPARISON")
        print("-" * 60)
        header = f"{'Model':<30} {'Pass':>5} {'Warn':>5} {'Fail':>5} {'Score':>6}"
        print(header)
        print("-" * 60)
        for r in all_results:
            print(f"{r['model']:<30} {r['passed']:>5} {r['warned']:>5} {r['failed']:>5} {r['score']:>5}%")

    if args.json:
        print("\n" + json.dumps(all_results, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
