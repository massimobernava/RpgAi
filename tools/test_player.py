#!/usr/bin/env python3
"""
RpgAi test player — automated adventure testing with AI decisions

Drives a playthrough using an LLM to decide player actions. Useful for:
  - QA testing new adventure scripts without manual play
  - Pre-generating scene images before release (--collect-images)
  - Exploring narrative paths with specific objectives (--objective)

Usage:
    python tools/test_player.py --api-key sk-or-... [options]

Install:
    pip install playwright requests
    playwright install chromium

Examples:
    # Basic test run
    python tools/test_player.py --api-key sk-or-... --script my_adventure.lua

    # Pre-generate images headless, save to img/ folder
    python tools/test_player.py --api-key sk-or-... --script my_adventure.lua \\
        --headless --image-every 2 --collect-images img/ --max-turns 40

    # Targeted test with specific objective
    python tools/test_player.py --api-key sk-or-... --script my_adventure.lua \\
        --objective "Find the hidden merchant and buy the rare sword" --max-turns 20
"""

import argparse
import base64
import os
import random
import sys
import time
import requests
from playwright.sync_api import sync_playwright, Page, Browser

# ---------------------------------------------------------------------------
# Selectors (matched against src/web_page.h)
# ---------------------------------------------------------------------------
SEL_INPUT       = "textarea#input"
SEL_SEND        = "#btn-send"
SEL_NARRATION   = ".msg-narration"
SEL_SCRIPT_LIST = "#script-list"
SEL_BTN_START   = "#btn-start"
SEL_SCENE_IMAGE = ".msg-image img"   # last scene image in the chat log
SEL_HUD         = "pre#hud"          # display_state shown above the chat

IMAGE_CMD = "/image regen"

# ---------------------------------------------------------------------------
# Build system prompt dynamically from welcome message
# ---------------------------------------------------------------------------
def build_system_prompt(welcome: str, objective: str | None, lang: str | None) -> str:
    obj_line  = objective or "Explore the world, interact with characters, and progress the story."
    lang_rule = f"\n- Reply in {lang}." if lang else ""
    return f"""You are playing a text RPG. The game started with this introduction:

---
{welcome}
---

OBJECTIVE: {obj_line}

REPLY RULES:
- Reply ONLY with the player action. One short sentence max.
- First person, present tense.
- No meta-commentary, no quotes around your reply.
- Be decisive and progress toward the objective each turn.
- Do NOT repeat the same action you just did.
- Do NOT include /image — the test runner handles image generation automatically.
- If the game asks you to choose between options (e.g. A/B/C or 1/2/3), reply with ONLY the letter or number — nothing else.{lang_rule}"""

# ---------------------------------------------------------------------------
# OpenRouter LLM call
# ---------------------------------------------------------------------------
def call_llm(messages: list, api_key: str, model: str) -> str:
    resp = requests.post(
        "https://openrouter.ai/api/v1/chat/completions",
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "HTTP-Referer": "https://github.com/massimobernava/RpgAi",
            "X-Title": "RpgAi Test Player",
        },
        json={
            "model":       model,
            "messages":    messages,
            "max_tokens":  150,
            "temperature": 0.75,
        },
        timeout=30,
    )
    resp.raise_for_status()
    return resp.json()["choices"][0]["message"]["content"].strip()

# ---------------------------------------------------------------------------
# Human-like typing (slower = more readable; fast in headless mode)
# ---------------------------------------------------------------------------
def type_human(page: Page, selector: str, text: str, cps: float):
    el = page.locator(selector)
    el.click()
    for ch in text:
        el.type(ch)
        base = 1.0 / cps
        if random.random() < 0.04:
            time.sleep(random.uniform(0.15, 0.4))
        else:
            time.sleep(base * random.uniform(0.55, 1.5))

# ---------------------------------------------------------------------------
# Wait until engine finishes responding
# ---------------------------------------------------------------------------
def wait_for_response(page: Page, timeout: float = 120.0):
    deadline = time.time() + timeout
    time.sleep(0.4)
    while time.time() < deadline:
        if page.locator(SEL_SEND).get_attribute("disabled") is None:
            return
        time.sleep(0.5)
    raise TimeoutError(f"Engine did not respond within {timeout}s")

# ---------------------------------------------------------------------------
# Read current narration and HUD from DOM
# ---------------------------------------------------------------------------
def get_latest_narration(page: Page) -> str:
    msgs = page.locator(SEL_NARRATION).all()
    return msgs[-1].inner_text().strip() if msgs else ""

def get_hud(page: Page) -> str:
    try:
        text = page.locator(SEL_HUD).inner_text().strip()
        return text if text else ""
    except Exception:
        return ""

# ---------------------------------------------------------------------------
# Trigger server-side save via REST API
# ---------------------------------------------------------------------------
def trigger_save(url: str):
    try:
        r = requests.post(f"{url}/api/save", timeout=10)
        if r.ok:
            print("  [save] Session saved.")
        else:
            print(f"  [save] Save failed: {r.status_code}")
    except Exception as e:
        print(f"  [save] Save error: {e}")

# ---------------------------------------------------------------------------
# Send text input and press Enter
# ---------------------------------------------------------------------------
def send_input(page: Page, text: str, cps: float):
    inp = page.locator(SEL_INPUT)
    inp.click()
    inp.fill("")
    type_human(page, SEL_INPUT, text, cps)
    time.sleep(random.uniform(0.2, 0.5))
    page.keyboard.press("Enter")

# ---------------------------------------------------------------------------
# Save the latest scene image to collect_dir
# ---------------------------------------------------------------------------
def collect_image(page: Page, collect_dir: str, label: str):
    try:
        imgs = page.locator(SEL_SCENE_IMAGE).all()
        if not imgs:
            return
        src = imgs[-1].get_attribute("src") or ""
        if not src.startswith("data:image"):
            return
        header, data = src.split(",", 1)
        ext = "jpg" if "jpeg" in header else "png"
        path = os.path.join(collect_dir, f"{label}.{ext}")
        with open(path, "wb") as f:
            f.write(base64.b64decode(data))
        print(f"  [image saved] {path}")
    except Exception as e:
        print(f"  [image collect failed] {e}")

# ---------------------------------------------------------------------------
# Setup: open UI, select script, start, enter player name
# ---------------------------------------------------------------------------
def setup_game(page: Page, url: str, script_name: str,
               player_name: str, cps: float) -> tuple[str, str]:
    print(f"[setup] {url}")
    page.goto(url)
    page.wait_for_load_state("networkidle")
    time.sleep(1.0)

    print(f"[setup] script: {script_name}")
    item = page.locator(f"{SEL_SCRIPT_LIST} li").filter(has_text=script_name).first
    item.wait_for(timeout=10000)
    item.click()
    time.sleep(0.5)

    page.locator(SEL_BTN_START).click()
    time.sleep(1.0)

    page.wait_for_selector(SEL_NARRATION, timeout=15000)
    welcome = get_latest_narration(page)
    print(f"[setup] welcome ({len(welcome)} chars)")
    time.sleep(1.5)

    print(f"[setup] name: {player_name}")
    wait_for_response(page)
    send_input(page, player_name, cps)
    wait_for_response(page)
    time.sleep(2.0)

    first = get_latest_narration(page)
    print(f"[setup] first narration: {first[:100]}...")
    return welcome, first

# ---------------------------------------------------------------------------
# Main test loop
# ---------------------------------------------------------------------------
END_KEYWORDS = [
    "VICTORY", "DEFEAT", "GAME OVER", "THE END", "YOU DIED",
    "ADVENTURE ENDS", "QUEST COMPLETE", "MISSION COMPLETE",
]

def run_test(page: Page, api_key: str, model: str, cps: float,
             max_turns: int, image_every: int, collect_dir: str | None,
             objective: str | None, lang: str | None, welcome: str,
             base_url: str = "http://localhost:8080"):

    system_prompt = build_system_prompt(welcome, objective, lang)
    conversation  = [{"role": "system", "content": system_prompt}]

    first = get_latest_narration(page)
    conversation.append({"role": "user", "content": f"[Game]: {first}"})

    turn           = 0
    last_was_image = False
    next_image_at  = image_every if image_every > 0 else 10 ** 9

    while turn < max_turns:
        turn += 1
        print(f"\n[turn {turn}/{max_turns}]")

        # Scheduled image turn?
        if image_every > 0 and turn >= next_image_at and not last_was_image:
            action = IMAGE_CMD
            next_image_at = turn + image_every
        else:
            try:
                action = call_llm(conversation, api_key, model)
            except Exception as e:
                print(f"  LLM error: {e} — retry in 5s")
                time.sleep(5)
                try:
                    action = call_llm(conversation, api_key, model)
                except Exception as e2:
                    print(f"  LLM failed: {e2} — skip turn")
                    continue

            # Strip any /image the LLM snuck in despite instructions
            if "/image" in action:
                action = action.replace("/image", "").strip().rstrip(".")
            if not action:
                action = "I look around, taking in my surroundings."

        is_image = action.strip() == IMAGE_CMD
        print(f"  → {action}")

        think = random.uniform(0.4, 1.0) if is_image else random.uniform(1.0, 2.5)
        time.sleep(think)

        send_input(page, action, cps)
        wait_for_response(page)

        if is_image:
            print("  waiting for image generation...")
            time.sleep(random.uniform(12.0, 16.0))
            if collect_dir:
                collect_image(page, collect_dir, f"turn_{turn:03d}")
            last_was_image = True
        else:
            time.sleep(random.uniform(1.2, 2.0))
            last_was_image = False

        narration = get_latest_narration(page)
        hud       = get_hud(page)
        print(f"  {narration[:120]}...")

        if not is_image:
            user_msg = f"[HUD]: {hud}\n[Game]: {narration}" if hud else f"[Game]: {narration}"
            conversation.append({"role": "assistant", "content": action})
            conversation.append({"role": "user",      "content": user_msg})

        if any(kw in narration.upper() for kw in END_KEYWORDS):
            print(f"\n[done] End state at turn {turn}.")
            trigger_save(base_url)
            time.sleep(3.0)
            break

    else:
        print(f"\n[done] Max turns ({max_turns}) reached.")
        trigger_save(base_url)

    print(f"[done] {turn} turns completed.")

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main():
    p = argparse.ArgumentParser(
        description="RpgAi automated test player",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("--api-key",        required=True,
                   help="OpenRouter API key (sk-or-...)")
    p.add_argument("--model",          default="google/gemini-flash-1.5",
                   help="OpenRouter model (default: gemini-flash-1.5)")
    p.add_argument("--url",            default="http://localhost:8080",
                   help="RpgAi web UI URL")
    p.add_argument("--script",         default="fantasy_demo.lua",
                   help="Lua script filename to select")
    p.add_argument("--name",           default="Adventurer",
                   help="Player name")
    p.add_argument("--objective",      default=None,
                   help="Optional test objective (e.g. 'Find the hidden merchant')")
    p.add_argument("--lang",           default=None,
                   help="Language for player replies (e.g. 'italiano', 'english')")
    p.add_argument("--cps",            type=float, default=30.0,
                   help="Typing speed chars/sec (default: 30; use 9 for demo recording)")
    p.add_argument("--max-turns",      type=int,   default=30,
                   help="Max turns before stopping (default: 30)")
    p.add_argument("--image-every",    type=int,   default=4,
                   help="Generate scene image every N turns (default: 4; 0 = disabled)")
    p.add_argument("--collect-images", metavar="DIR", default=None,
                   help="Save generated scene images to DIR")
    p.add_argument("--headless",       action="store_true",
                   help="Run Chromium headless (no visible window)")
    args = p.parse_args()

    if args.collect_images:
        os.makedirs(args.collect_images, exist_ok=True)
        print(f"[info] Images will be saved to: {args.collect_images}")

    with sync_playwright() as pw:
        browser: Browser = pw.chromium.launch(
            headless=args.headless,
            slow_mo=0 if args.headless else 30,
            args=["--window-size=1280,900"],
        )
        page = browser.new_page(viewport={"width": 1280, "height": 900})

        try:
            welcome, _ = setup_game(
                page, args.url, args.script, args.name, args.cps
            )
            run_test(
                page, args.api_key, args.model, args.cps,
                args.max_turns, args.image_every,
                args.collect_images, args.objective, args.lang, welcome,
                base_url=args.url,
            )
        except KeyboardInterrupt:
            print("\n[interrupted]")
        except Exception as e:
            print(f"\n[error] {e}")
            import traceback; traceback.print_exc()
        finally:
            time.sleep(2.0)
            browser.close()

if __name__ == "__main__":
    main()
