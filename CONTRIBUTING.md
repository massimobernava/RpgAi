# Contributing to RpgAi

Thank you for your interest in contributing!

## What we welcome

- **Bug fixes** — C++ crashes, Lua binding issues, web UI problems
- **New LLM providers** — follow the pattern in `src/llm_query.h`
- **New image providers** — follow the pattern in `src/llm_image.h`
- **Lua adventure scripts** — well-commented scripts that demonstrate new genres or mechanics
- **Documentation improvements** — corrections, clarifications, examples

## How to contribute

1. Fork the repository and create a branch from `main`
2. Make your changes — keep them focused on a single concern
3. Test with `./build.sh` (release) and `./build.sh debug` — both must build cleanly
4. Open a pull request with a clear description of what changed and why

## Code style

- C++17, no external dependencies beyond what is already in `vendor/`
- Comments in English
- Game logic belongs in Lua, not in C++
- New CLI flags go in `parse_args()` and must be documented in `--help` output

## Lua scripts

- Scripts must implement all required functions listed in `CLAUDE.md`
- Use `scripts/fantasy_demo.lua` as the reference template
- Include comments explaining non-obvious game mechanics

## Reporting bugs

Open a GitHub issue with:
- OS and architecture (e.g. macOS arm64, Ubuntu x86_64)
- Build type (release or debug)
- The exact command you ran
- The full error output
