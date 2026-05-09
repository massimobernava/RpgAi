#!/usr/bin/env python3
"""
Pipeline Racconto → Script RpgAi  v2
Multi-agent system with critic loop and composable skills.

Usage:
    python run_pipeline.py story.md script_name
    python run_pipeline.py story.md script_name --from-stage 3
    python run_pipeline.py story.md script_name --force
"""

import os
import sys
import re
import json
import argparse
from pathlib import Path

SCRIPT_DIR   = Path(__file__).parent
AGENTS_DIR   = SCRIPT_DIR / "agents"
SKILLS_DIR   = SCRIPT_DIR / "skills"
DEFAULT_MODEL        = "claude-sonnet-4-6"
DEFAULT_MODEL_OR     = "anthropic/claude-3.5-sonnet"  # default per OpenRouter

# Skills injected per agent (order matters: most general last)
AGENT_SKILLS = {
    "analyzer":       ["npc_extraction", "location_extraction", "progression_model"],
    "world_builder":  ["lua_tables"],
    "schema_designer":["json_schema"],
    "assembler":      ["dynamic_prompt", "state_validation", "lua_commands"],
}

# Per-stage programmatic validators (run before critic call)
def _check_entities(text: str) -> list[str]:
    issues = []
    try:
        d = json.loads(text)
    except Exception as e:
        return [f"JSON non valido: {e}"]
    for key in ("titolo", "protagonista", "npc_list", "location_list", "progressione_narrativa"):
        if key not in d:
            issues.append(f"Campo mancante: {key}")
    for npc in d.get("npc_list", []):
        for f in ("id", "name", "appearance", "personality", "routine"):
            if f not in npc:
                issues.append(f"NPC '{npc.get('id','?')}' manca campo '{f}'")
    for loc in d.get("location_list", []):
        if "acoustic" not in loc:
            issues.append(f"Location '{loc.get('id','?')}' manca 'acoustic'")
    return issues

def _check_world(text: str) -> list[str]:
    issues = []
    for sym in ("local NPC", "local locations", "local travel_map",
                "local function presenti", "local function clamp",
                "local function default_state"):
        if sym not in text:
            issues.append(f"Simbolo mancante: {sym}")
    return issues

def _check_mechanics(text: str) -> list[str]:
    issues = []
    for sym in ("function get_json_schema", "local SOGLIE", "local CAP_GIORNALIERI",
                "local SYSTEM_PROMPT_TEMPLATE"):
        if sym not in text:
            issues.append(f"Simbolo mancante: {sym}")
    # Validate the JSON schema string is itself parseable
    m = re.search(r'get_json_schema\(\)\s*\n\s*return\s*\[\[(.+?)\]\]', text, re.DOTALL)
    if m:
        try:
            json.loads(m.group(1))
        except Exception as e:
            issues.append(f"JSON schema non valido: {e}")
    return issues

def _check_script(text: str) -> list[str]:
    required = [
        "function get_welcome_message",
        "function set_initial_state",
        "function generate_initial_state",
        "function get_status_for_ai",
        "function get_system_prompt",
        "function get_json_schema",
        "function process_ai_response",
        "function process_player_input",
        "function get_display_state",
        "function get_state_snapshot",
        "function restore_state",
    ]
    issues = []
    for fn in required:
        if fn not in text:
            issues.append(f"Funzione mancante: {fn}")
    if "local state" in text and "state =" not in text.replace("local state", ""):
        issues.append("'state' deve essere globale (non local)")
    if "local json = require" not in text:
        issues.append("Manca: local json = require('json')")
    return issues

STAGE_VALIDATORS = {
    "Stage 1: Story Analyzer":         _check_entities,
    "Stage 2: World Builder":          _check_world,
    "Stage 3: Schema & Mechanics":     _check_mechanics,
    "Stage 4: Script Assembler":       _check_script,
}


# ── Helpers ───────────────────────────────────────────────────────────────────

def load_file(path: Path) -> str:
    return path.read_text(encoding="utf-8")

def save_file(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")

def strip_fences(text: str) -> str:
    """Remove markdown code fences (```lua, ```json, etc.)."""
    text = text.strip()
    text = re.sub(r'^```\w*\s*\n', '', text)
    text = re.sub(r'\n```\s*$', '', text)
    return text.strip()

def build_system_prompt(agent_name: str) -> str:
    """Load agent prompt, inject relevant skills."""
    agent_path = AGENTS_DIR / f"{agent_name}.md"
    base = load_file(agent_path)
    skills_text = ""
    for skill in AGENT_SKILLS.get(agent_name, []):
        sp = SKILLS_DIR / f"{skill}.md"
        if sp.exists():
            skills_text += f"\n\n---\n## SKILL: {skill.upper().replace('_', ' ')}\n\n{load_file(sp)}"
    if skills_text:
        base += "\n\n# REFERENCE SKILLS\n" + skills_text
    return base


# ── LLM client factory ────────────────────────────────────────────────────────

def make_client(provider: str, api_key: str):
    """Return (client, call_fn) pair. call_fn(client, model, system, user, max_tokens) → str."""
    if provider == "anthropic":
        try:
            import anthropic as _anthropic
        except ImportError:
            print("Missing: pip install anthropic")
            sys.exit(1)
        client = _anthropic.Anthropic(api_key=api_key)

        def call_fn(client, model, system, user, max_tokens=8192):
            msg = client.messages.create(
                model=model,
                max_tokens=max_tokens,
                system=[{"type": "text", "text": system,
                         "cache_control": {"type": "ephemeral"}}],
                messages=[{"role": "user", "content": user}]
            )
            return msg.content[0].text

        return client, call_fn

    elif provider == "openrouter":
        try:
            import openai as _openai
        except ImportError:
            print("Missing: pip install openai")
            sys.exit(1)
        client = _openai.OpenAI(
            api_key=api_key,
            base_url="https://openrouter.ai/api/v1",
        )

        def call_fn(client, model, system, user, max_tokens=8192):
            resp = client.chat.completions.create(
                model=model,
                max_tokens=max_tokens,
                messages=[
                    {"role": "system", "content": system},
                    {"role": "user",   "content": user},
                ],
            )
            return resp.choices[0].message.content

        return client, call_fn

    else:
        print(f"Provider sconosciuto: {provider}. Usa 'anthropic' o 'openrouter'.")
        sys.exit(1)


# ── LLM call helpers ──────────────────────────────────────────────────────────

def call_llm(client, call_fn, model: str,
             system: str, user: str, max_tokens: int = 8192) -> str:
    return call_fn(client, model, system, user, max_tokens)


def call_critic(client, call_fn, model: str,
                stage: str, output: str, context: str) -> dict:
    system = load_file(AGENTS_DIR / "critic.md")
    user = f"STAGE: {stage}\n\nCONTEXT:\n{context[:2000]}\n\nOUTPUT TO REVIEW:\n{output}"
    response = call_llm(client, call_fn, model, system, user, max_tokens=2048)
    try:
        m = re.search(r'```json\s*(.*?)\s*```', response, re.DOTALL)
        raw = m.group(1) if m else response
        return json.loads(raw)
    except Exception:
        return {"ok": True, "issues": [], "suggestions": []}


# ── Stage runner ──────────────────────────────────────────────────────────────

def run_stage(
    client,
    call_fn,
    model: str,
    agent_name: str,
    user_prompt: str,
    stage_label: str,
    output_path: Path,
    critique_context: str = "",
    skip_if_exists: bool = True,
    max_retries: int = 3,
) -> str:
    if skip_if_exists and output_path.exists():
        print(f"  ✓ {stage_label} — cache hit ({output_path.name})")
        return load_file(output_path)

    print(f"  ▶ {stage_label}...")
    system = build_system_prompt(agent_name)
    output = strip_fences(call_llm(client, call_fn, model, system, user_prompt))

    validator = STAGE_VALIDATORS.get(stage_label)

    for attempt in range(max_retries):
        # Programmatic checks first (fast, free)
        prog_issues = validator(output) if validator else []

        if not prog_issues:
            # Semantic critic check
            critique = call_critic(client, call_fn, model, stage_label, output, critique_context)
            if critique.get("ok", True):
                break
            issues   = critique.get("issues", [])
            suggestions = critique.get("suggestions", [])
        else:
            issues      = prog_issues
            suggestions = []

        if attempt == max_retries - 1:
            print(f"    ⚠ Max retry raggiunto con issues: {issues[:2]}")
            break

        print(f"    ✗ Retry {attempt+1}: {issues[0] if issues else '?'}")
        feedback = (
            "Il precedente output ha questi problemi:\n"
            + "\n".join(f"- {i}" for i in issues)
            + ("\n\nSuggerimenti:\n" + "\n".join(f"- {s}" for s in suggestions) if suggestions else "")
            + "\n\nOutput precedente:\n" + output
            + "\n\nCorreggi TUTTI i problemi e rigenera l'output completo."
        )
        output = strip_fences(call_llm(client, call_fn, model, system,
                                       user_prompt + "\n\n" + feedback))

    save_file(output_path, output)
    print(f"  ✓ {stage_label} → {output_path.name}")
    return output


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Pipeline Racconto → RpgAi")
    parser.add_argument("story",        help="File racconto (.md o .txt)")
    parser.add_argument("name",         help="Nome script output (senza .lua)")
    parser.add_argument("--work-dir",   default="pipeline_stages",
                        help="Directory per output intermedi (default: pipeline_stages)")
    parser.add_argument("--scripts-dir",default="scripts",
                        help="Directory output script finale (default: scripts)")
    parser.add_argument("--force",      action="store_true",
                        help="Forza riesecuzione di tutti gli stage")
    parser.add_argument("--from-stage", type=int, default=1,
                        help="Parti dallo stage N (1-4), ignora cache degli stage precedenti")
    parser.add_argument("--provider",   default="anthropic",
                        choices=["anthropic", "openrouter"],
                        help="Provider LLM: anthropic (default) o openrouter")
    parser.add_argument("--model",      default=None,
                        help=f"Modello. Default: {DEFAULT_MODEL} (anthropic) o {DEFAULT_MODEL_OR} (openrouter)")
    parser.add_argument("--retries",    type=int, default=3,
                        help="Retry per stage con critic feedback (default: 3)")
    parser.add_argument("--api-key",    default=None,
                        help="API key (o usa ANTHROPIC_API_KEY / OPENROUTER_API_KEY)")
    args = parser.parse_args()

    # Risolvi API key e modello default per provider
    if args.provider == "anthropic":
        api_key = args.api_key or os.environ.get("ANTHROPIC_API_KEY")
        model   = args.model or DEFAULT_MODEL
    else:
        api_key = args.api_key or os.environ.get("OPENROUTER_API_KEY")
        model   = args.model or DEFAULT_MODEL_OR

    story_path = Path(args.story)
    if not story_path.exists():
        print(f"Errore: file racconto non trovato: {story_path}")
        sys.exit(1)
    if not api_key:
        env_var = "ANTHROPIC_API_KEY" if args.provider == "anthropic" else "OPENROUTER_API_KEY"
        print(f"Errore: API key mancante. Imposta {env_var} o usa --api-key")
        sys.exit(1)

    story_text  = load_file(story_path)
    work_dir    = Path(args.work_dir)
    scripts_dir = Path(args.scripts_dir)
    work_dir.mkdir(exist_ok=True)
    scripts_dir.mkdir(exist_ok=True)

    client, call_fn = make_client(args.provider, api_key)

    def skip(stage_num: int) -> bool:
        return (not args.force) and args.from_stage <= stage_num

    print(f"\n  Pipeline Racconto → Script RpgAi v2")
    print(f"  Racconto:  {story_path}")
    print(f"  Script:    {scripts_dir / (args.name + '.lua')}")
    print(f"  Provider:  {args.provider}")
    print(f"  Modello:   {model}")
    print()

    def stage(agent, user, label, path, ctx=""):
        return run_stage(client, call_fn, model, agent, user, label, path,
                         critique_context=ctx,
                         skip_if_exists=skip(int(label.split()[1].rstrip(":"))),
                         max_retries=args.retries)

    # ── Stage 1 ───────────────────────────────────────────────────────────────
    entities_path = work_dir / "entities.json"
    entities_json = run_stage(
        client, call_fn, model, "analyzer",
        f"Analizza questo racconto ed estrai tutte le entità strutturate:\n\n{story_text}",
        "Stage 1: Story Analyzer",
        entities_path,
        critique_context=story_text,
        skip_if_exists=skip(1),
        max_retries=args.retries,
    )

    try:
        json.loads(entities_json)
    except json.JSONDecodeError as e:
        print(f"  ✗ entities.json non valido: {e}")
        sys.exit(1)

    # ── Stage 2 ───────────────────────────────────────────────────────────────
    world_path = work_dir / "world_data.lua"
    world_lua = run_stage(
        client, call_fn, model, "world_builder",
        f"Genera le strutture dati Lua dal seguente entities.json:\n\n{entities_json}",
        "Stage 2: World Builder",
        world_path,
        critique_context=entities_json,
        skip_if_exists=skip(2),
        max_retries=args.retries,
    )

    # ── Stage 3 ───────────────────────────────────────────────────────────────
    mechanics_path = work_dir / "mechanics.lua"
    mechanics_lua = run_stage(
        client, call_fn, model, "schema_designer",
        (
            "Progetta lo schema JSON e le meccaniche di gioco da:\n\n"
            f"ENTITIES:\n{entities_json}\n\n"
            f"WORLD DATA:\n{world_lua}"
        ),
        "Stage 3: Schema & Mechanics",
        mechanics_path,
        critique_context=entities_json,
        skip_if_exists=skip(3),
        max_retries=args.retries,
    )

    # ── Stage 4 ───────────────────────────────────────────────────────────────
    final_path = scripts_dir / f"{args.name}.lua"
    run_stage(
        client, call_fn, model, "assembler",
        (
            f"Assembla lo script Lua completo per RpgAi. Nome script: {args.name}\n\n"
            f"ENTITIES:\n{entities_json}\n\n"
            f"WORLD DATA:\n{world_lua}\n\n"
            f"MECHANICS:\n{mechanics_lua}"
        ),
        "Stage 4: Script Assembler",
        final_path,
        critique_context=entities_json,
        skip_if_exists=skip(4),
        max_retries=args.retries,
    )

    print()
    print("  Pipeline completata!")
    print(f"  Script → {final_path}")
    print()
    print("  Per testare con Ollama:")
    print(f"    ./build/rpgai --provider ollama --model llama3.2 \\")
    print(f"      --path scripts/ --script {args.name}.lua")
    print()
    print("  Per testare in web mode:")
    print(f"    ./build/rpgai --web --provider openrouter --or-key sk-or-... \\")
    print(f"      --or-model anthropic/claude-3.5-sonnet \\")
    print(f"      --path scripts/ --save-path saves/")
    print()


if __name__ == "__main__":
    main()
