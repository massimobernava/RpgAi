# Installation & Build

## System dependencies

These must be installed via your package manager — they are not in `vendor/`.

**macOS (Homebrew)**
```bash
brew install cmake luajit asio curl
```

**Ubuntu / Debian**
```bash
sudo apt install cmake luajit libluajit-5.1-dev libasio-dev libcurl4-openssl-dev build-essential
```

**Fedora / RHEL**
```bash
sudo dnf install cmake luajit luajit-devel asio-devel libcurl-devel gcc-c++
```

---

## Header-only dependencies

Place these in `vendor/` at the project root. The build system finds them automatically.

| Library | What it does | Where to get it | Expected path in `vendor/` |
|---|---|---|---|
| **sol2** | Lua/C++ binding | [github.com/ThePhD/sol2](https://github.com/ThePhD/sol2/releases) | `vendor/sol/sol.hpp` |
| **Crow** | HTTP server | [github.com/CrowCpp/Crow](https://github.com/CrowCpp/Crow/releases) | `vendor/crow/crow_all.h` |
| **nlohmann/json** | JSON parsing | [github.com/nlohmann/json](https://github.com/nlohmann/json/releases) | `vendor/nlohmann/json.hpp` |
| **ollama-hpp** | Ollama client | [github.com/jmont-dev/ollama-hpp](https://github.com/jmont-dev/ollama-hpp) | `vendor/ollama/ollama.hpp` |
| **stb** | Image load/write/resize | [github.com/nothings/stb](https://github.com/nothings/stb) | `vendor/stb_image.h` + `stb_image_write.h` + `stb_image_resize2.h` |

After downloading, `vendor/` should look like this:

```
vendor/
├── nlohmann/
│   └── json.hpp
├── sol/
│   └── sol.hpp
├── crow/
│   └── crow_all.h
├── ollama/
│   └── ollama.hpp
├── stb_image.h
├── stb_image_write.h
└── stb_image_resize2.h
```

> `vendor/` is git-ignored. Each developer fetches their own copy. If you prefer to commit them, remove `vendor/` from `.gitignore`.

---

## Build

```bash
# Standard release build
./build.sh

# Debug build (with symbols, no optimisation)
./build.sh debug

# Clean rebuild (wipes build/ directory first)
./build.sh clean
```

Binary is placed at `build/rpgai`.

### Custom header paths

```bash
SOL2_INCLUDE_DIR=/opt/mylibs ./build.sh
NLOHMANN_JSON_INCLUDE_DIR=/opt/mylibs ./build.sh
ASIO_INCLUDE_DIR=/usr/local/include/asio ./build.sh
```

### Manual cmake

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

---

## Verifying the build

```bash
./build/rpgai --help
```

Full CLI reference should print. If it runs, everything linked correctly.

---

## Troubleshooting

**`nlohmann/json not found` (or any other header)**
CMake caches `find_path` results. Always do a clean rebuild after adding files:
```bash
./build.sh clean
```

**LuaJIT not found on Linux**
```bash
sudo apt install luajit libluajit-5.1-dev   # Debian/Ubuntu
sudo dnf install luajit luajit-devel        # Fedora
```
If cmake still can't find it:
```bash
cmake .. -DLUAJIT_INCLUDE_DIR=/usr/include/luajit-2.1 \
         -DLUAJIT_LIBRARIES=/usr/lib/x86_64-linux-gnu/libluajit-5.1.so
```

**`asio.hpp not found`**
Must be standalone Asio, not Boost.Asio:
```bash
brew install asio          # macOS
sudo apt install libasio-dev  # Ubuntu
```

**macOS: `framework not found IOKit`**
LuaJIT not found — fix the LuaJIT path first.

**Crow / web server won't start**
Port 8080 may be in use:
```bash
lsof -i :8080
```

---

## Running your first game

**Console mode (terminal)**
```bash
./build/rpgai \
  --provider ollama \
  --model llama3.2 \
  --path scripts/ \
  --script fantasy_demo.lua
```

**Web mode (browser UI)**
```bash
./build/rpgai \
  --web \
  --provider openrouter \
  --or-key sk-or-YOUR_KEY \
  --or-model anthropic/claude-3-haiku \
  --path scripts/ \
  --save-path saves/
```
Then open **http://localhost:8080**, select a script and press **Start**.

**With local image generation (stable-diffusion.cpp)**
```bash
./build/rpgai \
  --web \
  --provider openrouter --or-key sk-or-YOUR_KEY \
  --img-provider sdcpp_local --img-url http://localhost:7860 \
  --path scripts/
```

**With cloud image generation (WaveSpeed i2i + OpenRouter t2i)**
```bash
./build/rpgai \
  --web \
  --provider openrouter --or-key sk-or-YOUR_KEY \
  --img-provider openrouter --img-key sk-or-YOUR_KEY \
  --img-t2i-model black-forest-labs/flux-1.1-pro \
  --img-i2i-provider wavespeed --img-i2i-key YOUR_WAVESPEED_KEY \
  --path scripts/ \
  --save-path saves/
```
