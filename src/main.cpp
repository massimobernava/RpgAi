
#define SOL_ALL_SAFETIES_ON 1
#define SOL_PRINT_ERRORS 1

#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <sol/sol.hpp>
#include <ollama/ollama.hpp>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <set>
#include <cctype>
#include <regex>
#include <crow/crow_all.h>
#include <filesystem>
#include <thread>
#include <csignal>
#include <atomic>
#include <glob.h>

using nlohmann::json;

static std::atomic<bool> g_shutdown{false};

// Sandbox (run_lua) deadline in epoch-ms. Thread-local so the Lua debug hook
// reads it from C++ storage the executed script cannot reach or overwrite.
static thread_local long long g_sandbox_deadline_ms = 0;

// Serializes the per-render override of the global img_cfg.strength. Web image
// jobs run on detached threads; without this two concurrent renders clobber the
// shared strength value. Held only around the i2i call, not the whole job.
static std::mutex g_img_gen_mutex;

// Guards cfg provider/endpoint fields between the /api/settings writer and the
// detached query_llm_async worker (the only LLM reader outside lua_mutex). Game
// turns and /api/settings already serialize via lua_mutex.
static std::mutex g_cfg_mutex;

// Stem of the currently loaded script (no path, no .lua extension). Used by
// vn_catalog_file() so each adventure gets its own catalog file. Updated
// every time active_script is assigned in the web-mode block.
static std::string g_active_script_stem;

// ---------------------------------------------------------------------------
// CSRF guard (Crow middleware)
//
// The web server binds to localhost, but any web page open in the user's
// browser can POST to http://localhost:8080 (cross-site request forgery).
// Several endpoints are dangerous — /api/servers/action runs system(),
// /api/coder/* writes files and runs Lua, /api/chat spends cloud LLM credits.
//
// Browsers always attach an Origin header to POST (even cross-origin no-cors
// requests), so we reject any state-changing POST whose Origin/Referer is not
// our own page. Non-browser clients (curl, the CLI) send neither header and
// are allowed — they already have full local access.
// ---------------------------------------------------------------------------
struct CsrfGuard {
    struct context {};
    static int port;   // set from cfg.webPort at startup (default 8080)

    static bool same_origin(const std::string& u) {
        if (u.empty()) return false;
        // Accept our own page on any loopback host at the configured port.
        std::string p = std::to_string(port);
        const std::string ok[] = {
            "http://localhost:" + p, "http://127.0.0.1:" + p, "http://[::1]:" + p
        };
        for (auto& o : ok)
            if (u.rfind(o, 0) == 0) return true;
        return false;
    }

    void before_handle(crow::request& req, crow::response& res, context&) {
        if (req.method != crow::HTTPMethod::Post) return;
        const std::string& origin  = req.get_header_value("Origin");
        const std::string& referer = req.get_header_value("Referer");
        // No browser headers → trusted local (non-browser) client.
        if (origin.empty() && referer.empty()) return;
        bool ok = origin.empty() ? same_origin(referer) : same_origin(origin);
        if (!ok) {
            res.code = 403;
            res.set_header("Content-Type", "application/json");
            res.body = R"json({"success":false,"error":"cross-origin POST blocked (CSRF guard)"})json";
            res.end();
        }
    }
    void after_handle(crow::request&, crow::response&, context&) {}
};
int CsrfGuard::port = 8080;

static bool ansi_enabled = false;

static void init_ansi() {
#ifdef _WIN32
    // Enable VT100 on modern Windows (10+)
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        ansi_enabled = true;
    }
#else
    ansi_enabled = isatty(STDOUT_FILENO);
#endif
}

// ANSI codes — internal use only, prefer the print_* functions
namespace C {
    // Text
    constexpr auto RESET       = "\033[0m";
    constexpr auto BOLD        = "\033[1m";
    constexpr auto DIM         = "\033[2m";
    constexpr auto ITALIC      = "\033[3m";

    // Foreground colors
    constexpr auto BLACK       = "\033[30m";
    constexpr auto RED         = "\033[31m";
    constexpr auto GREEN       = "\033[32m";
    constexpr auto YELLOW      = "\033[33m";
    constexpr auto BLUE        = "\033[34m";
    constexpr auto MAGENTA     = "\033[35m";
    constexpr auto CYAN        = "\033[36m";
    constexpr auto WHITE       = "\033[37m";
    constexpr auto BRIGHT_BLACK   = "\033[90m";
    constexpr auto BRIGHT_RED     = "\033[91m";
    constexpr auto BRIGHT_GREEN   = "\033[92m";
    constexpr auto BRIGHT_YELLOW  = "\033[93m";
    constexpr auto BRIGHT_BLUE    = "\033[94m";
    constexpr auto BRIGHT_MAGENTA = "\033[95m";
    constexpr auto BRIGHT_CYAN    = "\033[96m";
    constexpr auto BRIGHT_WHITE   = "\033[97m";
}

// Apply ANSI sequence only if the terminal supports it
static std::string ansi(const char* code) {
    return ansi_enabled ? code : "";
}

// ---------------------------------------------------------------------------
// Semantic print functions
// Each content type has its own color and format.
// ---------------------------------------------------------------------------

// Main narration — bright white text
static void print_narration(const std::string& text) {
    std::cout << "\n"
              << ansi(C::BRIGHT_WHITE)
              << text
              << ansi(C::RESET)
              << "\n";
}

// HUD / status display — cyan text
static void print_display(const std::string& text) {
    // Lua-formatted text is globally colored in dark cyan
    std::cout << ansi(C::CYAN) << text << ansi(C::RESET);
}

// System messages [SYSTEM], [RAG], etc. — blue
static void print_system(const std::string& msg) {
    std::cout << ansi(C::BRIGHT_BLUE) << "[SYSTEM] " << ansi(C::RESET) << msg << "\n";
}

// Errors — red
static void print_error(const std::string& msg) {
    std::cerr << ansi(C::BRIGHT_RED) << "[!] " << ansi(C::RESET) << msg << "\n";
}

// Warnings — yellow
static void print_warning(const std::string& msg) {
    std::cerr << ansi(C::YELLOW) << "[WARN] " << ansi(C::RESET) << msg << "\n";
}

// Titled box (summary, observe, etc.) — magenta
static void print_box(const std::string& title, const std::string& content) {
    std::string sep(50, '=');
    std::cout << "\n"
              << ansi(C::BRIGHT_MAGENTA) << sep << "\n"
              << "  " << title << "\n"
              << sep << ansi(C::RESET) << "\n"
              << ansi(C::WHITE) << content << ansi(C::RESET) << "\n"
              << ansi(C::BRIGHT_MAGENTA) << sep << ansi(C::RESET) << "\n\n";
}

// Prompt di input — verde, con supporto readline per backspace e history
static std::string read_input(const std::string& prompt_str = "> ") {
    std::cout << ansi(C::BRIGHT_GREEN) << prompt_str << ansi(C::RESET) << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return "/quit";
    return line;
}

// Simple spinner for long LLM operations
static void print_thinking(const std::string& msg = "Thinking...") {
    std::cout << ansi(C::DIM) << ansi(C::ITALIC) << "⟳  " << msg << ansi(C::RESET) << "\n" << std::flush;
}

// Separatore visivo
static void print_separator() {
    if (ansi_enabled)
        std::cout << ansi(C::BRIGHT_BLACK) << std::string(50, '-') << ansi(C::RESET) << "\n";
}

enum class AIProvider  { OLLAMA, GEMINI, OPENAI, CLAUDE, OPENROUTER };
enum class SaveMode    { LAST, FULL };    // LAST = sovrascrive sempre, FULL = appende

// ===========================================================================
// Configuration
// ===========================================================================

struct Config {
    std::string script       = "fantasy_demo.lua";
    std::string basePath     = "../scripts/";
    std::string loadFile;
    std::string saveFile     = "session_log.jsonl";
    std::string savePath     = "";          // directory per i file .jsonl (default: cwd)
    SaveMode    saveMode     = SaveMode::LAST;

    // Image generation
    bool        imgEnabled   = false;         // enabled only if --img-provider is specified
    std::string imgProvider  = "sdcpp_local";
    std::string imgUrl       = "http://localhost:7860";
    std::string imgKey;
    std::string imgT2iModel;
    std::string imgI2iModel;
    std::string imgI2iProvider;  // "" = segue imgProvider
    std::string imgI2iUrl;
    std::string imgI2iKey;
    int         imgWidth     = 1024;
    int         imgHeight    = 1024;
    int         imgSteps     = 28;
    int         imgI2iSteps  = 0;     // 0 = use imgSteps; --img-i2i-steps
    float       imgStrength  = 0.75f;
    std::string imgLora;              // --img-lora
    float       imgLoraScale    = 1.0f; // --img-lora-scale
    std::string imgLoraModel;         // --i2i-model-lora (default: edit-plus-lora)
    float       imgGuidanceScale = 1.0f; // --img-guidance-scale

    // Face-swap
    std::string faceswapUrl;        // --faceswap-url (empty = disabled)
    std::string pyEnvType = "system"; // system | venv | conda | uv
    std::string pyEnvPath;            // venv dir or conda env name
    std::string qwenLocaleArgs;       // extra CLI args forwarded on qwen_locale start
    std::string ttsLocaleArgs;        // extra CLI args forwarded on tts_locale start
    std::string ttsLocaleEnvType;     // per-server env override (empty = use global)
    std::string ttsLocaleEnvPath;
    std::string ttsUrl;               // TTS server base URL (default: http://localhost:8004)
    std::string ttsNarratorVoice;     // voice_id used for narration (must exist in TTS server voices/)

    // Session tracking — set on first save, used to filter images on load
    std::string sessionStart;       // ISO8601 UTC timestamp of first turn in this session
    int         maxHistory   = 30;
    int         maxRetries   = 3;

    // Language injection
    std::string langCode;         // e.g. "it" — empty means no injection
    std::string langFile    = "lang.txt";  // path to the language file
    std::string langInstruction;           // phrase loaded from lang.txt at startup

    bool        webMode      = false;
    bool        openBrowser  = false;   // --web opens browser; --rest does not
    int         webPort      = 8080;    // --port: web server port
    bool        debugGui     = false;   // --debug-gui: enable GUI possess/debug routes
    std::string assetRoot;              // --asset-root: base dir for asset/catalog (default cwd)

    // RAG
    std::string ragFile;
    int         ragExamples  = 3;

    // Embedding
    // Provider: "ollama" uses /api/embeddings, "openai" uses /v1/embeddings.
    // Defaults to the same provider as the text model.
    std::string embedProvider;      // "" = follows main provider
    std::string embedModel;         // embedding model (e.g. "nomic-embed-text")
    std::string embedUrl;           // base URL override for embeddings
    std::string embedKey;           // API key for OpenAI-compatible endpoint

    AIProvider  provider     = AIProvider::OLLAMA;
    std::string providerName = "ollama";

    std::string ollama_model   = "dolphin3:latest";
    std::string ollama_baseUrl = "http://localhost:11434";
    std::string gemini_baseUrl = "https://generativelanguage.googleapis.com/v1beta/models/";
    std::string gemini_model   = "gemini-flash-latest";
    std::string gemini_key;
    std::string openai_baseUrl = "https://api.openai.com/v1/chat/completions";
    std::string openai_model   = "gpt-4o-mini";
    std::string openai_key;
    std::string claude_baseUrl = "https://api.anthropic.com/v1/messages";
    std::string claude_model   = "claude-haiku-4-5-20251001";
    std::string claude_key;
    std::string openrouter_baseUrl   = "https://openrouter.ai/api/v1/chat/completions";
    std::string openrouter_model     = "qwen/qwen3-32b";
    std::string openrouter_key;
    std::string openrouter_app_url;
    std::string openrouter_app_title = "RpgAi";

    // Model tiers — optional per-role model/provider defaults read by the Lua
    // libs (via get_tier) when the script does not pass explicit overrides.
    //   gen     → entity generation (persona/world): strong model, amortized
    //   agent   → NPC agents (think_as_npc): frequent, short output
    //   ambient → off-screen NPC↔NPC events: cheapest, output never shown
    // "" = fall back to the main provider/model.
    std::string genModel,     genProvider;
    std::string agentModel,   agentProvider;
    std::string ambientModel, ambientProvider;

    // CoderAI — separate LLM session for script writing
    AIProvider  coderProvider;
    std::string coderProviderName = "";   // "" = inherit main provider
    std::string coderModel        = "";   // "" = inherit activeModel()
    std::string coderKey          = "";   // "" = inherit provider key (unused in Phase 1)
    std::string coderKnowledgePath = "";  // "" = basePath + "coder_knowledge/"
    std::string coderPersona = "";        // "" = DEFAULT_CODER_PERSONA (identity/tone; tool rules stay fixed)
    // Vision model for analyze_image — can differ from tool-calling coder model
    AIProvider  coderVisionProvider;
    std::string coderVisionProviderName = "";  // "" = inherit coderProvider
    std::string coderVisionModel        = "";  // "" = inherit coderModel
    std::string searchProvider = "duckduckgo"; // "duckduckgo" | "brave"
    std::string searchKey      = "";           // Brave API key (optional)
    std::string pixabayKey     = "";           // Pixabay image search key (optional, free at pixabay.com)

    std::string activeModel() const {
        switch (provider) {
            case AIProvider::GEMINI:     return gemini_model;
            case AIProvider::OPENAI:     return openai_model;
            case AIProvider::CLAUDE:     return claude_model;
            case AIProvider::OPENROUTER: return openrouter_model;
            default:                     return ollama_model;
        }
    }

    std::string validate() const {
        switch (provider) {
            case AIProvider::GEMINI:
                if (gemini_key.empty())     return "Gemini provider requires --g-key";
                break;
            case AIProvider::OPENAI:
                if (openai_key.empty())     return "OpenAI provider requires --oai-key";
                break;
            case AIProvider::CLAUDE:
                if (claude_key.empty())     return "Claude provider requires --claude-key";
                break;
            case AIProvider::OPENROUTER:
                if (openrouter_key.empty()) return "OpenRouter provider requires --or-key";
                break;
            default: break;
        }
        return {};
    }
};

struct Message { std::string role; std::string content; std::string player_id; std::string timestamp; };

struct ToolDef {
    std::string name;
    std::string description;
    std::string params_schema;  // JSON Schema string for parameters
};

static Config cfg;

// Aliases for compatibility with llm_query.h
std::string& script              = cfg.script;
std::string& basePath            = cfg.basePath;
std::string& ollama_model        = cfg.ollama_model;
std::string& ollama_baseUrl      = cfg.ollama_baseUrl;
std::string& gemini_baseUrl      = cfg.gemini_baseUrl;
std::string& gemini_model        = cfg.gemini_model;
std::string& gemini_key          = cfg.gemini_key;
std::string& openai_baseUrl      = cfg.openai_baseUrl;
std::string& openai_model        = cfg.openai_model;
std::string& openai_api_key      = cfg.openai_key;
std::string& claude_baseUrl      = cfg.claude_baseUrl;
std::string& claude_model        = cfg.claude_model;
std::string& claude_api_key      = cfg.claude_key;
std::string& openrouter_baseUrl  = cfg.openrouter_baseUrl;
std::string& openrouter_model    = cfg.openrouter_model;
std::string& openrouter_api_key  = cfg.openrouter_key;
std::string& openrouter_app_url  = cfg.openrouter_app_url;
std::string& openrouter_app_title= cfg.openrouter_app_title;

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION

#include "llm_query.h"
#include "llm_image.h"
#include "web_page.h"

// forward-declared here; defined after parse_args
AIProvider provider_from_string(const std::string& s);

// ===========================================================================
// Settings — file persistence
// ===========================================================================

static std::string settings_path() { return "./rpgai_settings.json"; }

// Default CoderAI personality (identity + tone). User-overridable via
// cfg.coderPersona (settings / Preferenze). The fixed tool rules in
// build_coder_system_prompt are appended after this regardless — only the
// identity/voice is configurable, so a custom persona can't break tool use.
static const char* DEFAULT_CODER_PERSONA =
    "You are CoderAI, a coding assistant specialized in writing Lua adventure "
    "scripts for the RpgAi engine. You are precise, terse, and proactive: you read "
    "before you write, verify with syntax checks, and prefer minimal targeted edits "
    "over rewrites.";

static json config_to_json() {
    json j;
    j["base_path"]        = cfg.basePath;
    j["provider"]         = cfg.providerName;
    j["ollama_model"]     = cfg.ollama_model;
    j["ollama_url"]       = cfg.ollama_baseUrl;
    j["gemini_model"]     = cfg.gemini_model;
    j["gemini_key"]       = cfg.gemini_key;
    j["openai_model"]     = cfg.openai_model;
    j["openai_key"]       = cfg.openai_key;
    j["openai_url"]       = cfg.openai_baseUrl;
    j["claude_model"]     = cfg.claude_model;
    j["claude_key"]       = cfg.claude_key;
    j["openrouter_model"] = cfg.openrouter_model;
    j["openrouter_key"]   = cfg.openrouter_key;
    j["img_enabled"]      = cfg.imgEnabled;
    j["img_provider"]     = cfg.imgProvider;
    j["img_url"]          = cfg.imgUrl;
    j["img_key"]          = cfg.imgKey;
    j["img_t2i_model"]    = cfg.imgT2iModel;
    j["img_i2i_model"]    = cfg.imgI2iModel;
    j["img_i2i_provider"] = cfg.imgI2iProvider;
    j["img_i2i_url"]      = cfg.imgI2iUrl;
    j["img_i2i_key"]      = cfg.imgI2iKey;
    j["img_width"]        = cfg.imgWidth;
    j["img_height"]       = cfg.imgHeight;
    j["img_steps"]        = cfg.imgSteps;
    j["img_strength"]     = cfg.imgStrength;
    j["img_lora"]           = cfg.imgLora;
    j["img_lora_scale"]     = cfg.imgLoraScale;
    j["img_lora_model"]     = cfg.imgLoraModel;
    j["img_i2i_steps"]      = cfg.imgI2iSteps;
    j["img_guidance_scale"] = cfg.imgGuidanceScale;
    j["faceswap_url"]     = cfg.faceswapUrl;
    j["py_env_type"]      = cfg.pyEnvType;
    j["py_env_path"]      = cfg.pyEnvPath;
    j["qwen_locale_args"] = cfg.qwenLocaleArgs;
    j["tts_locale_args"]      = cfg.ttsLocaleArgs;
    j["tts_locale_env_type"]  = cfg.ttsLocaleEnvType;
    j["tts_locale_env_path"]  = cfg.ttsLocaleEnvPath;
    j["tts_url"]              = cfg.ttsUrl;
    j["tts_narrator_voice"]   = cfg.ttsNarratorVoice;
    j["max_history"]      = cfg.maxHistory;
    j["max_retries"]      = cfg.maxRetries;
    j["save_mode"]        = (cfg.saveMode == SaveMode::FULL) ? "full" : "last";
    j["save_path"]        = cfg.savePath;
    j["rag_file"]         = cfg.ragFile;
    j["rag_examples"]     = cfg.ragExamples;
    j["embed_provider"]   = cfg.embedProvider;
    j["embed_model"]      = cfg.embedModel;
    j["embed_url"]        = cfg.embedUrl;
    j["embed_key"]        = cfg.embedKey;
    j["lang_code"]        = cfg.langCode;
    j["gen_model"]        = cfg.genModel;
    j["gen_provider"]     = cfg.genProvider;
    j["agent_model"]      = cfg.agentModel;
    j["agent_provider"]   = cfg.agentProvider;
    j["ambient_model"]    = cfg.ambientModel;
    j["ambient_provider"] = cfg.ambientProvider;
    j["coder_provider"]        = cfg.coderProviderName;
    j["coder_model"]           = cfg.coderModel;
    j["coder_key"]             = cfg.coderKey;
    j["coder_vision_provider"] = cfg.coderVisionProviderName;
    j["coder_vision_model"]    = cfg.coderVisionModel;
    j["coder_persona"]         = cfg.coderPersona.empty() ? std::string(DEFAULT_CODER_PERSONA)
                                                          : cfg.coderPersona;
    return j;
}

static void apply_settings_json(const json& j) {
    std::lock_guard<std::mutex> cfg_lk(g_cfg_mutex);
    auto gs = [&](const char* k, std::string& f) { if (j.contains(k) && j[k].is_string())          f = j[k]; };
    auto gi = [&](const char* k, int& f)         { if (j.contains(k) && j[k].is_number_integer())   f = j[k]; };
    auto gf = [&](const char* k, float& f)       { if (j.contains(k) && j[k].is_number())           f = j[k].get<float>(); };
    auto gb = [&](const char* k, bool& f)        { if (j.contains(k) && j[k].is_boolean())          f = j[k]; };

    if (j.contains("base_path") && j["base_path"].is_string()) {
        cfg.basePath = j["base_path"];
        if (!cfg.basePath.empty() && cfg.basePath.back() != '/') cfg.basePath += '/';
    }
    std::string prov;
    gs("provider", prov);
    if (!prov.empty()) { cfg.providerName = prov; cfg.provider = provider_from_string(prov); }

    gs("ollama_model",    cfg.ollama_model);
    gs("ollama_url",      cfg.ollama_baseUrl);
    gs("gemini_model",    cfg.gemini_model);
    gs("gemini_key",      cfg.gemini_key);
    gs("openai_model",    cfg.openai_model);
    gs("openai_key",      cfg.openai_key);
    gs("openai_url",      cfg.openai_baseUrl);
    gs("claude_model",    cfg.claude_model);
    gs("claude_key",      cfg.claude_key);
    gs("openrouter_model",cfg.openrouter_model);
    gs("openrouter_key",  cfg.openrouter_key);
    gb("img_enabled",     cfg.imgEnabled);
    gs("img_provider",    cfg.imgProvider);
    gs("img_url",         cfg.imgUrl);
    gs("img_key",         cfg.imgKey);
    gs("img_t2i_model",   cfg.imgT2iModel);
    gs("img_i2i_model",   cfg.imgI2iModel);
    gs("img_i2i_provider",cfg.imgI2iProvider);
    gs("img_i2i_url",     cfg.imgI2iUrl);
    gs("img_i2i_key",     cfg.imgI2iKey);
    gi("img_width",       cfg.imgWidth);
    gi("img_height",      cfg.imgHeight);
    gi("img_steps",       cfg.imgSteps);
    gf("img_strength",    cfg.imgStrength);
    gs("img_lora",           cfg.imgLora);
    gf("img_lora_scale",     cfg.imgLoraScale);
    gs("img_lora_model",     cfg.imgLoraModel);
    gi("img_i2i_steps",      cfg.imgI2iSteps);
    gf("img_guidance_scale", cfg.imgGuidanceScale);
    gs("faceswap_url",    cfg.faceswapUrl);
    gs("py_env_type",     cfg.pyEnvType);
    gs("py_env_path",     cfg.pyEnvPath);
    gs("qwen_locale_args", cfg.qwenLocaleArgs);
    gs("tts_locale_args",      cfg.ttsLocaleArgs);
    gs("tts_locale_env_type",  cfg.ttsLocaleEnvType);
    gs("tts_locale_env_path",  cfg.ttsLocaleEnvPath);
    gs("tts_url",              cfg.ttsUrl);
    gs("tts_narrator_voice",   cfg.ttsNarratorVoice);
    gi("max_history",     cfg.maxHistory);
    gi("max_retries",     cfg.maxRetries);
    if (j.contains("save_mode") && j["save_mode"].is_string())
        cfg.saveMode = (j["save_mode"].get<std::string>() == "full") ? SaveMode::FULL : SaveMode::LAST;
    gs("save_path",       cfg.savePath);
    gs("rag_file",        cfg.ragFile);
    gi("rag_examples",    cfg.ragExamples);
    gs("embed_provider",  cfg.embedProvider);
    gs("embed_model",     cfg.embedModel);
    gs("embed_url",       cfg.embedUrl);
    gs("embed_key",       cfg.embedKey);
    gs("lang_code",       cfg.langCode);
    gs("gen_model",        cfg.genModel);
    gs("gen_provider",     cfg.genProvider);
    gs("agent_model",      cfg.agentModel);
    gs("agent_provider",   cfg.agentProvider);
    gs("ambient_model",    cfg.ambientModel);
    gs("ambient_provider", cfg.ambientProvider);
    std::string cprov;
    gs("coder_provider", cprov);
    if (j.contains("coder_provider")) {
        if (!cprov.empty()) {
            cfg.coderProviderName = cprov;
            cfg.coderProvider     = provider_from_string(cprov);
        } else {
            // empty = inherit main (keeps name and enum consistent at runtime)
            cfg.coderProviderName = cfg.providerName;
            cfg.coderProvider     = cfg.provider;
        }
    }
    gs("coder_model", cfg.coderModel);
    if (j.contains("coder_model") && cfg.coderModel.empty())
        cfg.coderModel = cfg.activeModel();
    gs("coder_key",   cfg.coderKey);
    std::string cvprov;
    gs("coder_vision_provider", cvprov);
    if (j.contains("coder_vision_provider") && !cvprov.empty()) {
        cfg.coderVisionProviderName = cvprov;
        cfg.coderVisionProvider     = provider_from_string(cvprov);
    }
    gs("coder_vision_model", cfg.coderVisionModel);
    // If the user saves the default persona unchanged, store it as-is (harmless);
    // build_coder_system_prompt falls back to DEFAULT when empty either way.
    gs("coder_persona", cfg.coderPersona);
}

static void sync_img_cfg_from_config() {
    img_cfg.provider     = img_provider_from_string(cfg.imgProvider);
    img_cfg.providerName = cfg.imgProvider;
    img_cfg.url          = cfg.imgUrl;
    img_cfg.key          = cfg.imgKey;
    img_cfg.t2i_model    = cfg.imgT2iModel;
    img_cfg.i2i_model    = cfg.imgI2iModel;
    img_cfg.width        = cfg.imgWidth;
    img_cfg.height       = cfg.imgHeight;
    img_cfg.steps        = cfg.imgSteps;
    img_cfg.strength     = cfg.imgStrength;
    if (!cfg.imgI2iProvider.empty()) {
        img_cfg.i2i_provider_name = cfg.imgI2iProvider;
        img_cfg.i2i_provider      = img_provider_from_string(cfg.imgI2iProvider);
    }
    img_cfg.i2i_url    = cfg.imgI2iUrl;
    img_cfg.i2i_key    = cfg.imgI2iKey;
    img_cfg.lora_name      = cfg.imgLora;
    img_cfg.lora_scale     = cfg.imgLoraScale;
    img_cfg.lora_model     = cfg.imgLoraModel;
    img_cfg.i2i_steps      = cfg.imgI2iSteps;
    img_cfg.guidance_scale = cfg.imgGuidanceScale;
}

static bool load_settings_file() {
    std::ifstream in(settings_path());
    if (!in.is_open()) return false;
    try { apply_settings_json(json::parse(in)); return true; }
    catch (...) { return false; }
}

static void save_settings_file() {
    std::ofstream out(settings_path());
    if (out.is_open()) out << config_to_json().dump(2);
}

// Quick HTTP reachability check (HEAD, 2s timeout)
static bool http_ping(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

// ===========================================================================
// RAG ENGINE
// ===========================================================================

struct RagEntry {
    std::string player_input;
    std::string narration;
    std::string location;
    std::string time_of_day;
    std::set<std::string> npcs;
    // optional embedding: computed lazily if --embed-model is specified
    std::vector<float> embedding;
};

static std::set<std::string> tokenize(const std::string& s) {
    std::set<std::string> tokens;
    std::string cur;
    for (unsigned char c : s) {
        if (std::isalpha(c)) cur += std::tolower(c);
        else { if (cur.size() > 3) tokens.insert(cur); cur.clear(); }
    }
    if (cur.size() > 3) tokens.insert(cur);
    return tokens;
}

static std::vector<RagEntry> load_rag_entries(const std::string& filename) {
    std::vector<RagEntry> entries;
    std::ifstream in(filename);
    if (!in.is_open()) { std::cerr << "[RAG] Cannot open: " << filename << "\n"; return entries; }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            std::string narration    = j.value("narration", "");
            std::string player_input = j.value("player_input", "");
            if (narration.size() < 40) continue;

            std::string location, time_of_day;
            std::set<std::string> npcs;
            if (j.contains("state_after") && j["state_after"].is_string()) {
                try {
                    auto sa = json::parse(j["state_after"].get<std::string>());
                    location    = sa.value("location", "");
                    time_of_day = sa.value("ora", sa.value("time", ""));
                    if (sa.contains("npc_locations") && sa["npc_locations"].is_object())
                        for (auto& [npc_id, loc] : sa["npc_locations"].items())
                            if (loc.is_string() && loc.get<std::string>() == location)
                                npcs.insert(npc_id);
                } catch (...) {}
            }
            entries.push_back({ player_input, narration, location, time_of_day, npcs, {} });
        } catch (...) {}
    }
    std::cerr << "[RAG] Loaded " << entries.size() << " entries from " << filename << "\n";
    return entries;
}

// Computes embeddings for all RAG entries if an embed model is configured.
// Called once at startup, after load_rag_entries.
static void compute_rag_embeddings(std::vector<RagEntry>& db) {
    if (cfg.embedModel.empty()) return;
    std::cerr << "[RAG] Computing embeddings for " << db.size() << " entries...\n";
    int ok = 0;
    for (auto& e : db) {
        e.embedding = get_embedding(e.player_input + " " + e.narration.substr(0, 200));
        if (!e.embedding.empty()) ++ok;
    }
    std::cerr << "[RAG] Embeddings computed: " << ok << "/" << db.size() << "\n";
}

static int score_entry(const RagEntry& entry,
                        const std::string& cur_location,
                        const std::string& cur_time,
                        const std::set<std::string>& cur_npcs,
                        const std::string& cur_input,
                        const std::vector<float>& cur_embedding) {

    // If both embeddings are available, use cosine similarity (0-100 scale)
    if (!entry.embedding.empty() && !cur_embedding.empty())
        return static_cast<int>(cosine_similarity(entry.embedding, cur_embedding) * 100);

    // Fallback: lexical/contextual scoring
    int score = 0;
    if (!entry.location.empty() && entry.location == cur_location) score += 40;
    if (!entry.time_of_day.empty() && entry.time_of_day == cur_time) score += 15;
    for (const auto& npc : cur_npcs)
        if (entry.npcs.count(npc)) score += 10;
    auto tok_cur = tokenize(cur_input);
    auto tok_input = tokenize(entry.player_input);   // tokenize entry once, not per word
    auto tok_narr  = tokenize(entry.narration);
    for (const auto& w : tok_cur) {
        if (tok_input.count(w)) score += 1;
        if (tok_narr.count(w))  score += 1;
    }
    return score;
}

static std::vector<RagEntry> select_rag_examples(
        const std::vector<RagEntry>& db,
        const std::string& cur_location,
        const std::string& cur_time,
        const std::set<std::string>& cur_npcs,
        const std::string& cur_input,
        const std::vector<float>& cur_embedding,
        int n) {

    if (db.empty() || n <= 0) return {};
    std::vector<std::pair<int,int>> scored;
    scored.reserve(db.size());
    for (int i = 0; i < (int)db.size(); ++i)
        scored.push_back({ score_entry(db[i], cur_location, cur_time,
                                       cur_npcs, cur_input, cur_embedding), i });
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    std::vector<RagEntry> result;
    for (int i = 0; i < (int)scored.size() && (int)result.size() < n; ++i)
        result.push_back(db[scored[i].second]);
    return result;
}

static std::string build_rag_block(const std::vector<RagEntry>& examples) {
    if (examples.empty()) return "";
    std::ostringstream out;
    out << "\n\n════════════════════════════════════════\n"
        << "NARRATIVE STYLE EXAMPLES\n"
        << "════════════════════════════════════════\n"
        << "The following examples show the expected linguistic register and style.\n"
        << "They are NOT part of this story. Use them only as a quality reference.\n\n";
    for (int i = 0; i < (int)examples.size(); ++i) {
        out << "[ Example " << (i + 1) << " ]\n";
        if (!examples[i].player_input.empty())
            out << "Action: \"" << examples[i].player_input << "\"\n";
        out << "Narration:\n" << examples[i].narration << "\n\n";
    }
    out << "════════════════════════════════════════\n"
        << "Maintain this quality level in your response.\n";
    return out.str();
}

static void parse_current_context(const std::string& status_json,
                                   std::string& out_location,
                                   std::string& out_time,
                                   std::set<std::string>& out_npcs) {
    out_location.clear(); out_time.clear(); out_npcs.clear();
    try {
        auto j = json::parse(status_json);
        if (j.contains("mondo") && j["mondo"].is_object()) {
            out_location = j["mondo"].value("location", "");
            out_time     = j["mondo"].value("ora", "");
        }
        if (j.contains("persone_presenti") && j["persone_presenti"].is_array())
            for (auto& p : j["persone_presenti"])
                if (p.contains("id") && p["id"].is_string())
                    out_npcs.insert(p["id"].get<std::string>());
    } catch (...) {}
}

// ===========================================================================
// SESSION PERSISTENCE
//
// SaveMode::LAST  → file always contains only the last turn.
//                   Implemented with atomic write: written to a temp file
//                   then renamed, avoiding corrupt files.
//
// SaveMode::FULL  → appends every turn (original behaviour).
//                   Useful for producing JSONL files for RAG.
// ===========================================================================

static std::string utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

static std::string local_session_id() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_loc{};
#ifdef _WIN32
    localtime_s(&tm_loc, &t);
#else
    localtime_r(&t, &tm_loc);
#endif
    char buf[12];
    std::strftime(buf, sizeof(buf), "%m%d_%H%M", &tm_loc);
    return buf;
}

static json build_turn_json(const std::string& player_input,
                             const std::string& llm_response,
                             const std::string& narration,
                             const std::string& state_after,
                             const std::vector<Message>& history) {
    json j;
    std::string ts    = utc_timestamp();
    if (cfg.sessionStart.empty()) cfg.sessionStart = ts;
    j["timestamp"]    = ts;
    j["session_start"] = cfg.sessionStart;
    j["script"]       = cfg.script;
    j["player_input"] = player_input;
    j["llm_response"] = llm_response;
    j["narration"]    = narration;
    j["state_after"]  = state_after;
    json j_hist = json::array();
    for (size_t i = 0; i < history.size(); i++) {
        const auto& m = history[i];
        json entry = {{"role", m.role}, {"content", m.content}, {"player_id", m.player_id}};
        // Stamp the current turn's assistant/gm message with ts; preserve stored timestamps for earlier messages.
        if (i == history.size() - 1 && m.role == "assistant" && m.player_id == "gm")
            entry["timestamp"] = ts;
        else if (!m.timestamp.empty())
            entry["timestamp"] = m.timestamp;
        j_hist.push_back(entry);
    }
    j["chat_history"] = j_hist;
    return j;
}

// Writes the turn to file according to the save mode.
// In LAST mode uses atomic rename: never leaves the file in an inconsistent state.
static void write_turn(const std::string& save_path,
                        std::ofstream& full_stream,
                        SaveMode mode,
                        const std::string& player_input,
                        const std::string& llm_response,
                        const std::string& narration,
                        const std::string& state_after,
                        const std::vector<Message>& history) {

    json j = build_turn_json(player_input, llm_response, narration, state_after, history);

    if (mode == SaveMode::FULL) {
        full_stream << j.dump() << "\n";
        full_stream.flush();
    } else {
        // LAST: scrivi su tmp, poi rinomina
        std::string tmp = save_path + ".tmp";
        std::ofstream f(tmp, std::ios::trunc);
        if (f.is_open()) {
            f << j.dump() << "\n";
            f.close();
            std::rename(tmp.c_str(), save_path.c_str());
        } else {
            std::cerr << "[SAVE] Cannot write to: " << tmp << "\n";
        }
    }
}

bool load_session_from_jsonl(const std::string& filename,
                              sol::state& lua,
                              std::vector<Message>& history) {
    std::ifstream in(filename);
    if (!in.is_open()) { std::cerr << "[ERROR] Cannot open: " << filename << "\n"; return false; }
    // Always reads the last non-empty line (works with both LAST and FULL save modes)
    std::string line, last_line;
    while (std::getline(in, line))
        if (!line.empty()) last_line = line;
    if (last_line.empty()) { std::cerr << "[ERROR] Save file is empty.\n"; return false; }
    try {
        auto j = json::parse(last_line);
        sol::protected_function restore_fn = lua["restore_state"];
        sol::protected_function_result pfr = restore_fn(j["state_after"].get<std::string>());
        if (!pfr.valid()) {
            sol::error err = pfr;
            std::cerr << "[ERROR] restore_state error: " << err.what() << "\n";
            return false;
        }
        // restore_state may return a result table or nothing (nil = success assumed).
        if (pfr.get_type() == sol::type::table) {
            sol::table res = pfr;
            sol::object ok_val  = res["success"];
            sol::object err_val = res["error"];
            bool ok = (ok_val.get_type() == sol::type::boolean) ? ok_val.as<bool>() : true;
            if (!ok) {
                std::string errmsg = (err_val.get_type() == sol::type::string)
                                     ? err_val.as<std::string>() : "";
                std::cerr << "[ERROR] Lua restore failed: " << errmsg << "\n";
                return false;
            }
        }
        history.clear();
        for (const auto& item : j["chat_history"])
            history.push_back({item.value("role",""), item.value("content",""), item.value("player_id",""), item.value("timestamp","")});
        std::cout << "[SYSTEM] Session loaded from " << filename << "\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Parse error: " << e.what() << "\n";
        return false;
    }
}

std::vector<Message> trim_history(const std::vector<Message>& history, int max_turns) {
    if (max_turns < 0 || (int)history.size() <= max_turns) return history;
    return std::vector<Message>(history.end() - max_turns, history.end());
}

// Returns [{player_input, narration}, ...] for chat replay on load.
// FULL mode: reads every line. LAST mode: reconstructs from chat_history.
static json load_turns_for_replay(const std::string& filename) {
    json turns = json::array();
    std::ifstream in(filename);
    if (!in.is_open()) return turns;
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(in, line))
        if (!line.empty()) lines.push_back(line);
    if (lines.empty()) return turns;

    if (lines.size() > 1) {
        for (const auto& l : lines) {
            try {
                auto j = json::parse(l);
                std::string narr = j.value("narration", j.value("narrative", ""));
                if (narr.empty()) continue;
                json t;
                t["player_input"] = j.value("player_input", "");
                t["narration"]    = narr;
                t["timestamp"]    = j.value("timestamp", "");
                turns.push_back(t);
            } catch (...) {}
        }
    } else {
        try {
            auto j = json::parse(lines[0]);
            if (j.contains("chat_history") && j["chat_history"].is_array()) {
                std::string pending_player;
                for (const auto& msg : j["chat_history"]) {
                    std::string role = msg.value("role", "");
                    std::string pid  = msg.value("player_id", "");
                    if (role == "user" && pid == "player") {
                        pending_player = msg.value("content", "");
                    } else if (role == "assistant" && pid == "gm") {
                        std::string narr;
                        std::string raw_content = msg.value("content", "");
                        try {
                            auto jc = json::parse(raw_content);
                            narr = jc.value("narration", jc.value("narrative", ""));
                        } catch (...) {
                            narr = raw_content; // plain text (tool-calling without JSON output)
                        }
                        if (!narr.empty()) {
                            json t;
                            t["player_input"] = pending_player;
                            t["narration"]    = narr;
                            t["timestamp"]    = msg.value("timestamp", "");
                            turns.push_back(t);
                        }
                        pending_player.clear();
                    }
                }
            }
            // Fallback for old saves without per-message timestamps:
            // interpolate linearly between session_start and the last turn's timestamp.
            {
                std::string t0_str = j.value("session_start", j.value("timestamp", ""));
                std::string t1_str = j.value("timestamp", "");
                bool needs_interp = !turns.empty() && turns[0].value("timestamp","").empty();
                if (needs_interp && !t0_str.empty() && !t1_str.empty()) {
                    long long t0 = scene_cache::ts_to_utc_seconds(t0_str);
                    long long t1 = scene_cache::ts_to_utc_seconds(t1_str);
                    size_t n = turns.size();
                    for (size_t i = 0; i < n; i++) {
                        long long interp = (n == 1) ? t1 : t0 + (t1 - t0) * (long long)i / (long long)(n - 1);
                        std::tm tm_u{};
                        std::time_t tt = (std::time_t)interp;
#ifdef _WIN32
                        gmtime_s(&tm_u, &tt);
#else
                        gmtime_r(&tt, &tm_u);
#endif
                        char buf[32];
                        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_u);
                        turns[i]["timestamp"] = buf;
                    }
                }
            }
            // Fallback: top-level fields only
            if (turns.empty()) {
                std::string narr = j.value("narration", "");
                if (!narr.empty()) {
                    json t;
                    t["player_input"] = j.value("player_input", "");
                    t["narration"]    = narr;
                    turns.push_back(t);
                }
            }
        } catch (...) {}
    }
    return turns;
}

// Parse timestamp to UTC seconds since epoch.
// "YYYY-MM-DDTHH:MM:SSZ" → parsed as UTC via timegm.
// "YYYYMMDD_HHMMSS"      → parsed as LOCAL time via mktime.
// Returns -1 on failure.
static long long ts_to_utc_seconds(const std::string& ts) {
    if (ts.empty()) return -1;
    std::tm t = {};
    bool is_utc = false;
    try {
        if (ts.size() >= 19 && ts[4] == '-') {       // ISO8601 "YYYY-MM-DDTHH:MM:SSZ"
            t.tm_year = std::stoi(ts.substr(0, 4)) - 1900;
            t.tm_mon  = std::stoi(ts.substr(5, 2)) - 1;
            t.tm_mday = std::stoi(ts.substr(8, 2));
            t.tm_hour = std::stoi(ts.substr(11, 2));
            t.tm_min  = std::stoi(ts.substr(14, 2));
            t.tm_sec  = std::stoi(ts.substr(17, 2));
            is_utc = true;
        } else if (ts.size() >= 15 && ts[8] == '_') { // "YYYYMMDD_HHMMSS" (local)
            t.tm_year = std::stoi(ts.substr(0, 4)) - 1900;
            t.tm_mon  = std::stoi(ts.substr(4, 2)) - 1;
            t.tm_mday = std::stoi(ts.substr(6, 2));
            t.tm_hour = std::stoi(ts.substr(9, 2));
            t.tm_min  = std::stoi(ts.substr(11, 2));
            t.tm_sec  = std::stoi(ts.substr(13, 2));
        } else { return -1; }
    } catch (...) { return -1; }
    t.tm_isdst = -1;
#ifdef _WIN32
    std::time_t tt = is_utc ? _mkgmtime(&t) : std::mktime(&t);
#else
    std::time_t tt = is_utc ? timegm(&t) : std::mktime(&t);
#endif
    if (tt == (std::time_t)-1) return -1;
    return static_cast<long long>(tt);
}

// Returns cached scene images for script_name, optionally bounded by session timestamps.
// session_start / session_end: ISO8601 UTC; empty = no bound.
// Tolerance of ±3600 s (1 hour) guards against DST and sub-minute clock drift.
static json get_cached_scene_images(const std::string& script_name,
                                     const std::string& session_start = "",
                                     const std::string& session_end   = "") {
    json result = json::array();
    if (cfg.basePath.empty()) return result;
    std::string db_file = cfg.basePath + "images/scene_cache/cache_db.json";
    if (!std::filesystem::exists(db_file)) return result;
    try {
        std::ifstream f(db_file);
        json db; f >> db;
        if (!db.is_array()) return result;

        long long start_sec = session_start.empty() ? -1 : ts_to_utc_seconds(session_start);
        long long end_sec   = session_end.empty()   ? -1 : ts_to_utc_seconds(session_end);
        constexpr long long TOL = 3600; // 1 hour — covers DST and clock drift

        for (const auto& entry : db) {
            if (entry.value("script", "") != script_name) continue;
            std::string img_path = entry.value("image_path", "");
            if (img_path.empty() || !std::filesystem::exists(img_path)) continue;

            // Prefer utc_at (new entries) over generated_at (old entries, local tz)
            std::string utc_at_str  = entry.value("utc_at", "");
            std::string gen_at_str  = entry.value("generated_at", "");
            long long   img_sec     = utc_at_str.empty()
                                        ? ts_to_utc_seconds(gen_at_str)    // local→UTC
                                        : ts_to_utc_seconds(utc_at_str);   // already UTC ISO

            // Time-range filter (only when bounds are known)
            if (start_sec >= 0 || end_sec >= 0) {
                if (img_sec < 0) continue; // unparseable — skip
                if (start_sec >= 0 && img_sec < start_sec - TOL) continue;
                if (end_sec   >= 0 && img_sec > end_sec   + TOL) continue;
            }

            // utc_at sent to JS: use explicit field if present, else derive from epoch
            std::string utc_at_out = utc_at_str.empty()
                                       ? scene_cache::epoch_to_utc_iso(img_sec)
                                       : utc_at_str;

            json item;
            item["file"]         = std::filesystem::path(img_path).filename().string();
            item["generated_at"] = gen_at_str;
            item["utc_at"]       = utc_at_out;
            item["assets"]       = entry.value("assets", json::array());
            item["prompt"]       = entry.value("prompt", "");
            item["cache_key"]    = entry.value("cache_key", "");
            result.push_back(item);
        }
    } catch (...) {}
    return result;
}

// ---------------------------------------------------------------------------
// Language instruction helpers
// ---------------------------------------------------------------------------

// Reads lang.txt and returns the instruction string for the given code.
// Returns empty string if the code is not found or the file cannot be opened.
static std::string load_lang_instruction(const std::string& code,
                                          const std::string& lang_file = "lang.txt") {
    std::ifstream f(lang_file);
    if (!f.is_open()) return {};
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        // trim whitespace from key
        while (!key.empty() && std::isspace((unsigned char)key.back()))  key.pop_back();
        while (!key.empty() && std::isspace((unsigned char)key.front())) key.erase(key.begin());
        if (key == code) {
            std::string val = line.substr(colon + 1);
            while (!val.empty() && std::isspace((unsigned char)val.front())) val.erase(val.begin());
            return val;
        }
    }
    return {};
}

// Appends the language instruction to a system prompt when one is configured.
static std::string with_lang(const std::string& sys) {
    if (cfg.langInstruction.empty()) return sys;
    return sys + "\n\n" + cfg.langInstruction;
}

// ===========================================================================
// LLM dispatch
// ===========================================================================

std::string query_llm(AIProvider provider,
                       const std::string& sys_prompt,
                       const std::vector<Message>& history,
                       const std::string& user_prompt,
                       const std::string& json_schema,
                       const std::string& model) {

    //std::string san_sys_prompt=getSanitized(sys_prompt);
    //std::string san_user_prompt=getSanitized(user_prompt); // sanitization disabled

//std::cout << san_user_prompt;

    switch (provider) {
        case AIProvider::GEMINI:      return gemini_query    (sys_prompt, history, user_prompt, json_schema, model);
        case AIProvider::OPENAI:      return openai_query    (sys_prompt, history, user_prompt, json_schema, model);
        case AIProvider::CLAUDE:      return claude_query    (sys_prompt, history, user_prompt, json_schema, model);
        case AIProvider::OPENROUTER:  return openrouter_query(sys_prompt, history, user_prompt, json_schema, model);
        default:                      return ollama_query    (sys_prompt, history, user_prompt, json_schema, model);
    }
}

// ===========================================================================
// TOOL CALLING SUBSYSTEM
// ===========================================================================

// Script-defined tools for the current session.
// Populated by load_script_tools(); cleared on every script reload.
static std::vector<ToolDef>                              active_tools;
static std::map<std::string, sol::protected_function>    active_tool_fns;
static bool                                              script_has_tools = false;

// Dispatches one LLM call with tool support.
// Falls back to standard schema mode for providers without tool calling (Gemini, Ollama).
static std::string query_llm_with_tools(
        AIProvider provider,
        const std::string& sys_prompt,
        const std::vector<Message>& history,
        const std::string& user_prompt,
        const std::string& json_schema,
        const std::vector<ToolDef>& tools,
        std::function<std::string(const std::string&, const std::string&)> executor,
        const std::string& model,
        int max_iter = 8)
{
    g_llm_label = "narrator";   // token accounting: the main game turn
    switch (provider) {
        case AIProvider::OPENAI:
            return openai_tool_loop(cfg.openai_baseUrl, cfg.openai_key, model,
                sys_prompt, history, user_prompt, json_schema, tools, executor, max_iter);
        case AIProvider::OPENROUTER:
            return openai_tool_loop(cfg.openrouter_baseUrl, cfg.openrouter_key, model,
                sys_prompt, history, user_prompt, json_schema, tools, executor, max_iter);
        case AIProvider::CLAUDE:
            return claude_tool_loop(sys_prompt, history, user_prompt, model,
                tools, executor, max_iter);
        case AIProvider::OLLAMA:
            return openai_tool_loop(cfg.ollama_baseUrl + "/v1/chat/completions", "", model,
                sys_prompt, history, user_prompt, json_schema, tools, executor, max_iter);
        default:
            std::cerr << "[TOOLS] Provider doesn't support tool calling; falling back to schema mode\n";
            return query_llm(provider, sys_prompt, history, user_prompt, json_schema, model);
    }
}

// Execute a named tool by calling its Lua function.
// Called synchronously from within query_llm_with_tools — lua_mutex must already be held.
static std::string execute_tool(const std::string& name, const std::string& args_json) {
    auto it = active_tool_fns.find(name);
    if (it == active_tool_fns.end())
        return json{{"error", "unknown tool: " + name}}.dump();
    try {
        sol::protected_function_result r = it->second(args_json);
        if (!r.valid()) {
            sol::error err = r;
            return json{{"error", std::string(err.what())}}.dump();
        }
        auto ret = r.get<sol::optional<std::string>>();
        return ret.value_or(json{{"result","ok"}}.dump());
    } catch (const std::exception& e) {
        return json{{"error", std::string(e.what())}}.dump();
    }
}

// Optional script globals that must NOT bleed across a script swap/load/reload.
// A newly loaded script that doesn't redefine one would otherwise inherit the
// previous adventure's function (e.g. villa's get_character_questions firing for
// vn_demo, or stale before_ai_turn hooks). Required globals are always redefined
// by every script, so they don't need clearing. Call clear_optional_script_globals
// before EVERY lua.script_file() that swaps the active adventure.
static const char* OPTIONAL_SCRIPT_GLOBALS[] = {
    "get_commands", "get_scene_images", "get_asset_path", "get_asset_prompt",
    "get_tools", "get_pin_key", "get_image_style",
    "get_character_questions", "generate_arrival",
    "get_vn_scene", "get_visual_world", "get_modes",
    "get_npc_list", "get_location_info", "get_npc_info",
    "before_ai_turn", "after_ai_turn",
    "vn_reload_catalog", "debug_npc_action",
    "get_npc_description", "get_adventure_style",
    "editor_npcs", "editor_npc_get", "editor_npc_patch", "editor_npc_create",
    "editor_locations", "editor_location_get", "editor_location_patch",
    "editor_location_create", "scaffold_vn",
    "editor_objects", "editor_object_get", "editor_object_patch", "editor_object_create",
};
static void clear_optional_script_globals(sol::state& lua) {
    for (auto* fn : OPTIONAL_SCRIPT_GLOBALS) lua[fn] = sol::lua_nil;
    // Tolerant boundary: LLM-written adventures often call quick.define{...}
    // without the require line. Predefine the global so the omission is
    // harmless (pcall: engines without lib/quickstart just skip).
    lua.script("pcall(function() quick = require('lib/quickstart') end)");
}

// Per-adventure VN catalog: <ASSET_ROOT>/catalog/<stem>_vn.json.
// Falls back to vn_scene.json when g_active_script_stem is empty (console mode,
// pre-init). The Lua scaffold_vn() function receives this path so all routes
// (GET/POST /api/vn/catalog, /api/scaffold, maybe_autoscaffold_vn) share it.
static std::string vn_catalog_file() {
    std::error_code ec;
    std::filesystem::path root = cfg.assetRoot.empty()
        ? std::filesystem::current_path(ec)
        : std::filesystem::absolute(cfg.assetRoot, ec);
    std::string fname = g_active_script_stem.empty()
        ? "vn_scene.json"
        : (g_active_script_stem + "_vn.json");
    return (root / "catalog" / fname).string();
}

// Extract stem from a script filename: "path/to/villa_vacanze.lua" → "villa_vacanze"
static std::string script_stem(const std::string& name) {
    std::string s = name;
    auto sl = s.rfind('/');  if (sl != std::string::npos) s = s.substr(sl + 1);
    auto bs = s.rfind('\\'); if (bs != std::string::npos) s = s.substr(bs + 1);
    if (s.size() > 4 && s.substr(s.size() - 4) == ".lua") s = s.substr(0, s.size() - 4);
    return s;
}

// Persistence for the one-click VN conversion: once a VN catalog exists for an
// adventure, every later load auto-installs get_vn_scene so it stays VN-capable
// without editing the .lua. Only fires when the script isn't already VN-capable,
// the adventure uses adventure.lua (scaffold_vn global present), and the catalog
// file exists (i.e. it was converted before). Call AFTER set_config has run
// (i.e. after /api/init and /api/load), since scaffold_vn is installed there.
static void maybe_autoscaffold_vn(sol::state& lua) {
    if (lua["get_vn_scene"].valid()) return;
    sol::object sf = lua["scaffold_vn"];
    if (!sf.valid() || sf.get_type() != sol::type::function) return;
    std::error_code ec;
    std::string path = vn_catalog_file();
    if (!std::filesystem::exists(path, ec)) return;
    sol::protected_function fn = sf;
    auto r = fn(path);
    (void)r;
}

// Load tools from the current Lua script. Call after every script_file() load.
// Idempotent: clears previous tools before populating.
static void load_script_tools(sol::state& lua) {
    active_tools.clear();
    active_tool_fns.clear();
    script_has_tools = false;

    sol::protected_function gtools = lua["get_tools"];
    if (!gtools.valid()) return;

    sol::protected_function_result r = gtools();
    if (!r.valid()) {
        sol::error err = r;
        std::cerr << "[TOOLS] get_tools() error: " << err.what() << "\n";
        return;
    }

    sol::table tbl = r;
    for (auto& [k, v] : tbl) {
        if (v.get_type() != sol::type::table) continue;
        sol::table entry = v.as<sol::table>();

        ToolDef td;
        td.name          = entry.get_or<std::string>("name", "");
        td.description   = entry.get_or<std::string>("description", "");
        td.params_schema = entry.get_or<std::string>("params", "{}");
        if (td.name.empty()) continue;

        sol::protected_function fn = entry["fn"];
        if (!fn.valid()) continue;

        active_tools.push_back(td);
        active_tool_fns[td.name] = fn;
    }

    script_has_tools = !active_tools.empty();
    if (script_has_tools)
        print_system("Tools loaded: " + std::to_string(active_tools.size()));
}

AIProvider provider_from_string(const std::string& s) {
    if (s == "gemini")     return AIProvider::GEMINI;
    if (s == "openai")     return AIProvider::OPENAI;
    if (s == "claude")     return AIProvider::CLAUDE;
    if (s == "openrouter") return AIProvider::OPENROUTER;
    return AIProvider::OLLAMA;
}

// ===========================================================================
// CLI
// ===========================================================================

void print_help(const char* progName) {
    auto opt = [](const std::string& flag, const std::string& desc, const std::string& def = "") {
        std::string line = "  " + flag;
        if (line.size() < 36) line += std::string(36 - line.size(), ' ');
        line += desc;
        if (!def.empty()) line += " (default: " + def + ")";
        std::cout << line << "\n";
    };
    std::cout
        << "\nRpgAi — LLM-powered RPG Engine\n"
        << "================================\n\n"
        << "USAGE\n  " << progName << " [OPTIONS]\n\n"
        << "GENERAL OPTIONS\n";
    opt("--provider <n>",         "ollama|gemini|openai|claude|openrouter", "ollama");
    opt("--script <file>",        "Lua game script",                         cfg.script);
    opt("--path <dir>",           "Base directory for Lua scripts",          cfg.basePath);
    opt("--load <file>",          "Restore session from file",               "");
    opt("--save <file>",          "File di output",                          cfg.saveFile);
    opt("--save-mode <m>",        "last (sovrascrive) | full (appende)",     "last");
    opt("--save-path <dir>",      "Directory for save files",                "(cwd)");
    opt("--rag <file>",           "JSONL per stile narrativo",               "");
    opt("--rag-examples <n>",     "RAG examples per turn",                   std::to_string(cfg.ragExamples));
    opt("--max-history <n>",      "Max messaggi nel contesto LLM",           std::to_string(cfg.maxHistory));
    opt("--max-retries <n>",      "Max retries per turn",                    std::to_string(cfg.maxRetries));
    opt("--lang <code>",          "Language code (e.g. it, fr, de). Injects",  "");
    opt("",                       "  a language instruction into every LLM call", "");
    opt("--lang-file <file>",     "Path to language file (default: lang.txt)", "lang.txt");
    opt("--web",                  "Enable web mode (REST server + open browser)", "");
    opt("--rest",                 "REST server only (no browser; for rpgai-gui)", "");
    opt("--port <n>",             "Web server port",                         "8080");
    opt("--llm-timeout <s>",      "Per-request LLM timeout (s)",             "120");
    opt("--max-output-tokens <n>","Cap completion tokens per LLM call (0=provider default)", "1024");
    opt("--debug-gui",            "Enable GUI debug routes (possess NPC)",   "");
    opt("--asset-root <path>",    "Base dir for asset/ & catalog/ (abs paths)", "cwd");
    print_image_help();
    std::cout << "\nEMBEDDING OPTIONS\n";
    opt("--embed-provider <n>",   "ollama|openai (default: segue --provider)", "");
    opt("--embed-model <n>",      "Modello embedding (attiva calcolo vettori)", "");
    opt("--embed-url <url>",      "Override base URL per embedding",         "");
    opt("--embed-key <key>",      "API key per endpoint embedding",          "");
    std::cout << "\nOLLAMA\n";
    opt("--model <n>",            "Model name",  cfg.ollama_model);
    opt("--url <url>",            "Server URL",  cfg.ollama_baseUrl);
    std::cout << "\nGEMINI\n";
    opt("--g-key <key>",          "API key", "");
    opt("--g-model <n>",          "Model",   cfg.gemini_model);
    std::cout << "\nOPENAI\n";
    opt("--oai-key <key>",        "API key", "");
    opt("--oai-model <n>",        "Model",   cfg.openai_model);
    std::cout << "\nCLAUDE\n";
    opt("--claude-key <key>",     "API key", "");
    opt("--claude-model <n>",     "Model",   cfg.claude_model);
    std::cout << "\nOPENROUTER\n";
    opt("--or-key <key>",         "API key", "");
    opt("--or-model <n>",         "Model",   cfg.openrouter_model);
    std::cout << "\nCODERAI (script writing assistant)\n";
    opt("--coder-provider <n>",   "Provider for CoderAI (default: inherits --provider)", "");
    opt("--coder-model <n>",      "Model for CoderAI  (default: inherits --model)",      "");
    opt("--coder-key <key>",      "API key override for CoderAI",                        "");
    opt("--coder-path <dir>",     "Knowledge base dir  (default: <--path>/coder_knowledge/)", "");
    opt("--coder-persona <text>", "CoderAI personality/identity (default: built-in; editable in Preferenze)", "");
    std::cout << "\nMODEL TIERS (optional per-role defaults, read by Lua libs via get_tier)\n";
    opt("--gen-model <m>",        "Entity generation model (persona/world)", "main model");
    opt("--gen-provider <p>",     "Entity generation provider", "main provider");
    opt("--agent-model <m>",      "NPC agent model (think_as_npc)", "main model");
    opt("--agent-provider <p>",   "NPC agent provider", "main provider");
    opt("--ambient-model <m>",    "Ambient NPC-NPC events model", "main model");
    opt("--ambient-provider <p>", "Ambient NPC-NPC events provider", "main provider");
    std::cout << "\nIN-GAME BUILT-IN COMMANDS\n"
              << "  /save                 Save session\n"
              << "  /status               Stampa stato grezzo JSON\n"
              << "  /quit  /q             Esci\n"
              << "  /help                 Questo help\n"
              << "  /summary [N]          Summarise and compress history\n"
              << "                        N = recent turns to keep after summary (default 2)\n"
              << "  /fix <instruction>    Rewrite the last scene with a correction\n"
              << "                        E.g.: /fix elena was not there\n"
              << "  /observe [subject]    Detailed description without advancing time\n"
              << "                        E.g.: /observe  or  /observe the table\n"
              << "  (other /xxx commands are delegated to the Lua script)\n\n";
}

bool parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "[ERROR] Missing value for " << arg << "\n"; return {}; }
            return argv[++i];
        };
        auto next_int = [&](int def) -> int {
            std::string v = next();
            try { return std::stoi(v); }
            catch (...) { std::cerr << "[ERROR] Invalid integer for " << arg << ": '" << v << "'\n"; return def; }
        };
        auto next_float = [&](float def) -> float {
            std::string v = next();
            try { return std::stof(v); }
            catch (...) { std::cerr << "[ERROR] Invalid number for " << arg << ": '" << v << "'\n"; return def; }
        };
        if      (arg == "--help" || arg == "-h") { print_help(argv[0]); std::exit(0); }
        else if (arg == "--provider")      { cfg.providerName = next(); cfg.provider = provider_from_string(cfg.providerName); }
        else if (arg == "--script")        { cfg.script      = next(); }
        else if (arg == "--path")          { cfg.basePath    = next(); if (cfg.basePath.empty()) cfg.basePath = "./"; else if (cfg.basePath.back() != '/') cfg.basePath += '/'; }
        else if (arg == "--load")          { cfg.loadFile    = next(); }
        else if (arg == "--save")          { cfg.saveFile    = next(); }
        else if (arg == "--save-mode")     { auto m = next(); cfg.saveMode = (m == "full") ? SaveMode::FULL : SaveMode::LAST; }
        else if (arg == "--save-path")     { cfg.savePath = next(); if (!cfg.savePath.empty() && cfg.savePath.back() != '/') cfg.savePath += '/'; }
        else if (arg == "--img-provider")  { cfg.imgProvider = next(); cfg.imgEnabled = true; }
        else if (arg == "--img-url")       { cfg.imgUrl      = next(); }
        else if (arg == "--img-key")       { cfg.imgKey      = next(); }
        else if (arg == "--img-t2i-model") { cfg.imgT2iModel = next(); }
        else if (arg == "--img-i2i-model") { cfg.imgI2iModel = next(); }
        else if (arg == "--img-width")     { cfg.imgWidth    = next_int(cfg.imgWidth); }
        else if (arg == "--img-height")    { cfg.imgHeight   = next_int(cfg.imgHeight); }
        else if (arg == "--img-steps")     { cfg.imgSteps    = next_int(cfg.imgSteps); }
        else if (arg == "--img-strength")  { cfg.imgStrength  = next_float(cfg.imgStrength); }
        else if (arg == "--img-i2i-provider") { cfg.imgI2iProvider = next(); }
        else if (arg == "--img-i2i-url")   { cfg.imgI2iUrl    = next(); }
        else if (arg == "--tts-url")       { cfg.ttsUrl       = next(); }
        else if (arg == "--img-i2i-key")   { cfg.imgI2iKey    = next(); }
        else if (arg == "--img-lora")       { cfg.imgLora       = next(); }
        else if (arg == "--img-lora-scale") { cfg.imgLoraScale   = next_float(cfg.imgLoraScale); }
        else if (arg == "--i2i-model-lora")       { cfg.imgLoraModel      = next(); }
        else if (arg == "--img-i2i-steps")        { cfg.imgI2iSteps       = next_int(cfg.imgI2iSteps); }
        else if (arg == "--img-guidance-scale")   { cfg.imgGuidanceScale  = next_float(cfg.imgGuidanceScale); }
        else if (arg == "--faceswap-url")  { cfg.faceswapUrl   = next(); }
        else if (arg == "--web")           { cfg.webMode     = true; cfg.openBrowser = true; }
        else if (arg == "--rest")          { cfg.webMode     = true; }
        else if (arg == "--port")          { cfg.webPort     = std::stoi(next()); }
        else if (arg == "--llm-timeout")   { g_llm_timeout_s = std::stol(next()); }
        else if (arg == "--max-output-tokens") { g_llm_max_tokens = std::stol(next()); }
        else if (arg == "--debug-gui")     { cfg.debugGui    = true; }
        else if (arg == "--asset-root")    { cfg.assetRoot   = next(); }
        else if (arg == "--rag")           { cfg.ragFile     = next(); }
        else if (arg == "--rag-examples")  { cfg.ragExamples = next_int(cfg.ragExamples); }
        else if (arg == "--max-history")   { cfg.maxHistory  = next_int(cfg.maxHistory); }
        else if (arg == "--max-retries")   { cfg.maxRetries  = next_int(cfg.maxRetries); }
        else if (arg == "--lang")          { cfg.langCode    = next(); }
        else if (arg == "--lang-file")     { cfg.langFile    = next(); }
        else if (arg == "--embed-provider"){ cfg.embedProvider = next(); }
        else if (arg == "--embed-model")   { cfg.embedModel    = next(); }
        else if (arg == "--embed-url")     { cfg.embedUrl      = next(); }
        else if (arg == "--embed-key")     { cfg.embedKey      = next(); }
        else if (arg == "--model")         { cfg.ollama_model   = next(); }
        else if (arg == "--url")           { cfg.ollama_baseUrl = next(); }
        else if (arg == "--g-key")         { cfg.gemini_key    = next(); }
        else if (arg == "--g-model")       { cfg.gemini_model  = next(); }
        else if (arg == "--oai-key")       { cfg.openai_key    = next(); }
        else if (arg == "--oai-model")     { cfg.openai_model  = next(); }
        else if (arg == "--claude-key")    { cfg.claude_key    = next(); }
        else if (arg == "--claude-model")  { cfg.claude_model  = next(); }
        else if (arg == "--or-key")        { cfg.openrouter_key   = next(); }
        else if (arg == "--or-model")      { cfg.openrouter_model = next(); }
        else if (arg == "--coder-provider"){ cfg.coderProviderName = next(); cfg.coderProvider = provider_from_string(cfg.coderProviderName); }
        else if (arg == "--coder-model")   { cfg.coderModel  = next(); }
        else if (arg == "--coder-key")     { cfg.coderKey    = next(); }
        else if (arg == "--coder-vision-provider") { cfg.coderVisionProviderName = next(); cfg.coderVisionProvider = provider_from_string(cfg.coderVisionProviderName); }
        else if (arg == "--coder-vision-model")    { cfg.coderVisionModel = next(); }
        else if (arg == "--coder-persona") { cfg.coderPersona = next(); }
        else if (arg == "--gen-model")        { cfg.genModel        = next(); }
        else if (arg == "--gen-provider")     { cfg.genProvider     = next(); }
        else if (arg == "--agent-model")      { cfg.agentModel      = next(); }
        else if (arg == "--agent-provider")   { cfg.agentProvider   = next(); }
        else if (arg == "--ambient-model")    { cfg.ambientModel    = next(); }
        else if (arg == "--ambient-provider") { cfg.ambientProvider = next(); }
        else if (arg == "--coder-path")    { cfg.coderKnowledgePath = next(); if (!cfg.coderKnowledgePath.empty() && cfg.coderKnowledgePath.back() != '/') cfg.coderKnowledgePath += '/'; }
        else if (arg == "--search-provider") { cfg.searchProvider = next(); }
        else if (arg == "--search-key")      { cfg.searchKey = next(); }
        else if (arg == "--pixabay-key")     { cfg.pixabayKey = next(); }
        else { std::cerr << "[ERROR] Unknown argument: " << arg << "\n"; print_help(argv[0]); return false; }
    }
    return true;
}

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char* argv[]) {
    init_ansi();

    load_settings_file();              // base: file-saved settings
    if (!parse_args(argc, argv)) return 1;  // CLI overrides file

    std::string key_error = cfg.validate();
    if (!key_error.empty()) { print_error(key_error); return 1; }

    std::string active_model = cfg.activeModel();
    std::string save_mode_str = (cfg.saveMode == SaveMode::FULL) ? "full" : "last";

    // Prepend savePath to saveFile so console mode writes to the right directory
    if (!cfg.savePath.empty())
        cfg.saveFile = cfg.savePath + cfg.saveFile;

    // Load language instruction if --lang was specified
    if (!cfg.langCode.empty()) {
        cfg.langInstruction = load_lang_instruction(cfg.langCode, cfg.langFile);
        if (cfg.langInstruction.empty())
            print_warning("Language code '" + cfg.langCode +
                          "' not found in " + cfg.langFile + " — no injection applied.");
    }

    // CoderAI defaults — inherit main provider/model if not overridden
    if (cfg.coderProviderName.empty()) {
        cfg.coderProvider     = cfg.provider;
        cfg.coderProviderName = cfg.providerName;
    }
    if (cfg.coderModel.empty())
        cfg.coderModel = cfg.activeModel();
    // Vision model — defaults to coder model/provider if not explicitly set
    if (cfg.coderVisionProviderName.empty()) {
        cfg.coderVisionProvider     = cfg.coderProvider;
        cfg.coderVisionProviderName = cfg.coderProviderName;
    }
    if (cfg.coderVisionModel.empty())
        cfg.coderVisionModel = cfg.coderModel;

    print_system("RpgAi Engine starting...");
    print_system("Provider:   " + cfg.providerName + " | Model: " + active_model);
    print_system("CoderAI:    " + cfg.coderProviderName + " | Model: " + cfg.coderModel);
    if (cfg.coderVisionModel != cfg.coderModel)
        print_system("CoderVision:" + cfg.coderVisionProviderName + " | Model: " + cfg.coderVisionModel);
    print_system("Script:     " + cfg.basePath + cfg.script);
    print_system("Save mode:  " + save_mode_str + " → " + cfg.saveFile);
    if (!cfg.langInstruction.empty())
        print_system("Language:   [" + cfg.langCode + "] " + cfg.langInstruction);

    // Configura image generation
    if (cfg.imgEnabled) {
        img_cfg.provider     = img_provider_from_string(cfg.imgProvider);
        img_cfg.providerName = cfg.imgProvider;
        img_cfg.url          = cfg.imgUrl;
        img_cfg.key          = cfg.imgKey;
        img_cfg.t2i_model    = cfg.imgT2iModel;
        img_cfg.i2i_model    = cfg.imgI2iModel;
        img_cfg.width        = cfg.imgWidth;
        img_cfg.height       = cfg.imgHeight;
        img_cfg.steps        = cfg.imgSteps;
        img_cfg.strength          = cfg.imgStrength;
        if (!cfg.imgI2iProvider.empty()) {
            img_cfg.i2i_provider_name = cfg.imgI2iProvider;
            img_cfg.i2i_provider      = img_provider_from_string(cfg.imgI2iProvider);
        }
        img_cfg.i2i_url           = cfg.imgI2iUrl;
        img_cfg.i2i_key           = cfg.imgI2iKey;
        img_cfg.lora_name         = cfg.imgLora;
        img_cfg.lora_scale        = cfg.imgLoraScale;
        img_cfg.lora_model        = cfg.imgLoraModel;
        img_cfg.i2i_steps         = cfg.imgI2iSteps;
        img_cfg.guidance_scale    = cfg.imgGuidanceScale;
        print_system("Image t2i:  provider=" + cfg.imgProvider + " url=" + cfg.imgUrl);
        if (!cfg.imgI2iProvider.empty())
            print_system("Image i2i:  provider=" + cfg.imgI2iProvider
                         + (cfg.imgI2iUrl.empty() ? "" : " url=" + cfg.imgI2iUrl));
    }

    // --- RAG ---
    std::vector<RagEntry> rag_db;
    bool rag_active = !cfg.ragFile.empty();
    if (rag_active) {
        print_system("RAG: Loading from " + cfg.ragFile);
        rag_db = load_rag_entries(cfg.ragFile);
        if (rag_db.empty()) {
            print_warning("RAG: No usable entries. RAG disabled.");
            rag_active = false;
        } else {
            if (!cfg.embedModel.empty()) {
                print_system("RAG: Embedding model: " + cfg.embedModel);
                compute_rag_embeddings(rag_db);
            }
            print_system("RAG: " + std::to_string(rag_db.size()) + " examples, "
                         + std::to_string(cfg.ragExamples) + " per turn.");
        }
    }
    if (!cfg.embedModel.empty())
        print_system("EMBED: Model: " + cfg.embedModel + " (" + effective_embed_provider() + ")");
    std::cout << "\n";

    std::signal(SIGINT, [](int) { g_shutdown = true; });

    curl_global_init(CURL_GLOBAL_ALL);

    // --- Lua setup ---
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::string,
                       sol::lib::table, sol::lib::math, sol::lib::os, sol::lib::io,
                       sol::lib::debug);
    const std::string lua_default_path = lua["package"]["path"].get<std::string>();
    auto update_lua_path = [&]() {
        lua["package"]["path"] = cfg.basePath + "?.lua;" + cfg.basePath + "lib/?.lua;" + lua_default_path;
    };
    update_lua_path();

    // Expose current language code to Lua scripts.
    // Scripts can read LANG directly or call get_lang() to decide
    // whether to localise the welcome message or other user-facing strings.
    lua["LANG"] = cfg.langCode;   // e.g. "it", "fr", "" when not set
    lua.set_function("get_lang", [&]() -> std::string { return cfg.langCode; });

    lua.set_function("print", [](sol::variadic_args va) {
        bool first = true;
        for (auto v : va) {
            if (!first) std::cout << "\t";
            first = false;
            switch (v.get_type()) {
                case sol::type::string:  std::cout << v.as<std::string>(); break;
                case sol::type::number:  std::cout << v.as<double>();      break;
                case sol::type::boolean: std::cout << (v.as<bool>() ? "true" : "false"); break;
                case sol::type::lua_nil:     std::cout << "nil"; break;
                default:                 std::cout << "(userdata)"; break;
            }
        }
        std::cout << "\n";
    });

    lua.set_exception_handler([](lua_State* L, sol::optional<const std::exception&> ex,
                                  sol::string_view desc) -> int {
        std::cerr << "--- LUA EXCEPTION ---\n" << desc;
        if (ex) std::cerr << "\n" << ex->what();
        std::cerr << "\n";
        return sol::stack::push(L, desc);
    });

    // ------------------------------------------------------------------
    // ASSET_ROOT: absolute base dir for asset/ and catalog/. Scripts and
    // visual_gen prepend it so the GUI (possibly a different cwd) finds the
    // PNG files referenced in the visual config. Default = current dir.
    // ------------------------------------------------------------------
    {
        std::error_code ec;
        std::filesystem::path root = cfg.assetRoot.empty()
            ? std::filesystem::current_path(ec)
            : std::filesystem::absolute(cfg.assetRoot, ec);
        lua["ASSET_ROOT"] = root.string();
    }

    // ------------------------------------------------------------------
    // query_llm exposed to Lua — used by scripts for autonomous calls
    // (e.g. dream generation, NPC monologues) without touching the C++ loop.
    //
    // Lua signature:
    //   query_llm(sys, history_json, user, schema) -> string
    //
    // history_json is a serialized JSON array (use "[]" if no history needed).
    // ------------------------------------------------------------------
    lua.set_function("query_llm",
        [&](const std::string& sys_prompt,
            const std::string& history_json,
            const std::string& user_prompt,
            const std::string& schema,
            sol::optional<std::string> model_override,
            sol::optional<std::string> provider_override,
            sol::optional<std::string> label) -> std::string {

            std::vector<Message> lua_history;
            try {
                auto jarr = json::parse(history_json);
                if (jarr.is_array())
                    for (auto& item : jarr)
                        lua_history.push_back({
                            item.value("role", "user"),
                            item.value("content", ""),
                            item.value("player_id", "")
                        });
            } catch (...) {}

            std::string model       = model_override.value_or(cfg.activeModel());
            AIProvider  provider    = cfg.provider;
            if (provider_override.has_value() && !provider_override->empty())
                provider = provider_from_string(*provider_override);
            // Token accounting: attribute this call's tokens to the given label
            // (e.g. "agent", "gen", "ambient"); default "lua" for untagged calls.
            g_llm_label = label.value_or("lua");
            return ::query_llm(provider, with_lang(sys_prompt), lua_history,
                               user_prompt, schema, model);
        });

    // ------------------------------------------------------------------
    // get_token_usage() → { [label] = { prompt, completion, total, calls }, ... }
    // Dev-phase token accounting: how many tokens each component spent.
    // ------------------------------------------------------------------
    lua.set_function("get_token_usage", [&]() -> sol::table {
        sol::table out = lua.create_table();
        std::lock_guard<std::mutex> lk(g_llm_token_mutex);
        for (auto& kv : g_llm_token_usage) {
            sol::table e = lua.create_table();
            e["prompt"]     = (double)kv.second.prompt;
            e["completion"] = (double)kv.second.completion;
            e["total"]      = (double)kv.second.total;
            e["calls"]      = (double)kv.second.calls;
            out[kv.first]   = e;
        }
        return out;
    });

    // ------------------------------------------------------------------
    // get_tier(name) → { model=..., provider=... }
    // Per-role model defaults configurable via CLI (--gen-model, ...) and
    // the web Settings panel (/api/settings). Names: "gen" (entity
    // generation), "agent" (NPC agents), "ambient" (NPC↔NPC off-screen).
    // Empty string = unset → Lua libs fall back to the main provider/model.
    // Read live from cfg, so runtime settings changes apply immediately.
    // ------------------------------------------------------------------
    lua.set_function("get_tier",
        [](const std::string& name, sol::this_state s) -> sol::table {
            sol::state_view L(s);
            sol::table t = L.create_table();
            std::lock_guard<std::mutex> lk(g_cfg_mutex);
            if      (name == "gen")     { t["model"] = cfg.genModel;     t["provider"] = cfg.genProvider; }
            else if (name == "agent")   { t["model"] = cfg.agentModel;   t["provider"] = cfg.agentProvider; }
            else if (name == "ambient") { t["model"] = cfg.ambientModel; t["provider"] = cfg.ambientProvider; }
            else                        { t["model"] = "";               t["provider"] = ""; }
            return t;
        });

    // ------------------------------------------------------------------
    // query_llm_async / query_llm_poll — non-blocking LLM calls from Lua.
    //
    // Designed for off-screen NPC ambient events: the script fires a call,
    // continues the current turn, and harvests results in a later turn.
    // The background thread touches ONLY C++ data (never the Lua state).
    //
    // Lua signatures:
    //   local job_id = query_llm_async(sys, history_json, user, schema
    //                                  [, model [, provider]])  → string
    //   local result = query_llm_poll(job_id)  → string (done) | nil (pending) | false (error)
    // ------------------------------------------------------------------
    {
        struct LlmJob {
            enum class State { PENDING, DONE, ERROR } state = State::PENDING;
            std::string result;
            std::string error;
        };
        auto llm_jobs         = std::make_shared<std::map<std::string, LlmJob>>();
        auto llm_jobs_mutex   = std::make_shared<std::mutex>();
        auto llm_job_counter  = std::make_shared<std::atomic<int>>(0);

        lua.set_function("query_llm_async",
            [&, llm_jobs, llm_jobs_mutex, llm_job_counter](
                const std::string& sys_prompt,
                const std::string& history_json,
                const std::string& user_prompt,
                const std::string& schema,
                sol::optional<std::string> model_override,
                sol::optional<std::string> provider_override,
                sol::optional<std::string> label) -> std::string {

                std::string job_id   = "llmjob_" + std::to_string(++(*llm_job_counter));
                std::string job_label = label.value_or("ambient");

                {
                    std::lock_guard<std::mutex> lk(*llm_jobs_mutex);
                    (*llm_jobs)[job_id] = LlmJob{};
                }

                std::string model     = model_override.value_or(cfg.activeModel());
                AIProvider  provider  = cfg.provider;
                if (provider_override.has_value() && !provider_override->empty())
                    provider = provider_from_string(*provider_override);

                std::vector<Message> lua_history;
                try {
                    auto jarr = json::parse(history_json);
                    if (jarr.is_array())
                        for (auto& item : jarr)
                            lua_history.push_back({
                                item.value("role", "user"),
                                item.value("content", ""),
                                item.value("player_id", "")
                            });
                } catch (...) {}

                std::thread([llm_jobs, llm_jobs_mutex, job_id, job_label,
                             provider, sys_prompt, lua_history, user_prompt, schema, model]() {
                    g_llm_label = job_label;   // attribute tokens on THIS worker thread
                    try {
                        std::string res;
                        {
                            // Guard against concurrent /api/settings rewriting cfg
                            // endpoint/key fields that the provider functions read.
                            std::lock_guard<std::mutex> cfg_lk(g_cfg_mutex);
                            res = ::query_llm(provider, sys_prompt, lua_history,
                                              user_prompt, schema, model);
                        }
                        std::lock_guard<std::mutex> lk(*llm_jobs_mutex);
                        (*llm_jobs)[job_id].result = std::move(res);
                        (*llm_jobs)[job_id].state  = LlmJob::State::DONE;
                    } catch (const std::exception& ex) {
                        std::lock_guard<std::mutex> lk(*llm_jobs_mutex);
                        (*llm_jobs)[job_id].error  = ex.what();
                        (*llm_jobs)[job_id].state  = LlmJob::State::ERROR;
                    }
                }).detach();

                return job_id;
            });

        lua.set_function("query_llm_poll",
            [llm_jobs, llm_jobs_mutex](sol::this_state L,
                                       const std::string& job_id) -> sol::object {
                std::lock_guard<std::mutex> lk(*llm_jobs_mutex);
                auto it = llm_jobs->find(job_id);
                if (it == llm_jobs->end()) return sol::lua_nil;

                auto& job = it->second;
                if (job.state == LlmJob::State::PENDING) return sol::lua_nil;

                sol::state_view sv(L);
                if (job.state == LlmJob::State::ERROR) {
                    llm_jobs->erase(it);
                    return sol::make_object(sv, false);
                }
                // DONE: return result string and clean up
                std::string res = std::move(job.result);
                llm_jobs->erase(it);
                return sol::make_object(sv, res);
            });
    }

    // ------------------------------------------------------------------
    // get_embedding exposed to Lua — returns a Lua table of floats.
    //
    // Lua signature:
    //   local vec = get_embedding("text to embed")
    //   -- vec is a table {float, float, ...} or nil on error
    //
    // Typical script uses:
    //   - classifying player input
    //   - semantic matching between actions and log events
    //   - future in-script semantic RAG extensions
    // ------------------------------------------------------------------

    lua.set_function("get_embedding",
        [](sol::this_state L, const std::string& text) -> sol::object {
            auto vec = get_embedding(text);
            if (vec.empty()) return sol::lua_nil;
            sol::state_view sv(L);
            sol::table t = sv.create_table((int)vec.size());
            for (int i = 0; i < (int)vec.size(); ++i)
                t[i + 1] = vec[i];   // Lua uses 1-based indices
            return t;
        });

    // cosine_similarity exposed to Lua for vector matching in scripts.
    // Lua signature:
    //   local sim = cosine_similarity(vec_a, vec_b)  -- returns float [0,1]
    lua.set_function("cosine_similarity",
        [](sol::table a, sol::table b) -> float {
            std::vector<float> va, vb;
            for (auto& kv : a) va.push_back(kv.second.as<float>());
            for (auto& kv : b) vb.push_back(kv.second.as<float>());
            return cosine_similarity(va, vb);
        });

    lua.set_function("get_pinned_scene_path",
        [&](const std::string& key) -> sol::optional<std::string> {
            auto path = pin_cache::lookup(cfg.basePath, key);
            if (path.empty()) return sol::nullopt;
            return path;
        });

    // composite_images exposed to Lua — alpha-blend a stack of PNG layers into one file.
    // Lua signature:
    //   local ok, err = composite_images(layers_table, output_rel_path)
    //   Each entry in layers_table is either:
    //     - a string path (relative to --path): resized to canvas, placed at (0,0)
    //     - a table {path=string, x=int, y=int}: placed at pixel offset, natural size
    //   Canvas dimensions are taken from the first layer (must be a plain string path).
    //   output_rel_path: destination relative to --path base dir
    //   returns: true, "" on success | false, errmsg on failure
    // composite_images — alpha-blend a stack of PNG layers into one output file.
    //
    // Lua signatures (all backward-compatible):
    //   composite_images(layers, out_path)
    //
    // Each entry in layers can be:
    //   "path/to/file.png"
    //       → resize to canvas size, place at (0,0), no transforms
    //   { path="...", x=0, y=0 }
    //       → natural size, place at pixel offset (x,y)
    //   { path="...", x=129, y=144, xzoom=-0.76, yzoom=0.76, alpha=1.0 }
    //       → full transform: scale by (|xzoom|,|yzoom|), flip if negative, global alpha
    //
    // Canvas size comes from the first layer (must be loadable).
    // xzoom/yzoom: 1.0 = natural size; negative = mirror that axis; 0 = skip
    // alpha: [0,1] global opacity multiplier applied before compositing
    lua.set_function("composite_images",
        [&](sol::table layers_tbl, const std::string& out_rel) -> std::tuple<bool, std::string> {
            namespace fs = std::filesystem;

            struct Layer {
                std::string path;
                int   x = 0, y = 0;
                float xzoom = 0.0f, yzoom = 0.0f; // 0 = full-canvas resize; non-zero = explicit scale
                float alpha = 1.0f;
            };

            try {
                fs::path base(cfg.basePath);
                fs::path out_abs = base / out_rel;
                fs::create_directories(out_abs.parent_path());

                std::vector<Layer> layers;
                for (auto& kv : layers_tbl) {
                    if (kv.second.get_type() == sol::type::string) {
                        // plain string → full-canvas layer
                        layers.push_back({ (base / kv.second.as<std::string>()).string() });
                    } else if (kv.second.get_type() == sol::type::table) {
                        sol::table t = kv.second.as<sol::table>();
                        Layer l;
                        l.path  = (base / t.get_or<std::string>("path", "")).string();
                        l.x     = t.get_or("x",     0);
                        l.y     = t.get_or("y",     0);
                        l.alpha = t.get_or("alpha", 1.0f);
                        // zoom shortcuts: "zoom" sets both axes uniformly
                        float zoom = t.get_or("zoom", 0.0f);
                        l.xzoom   = t.get_or("xzoom", zoom);
                        l.yzoom   = t.get_or("yzoom", zoom);
                        if (!l.path.empty()) layers.push_back(l);
                    }
                }
                if (layers.empty()) return {false, "no layers"};

                // Canvas size from first layer
                int W = 0, H = 0;
                {
                    int w, h, ch;
                    stbi_uc* tmp = stbi_load(layers[0].path.c_str(), &w, &h, &ch, 4);
                    if (!tmp) return {false, "cannot load first layer: " + layers[0].path};
                    W = w; H = h;
                    stbi_image_free(tmp);
                }

                std::vector<uint8_t> canvas(W * H * 4, 0);

                // Alpha-composite src region onto canvas at (ox, oy).
                // layer_alpha: per-layer global opacity multiplier.
                auto blend_region = [&](const uint8_t* src, int src_w, int src_h,
                                        int ox, int oy, float layer_alpha) {
                    for (int sy = 0; sy < src_h; ++sy) {
                        int dy = oy + sy;
                        if (dy < 0 || dy >= H) continue;
                        for (int sx = 0; sx < src_w; ++sx) {
                            int dx = ox + sx;
                            if (dx < 0 || dx >= W) continue;
                            int si = (sy * src_w + sx) * 4;
                            int di = (dy * W + dx) * 4;
                            float sa = (src[si+3] / 255.0f) * layer_alpha;
                            float da = canvas[di+3] / 255.0f;
                            float oa = sa + da * (1.0f - sa);
                            if (oa > 0.0f) {
                                for (int c = 0; c < 3; ++c)
                                    canvas[di+c] = static_cast<uint8_t>(
                                        (src[si+c]*sa + canvas[di+c]*da*(1.0f-sa)) / oa);
                            }
                            canvas[di+3] = static_cast<uint8_t>(oa * 255.0f);
                        }
                    }
                };

                // Flip pixels horizontally in-place (mirror around vertical axis).
                auto flip_x = [](uint8_t* px, int w, int h) {
                    for (int y = 0; y < h; ++y) {
                        uint8_t* row = px + y * w * 4;
                        for (int x = 0; x < w / 2; ++x) {
                            uint8_t* a = row + x * 4;
                            uint8_t* b = row + (w - 1 - x) * 4;
                            for (int c = 0; c < 4; ++c) std::swap(a[c], b[c]);
                        }
                    }
                };

                // Flip pixels vertically in-place.
                auto flip_y = [](uint8_t* px, int w, int h) {
                    for (int y = 0; y < h / 2; ++y) {
                        uint8_t* top = px + y * w * 4;
                        uint8_t* bot = px + (h - 1 - y) * w * 4;
                        for (int x = 0; x < w * 4; ++x) std::swap(top[x], bot[x]);
                    }
                };

                for (const auto& layer : layers) {
                    int w, h, ch;
                    stbi_uc* img = stbi_load(layer.path.c_str(), &w, &h, &ch, 4);
                    if (!img) {
                        std::cerr << "[COMPOSITE] skip: " << layer.path
                                  << " — " << stbi_failure_reason() << "\n";
                        continue;
                    }

                    bool has_zoom = (layer.xzoom != 0.0f || layer.yzoom != 0.0f);

                    if (!has_zoom && layer.x == 0 && layer.y == 0) {
                        // Full-canvas mode: resize to (W,H), blit at (0,0)
                        if (w != W || h != H) {
                            std::vector<uint8_t> buf(W * H * 4);
                            stbir_resize_uint8_linear(img, w, h, 0, buf.data(), W, H, 0, STBIR_RGBA);
                            blend_region(buf.data(), W, H, 0, 0, layer.alpha);
                        } else {
                            blend_region(img, W, H, 0, 0, layer.alpha);
                        }
                    } else {
                        // Positioned mode: apply optional scale/flip, then blit at (x,y)
                        std::vector<uint8_t> work;
                        int dst_w = w, dst_h = h;

                        float sx = has_zoom ? std::abs(layer.xzoom) : 1.0f;
                        float sy = has_zoom ? std::abs(layer.yzoom) : 1.0f;
                        bool do_flip_x = has_zoom && (layer.xzoom < 0.0f);
                        bool do_flip_y = has_zoom && (layer.yzoom < 0.0f);

                        if (sx != 1.0f || sy != 1.0f) {
                            dst_w = std::max(1, static_cast<int>(std::round(w * sx)));
                            dst_h = std::max(1, static_cast<int>(std::round(h * sy)));
                            work.resize(dst_w * dst_h * 4);
                            stbir_resize_uint8_linear(img, w, h, 0,
                                                      work.data(), dst_w, dst_h, 0, STBIR_RGBA);
                        } else {
                            work.assign(img, img + w * h * 4);
                        }

                        if (do_flip_x) flip_x(work.data(), dst_w, dst_h);
                        if (do_flip_y) flip_y(work.data(), dst_w, dst_h);

                        blend_region(work.data(), dst_w, dst_h, layer.x, layer.y, layer.alpha);
                    }
                    stbi_image_free(img);
                }

                int ok = stbi_write_png(out_abs.string().c_str(), W, H, 4, canvas.data(), W * 4);
                if (!ok) return {false, "write failed: " + out_abs.string()};
                return {true, ""};
            } catch (const std::exception& e) {
                return {false, std::string(e.what())};
            }
        });

    if (!cfg.webMode) {
        clear_optional_script_globals(lua);   // also predefines the `quick` global
        lua.script_file(cfg.basePath + cfg.script);
        load_script_tools(lua);
    }

    // --- Session init ---
    std::vector<Message> chat_history;

    if (!cfg.loadFile.empty()) {
        print_system("Loading session from: " + cfg.loadFile + "...");
        if (load_session_from_jsonl(cfg.loadFile, lua, chat_history)) {
            cfg.saveFile = cfg.loadFile;
        } else {
            print_error("Load failed.");
            curl_global_cleanup();
            return 1;
        }
    } else if (!cfg.webMode) {
        // Console mode: interactive init
        std::string welcome = lua["get_welcome_message"]();
        std::cout << ansi(C::BRIGHT_YELLOW) << welcome << ansi(C::RESET) << "\n";
        std::string init_choice = read_input("> ");
        if (init_choice == "auto") lua["generate_initial_state"]();
        else                       lua["set_initial_state"](init_choice);
    }
    // In web mode the state is initialized via POST /start

    // Open file only in FULL mode (LAST rewrites from scratch each time)
    // Declared here (outside webMode) because run_turn uses it in web mode too
    std::ofstream full_stream;
    if (!cfg.webMode && cfg.saveMode == SaveMode::FULL) {
        full_stream.open(cfg.saveFile,
                         cfg.loadFile.empty() ? std::ios::trunc : std::ios::app);
        if (!full_stream.is_open()) {
            print_error("Cannot open save file: " + cfg.saveFile);
            curl_global_cleanup();
            return 1;
        }
    }

    // In web mode we skip HUD printing and do not enter the console loop
    if (!cfg.webMode) {
        print_display(lua["get_display_state"]().get<std::string>());
        std::cout << "\n";
    }

    // sys_prompt and schema are re-read dynamically each turn in web mode
    // (some scripts recompute them on every call based on current state)
    // In console mode we read them once for consistency with original behaviour
    std::string lua_sys_prompt_console;
    std::string json_schema_console;
    if (!cfg.webMode) {
        lua_sys_prompt_console = lua["get_system_prompt"]();
        json_schema_console    = lua["get_json_schema"]();
    }

    // References to last responses for the /fix command (console mode)
    std::string last_llm_reply;
    std::string last_player_input;

    // ===========================================================================
    // Main loop (console mode)
    // ===========================================================================
    bool running = !cfg.webMode;

    while (running && !g_shutdown) {
        std::string player_input = read_input("❯ ");
        if (g_shutdown) break;
        if (player_input.empty()) continue;

        // ------------------------------------------------------------------
        // Comandi built-in C++
        // ------------------------------------------------------------------
        if (player_input == "/quit" || player_input == "/q") {
            std::cout << ansi(C::BRIGHT_YELLOW) << "Goodbye, adventurer.\n" << ansi(C::RESET);
            break;
        }
        if (player_input == "/status") {
            print_box("RAW STATE", lua["get_state_snapshot"]().get<std::string>());
            continue;
        }
        if (player_input == "/save") {
            write_turn(cfg.saveFile, full_stream, cfg.saveMode,
                       "[manual save]", "", "", lua["get_state_snapshot"](), chat_history);
            print_system("Saved to " + cfg.saveFile);
            continue;
        }
        if (player_input == "/help") { print_help(argv[0]); continue; }

        // ------------------------------------------------------------------
        // /summary [N] — generate a narrative summary and compress history.
        //
        // Generates a free-text summary of the oldest messages, inserts it
        // as a single message in history and removes the messages it covers,
        // reducing context window consumption.
        //
        // /summary    → compress everything except the last 4 messages (2 turns)
        // /summary 6  → keep the last 6 messages (3 turns) after the summary
        // ------------------------------------------------------------------
        bool is_riassunto = (player_input == "/summary" || player_input == "/summarize"
                             || player_input.rfind("/summary ", 0) == 0);
        if (is_riassunto) {
            // How many recent messages to keep after the summary (user/assistant pairs)
            int keep_recent = 4;
            auto sp = player_input.find(' ');
            if (sp != std::string::npos) {
                try { keep_recent = std::stoi(player_input.substr(sp + 1)) * 2; }
                catch (...) {}
            }
            keep_recent = std::max(0, std::min(keep_recent, (int)chat_history.size()));
            int to_summarize = (int)chat_history.size() - keep_recent;

            if (to_summarize <= 0) {
                print_system("Not enough messages to summarise.");
                continue;
            }

            print_thinking("Generating narrative summary...");

            // Costruisce il testo da riassumere estraendo solo le narrazioni
            std::string history_text;
            for (int i = 0; i < to_summarize; ++i) {
                const auto& msg = chat_history[i];
                if (msg.role == "user") {
                    history_text += "Player: " + msg.content + "\n";
                } else {
                    // Prova ad estrarre solo il campo narration dal JSON
                    try {
                        auto j = json::parse(msg.content);
                        history_text += "Narrator: " + j.value("narration", msg.content) + "\n";
                    } catch (...) {
                        history_text += "Narrator: " + msg.content + "\n";
                    }
                }
            }

            std::string sum_sys =
                "You are the narrator of a role-playing game. Write a concise "
                "and compelling summary of the events in chronological order, in second person "
                "('you met', 'you moved to', etc.). "
                "Narrative text only, no lists or structure.";
            std::string sum_user =
                "Here is the game story so far:\n\n" + history_text +
                "\n\nWrite a narrative summary of the main events.";

            std::string summary = query_llm(cfg.provider, with_lang(sum_sys), {}, sum_user, "", active_model);

            print_box("STORY SUMMARY", summary);

            // Comprimi la history: sostituisce i messaggi riassunti con uno solo
            std::vector<Message> recent_tail(
                chat_history.end() - keep_recent, chat_history.end());
            chat_history.clear();
            chat_history.push_back({
                "assistant",
                "[SUMMARY OF PREVIOUS EVENTS]\n" + summary,
                "gm"
            });
            for (auto& m : recent_tail) chat_history.push_back(m);

            print_system("History compressed: " + std::to_string(to_summarize) +
                         " messages → 1 summary + " +
                         std::to_string(keep_recent) + " recent messages.");
            continue;
        }

        // ------------------------------------------------------------------
        // /fix <instruction> — corrects the last LLM response.
        //
        // The LLM rewrites the previous scene with the given correction.
        // Time does NOT advance; Lua state is not updated further.
        // The last turn in history is replaced with the corrected one.
        //
        // Examples:
        //   /fix rossana was not there, she was in the kitchen
        //   /fix the protagonist has not entered the house yet
        //   /fix wrong name: she is called Giulia not Giada
        // ------------------------------------------------------------------
        if (player_input.rfind("/fix", 0) == 0) {
            std::string fix_request;
            auto sp = player_input.find(' ');
            if (sp != std::string::npos) fix_request = player_input.substr(sp + 1);
            else fix_request = "correct any narrative or continuity errors";

            if (last_llm_reply.empty()) {
                print_system("No previous response to correct.");
                continue;
            }

            print_thinking("Rewriting scene...");

            std::string prev_narration;
            try {
                auto j = json::parse(last_llm_reply);
                prev_narration = j.value("narration", "");
            } catch (...) { prev_narration = last_llm_reply; }

            std::string current_state = lua["get_status_for_ai"]();

            std::string fix_sys = lua_sys_prompt_console +
                "\n\nIMPORTANT — CORRECTION MODE: You are rewriting an already narrated scene. "
                "Do NOT advance time (avanza_tempo = 0). "
                "Keep the same JSON schema "
                "but produce a corrected narration according to the given instructions.";

            std::string fix_user =
                "Current state:\n" + current_state +
                "\n\nPrevious narration to correct:\n" + prev_narration +
                "\n\nRequested correction: " + fix_request +
                "\n\nRewrite the scene with the correction applied. "
                "Time does not advance. Reply ONLY with the specified JSON.";

            // History without the last turn (the one being corrected)
            std::vector<Message> history_without_last = chat_history;
            if (history_without_last.size() >= 2) {
                history_without_last.pop_back();
                history_without_last.pop_back();
            }

            bool fix_ok = false;
            for (int attempt = 0; attempt < cfg.maxRetries && !fix_ok; ++attempt) {
                if (attempt > 0) print_warning("Retry " + std::to_string(attempt) + "...");

                auto trimmed = trim_history(history_without_last, cfg.maxHistory);
                std::string fix_reply = query_llm(cfg.provider, with_lang(fix_sys), trimmed,
                                                  fix_user, json_schema_console, active_model);
                sol::table result = lua["process_ai_response"](fix_reply);

                if (result["success"].get<bool>()) {
                    fix_ok = true;
                    std::string narration = result["narration"].get<std::string>();

                    std::cout << ansi(C::BRIGHT_MAGENTA) << "\n[CORRECTED SCENE]\n" << ansi(C::RESET);
                    print_narration(narration);
                    print_separator();
                    print_display(lua["get_display_state"]().get<std::string>());

                    // Replace the last turn in history
                    if (chat_history.size() >= 2) {
                        chat_history.pop_back();
                        chat_history.pop_back();
                    }
                    std::string cfix_ts = utc_timestamp();
                    chat_history.push_back({"user",      last_player_input, "player", ""});
                    chat_history.push_back({"assistant", fix_reply,         "gm",     cfix_ts});
                    last_llm_reply = fix_reply;

                    std::string state_after = lua["get_state_snapshot"]();
                    write_turn(cfg.saveFile, full_stream, cfg.saveMode,
                               last_player_input + " [FIX: " + fix_request + "]",
                               fix_reply, narration, state_after, chat_history);
                } else {
                    print_error(result["error"].get<std::string>());
                    fix_user += "\n\nERROR: " + result["error"].get<std::string>() + "\nFix the JSON.";
                }
            }
            if (!fix_ok)
                print_error("Fix failed after " + std::to_string(cfg.maxRetries) + " attempts.");
            continue;
        }

        // ------------------------------------------------------------------
        // /observe [subject] — detailed description without modifying state.
        //
        // The LLM describes the current scene or a specific element in detail.
        // No changes to state, history or save file.
        //
        // Examples:
        //   /observe              → describe the whole scene
        //   /observe elena        → describe Elena in detail
        //   /observe the kitchen  → describe the current location
        //   /observe the clock    → describe a specific object
        // ------------------------------------------------------------------
        if (player_input.rfind("/observe", 0) == 0) {
            std::string subject;
            auto sp = player_input.find(' ');
            if (sp != std::string::npos) subject = player_input.substr(sp + 1);

            print_thinking(subject.empty() ? "Observing scene..." :
                           "Observing: " + subject + "...");

            std::string current_state = lua["get_status_for_ai"]();

            std::string obs_sys = lua_sys_prompt_console +
                "\n\nOBSERVATION MODE: Describe a scene element in detail. "
                "Do not advance time, do not modify any game state. "
                "Reply in FREE TEXT (not JSON), with a rich sensory description: "
                "sight, sound, smell, touch, atmosphere. At least 4 evocative sentences.";

            std::string obs_user;
            if (subject.empty()) {
                obs_user = "Current state:\n" + current_state +
                           "\n\nDescribe the current scene in detail: the environment, "
                           "atmosphere, characters present, sounds, smells, "
                           "everything the protagonist perceives right now.";
            } else {
                obs_user = "Current state:\n" + current_state +
                           "\n\nDescribe in detail: " + subject +
                           "\nFocus on physical, emotional and sensory aspects. "
                           "What does the protagonist see, hear, perceive?";
            }

            std::string obs = query_llm(cfg.provider, with_lang(obs_sys),
                                        trim_history(chat_history, cfg.maxHistory),
                                        obs_user, "", active_model);

            std::string title = subject.empty() ? "OBSERVATION" : "OBSERVATION: " + subject;
            print_box(title, obs);
            // No changes to state, history or save file
            continue;
        }

        // ------------------------------------------------------------------
        // Delegate to Lua
        // ------------------------------------------------------------------
        sol::protected_function ppi_func = lua["process_player_input"];
        sol::protected_function_result ppi_res = ppi_func(player_input);
        if (!ppi_res.valid()) {
            sol::error ppi_err = ppi_res;
            print_error("process_player_input error: " + std::string(ppi_err.what()));
            continue;
        }
        sol::table cmd_result = ppi_res;

        if (!cmd_result["success"].get<bool>()) {
            print_error(cmd_result["error"].get<std::string>());
            continue;
        }

        bool cmd_handled = cmd_result["handled"].get<bool>();

        // Lua command with direct output (e.g. /dreams, /map, /relations)
        if (cmd_handled) {
            sol::optional<std::string> lua_output = cmd_result["output"];
            if (lua_output && !lua_output->empty()) {
                print_box("INFO", *lua_output);
                print_display(lua["get_display_state"]().get<std::string>());
                continue;
            }
        }

        // ------------------------------------------------------------------
        // Build context and RAG
        // ------------------------------------------------------------------
        std::string current_state = lua["get_status_for_ai"]();
        std::string effective_sys_prompt = lua_sys_prompt_console;

        if (rag_active) {
            std::string ctx_location, ctx_time;
            std::set<std::string> ctx_npcs;
            parse_current_context(current_state, ctx_location, ctx_time, ctx_npcs);

            std::vector<float> cur_embedding;
            if (!cfg.embedModel.empty())
                cur_embedding = get_embedding(player_input);

            auto examples = select_rag_examples(
                rag_db, ctx_location, ctx_time, ctx_npcs,
                player_input, cur_embedding, cfg.ragExamples);

            std::string rag_block = build_rag_block(examples);
            if (!rag_block.empty())
                effective_sys_prompt += rag_block;
        }

        // Build user prompt
        std::string user_prompt;
        if (cmd_handled) {
            user_prompt =
                "Current state (already updated):\n" + current_state +
                "\n\nThe player did: " + player_input +
                "\n\nNarrate what happened and describe the new situation. "
                "Reply ONLY with the specified JSON object.";
        } else {
            user_prompt =
                "Current state:\n" + current_state +
                "\n\nThe player says/does: " + player_input +
                "\n\nDecide the outcome, update the state and narrate the result. "
                "Reply ONLY with the specified JSON object.";
        }

        // LLM call with retry
        print_thinking();
        bool turn_ok = false;
        if (script_has_tools) {
            static int tool_turn = 0;
            std::cerr << "\033[2m── TURNO " << ++tool_turn
                      << " ──────────────────────────────────────\033[0m\n";
        }
        for (int attempt = 0; attempt < cfg.maxRetries && !turn_ok; ++attempt) {
            if (attempt > 0)
                print_warning("Retry " + std::to_string(attempt) +
                              "/" + std::to_string(cfg.maxRetries - 1) + "...");

            auto trimmed = trim_history(chat_history, cfg.maxHistory);
            // Tools only on first attempt — retries re-ask for valid JSON without
            // re-running the (state-mutating) tool loop. See web run_turn for rationale.
            std::string llm_reply = (script_has_tools && attempt == 0)
                ? query_llm_with_tools(cfg.provider, with_lang(effective_sys_prompt), trimmed,
                                       user_prompt, json_schema_console, active_tools,
                                       execute_tool, active_model)
                : query_llm(cfg.provider, with_lang(effective_sys_prompt), trimmed,
                            user_prompt, json_schema_console, active_model);

            //sol::table result = lua["process_ai_response"](llm_reply);

// 1. Retrieve the traceback function from Lua
sol::function traceback = lua["debug"]["traceback"];

// 2. Prepare the protected function to call
sol::protected_function target_func = lua["process_ai_response"];

// 3. Execute: pass the error handler directly as second argument
// Syntax: lua_function(args..., error_handler)
sol::protected_function_result f_result = target_func(llm_reply, traceback);

// 4. Check result
if (!f_result.valid()) {
    sol::error err = f_result;
    std::string what = err.what();
    std::cout << "LUA DETAILED ERROR:\n" << what << std::endl;
}
sol::table result = f_result;

            if (result["success"].get<bool>()) {
                turn_ok = true;
                std::string narration = result["narration"].get<std::string>();

                print_narration(narration);
                print_separator();
                print_display(lua["get_display_state"]().get<std::string>());

                // Save references for /fix
                last_llm_reply    = llm_reply;
                last_player_input = player_input;

                std::string cons_ts = utc_timestamp();
                chat_history.push_back({"user",      player_input, "player", ""});
                chat_history.push_back({"assistant", llm_reply,    "gm",     cons_ts});

                std::string state_after = lua["get_state_snapshot"]();
                write_turn(cfg.saveFile, full_stream, cfg.saveMode,
                           player_input, llm_reply, narration, state_after, chat_history);

                if (result["game_over"].get<bool>()) {
                    std::cout << "\n" << ansi(C::BRIGHT_YELLOW)
                              << "========== THE END =========="
                              << ansi(C::RESET) << "\n";
                    running = false;
                }
            } else {
                print_error(result["error"].get<std::string>());
                user_prompt += "\n\nERROR in previous response: " +
                               result["error"].get<std::string>() +
                               "\nFix it and return valid JSON.";
            }
        }

        if (!turn_ok)
            print_error("LLM failed after " + std::to_string(cfg.maxRetries) + " attempts. Skipping turn.");
    }


    // ===========================================================================
    // WEB MODE  (--web)
    // ===========================================================================
    // Struttura:
    //   GET  /                  → UI HTML single-page
    //   GET  /api/scripts       → lista file .lua in basePath
    //   POST /api/start         → load script, init game  body: {"script":"x.lua","player_name":"Marco"}
    //   POST /api/chat          → normal turn           body: {"input":"..."}
    //   POST /api/command       → comando /xxx          body: {"input":"/relazioni"}
    //   GET  /api/status        → current state snapshot
    //   POST /api/save          → force manual save
    //
    // Thread safety: ogni route acquisisce lua_mutex prima di toccare Lua o chat_history.
    // Only one active session at a time — if POST /start arrives while a game is running,
    // the previous session is reset.
    // ===========================================================================

    if (cfg.webMode) {

        std::mutex lua_mutex;

        // Ring buffer for Lua/script errors (readable by CoderAI via get_script_errors tool)
        std::deque<std::string> script_error_ring;
        std::mutex              script_error_mutex;
        constexpr size_t        SCRIPT_ERROR_MAX = 20;
        auto push_script_error = [&](const std::string& err) {
            std::lock_guard<std::mutex> g(script_error_mutex);
            script_error_ring.push_back(err);
            while (script_error_ring.size() > SCRIPT_ERROR_MAX) script_error_ring.pop_front();
        };

        // Three possible states for the web session
        enum class SessionState { IDLE, AWAITING_INIT, PLAYING };
        SessionState session_state = SessionState::IDLE;
        std::string  active_script;

        std::string web_last_llm_reply;
        std::string web_last_player_input;

        // Undo stack: stores (lua_snapshot, chat_history) before each turn.
        std::deque<std::pair<std::string, std::vector<Message>>> undo_stack;
        constexpr size_t MAX_UNDO_STEPS = 10;

        // Resolves the full path for a save file.
        // Uses savePath as directory if set, otherwise current working directory.
        auto resolve_save_path = [&](const std::string& filename) -> std::string {
            return cfg.savePath + filename;
        };

        // ---------------------------------------------------------------------------
        // run_turn: executes a full LLM turn. Must be called with lua_mutex held.
        // ---------------------------------------------------------------------------
        auto run_turn = [&](const std::string& player_input,
                            bool cmd_handled,
                            std::vector<Message>& hist,
                            std::ofstream& fstream) -> json {

            std::string sys_prompt = lua["get_system_prompt"]();
            std::string schema     = lua["get_json_schema"]();
            std::string cur_state  = lua["get_status_for_ai"]();

            if (rag_active) {
                std::string ctx_loc, ctx_time;
                std::set<std::string> ctx_npcs;
                parse_current_context(cur_state, ctx_loc, ctx_time, ctx_npcs);
                std::vector<float> cur_emb;
                if (!cfg.embedModel.empty()) cur_emb = get_embedding(player_input);
                auto examples = select_rag_examples(
                    rag_db, ctx_loc, ctx_time, ctx_npcs, player_input, cur_emb, cfg.ragExamples);
                std::string rag_block = build_rag_block(examples);
                if (!rag_block.empty()) sys_prompt += rag_block;
            }

            std::string user_prompt = cmd_handled
                ? "Current state (already updated):\n" + cur_state +
                  "\n\nThe player did: " + player_input +
                  "\n\nNarrate what happened and describe the new situation. "
                  "Reply ONLY with the specified JSON object."
                : "Current state:\n" + cur_state +
                  "\n\nThe player says/does: " + player_input +
                  "\n\nDecide the outcome, update the state and narrate the result. "
                  "Reply ONLY with the specified JSON object.";

            json result_json;
            result_json["success"] = false;

            // Hook: before_ai_turn(player_input) — optional.
            // If it returns {skip_llm=true, narration="..."} the LLM call is skipped entirely.
            // The hook may modify Lua state directly before returning.
            sol::protected_function bat_fn = lua["before_ai_turn"];
            if (bat_fn.valid()) {
                sol::protected_function_result bat_r;
                try {
                    bat_r = bat_fn(player_input);
                } catch (const std::exception& ex) {
                    std::string msg = std::string("before_ai_turn threw: ") + ex.what();
                    push_script_error(msg);
                    std::cerr << "[HOOK THROW] " << msg << std::endl;
                    result_json["success"] = false; result_json["error"] = msg;
                    return result_json;
                } catch (...) {
                    std::string msg = "before_ai_turn threw a non-std exception.";
                    push_script_error(msg);
                    std::cerr << "[HOOK THROW] " << msg << std::endl;
                    result_json["success"] = false; result_json["error"] = msg;
                    return result_json;
                }
                if (!bat_r.valid()) {
                    sol::error e = bat_r;
                    std::string msg = std::string("before_ai_turn: ") + e.what();
                    push_script_error(msg);
                    std::cerr << "[HOOK ERROR] " << msg << std::endl;
                    result_json["success"] = false;
                    result_json["error"]   = msg;
                    return result_json;
                }
                if (bat_r.valid() && bat_r.get_type() == sol::type::table) {
                    sol::table bt = bat_r;
                    if (bt.get_or("skip_llm", false)) {
                        std::string pre_narr = bt.get_or<std::string>("narration", "");
                        std::string pre_snap = lua["get_state_snapshot"]();
                        std::string turn_ts  = utc_timestamp();
                        hist.push_back({"user",      player_input, "player", ""});
                        hist.push_back({"assistant", pre_narr,     "gm",     turn_ts});
                        write_turn(resolve_save_path(cfg.saveFile), fstream, cfg.saveMode,
                                   player_input, pre_narr, pre_narr, pre_snap, hist);
                        web_last_llm_reply    = pre_narr;
                        web_last_player_input = player_input;
                        result_json["success"]           = true;
                        result_json["narration"]         = pre_narr;
                        result_json["display"]           = lua["get_display_state"]().get<std::string>();
                        result_json["game_over"]         = false;
                        result_json["suggested_actions"] = json::array();
                        result_json["actions"]           = json::array();
                        result_json["game_over_reason"]  = "";
                        return result_json;
                    }
                }
            }

            if (script_has_tools) {
                static int web_tool_turn = 0;
                std::cerr << "\033[2m── TURNO " << ++web_tool_turn
                          << " ──────────────────────────────────────\033[0m\n";
            }
            for (int attempt = 0; attempt < cfg.maxRetries; ++attempt) {
                auto trimmed  = trim_history(hist, cfg.maxHistory);
                // Tools run only on the first attempt: they mutate Lua state as a
                // side effect, so retrying the full tool loop after a JSON-validation
                // failure would apply those changes twice. Retries re-ask for valid
                // narration via plain schema mode (state already updated).
                std::string reply = (script_has_tools && attempt == 0)
                    ? query_llm_with_tools(cfg.provider, with_lang(sys_prompt), trimmed,
                                           user_prompt, schema, active_tools,
                                           execute_tool, cfg.activeModel())
                    : query_llm(cfg.provider, with_lang(sys_prompt), trimmed,
                                user_prompt, schema, cfg.activeModel());

                sol::protected_function pf = lua["process_ai_response"];
                sol::function tb           = lua["debug"]["traceback"];
                sol::protected_function_result pfr = pf(reply, tb);

                if (!pfr.valid()) {
                    sol::error err = pfr;
                    result_json["error"] = std::string(err.what());
                    user_prompt += "\n\nERROR: " + result_json["error"].get<std::string>()
                                 + "\nFix the JSON.";
                    continue;
                }
                sol::table res = pfr;
                if (!res["success"].get<bool>()) {
                    result_json["error"] = res["error"].get<std::string>();
                    user_prompt += "\n\nERROR: " + result_json["error"].get<std::string>()
                                 + "\nFix the JSON.";
                    continue;
                }

                // Extract response fields with safe defaults: a script's
                // process_ai_response may legitimately omit game_over/_reason
                // (e.g. it only returns {success, narration}). Calling .get<T>()
                // on a missing/nil field throws and escapes as a non-std error.
                std::string narration = res.get_or<std::string>("narration", "");
                bool game_over        = res.get_or("game_over", false);
                std::string go_reason = res.get_or<std::string>("game_over_reason", "");
                std::string display   = lua["get_display_state"]();
                std::string snap      = lua["get_state_snapshot"]();

                // Optional: suggested_actions (array of strings from script)
                json suggested = json::array();
                sol::object sa_obj = res["suggested_actions"];
                if (sa_obj.valid() && sa_obj.get_type() == sol::type::table) {
                    sol::table sa_tbl = sa_obj.as<sol::table>();
                    for (auto& kv : sa_tbl) {
                        if (kv.second.get_type() == sol::type::string)
                            suggested.push_back(kv.second.as<std::string>());
                    }
                }

                // Optional: actions (array of {type, ...} tables from script)
                json actions = json::array();
                sol::object act_obj = res["actions"];
                if (act_obj.valid() && act_obj.get_type() == sol::type::table) {
                    sol::table act_tbl = act_obj.as<sol::table>();
                    for (auto& kv : act_tbl) {
                        if (kv.second.get_type() == sol::type::table) {
                            sol::table item = kv.second.as<sol::table>();
                            json action;
                            for (auto& field : item) {
                                if (field.first.get_type() != sol::type::string) continue;
                                std::string key = field.first.as<std::string>();
                                if (field.second.get_type() == sol::type::string)
                                    action[key] = field.second.as<std::string>();
                                else if (field.second.get_type() == sol::type::number)
                                    action[key] = field.second.as<double>();
                                else if (field.second.get_type() == sol::type::boolean)
                                    action[key] = field.second.as<bool>();
                            }
                            if (!action.empty()) actions.push_back(action);
                        }
                    }
                }

                std::string turn_ts = utc_timestamp();
                hist.push_back({"user",      player_input, "player", ""});
                hist.push_back({"assistant", reply,        "gm",     turn_ts});

                write_turn(resolve_save_path(cfg.saveFile), fstream, cfg.saveMode,
                           player_input, reply, narration, snap, hist);

                web_last_llm_reply    = reply;
                web_last_player_input = player_input;

                // Hook: after_ai_turn(narration, raw_reply) — optional.
                // Called after LLM response is validated and saved.
                // May modify Lua state for side-effects (NPC movement, event scheduling, etc.).
                sol::protected_function aat_fn = lua["after_ai_turn"];
                if (aat_fn.valid()) {
                    sol::protected_function_result aat_r = aat_fn(narration, reply);
                    if (!aat_r.valid()) {
                        sol::error e = aat_r;
                        std::string msg = std::string("after_ai_turn: ") + e.what();
                        push_script_error(msg);
                        std::cerr << "[HOOK ERROR] " << msg << std::endl;
                    }
                }

                result_json["success"]            = true;
                result_json["narration"]           = narration;
                result_json["display"]             = display;
                result_json["game_over"]           = game_over;
                result_json["suggested_actions"]   = suggested;
                result_json["actions"]             = actions;
                result_json["game_over_reason"] = go_reason;
                return result_json;
            }

            if (!result_json.contains("error"))
                result_json["error"] = "LLM failed after max retries";
            return result_json;
        };

        crow::App<CsrfGuard> app;

        // -----------------------------------------------------------------
        // GET /  →  UI HTML (da web_page.h)
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/")([]() {
            crow::response res;
            res.set_header("Content-Type", "text/html; charset=utf-8");
            res.body = main_page;
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/scripts  →  lista .lua in basePath
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/scripts")([&]() {
            json result;
            json arr = json::array();
            try {
                if (std::filesystem::is_directory(cfg.basePath)) {
                    for (const auto& e : std::filesystem::directory_iterator(cfg.basePath)) {
                        if (e.is_regular_file() && e.path().extension() == ".lua") {
                            std::string fn = e.path().filename().string();
                            if (fn[0] != '_') arr.push_back(fn);
                        }
                    }
                    std::sort(arr.begin(), arr.end());
                }
                result["success"] = true;
                result["scripts"] = arr;
                result["path"]    = cfg.basePath;
            } catch (const std::exception& ex) {
                result["success"] = false;
                result["error"]   = std::string(ex.what());
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/capabilities?script=<f>  →  render modes a script supports,
        // WITHOUT loading it (static text scan, zero side effects — see plan
        // §Rischi). chat always; tile if get_visual_world defined; vn if
        // get_vn_scene defined. Used by the Sessione hub to show mode badges.
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/capabilities")([&](const crow::request& req) {
            json result;
            std::string script = req.url_params.get("script") ? req.url_params.get("script") : "";
            if (script.empty() || script.find('/') != std::string::npos
                || script.find('\\') != std::string::npos
                || script.find("..") != std::string::npos) {
                result["success"] = false; result["error"] = "Invalid script name";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            std::ifstream f(cfg.basePath + script);
            if (!f.good()) {
                result["success"] = false; result["error"] = "Script not found";
                crow::response res(404, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            std::stringstream ss; ss << f.rdbuf();
            std::string src = ss.str();
            auto has = [&](const std::string& fn) {
                return src.find("function " + fn) != std::string::npos
                    || src.find(fn + " =") != std::string::npos
                    || src.find(fn + "=")  != std::string::npos;
            };
            // Also check if a VN catalog exists for this script — adventure.lua
            // adventures install get_vn_scene dynamically (not visible in source)
            // but the catalog file is the authoritative persistence indicator.
            bool vn_by_catalog = false;
            {
                std::string stem = std::filesystem::path(script).stem().string();
                std::error_code ec2;
                std::filesystem::path asset_root = cfg.assetRoot.empty()
                    ? std::filesystem::current_path(ec2)
                    : std::filesystem::absolute(cfg.assetRoot, ec2);
                std::filesystem::path cat = asset_root / "catalog" / (stem + "_vn.json");
                vn_by_catalog = std::filesystem::exists(cat, ec2);
            }
            json modes = json::array();
            modes.push_back("chat");
            if (has("get_visual_world")) modes.push_back("tile");
            if (has("get_vn_scene") || vn_by_catalog) modes.push_back("vn");
            result["success"] = true;
            result["modes"]   = modes;
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json"); return res;
        });

        // -----------------------------------------------------------------
        // GET /api/saves  →  lista .jsonl in savePath (o cwd)
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/saves")([&]() {
            json result;
            json arr = json::array();
            try {
                std::string dir = cfg.savePath.empty() ? "." : cfg.savePath;
                std::filesystem::create_directories(dir);
                for (const auto& e : std::filesystem::directory_iterator(dir)) {
                    if (e.is_regular_file() && e.path().extension() == ".jsonl")
                        arr.push_back(e.path().filename().string());
                }
                std::sort(arr.begin(), arr.end(), std::greater<std::string>());
                result["success"] = true;
                result["saves"]   = arr;
            } catch (const std::exception& ex) {
                result["success"] = false;
                result["error"]   = std::string(ex.what());
                result["saves"]   = json::array();
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/status
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/status")([&]() {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            if (session_state == SessionState::IDLE) {
                result["success"] = false;
                result["error"]   = "No active session.";
            } else {
                result["success"]  = true;
                result["display"]  = lua["get_display_state"]().get<std::string>();
                result["snapshot"] = lua["get_state_snapshot"]().get<std::string>();
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/start  →  load script, show welcome, wait for /api/init
        // Body: { "script": "my_adventure.lua" }
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/start").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            try {
                auto body = json::parse(req.body);
                std::string script_name = body.value("script", "");

                if (script_name.empty()) {
                    result["success"] = false;
                    result["error"]   = "Field 'script' is required.";
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                if (script_name.find('/') != std::string::npos ||
                    script_name.find('\\') != std::string::npos ||
                    script_name.find("..") != std::string::npos) {
                    result["success"] = false;
                    result["error"]   = "Invalid script name.";
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }

                cfg.script = script_name;

                // Each new start gets its own file: scriptname_MMDD_HHMM.jsonl
                {
                    std::string base = script_name;
                    auto dot = base.rfind(".lua");
                    if (dot != std::string::npos) base = base.substr(0, dot);
                    cfg.saveFile = base + "_" + local_session_id() + ".jsonl";
                }
                // Close any previously open FULL-mode stream.
                if (full_stream.is_open()) full_stream.close();

                clear_optional_script_globals(lua);
                lua.script_file(cfg.basePath + cfg.script);
                load_script_tools(lua);

                chat_history.clear();
                web_last_llm_reply.clear();
                web_last_player_input.clear();
                session_state = SessionState::IDLE;

                std::string welcome = lua["get_welcome_message"]();
                session_state = SessionState::AWAITING_INIT;
                active_script = script_name;
                g_active_script_stem = script_stem(script_name);

                result["success"]    = true;
                result["welcome"]    = welcome;
                result["needs_init"] = true;
                result["display"]    = "";
                result["save_file"]  = cfg.saveFile;

                // Render modes the just-loaded script supports. chat is always
                // available; tile needs get_visual_world; vn needs get_vn_scene.
                // A script may override with get_modes() → ["chat","tile","vn"].
                {
                    json modes = json::array();
                    sol::object gm = lua["get_modes"];
                    if (gm.valid() && gm.get_type() == sol::type::function) {
                        sol::protected_function fn = gm;
                        auto mr = fn();
                        if (mr.valid() && mr.get_type() == sol::type::table) {
                            for (auto& kv : mr.get<sol::table>())
                                if (kv.second.is<std::string>()) modes.push_back(kv.second.as<std::string>());
                        }
                    }
                    if (modes.empty()) {
                        modes.push_back("chat");
                        if (lua["get_visual_world"].valid()) modes.push_back("tile");
                        if (lua["get_vn_scene"].valid())     modes.push_back("vn");
                    }
                    result["modes"] = modes;
                }

                // Optional scripted character-creation questionnaire.
                // Lua: get_character_questions() → array of
                //   { field=, prompt=, type="text"|"choice", options={...} }
                // Absent → client falls back to the single-name init prompt.
                sol::object cq = lua["get_character_questions"];
                if (cq.valid() && cq.get_type() == sol::type::function) {
                    sol::protected_function fn = cq;
                    sol::protected_function_result qr = fn();
                    if (qr.valid()) {
                        sol::object qo = qr;
                        if (qo.is<sol::table>()) {
                            json qjson = json::array();
                            sol::table qt = qo.as<sol::table>();
                            for (std::size_t i = 1; i <= qt.size(); ++i) {
                                sol::object e = qt[i];
                                if (!e.is<sol::table>()) continue;
                                sol::table q = e.as<sol::table>();
                                json item;
                                item["field"]  = q.get_or<std::string>("field", "");
                                item["prompt"] = q.get_or<std::string>("prompt", "");
                                item["type"]   = q.get_or<std::string>("type", "text");
                                sol::object opts = q["options"];
                                if (opts.is<sol::table>()) {
                                    json arr = json::array();
                                    sol::table ot = opts.as<sol::table>();
                                    for (std::size_t j = 1; j <= ot.size(); ++j)
                                        arr.push_back(ot.get_or<std::string>(j, ""));
                                    item["options"] = arr;
                                }
                                qjson.push_back(item);
                            }
                            if (!qjson.empty()) result["questions"] = qjson;
                        }
                    }
                }

            } catch (const std::exception& ex) {
                result["success"] = false;
                result["error"]   = std::string(ex.what());
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/init  →  first player response (name or empty = default)
        // Body: { "input": "Marco" }  oppure  { "input": "" }
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/init").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            if (session_state != SessionState::AWAITING_INIT) {
                result["success"] = false;
                result["error"]   = "No session waiting for initialisation.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            try {
                auto body        = json::parse(req.body);
                std::string init_input = body.value("input", "");

                if (init_input.empty()) lua["generate_initial_state"]();
                else                    lua["set_initial_state"](init_input);

                // set_config has now run → if this adventure was previously converted
                // to VN (catalog exists), re-enable VN automatically (persistence).
                maybe_autoscaffold_vn(lua);

                // Open full_stream if in FULL save mode
                if (cfg.saveMode == SaveMode::FULL && !full_stream.is_open()) {
                    full_stream.open(resolve_save_path(cfg.saveFile), std::ios::app);
                }

                session_state = SessionState::PLAYING;

                // Optional LLM-narrated arrival scene. Lua: generate_arrival()
                // → narration string (master owns the prompt: weaves player
                // traits + present/just-generated NPCs). Shown as turn 0 and
                // persisted like a normal turn so undo/reload see it.
                std::string arrival;
                sol::object ga = lua["generate_arrival"];
                if (ga.valid() && ga.get_type() == sol::type::function) {
                    sol::protected_function fn = ga;
                    sol::protected_function_result ar = fn();
                    if (ar.valid()) {
                        sol::object ao = ar;
                        if (ao.is<std::string>()) arrival = ao.as<std::string>();
                    } else {
                        sol::error err = ar;
                        push_script_error(std::string("generate_arrival: ") + err.what());
                    }
                }
                if (!arrival.empty()) {
                    std::string arr_ts = utc_timestamp();
                    chat_history.push_back({"assistant", arrival, "gm", arr_ts});
                    web_last_llm_reply    = arrival;
                    web_last_player_input = "";
                    std::string snap;
                    try { snap = lua["get_state_snapshot"]().get<std::string>(); } catch (...) {}
                    write_turn(resolve_save_path(cfg.saveFile), full_stream, cfg.saveMode,
                               "", arrival, arrival, snap, chat_history);
                    result["arrival"] = arrival;
                }

                result["success"] = true;
                result["display"] = lua["get_display_state"]().get<std::string>();

            } catch (const std::exception& ex) {
                push_script_error(std::string(ex.what()));
                result["success"] = false;
                result["error"]   = std::string(ex.what());
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/load  →  restore session from .jsonl file
        // Body: { "save": "session_log.jsonl" }
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/load").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            try {
                auto body      = json::parse(req.body);
                std::string save_name = body.value("save", "");

                if (save_name.empty()) {
                    result["success"] = false;
                    result["error"]   = "Field 'save' is required.";
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                if (save_name.find('/') != std::string::npos ||
                    save_name.find('\\') != std::string::npos ||
                    save_name.find("..") != std::string::npos) {
                    result["success"] = false;
                    result["error"]   = "Invalid filename.";
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }

                std::string full_path = resolve_save_path(save_name);

                // Read last line to determine which script to reload
                std::ifstream probe(full_path);
                if (!probe.is_open()) {
                    result["success"] = false;
                    result["error"]   = "File not found: " + save_name;
                    crow::response res(404, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                std::string line, last_line;
                while (std::getline(probe, line)) if (!line.empty()) last_line = line;
                probe.close();

                // Look for the "script" field in the save (old saves lack it: fallback to cfg.script)
                std::string script_to_load = cfg.script;
                try {
                    auto jl = json::parse(last_line);
                    if (jl.contains("script") && jl["script"].is_string())
                        script_to_load = jl["script"].get<std::string>();
                } catch (...) {}

                cfg.script = script_to_load;
                clear_optional_script_globals(lua);
                lua.script_file(cfg.basePath + cfg.script);
                load_script_tools(lua);

                chat_history.clear();
                if (!load_session_from_jsonl(full_path, lua, chat_history)) {
                    result["success"] = false;
                    result["error"]   = "Failed to restore session.";
                    crow::response res(500, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }

                cfg.saveFile = save_name;
                if (cfg.saveMode == SaveMode::FULL && !full_stream.is_open())
                    full_stream.open(full_path, std::ios::app);

                maybe_autoscaffold_vn(lua);   // re-enable VN if previously converted

                web_last_llm_reply.clear();
                web_last_player_input.clear();
                session_state = SessionState::PLAYING;
                active_script = script_to_load;
                g_active_script_stem = script_stem(script_to_load);

                // Extract session bounds from the save file for image filtering.
                // FULL mode: multiple lines with per-turn timestamps.
                // LAST mode: single line with session_start + timestamp fields.
                std::string sess_start, sess_end;
                {
                    std::ifstream sf(full_path);
                    std::string first_line, last_line, ln;
                    while (std::getline(sf, ln))
                        if (!ln.empty()) { if (first_line.empty()) first_line = ln; last_line = ln; }
                    try {
                        auto fj = json::parse(first_line);
                        // session_start field (written by new builds); fallback to timestamp
                        sess_start = fj.value("session_start", fj.value("timestamp", ""));
                    } catch (...) {}
                    try {
                        auto lj = json::parse(last_line);
                        sess_end = lj.value("timestamp", "");
                    } catch (...) {}
                }

                result["success"]       = true;
                result["script"]        = script_to_load;
                result["display"]       = lua["get_display_state"]().get<std::string>();
                result["turns"]         = load_turns_for_replay(full_path);
                result["cached_images"] = get_cached_scene_images(script_to_load, sess_start, sess_end);

            } catch (const std::exception& ex) {
                push_script_error(std::string(ex.what()));
                result["success"] = false;
                result["error"]   = std::string(ex.what());
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/chat  →  normal game turn
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/chat").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            if (session_state != SessionState::PLAYING) {
                result["success"] = false;
                result["error"]   = session_state == SessionState::IDLE
                    ? "No active session."
                    : "Session waiting for initialisation. Send a response first.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            try {
                auto body = json::parse(req.body);
                std::string input_text = body.value("input", "");
                if (input_text.empty()) {
                    result["success"] = false;
                    result["error"]   = "Empty input.";
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                sol::table cmd_result = lua["process_player_input"](input_text);
                bool cmd_handled = cmd_result["success"].get<bool>() &&
                                   cmd_result["handled"].get<bool>();
                // Capture undo checkpoint before turn executes
                std::string undo_snap = lua["get_state_snapshot"]();
                auto        undo_hist = chat_history;
                result = run_turn(input_text, cmd_handled, chat_history, full_stream);
                if (result.value("success", false)) {
                    if (undo_stack.size() >= MAX_UNDO_STEPS) undo_stack.pop_front();
                    undo_stack.push_back({std::move(undo_snap), std::move(undo_hist)});
                }
            } catch (const std::exception& ex) {
                push_script_error(std::string(ex.what()));
                result["success"] = false;
                result["error"]   = std::string(ex.what());
            } catch (...) {
                push_script_error("Unknown non-std exception in /api/chat turn.");
                result["success"] = false;
                result["error"]   = "Internal error during turn (non-std exception).";
            }
            // dump() with the replace handler so invalid UTF-8 in a narration or a
            // Lua error message can never throw here and turn the reply into a
            // non-JSON 500 (frontend: "string did not match the expected pattern").
            crow::response res(result.dump(-1, ' ', false,
                                           json::error_handler_t::replace));
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/command  →  comandi /xxx
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/command").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            if (session_state != SessionState::PLAYING) {
                result["success"] = false;
                result["error"]   = "No active session.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            try {
                auto body = json::parse(req.body);
                std::string cmd_text  = body.value("input", "");
                std::string lower_cmd = cmd_text;
                std::transform(lower_cmd.begin(), lower_cmd.end(), lower_cmd.begin(), ::tolower);
                auto first_ch = lower_cmd.find_first_not_of(" \t");
                lower_cmd = (first_ch == std::string::npos) ? "" : lower_cmd.substr(first_ch);

                // ---- /status ----
                if (lower_cmd == "/status") {
                    result["success"]  = true;
                    result["output"]   = lua["get_state_snapshot"]().get<std::string>();
                    result["display"]  = lua["get_display_state"]().get<std::string>();
                    crow::response res(result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }

                // ---- /save ----
                if (lower_cmd == "/save") {
                    write_turn(resolve_save_path(cfg.saveFile), full_stream, cfg.saveMode,
                               "[manual save]", "", "", lua["get_state_snapshot"](), chat_history);
                    result["success"] = true;
                    result["output"]  = "Session saved to: " + cfg.saveFile;
                    result["display"] = lua["get_display_state"]().get<std::string>();
                    crow::response res(result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }

                // ---- /summary [N] ----
                bool is_riassunto = (lower_cmd == "/summary" || lower_cmd == "/summarize"
                    || lower_cmd.rfind("/summary ", 0) == 0);

                if (is_riassunto) {
                    int keep_recent = 4;
                    auto sp = cmd_text.find(' ');
                    if (sp != std::string::npos) {
                        try { keep_recent = std::stoi(cmd_text.substr(sp + 1)) * 2; }
                        catch (...) {}
                    }
                    keep_recent  = std::max(0, std::min(keep_recent, (int)chat_history.size()));
                    int to_summarize = (int)chat_history.size() - keep_recent;
                    if (to_summarize <= 0) {
                        result["success"] = true;
                        result["output"]  = "Not enough messages to summarise.";
                        result["display"] = lua["get_display_state"]().get<std::string>();
                        crow::response res(result.dump());
                        res.set_header("Content-Type", "application/json"); return res;
                    }
                    std::string history_text;
                    for (int i = 0; i < to_summarize; ++i) {
                        const auto& msg = chat_history[i];
                        if (msg.role == "user") {
                            history_text += "Player: " + msg.content + "\n";
                        } else {
                            try {
                                auto jj = json::parse(msg.content);
                                history_text += "Narrator: " + jj.value("narration", msg.content) + "\n";
                            } catch (...) {
                                history_text += "Narrator: " + msg.content + "\n";
                            }
                        }
                    }
                    std::string sum_sys =
                        "You are the narrator of a role-playing game. Write a concise "
                        "and compelling summary of the events in chronological order, in second person. "
                        "Narrative text only, no lists or structure.";
                    std::string sum_user =
                        "Here is the game story so far:\n\n" + history_text +
                        "\n\nWrite a narrative summary of the main events.";
                    std::string summary = query_llm(cfg.provider, with_lang(sum_sys), {}, sum_user, "",
                                                    cfg.activeModel());
                    std::vector<Message> recent_tail(
                        chat_history.end() - keep_recent, chat_history.end());
                    chat_history.clear();
                    chat_history.push_back({"assistant",
                        "[SUMMARY OF PREVIOUS EVENTS]\n" + summary, "gm"});
                    for (auto& m : recent_tail) chat_history.push_back(m);
                    result["success"] = true;
                    result["output"]  = "[SUMMARY]\n" + summary + "\n\n— History compressed: "
                        + std::to_string(to_summarize) + " messages -> 1 summary + "
                        + std::to_string(keep_recent) + " recent.";
                    result["display"] = lua["get_display_state"]().get<std::string>();
                    crow::response res(result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }

                // ---- /fix <istruzione> ----
                if (lower_cmd.rfind("/fix", 0) == 0) {
                    std::string fix_request;
                    auto sp = cmd_text.find(' ');
                    if (sp != std::string::npos) fix_request = cmd_text.substr(sp + 1);
                    else fix_request = "correct any narrative or continuity errors";
                    if (web_last_llm_reply.empty()) {
                        result["success"] = true;
                        result["output"]  = "No previous response to correct.";
                        result["display"] = lua["get_display_state"]().get<std::string>();
                        crow::response res(result.dump());
                        res.set_header("Content-Type", "application/json"); return res;
                    }
                    std::string prev_narration;
                    try {
                        auto jj = json::parse(web_last_llm_reply);
                        prev_narration = jj.value("narration", "");
                    } catch (...) { prev_narration = web_last_llm_reply; }
                    std::string cur_state  = lua["get_status_for_ai"]();
                    std::string sys_prompt = lua["get_system_prompt"]();
                    std::string schema     = lua["get_json_schema"]();
                    std::string fix_sys = sys_prompt +
                        "\n\nIMPORTANT - CORRECTION MODE: You are rewriting an already narrated scene. "
                        "Do NOT advance time (avanza_tempo = 0). "
                        "Keep the same JSON schema but produce a corrected narration "
                        "according to the given instructions.";
                    std::string fix_user =
                        "Current state:\n" + cur_state +
                        "\n\nPrevious narration to correct:\n" + prev_narration +
                        "\n\nRequested correction: " + fix_request +
                        "\n\nRewrite the scene with the correction applied. "
                        "Time does not advance. Reply ONLY with the specified JSON.";
                    std::vector<Message> hist_no_last = chat_history;
                    if (hist_no_last.size() >= 2) {
                        hist_no_last.pop_back();
                        hist_no_last.pop_back();
                    }
                    bool fix_ok = false;
                    for (int attempt = 0; attempt < cfg.maxRetries && !fix_ok; ++attempt) {
                        auto trimmed = trim_history(hist_no_last, cfg.maxHistory);
                        std::string fix_reply = query_llm(cfg.provider, with_lang(fix_sys), trimmed,
                                                          fix_user, schema, cfg.activeModel());
                        sol::protected_function pf = lua["process_ai_response"];
                        sol::function tb           = lua["debug"]["traceback"];
                        sol::protected_function_result pfr = pf(fix_reply, tb);
                        if (!pfr.valid()) {
                            sol::error err = pfr;
                            fix_user += "\n\nERROR: " + std::string(err.what()) + "\nFix the JSON.";
                            continue;
                        }
                        sol::table res_lua = pfr;
                        if (!res_lua["success"].get<bool>()) {
                            fix_user += "\n\nERROR: " + res_lua["error"].get<std::string>() + "\nFix the JSON.";
                            continue;
                        }
                        fix_ok = true;
                        std::string narration = res_lua["narration"].get<std::string>();
                        std::string display   = lua["get_display_state"]();
                        std::string snap      = lua["get_state_snapshot"]();
                        if (chat_history.size() >= 2) {
                            chat_history.pop_back();
                            chat_history.pop_back();
                        }
                        std::string fix_ts = utc_timestamp();
                        chat_history.push_back({"user",      web_last_player_input, "player", ""});
                        chat_history.push_back({"assistant", fix_reply,             "gm",     fix_ts});
                        web_last_llm_reply = fix_reply;
                        write_turn(resolve_save_path(cfg.saveFile), full_stream, cfg.saveMode,
                                   web_last_player_input + " [FIX: " + fix_request + "]",
                                   fix_reply, narration, snap, chat_history);
                        result["success"]         = true;
                        result["narration"]        = narration;
                        result["display"]          = display;
                        result["game_over"]        = res_lua["game_over"].get<bool>();
                        result["game_over_reason"] = res_lua["game_over_reason"].get<std::string>();
                        crow::response res(result.dump());
                        res.set_header("Content-Type", "application/json"); return res;
                    }
                    result["success"] = false;
                    result["error"]   = "Fix failed after " + std::to_string(cfg.maxRetries) + " attempts.";
                    crow::response res(result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }

                // ---- /osserva [argomento] ----
                if (lower_cmd.rfind("/observe", 0) == 0) {
                    std::string subject;
                    auto sp = cmd_text.find(' ');
                    if (sp != std::string::npos) subject = cmd_text.substr(sp + 1);
                    std::string cur_state  = lua["get_status_for_ai"]();
                    std::string sys_prompt = lua["get_system_prompt"]();
                    std::string obs_sys = sys_prompt +
                        "\n\nOBSERVATION MODE: Describe a scene element in detail. "
                        "Do not advance time, do not modify game state. "
                        "Reply in FREE TEXT (not JSON), with a rich sensory description, "
                        "at least 4 evocative sentences.";
                    std::string obs_user = subject.empty()
                        ? "Current state:\n" + cur_state +
                          "\n\nDescribe the current scene: environment, atmosphere, characters present, "
                          "sounds, smells, everything the protagonist perceives."
                        : "Current state:\n" + cur_state +
                          "\n\nDescribe in detail: " + subject +
                          "\nPhysical, emotional and sensory aspects. What does the protagonist perceive?";
                    std::string obs = query_llm(cfg.provider, with_lang(obs_sys),
                                                trim_history(chat_history, cfg.maxHistory),
                                                obs_user, "", cfg.activeModel());
                    result["success"] = true;
                    result["output"]  = obs;
                    result["display"] = lua["get_display_state"]().get<std::string>();
                    crow::response res(result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }

                // ---- /cost — token usage report ----
                if (lower_cmd == "/cost") {
                    std::lock_guard<std::mutex> lk(g_llm_token_mutex);
                    long long grand = 0;
                    struct Row { std::string label; long long total, calls; };
                    std::vector<Row> rows;
                    for (auto& kv : g_llm_token_usage) {
                        rows.push_back({kv.first, kv.second.total, kv.second.calls});
                        grand += kv.second.total;
                    }
                    std::sort(rows.begin(), rows.end(),
                              [](const Row& a, const Row& b){ return a.total > b.total; });
                    std::string out = "TOKEN per componente:\n";
                    char buf[256];
                    for (auto& r : rows) {
                        double pct = grand > 0 ? 100.0 * r.total / grand : 0.0;
                        long long avg = r.calls > 0 ? r.total / r.calls : 0;
                        std::snprintf(buf, sizeof(buf),
                            "  %-12s %8lld tok  (%4.1f%%)  in %lld chiamate (media %lld/chiamata)\n",
                            r.label.c_str(), r.total, pct, r.calls, avg);
                        out += buf;
                    }
                    std::snprintf(buf, sizeof(buf),
                        "  %-12s %8lld tok totali", "TOTALE", grand);
                    out += buf;
                    result["success"] = true;
                    result["output"]  = out;
                    result["display"] = lua["get_display_state"]().get<std::string>();
                    crow::response res(result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }

                // ---- Delegate to Lua (script-specific commands) ----
                sol::table cmd_result = lua["process_player_input"](cmd_text);
                if (!cmd_result["success"].get<bool>()) {
                    result["success"] = false;
                    result["error"]   = cmd_result["error"].get<std::string>();
                    crow::response res(result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                if (cmd_result["handled"].get<bool>()) {
                    sol::optional<std::string> out        = cmd_result["output"];
                    sol::optional<std::string> scene_path = cmd_result["scene_path"];
                    sol::optional<sol::table>  scene_paths= cmd_result["scene_paths"];
                    bool scene_loop = cmd_result["scene_loop"].get_or(false);
                    result["success"] = true;
                    result["output"]  = out ? *out : "";
                    result["display"] = lua["get_display_state"]().get<std::string>();
                    if (scene_paths) {
                        // Slideshow: return images array
                        json imgs = json::array();
                        for (auto& kv : *scene_paths) {
                            std::string fp = kv.second.as<std::string>();
                            if (!std::filesystem::path(fp).is_absolute())
                                fp = cfg.basePath + fp;
                            if (std::filesystem::exists(fp)) {
                                std::ifstream imgf(fp, std::ios::binary);
                                std::vector<uint8_t> imgbytes(
                                    std::istreambuf_iterator<char>(imgf), {});
                                imgs.push_back(bytes_to_base64(imgbytes));
                            }
                        }
                        result["images"] = imgs;
                        result["mime"]   = "image/png";
                        result["loop"]   = scene_loop;
                    } else if (scene_path) {
                        std::string fp = *scene_path;
                        if (!std::filesystem::path(fp).is_absolute())
                            fp = cfg.basePath + fp;
                        if (std::filesystem::exists(fp)) {
                            std::ifstream imgf(fp, std::ios::binary);
                            std::vector<uint8_t> imgbytes(
                                std::istreambuf_iterator<char>(imgf), {});
                            result["image"]    = bytes_to_base64(imgbytes);
                            result["mime"]     = "image/png";
                            result["asset_id"] = "scene";
                        }
                    }
                } else {
                    // Slash command not recognised by engine or script — never fall through to LLM
                    result["success"] = false;
                    result["error"]   = "Unknown command: " + cmd_text
                                        + "\nType /commands to see available commands.";
                    crow::response res(result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }

            } catch (const std::exception& ex) {
                push_script_error(std::string(ex.what()));
                result["success"] = false;
                result["error"]   = std::string(ex.what());
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/save  →  manual save
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/save").methods("POST"_method)([&]() {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            if (session_state == SessionState::IDLE) {
                result["success"] = false;
                result["error"]   = "No active session.";
            } else {
                write_turn(resolve_save_path(cfg.saveFile), full_stream, cfg.saveMode,
                           "[manual save]", "", "", lua["get_state_snapshot"](), chat_history);
                result["success"] = true;
                result["message"] = "Saved to: " + cfg.saveFile;
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/undo  →  revert last turn (up to MAX_UNDO_STEPS)
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/undo").methods("POST"_method)([&]() {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            if (session_state != SessionState::PLAYING) {
                result["success"] = false;
                result["error"]   = "No active session.";
            } else if (undo_stack.empty()) {
                result["success"] = false;
                result["error"]   = "Nothing to undo.";
            } else {
                auto snap = undo_stack.back().first;
                auto hist = undo_stack.back().second;
                sol::protected_function rs = lua["restore_state"];
                sol::protected_function_result rr = rs(snap);
                if (!rr.valid()) {
                    // Leave the checkpoint on the stack — restore failed, nothing changed.
                    sol::error e = rr;
                    result["success"] = false;
                    result["error"]   = std::string("restore failed: ") + e.what();
                } else {
                    chat_history = hist;
                    undo_stack.pop_back();
                    // Do NOT clear web_last_llm_reply / web_last_player_input:
                    // they keep the undone turn's data so /fix can still correct it.
                    result["success"] = true;
                    result["display"] = lua["get_display_state"]().get<std::string>();
                }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // =================================================================
        // IMAGE JOB SYSTEM
        // Ogni /generate_asset e /image asincrona usa questo sistema.
        // Jobs are kept in memory for the entire session.
        // =================================================================

        // Windows headers define ERROR as a macro — undef before the enum
#ifdef ERROR
#undef ERROR
#endif
        struct ImageJob {
            enum class State { PENDING, DONE, ERROR };
            State       state   = State::PENDING;
            std::string image_b64;   // risultato (base64 PNG)
            std::string error;
            std::string asset_id;    // id asset coinvolto (per /generate_asset)
            std::string prompt;      // visual prompt usato per la generazione
            std::string utc_at;      // timestamp completamento (UTC ISO)
            std::string mime;        // image mime type
        };

        // static: detached image-job threads capture these by reference and may
        // still be running when the webMode block unwinds at shutdown. Static
        // storage outlives the block, avoiding use-after-free.
        static std::mutex                          img_jobs_mutex;
        static std::map<std::string, ImageJob>     img_jobs;
        static std::atomic<int>                    img_job_counter{0};

        auto new_job_id = [&]() -> std::string {
            return "imgjob_" + std::to_string(++img_job_counter);
        };

        // CoderAI async image-edit jobs (detached threads, static storage)
        struct CoderImgJob {
            enum class St { RUNNING, DONE, ERROR } status = St::RUNNING;
            std::string output_path;
            std::string error;
        };
        static std::mutex                             coder_img_mutex;
        static std::map<std::string, CoderImgJob>     coder_img_jobs;
        static std::atomic<int>                       coder_img_counter{0};

        // Lancia la generazione su un thread separato e aggiorna il job.
        // La lambda restituisce {bytes, prompt}: il prompt viene esposto nel job
        // per il tooltip nell'UI.
        auto launch_image_job = [&](const std::string& job_id,
                                    std::function<std::pair<std::vector<uint8_t>, std::string>()> fn) {
            // img_jobs / img_jobs_mutex have static storage — referenced directly,
            // not captured (capturing statics is ill-formed and they outlive threads).
            std::thread([job_id, fn = std::move(fn)]() {
                try {
                    auto [bytes, prompt] = fn();
                    std::string b64 = bytes_to_base64(bytes);
                    std::lock_guard<std::mutex> lk(img_jobs_mutex);
                    img_jobs[job_id].image_b64 = std::move(b64);
                    img_jobs[job_id].prompt    = std::move(prompt);
                    img_jobs[job_id].utc_at    = utc_timestamp();
                    img_jobs[job_id].state     = ImageJob::State::DONE;
                } catch (const std::exception& ex) {
                    std::lock_guard<std::mutex> lk(img_jobs_mutex);
                    img_jobs[job_id].error = ex.what();
                    img_jobs[job_id].state = ImageJob::State::ERROR;
                }
            }).detach();
        };

        // -----------------------------------------------------------------
        // GET /api/image/job/{id}  →  polling stato job immagine
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/image/job/<string>")([&](const std::string& job_id) {
            std::lock_guard<std::mutex> lk(img_jobs_mutex);
            json result;
            auto it = img_jobs.find(job_id);
            if (it == img_jobs.end()) {
                result["status"] = "not_found";
                crow::response res(404, result.dump());
                res.set_header("Content-Type", "application/json");
                return res;
            }
            const auto& job = it->second;
            switch (job.state) {
                case ImageJob::State::PENDING:
                    result["status"] = "pending";
                    break;
                case ImageJob::State::DONE:
                    result["status"] = "done";
                    result["image"]  = job.image_b64;
                    if (!job.asset_id.empty()) result["asset_id"] = job.asset_id;
                    if (!job.prompt.empty())   result["prompt"]   = job.prompt;
                    if (!job.utc_at.empty())   result["utc_at"]   = job.utc_at;
                    if (!job.mime.empty())     result["mime"]     = job.mime;
                    break;
                case ImageJob::State::ERROR:
                    result["status"] = "error";
                    result["error"]  = job.error;
                    break;
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/image
        // Generate the current scene image.
        // Optional body: { "partial": true }  → proceeds even with missing assets
        //
        // Immediate response:
        //   { "success": true, "job_id": "imgjob_1" }                    → job started
        //   { "success": false, "missing": [...], "available": [...] } → missing assets
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/image").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;

            if (!cfg.imgEnabled) {
                result["success"] = false;
                result["error"]   = "Image generation not enabled. Use --img-provider.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            if (session_state != SessionState::PLAYING) {
                result["success"] = false;
                result["error"]   = "No active session.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            bool partial = false;
            bool apply_lora = false;
            std::string img_mode;         // "" | "regen" | "refine" | "fix" | "compose"
            std::string img_instruction;  // user instruction for "fix" mode
            float img_strength_override = -1.0f; // <0 = use img_cfg.strength
            try {
                if (!req.body.empty()) {
                    auto body    = json::parse(req.body);
                    partial      = body.value("partial", false);
                    apply_lora   = body.value("lora", false);
                    img_mode     = body.value("mode", "");
                    img_instruction = body.value("instruction", "");
                }
            } catch (...) {}

            // Strip --strength / --s <value> from fix instruction
            if (img_mode == "fix" && !img_instruction.empty()) {
                std::istringstream iss(img_instruction);
                std::string tok, rebuilt;
                std::vector<std::string> toks;
                while (iss >> tok) toks.push_back(tok);
                for (size_t i = 0; i < toks.size(); ++i) {
                    if ((toks[i] == "--strength" || toks[i] == "--s") &&
                        i + 1 < toks.size()) {
                        try { img_strength_override = std::stof(toks[i + 1]); } catch (...) {}
                        ++i; // skip value token
                    } else {
                        if (!rebuilt.empty()) rebuilt += ' ';
                        rebuilt += toks[i];
                    }
                }
                if (img_strength_override > 0.0f) img_instruction = rebuilt;
            }

            // Ask Lua for the asset list of the current scene
            sol::protected_function pf = lua["get_scene_images"];
            if (!pf.valid()) {
                result["success"] = false;
                result["error"]   = "Script does not implement get_scene_images().";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            sol::protected_function_result pfr = pf();
            if (!pfr.valid()) {
                sol::error err = pfr;
                result["success"] = false;
                result["error"]   = std::string("get_scene_images() error: ") + err.what();
                crow::response res(500, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // Parse the Lua table returned by get_scene_images().
            //
            // Supported return formats (both backward-compatible):
            //
            //   Format A — simple array (original):
            //     { {id="bg", path="..."}, {id="npc", path="..."} }
            //
            //   Format B — table with assets + optional base_image:
            //     {
            //       assets     = { {id="bg", path="..."}, ... },
            //       base_image = "last" | "/abs/path/to/image.jpg" | nil
            //     }
            //
            // base_image meaning:
            //   nil / not present  → standard collage behaviour
            //   "last"             → find the most recent cached scene with
            //                        the same asset set and use it as i2i base
            //   any other string   → treat as a file path and use directly

            // If the script returns nil, it has no scene images defined.
            if (pfr.get_type() == sol::type::lua_nil) {
                result["success"] = false;
                result["error"]   = "No scene images defined for this script. "
                                    "Implement get_scene_images() and use /generate_asset <id> "
                                    "to create assets first.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            sol::table img_table = pfr;
            std::vector<CollageEntry> available;
            json missing_list = json::array();
            std::string base_image_hint;  // empty = use collage (default)

            // Detect format: if the table has an "assets" key it is Format B
            sol::object assets_field = img_table["assets"];
            sol::table  asset_list   = img_table;  // default: iterate img_table itself

            if (assets_field.valid() && assets_field.get_type() == sol::type::table) {
                // Format B
                asset_list = assets_field.as<sol::table>();

                // Read base_image — may be nil, "last", or a path string
                sol::object bi = img_table["base_image"];
                if (bi.valid() && bi.get_type() == sol::type::string)
                    base_image_hint = bi.as<std::string>();
            }

            for (auto& kv : asset_list) {
                sol::table entry = kv.second;
                std::string id   = entry.get_or<std::string>("id",   "");
                std::string path = entry.get_or<std::string>("path", "");
                if (path.empty()) continue;

                std::string full_path = path;
                if (!std::filesystem::path(path).is_absolute())
                    full_path = cfg.basePath + path;

                if (std::filesystem::exists(full_path)) {
                    available.push_back({ full_path, id });
                } else {
                    json m;
                    m["id"]   = id;
                    m["path"] = path;
                    m["hint"] = "/generate_asset " + id;
                    missing_list.push_back(m);
                }
            }

            // Missing assets and not partial → report and stop
            if (!missing_list.empty() && !partial) {
                result["success"]   = false;
                result["missing"]   = missing_list;
                json avail_arr = json::array();
                for (const auto& e : available) avail_arr.push_back(e.tag);
                result["available"] = avail_arr;
                result["hint"]      = "Use /generate_asset <id> to generate the missing assets, "
                                      "or /image --partial to proceed with the available ones.";
                crow::response res(result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            if (available.empty()) {
                result["success"] = false;
                result["error"]   = "No images available for the current scene.";
                crow::response res(result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // Read prompt-relevant info before releasing lua_mutex
            std::string cur_state   = lua["get_status_for_ai"]();
            std::string sys_prompt  = lua["get_system_prompt"]();
            std::string last_narr   = web_last_llm_reply;
            std::string missing_note;
            if (!missing_list.empty()) {
                missing_note = " Note: the following images are missing and will not appear in the scene: ";
                for (const auto& m : missing_list)
                    missing_note += m["id"].get<std::string>() + " ";
            }

            // Optional style hook — captured while mutex is held, copied to thread
            std::string image_style;
            {
                sol::protected_function fn = lua["get_image_style"];
                if (fn.valid()) {
                    auto r = fn();
                    if (r.valid()) {
                        sol::optional<std::string> s = r;
                        if (s) image_style = *s;
                    }
                }
            }

            // Create job and launch thread (copy needed data — no Lua pointers)
            std::string job_id = new_job_id();
            {
                std::lock_guard<std::mutex> lk(img_jobs_mutex);
                img_jobs[job_id] = ImageJob{};
            }

            // Capture everything the thread needs (no Lua pointers after mutex release)
            std::vector<CollageEntry> entries_copy  = available;
            std::string sys_copy    = sys_prompt;
            std::string state_copy  = cur_state;
            std::string narr_copy   = last_narr;
            std::string miss_copy   = missing_note;
            std::string style_copy  = image_style;
            int collage_h           = img_cfg.height;
            std::string script_copy = cfg.script;
            std::string base_copy   = cfg.basePath;
            std::string hint_copy        = base_image_hint;
            std::string mode_copy        = img_mode;
            std::string instruction_copy = img_instruction;
            std::string sess_start_copy  = cfg.sessionStart;
            float       strength_copy    = img_strength_override;
            bool        lora_copy        = apply_lora;

            launch_image_job(job_id, [=]() mutable -> std::pair<std::vector<uint8_t>, std::string> {
                // Resolve i2i base image path / bytes.
                // Priority: explicit mode (regen/refine/fix/compose) > Lua hint > collage
                std::string base_image_path;
                std::vector<uint8_t> base_image_bytes;  // compose mode: bg-only collage
                bool bypass_cache = false;

                if (mode_copy == "regen") {
                    // Force fresh collage as i2i source — skip cache entirely
                    bypass_cache = true;
                    std::cerr << "[IMG] Mode: regen — bypass cache, fresh collage\n";

                } else if (mode_copy == "refine") {
                    // Use last cached render as i2i source — bypass cache key check.
                    // No session filter: same reasoning as fix — find most recent for this scene.
                    bypass_cache = true;
                    base_image_path = scene_cache::lookup_last(
                        base_copy, script_copy, entries_copy, "");
                    if (base_image_path.empty())
                        std::cerr << "[IMG] Mode: refine — no cached scene found, "
                                     "falling back to collage\n";
                    else
                        std::cerr << "[IMG] Mode: refine — using last cached: "
                                  << base_image_path << "\n";

                } else if (mode_copy == "fix") {
                    // User-provided instruction — no LLM prompt, use last cached as base.
                    // No session filter: fix should find the most recent render for this
                    // scene regardless of which session generated it.
                    bypass_cache    = true;
                    base_image_path = scene_cache::lookup_last(
                        base_copy, script_copy, entries_copy, "");
                    if (base_image_path.empty())
                        throw std::runtime_error(
                            "No cached scene image found. Run /image first.");
                    std::cerr << "[IMG] Mode: fix — instruction: "
                              << instruction_copy.substr(0, 80) << "\n";

                } else if (mode_copy == "compose") {
                    // Anti-collage mode: use ONLY the background asset (first entry) as
                    // i2i base. The model draws characters from scratch via the prompt.
                    bypass_cache = true;
                    if (!entries_copy.empty()) {
                        std::vector<CollageEntry> bg_entry = { entries_copy[0] };
                        base_image_bytes = build_collage(bg_entry, collage_h);
                        std::cerr << "[IMG] Mode: compose — bg: " << entries_copy[0].path
                                  << "  bytes=" << base_image_bytes.size() << "\n";
                    }
                    if (base_image_bytes.empty())
                        std::cerr << "[IMG] Mode: compose — WARNING: bg bytes empty, "
                                     "falling back to full collage\n";
                    // Default compose to a high strength unless the caller overrode it.
                    // Applied to the global only inside the locked region below.
                    if (strength_copy <= 0.0f)
                        strength_copy = 0.95f;
                    std::cerr << "[IMG] Mode: compose — strength=" << strength_copy << "\n";

                } else if (hint_copy == "last") {
                    // Lua script requested last-render as i2i base
                    base_image_path = scene_cache::lookup_last(
                        base_copy, script_copy, entries_copy, sess_start_copy);
                    if (base_image_path.empty())
                        std::cerr << "[IMG] base_image=last: no cached scene found, "
                                     "falling back to collage\n";
                } else if (!hint_copy.empty()) {
                    std::string resolved = hint_copy;
                    if (!std::filesystem::path(hint_copy).is_absolute())
                        resolved = base_copy + hint_copy;
                    if (std::filesystem::exists(resolved))
                        base_image_path = resolved;
                    else
                        std::cerr << "[IMG] base_image path not found: "
                                  << resolved << ", falling back to collage\n";
                }

                // Build collage — always needed as fallback and for the cache record
                auto collage = build_collage(entries_copy, collage_h);
                if (collage.empty() && base_image_path.empty())
                    throw std::runtime_error("build_collage returned empty bytes");

                // Generate visual prompt — skip LLM if mode is "fix" (use user instruction)
                std::string img_prompt;

                if (mode_copy == "fix" && !instruction_copy.empty()) {
                    img_prompt = instruction_copy;
                } else {
                    std::string tags;
                    for (const auto& e : entries_copy)
                        tags += "[" + e.tag + "] ";

                    // Collage layout prefix — tells every i2i provider this is ONE
                    // unified scene, not a grid/collage of separate panels.
                    // Background = first entry whose tag starts with "bg" or "background",
                    // or the first entry overall. Everything else = NPC characters.
                    auto is_bg_tag = [](const std::string& t) {
                        if (t.size() >= 2 && t.substr(0, 2) == "bg") return true;
                        if (t.size() >= 10 && t.substr(0, 10) == "background") return true;
                        return false;
                    };

                    std::string bg_tag;
                    int npc_count = 0;
                    for (const auto& e : entries_copy) {
                        if (bg_tag.empty() && is_bg_tag(e.tag))
                            bg_tag = e.tag;
                        else
                            ++npc_count;
                    }
                    // If no explicit bg tag, treat first entry as background
                    if (bg_tag.empty() && !entries_copy.empty()) {
                        bg_tag = entries_copy[0].tag;
                        npc_count = static_cast<int>(entries_copy.size()) - 1;
                    }

                    std::string layout_prefix =
                        "Render as a single unified photorealistic scene (NOT a collage, "
                        "NOT a grid, NOT side-by-side panels). "
                        "The background environment fills the entire frame. ";
                    if (npc_count == 1)
                        layout_prefix += "One character is naturally placed within the scene. ";
                    else if (npc_count == 2)
                        layout_prefix += "Two characters are naturally placed within the scene, "
                                         "positioned from left to right. ";
                    else if (npc_count > 2)
                        layout_prefix += std::to_string(npc_count) +
                                         " characters are naturally distributed within the scene. ";

                    /*std::string prompt_sys =
                        "You are a visual prompt engineer for Stable Diffusion and image editing models. "
                        "Given a reference image and scene context, write a concise image generation prompt.\n"
                        "STRICT RULES:\n"
                        "1. Describe ONLY visual elements: lighting, colors, composition, clothing, "
                        "setting, atmosphere, body language, facial expressions, spatial relationships.\n"
                        "2. NEVER use character names, place names, or any proper nouns — "
                        "replace them with physical descriptors "
                        "(e.g. 'young woman with dark curly hair and green eyes' instead of a name).\n"
                        "3. NEVER include dialogue, inner thoughts, or narrative text.\n"
                        "4. Use comma-separated tags and short descriptive phrases. "
                        "Photorealistic style. Max 100 words. No JSON, no lists, no quotes.";*/

                    std::string prompt_sys =
                        "You are an expert visual prompt engineer for Stable Diffusion. "
                        "Given a scene context and a reference image, write a highly descriptive image generation prompt.\n"
                        "STRICT RULES:\n"
                        "1. FOCUS ON PHYSICAL INTERACTION: You MUST explicitly translate narrative actions into precise, literal body poses. "
                        "Describe exactly where hands, faces, and limbs are positioned relative to the other characters. "
                        "Do not abstract the core action.\n"
                        "2. Describe ONLY visual elements: exact anatomical posing, physical proximity, lighting, setting, "
                        "atmosphere, and facial expressions.\n"
                        "3. NEVER use character names, place names, or proper nouns. Use physical descriptors "
                        "(e.g. 'young woman with dark hair', 'tall man').\n"
                        "4. NEVER include dialogue, inner thoughts, metaphors, narrative text, or off-screen elements (e.g., people in another room).\n"
                        "5. Define the camera angle and focal point (e.g., 'close-up on faces', 'wide shot of the room').\n"
                        "6. Format: comma-separated tags, short descriptive phrases. Photorealistic style. Max 100 words. No JSON, no lists.";

                    std::string prompt_user =
                        "Scene assets in order: " + tags + miss_copy +
                        "\n\nCurrent game state:\n" + state_copy +
                        "\n\nLast narration:\n";
                    try {
                        auto jn = json::parse(narr_copy);
                        prompt_user += jn.value("narration", narr_copy);
                    } catch (...) {
                        prompt_user += narr_copy;
                    }
                    prompt_user += "\n\nWrite the image generation prompt in English. "
                                   "Remember: no character names, visual descriptors only.";
                    if (!style_copy.empty())
                        prompt_user += "\n\nVisual style to preserve: " + style_copy;

                    img_prompt = ::query_llm(
                        ::cfg.provider, prompt_sys, {}, prompt_user, "", ::cfg.activeModel());

                    // Prepend layout prefix + append face-preservation tokens + optional style
                    img_prompt = layout_prefix + img_prompt +
                        ", preserve facial features of all characters, maintain face identity, "
                        "keep faces unchanged from reference, consistent character appearance, "
                        "high fidelity face reproduction, faithful to reference image";
                    if (!style_copy.empty())
                        img_prompt += ", " + style_copy;
                }

                std::cerr << "[IMG] Scene prompt: " << img_prompt.substr(0, 120) << "...\n";

                // image-to-image — base_image_bytes (compose) > base_image_path > collage.
                // Lock around the strength override + i2i call so concurrent renders
                // don't clobber the shared global; restore is exception-safe.
                std::vector<uint8_t> img_bytes;
                {
                    std::lock_guard<std::mutex> gen_lk(g_img_gen_mutex);
                    float saved_strength = img_cfg.strength;
                    if (strength_copy > 0.0f) {
                        img_cfg.strength = strength_copy;
                        std::cerr << "[IMG] Strength override: " << strength_copy << "\n";
                    }
                    try {
                        img_bytes = image_to_image(collage, img_prompt,
                                                   base_copy, script_copy, entries_copy,
                                                   base_image_path, bypass_cache,
                                                   sess_start_copy, base_image_bytes,
                                                   lora_copy);
                    } catch (...) {
                        img_cfg.strength = saved_strength;  // restore on error
                        throw;
                    }
                    img_cfg.strength = saved_strength;  // restore
                }
                return {std::move(img_bytes), img_prompt};
            });

            result["success"] = true;
            result["job_id"]  = job_id;
            if (!missing_list.empty()) {
                result["warning"] = "Proceeding with partial images. Missing: "
                    + std::to_string(missing_list.size());
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/generate_asset
        // Body: { "id": "elena" }
        // Calls get_asset_prompt(id) on Lua, launches txt2img, saves to disk.
        // Immediate response: { "success": true, "job_id": "imgjob_2" }
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/generate_asset").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;

            if (!cfg.imgEnabled) {
                result["success"] = false;
                result["error"]   = "Image generation not enabled. Use --img-provider.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            if (session_state != SessionState::PLAYING) {
                result["success"] = false;
                result["error"]   = "No active session.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            std::string asset_id;
            try {
                auto body = json::parse(req.body);
                asset_id  = body.value("id", "");
            } catch (...) {}

            if (asset_id.empty()) {
                result["success"] = false;
                result["error"]   = "Field 'id' is required.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // Ask Lua for the data to generate the asset
            sol::protected_function pf = lua["get_asset_prompt"];
            if (!pf.valid()) {
                result["success"] = false;
                result["error"]   = "Script does not implement get_asset_prompt().";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            sol::protected_function_result pfr = pf(asset_id);
            if (!pfr.valid()) {
                sol::error err = pfr;
                result["success"] = false;
                result["error"]   = std::string("get_asset_prompt() error: ") + err.what();
                crow::response res(500, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // nil = unrecognized id
            if (pfr.get_type() == sol::type::lua_nil || pfr.get_type() == sol::type::none) {
                result["success"] = false;
                result["error"]   = "Asset id '" + asset_id + "' not recognized by the script.";
                crow::response res(404, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            sol::table asset_data = pfr;
            std::string prompt = asset_data.get_or<std::string>("prompt", "");
            std::string path   = asset_data.get_or<std::string>("path",   "");

            if (prompt.empty()) {
                result["success"] = false;
                result["error"]   = "get_asset_prompt() returned an empty prompt.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // Resolve path
            std::string full_path = path;
            if (!path.empty() && !std::filesystem::path(path).is_absolute())
                full_path = cfg.basePath + path;

            // Create directory if it does not exist
            if (!full_path.empty()) {
                auto dir = std::filesystem::path(full_path).parent_path();
                if (!dir.empty()) std::filesystem::create_directories(dir);
            }

            std::string job_id = new_job_id();
            {
                std::lock_guard<std::mutex> lk(img_jobs_mutex);
                auto& job       = img_jobs[job_id];
                job.asset_id    = asset_id;
            }

            std::string prompt_copy    = prompt;
            std::string full_path_copy = full_path;

            launch_image_job(job_id, [=]() -> std::pair<std::vector<uint8_t>, std::string> {
                auto bytes = text_to_image(prompt_copy, full_path_copy);
                if (bytes.empty())
                    throw std::runtime_error("text_to_image returned empty result");
                std::cerr << "[IMG] Asset '" << asset_id << "' saved to: " << full_path_copy << "\n";
                return {std::move(bytes), prompt_copy};
            });

            result["success"]  = true;
            result["job_id"]   = job_id;
            result["asset_id"] = asset_id;
            result["path"]     = path;
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/show_asset?id=<id>
        // Calls get_asset_path(id) on Lua, reads the file, returns base64.
        // Synchronous response — no job, the file must already exist.
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/show_asset")([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;

            std::string asset_id = req.url_params.get("id") ? req.url_params.get("id") : "";

            if (session_state == SessionState::IDLE) {
                result["success"] = false;
                result["error"]   = "No active session.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // No id → list all assets in the current scene from get_scene_images()
            if (asset_id.empty()) {
                sol::protected_function gsi = lua["get_scene_images"];
                if (!gsi.valid()) {
                    result["success"] = false;
                    result["error"]   = "Script does not implement get_scene_images().";
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                sol::protected_function_result gsi_r = gsi();
                if (!gsi_r.valid() || gsi_r.get_type() == sol::type::lua_nil) {
                    result["success"] = false;
                    result["error"]   = "get_scene_images() returned nil.";
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                sol::table img_table = gsi_r;
                // Handle Format A (array) and Format B (table with "assets" key)
                sol::object af = img_table["assets"];
                sol::table  al = (af.valid() && af.get_type() == sol::type::table)
                                 ? af.as<sol::table>() : img_table;

                json assets_arr = json::array();
                for (auto& kv : al) {
                    if (kv.second.get_type() != sol::type::table) continue;
                    sol::table entry = kv.second;
                    std::string aid  = entry.get_or<std::string>("id",   "");
                    std::string apath= entry.get_or<std::string>("path", "");
                    if (apath.empty()) continue;
                    std::string full = std::filesystem::path(apath).is_absolute()
                                       ? apath : cfg.basePath + apath;
                    bool exists = std::filesystem::exists(full);
                    json a;
                    a["id"]     = aid;
                    a["path"]   = apath;
                    a["exists"] = exists;
                    assets_arr.push_back(a);
                }
                result["success"] = true;
                result["assets"]  = assets_arr;
                crow::response res(result.dump());
                res.set_header("Content-Type", "application/json");
                return res;
            }

            // Special id "pin" → serve pinned image for current scene
            if (asset_id == "pin") {
                std::string scene_key;
                sol::protected_function fn = lua["get_pin_key"];
                if (fn.valid()) {
                    auto r = fn();
                    if (r.valid() && r.get_type() != sol::type::lua_nil)
                        scene_key = r.get<std::string>();
                }
                if (scene_key.empty()) {
                    result["success"] = false; result["error"] = "Script does not implement get_pin_key()";
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                auto pin_path = pin_cache::lookup(cfg.basePath, scene_key);
                if (pin_path.empty()) {
                    result["success"]   = false;
                    result["error"]     = "No pin for scene: " + scene_key;
                    result["scene_key"] = scene_key;
                    crow::response res(404, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                std::ifstream pf2(pin_path, std::ios::binary);
                std::vector<uint8_t> pb((std::istreambuf_iterator<char>(pf2)), std::istreambuf_iterator<char>());
                result["success"]   = true;
                result["asset_id"]  = "pin";
                result["scene_key"] = scene_key;
                result["path"]      = pin_path;
                result["image"]     = bytes_to_base64(pb);
                result["mime"]      = "image/jpeg";
                result["pinned"]    = true;
                crow::response res(result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // Ask Lua for the asset path
            sol::protected_function pf = lua["get_asset_path"];
            if (!pf.valid()) {
                result["success"] = false;
                result["error"]   = "Script does not implement get_asset_path().";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            sol::protected_function_result pfr = pf(asset_id);
            if (!pfr.valid() || pfr.get_type() == sol::type::lua_nil) {
                result["success"] = false;
                result["error"]   = "Asset '" + asset_id + "' not recognized.";
                crow::response res(404, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            std::string path      = pfr.get<std::string>();
            std::string full_path = path;
            if (!std::filesystem::path(path).is_absolute())
                full_path = cfg.basePath + path;

            if (!std::filesystem::exists(full_path)) {
                result["success"] = false;
                result["error"]   = "File not found: " + path;
                result["hint"]    = "Use /generate_asset " + asset_id + " to generate it.";
                crow::response res(404, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // Read the file
            std::ifstream f(full_path, std::ios::binary);
            std::vector<uint8_t> bytes(
                (std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

            result["success"]  = true;
            result["asset_id"] = asset_id;
            result["path"]     = path;
            result["image"]    = bytes_to_base64(bytes);

            // Determine MIME type from extension
            auto ext = std::filesystem::path(full_path).extension().string();
            if      (ext == ".jpg" || ext == ".jpeg") result["mime"] = "image/jpeg";
            else if (ext == ".webp")                  result["mime"] = "image/webp";
            else                                      result["mime"] = "image/png";

            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/pin  →  pin a generated image as approved for the current scene
        // Body: { "job_id": "imgjob_N" }
        // Calls Lua get_pin_key() for scene key, copies job image to images/pinned/.
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/pin").methods("POST"_method)([&](const crow::request& req) {
            json result;
            json body;
            try { body = json::parse(req.body); } catch (...) {}

            std::string job_id = body.value("job_id", "");
            if (job_id.empty()) {
                result["success"] = false; result["error"] = "job_id required";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // Get image bytes from completed job
            std::vector<uint8_t> img_bytes;
            std::string img_prompt;
            {
                std::lock_guard<std::mutex> lk(img_jobs_mutex);
                auto it = img_jobs.find(job_id);
                if (it == img_jobs.end() || it->second.state != ImageJob::State::DONE) {
                    result["success"] = false; result["error"] = "Job not found or not done";
                    crow::response res(404, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                img_bytes  = base64_to_bytes(it->second.image_b64);
                img_prompt = it->second.prompt;
            }

            // Get scene key from Lua
            std::string scene_key;
            {
                std::lock_guard<std::mutex> lk(lua_mutex);
                if (session_state == SessionState::IDLE) {
                    result["success"] = false; result["error"] = "No active session";
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                sol::protected_function fn = lua["get_pin_key"];
                if (!fn.valid()) {
                    result["success"] = false; result["error"] = "Script does not implement get_pin_key()";
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                auto r = fn();
                if (!r.valid() || r.get_type() == sol::type::lua_nil) {
                    result["success"] = false; result["error"] = "get_pin_key() returned nil";
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                scene_key = r.get<std::string>();
            }

            json meta;
            meta["scene_key"] = scene_key;
            meta["prompt"]    = img_prompt;
            meta["job_id"]    = job_id;

            auto path = pin_cache::upsert(cfg.basePath, scene_key, img_bytes, meta);
            result["success"]   = true;
            result["scene_key"] = scene_key;
            result["path"]      = path;
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json"); return res;
        });

        // -----------------------------------------------------------------
        // POST /api/depin  →  remove pin for current scene (or given key)
        // Body (optional): { "key": "script|loc|slot|npcs" }
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/depin").methods("POST"_method)([&](const crow::request& req) {
            json result;
            json body;
            try { body = json::parse(req.body); } catch (...) {}
            std::string scene_key = body.value("key", "");

            if (scene_key.empty()) {
                std::lock_guard<std::mutex> lk(lua_mutex);
                if (session_state != SessionState::IDLE) {
                    sol::protected_function fn = lua["get_pin_key"];
                    if (fn.valid()) {
                        auto r = fn();
                        if (r.valid() && r.get_type() != sol::type::lua_nil)
                            scene_key = r.get<std::string>();
                    }
                }
            }
            if (scene_key.empty()) {
                result["success"] = false; result["error"] = "Could not determine scene key";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            bool removed = pin_cache::remove(cfg.basePath, scene_key);
            result["success"]   = removed;
            result["scene_key"] = scene_key;
            if (!removed) result["error"] = "No pin found for this scene";
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json"); return res;
        });

        // -----------------------------------------------------------------
        // GET /api/tts?text=<text>&voice=<voice>  →  proxy to TTS server
        // Forwards to the tts_locale server on port 8004, returns audio bytes.
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/tts")([&](const crow::request& req) {
            std::string text  = req.url_params.get("text")  ? req.url_params.get("text")  : "";
            std::string voice = req.url_params.get("voice") ? req.url_params.get("voice") : "";
            if (text.empty()) {
                crow::response res(400, "Missing text parameter");
                return res;
            }
            // POST to TTS server /synthesize with form data
            std::string base_url = cfg.ttsUrl.empty() ? "http://localhost:8004" : cfg.ttsUrl;
            std::string tts_url  = base_url + "/synthesize";
            // URL-encode text so UTF-8 chars (è, à, ì…) survive form-urlencoded transfer
            char* esc = curl_easy_escape(nullptr, text.c_str(), (int)text.size());
            std::string form_data = "text=" + std::string(esc) + "&voice_id=" + voice + "&language=it";
            curl_free(esc);

            std::vector<uint8_t> audio_bytes;
            CURL* curl = curl_easy_init();
            if (!curl) {
                crow::response res(500, "curl init failed");
                return res;
            }
            curl_easy_setopt(curl, CURLOPT_URL, tts_url.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form_data.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                +[](char* ptr, size_t sz, size_t nmemb, void* ud) -> size_t {
                    auto* v = static_cast<std::vector<uint8_t>*>(ud);
                    v->insert(v->end(), ptr, ptr + sz * nmemb);
                    return sz * nmemb;
                });
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &audio_bytes);
            long http_code = 0;
            CURLcode cc = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_cleanup(curl);

            if (cc != CURLE_OK || http_code != 200 || audio_bytes.empty()) {
                crow::response res(502, "TTS server unavailable or returned error");
                return res;
            }
            crow::response res(std::string(audio_bytes.begin(), audio_bytes.end()));
            res.set_header("Content-Type", "audio/wav");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/tts/voices  →  proxy to TTS server /voices
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/tts/voices")([&](const crow::request& req) {
            // ?url= overrides saved cfg so user can refresh before hitting Save
            std::string override_url = req.url_params.get("url") ? req.url_params.get("url") : "";
            std::string base_url = !override_url.empty() ? override_url
                                 : (!cfg.ttsUrl.empty()  ? cfg.ttsUrl : "http://localhost:8004");
            std::string url = base_url + "/voices";
            std::string body;
            CURL* curl = curl_easy_init();
            if (!curl) return crow::response(500, "{\"voices\":[]}");
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                +[](char* p, size_t s, size_t n, void* u) -> size_t {
                    static_cast<std::string*>(u)->append(p, s * n);
                    return s * n;
                });
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
            long code = 0;
            curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            curl_easy_cleanup(curl);
            if (code == 200 && !body.empty()) {
                crow::response res(body);
                res.set_header("Content-Type", "application/json");
                return res;
            }
            return crow::response(502, "{\"voices\":[]}");
        });

        // -----------------------------------------------------------------
        // GET /api/serve_file?path=<rel_path>  →  serve script-relative file as base64 JSON
        // Used by action type="image" to show arbitrary script assets by path.
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/serve_file")([&](const crow::request& req) {
            json result;
            std::string rel = req.url_params.get("path") ? req.url_params.get("path") : "";
            if (rel.empty()) {
                result["success"] = false; result["error"] = "Missing path";
                crow::response r(400, result.dump());
                r.set_header("Content-Type", "application/json"); return r;
            }
            // Security: resolve and verify it stays within basePath
            std::filesystem::path base  = std::filesystem::canonical(cfg.basePath);
            std::filesystem::path full;
            try {
                full = std::filesystem::weakly_canonical(base / rel);
            } catch (...) {
                result["success"] = false; result["error"] = "Invalid path";
                crow::response r(400, result.dump());
                r.set_header("Content-Type", "application/json"); return r;
            }
            auto [base_end, _] = std::mismatch(base.begin(), base.end(), full.begin());
            if (base_end != base.end()) {
                result["success"] = false; result["error"] = "Path outside basePath";
                crow::response r(403, result.dump());
                r.set_header("Content-Type", "application/json"); return r;
            }
            if (!std::filesystem::exists(full)) {
                result["success"] = false; result["error"] = "File not found: " + rel;
                crow::response r(404, result.dump());
                r.set_header("Content-Type", "application/json"); return r;
            }
            std::ifstream f(full, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
            result["success"] = true;
            result["image"]   = bytes_to_base64(bytes);
            auto ext = full.extension().string();
            if      (ext == ".jpg" || ext == ".jpeg") result["mime"] = "image/jpeg";
            else if (ext == ".webp")                  result["mime"] = "image/webp";
            else                                      result["mime"] = "image/png";
            crow::response r(result.dump());
            r.set_header("Content-Type", "application/json"); return r;
        });

        // -----------------------------------------------------------------
        // GET /api/serve_audio?path=<rel_path>  →  serve script-relative audio file
        // Returns raw bytes with correct Content-Type (mp3/wav/ogg/m4a).
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/serve_audio")([&](const crow::request& req) {
            std::string rel = req.url_params.get("path") ? req.url_params.get("path") : "";
            if (rel.empty()) {
                crow::response r(400, "Missing path"); return r;
            }
            std::filesystem::path base = std::filesystem::canonical(cfg.basePath);
            std::filesystem::path full;
            try { full = std::filesystem::weakly_canonical(base / rel); }
            catch (...) { crow::response r(400, "Invalid path"); return r; }
            auto [base_end, _] = std::mismatch(base.begin(), base.end(), full.begin());
            if (base_end != base.end()) {
                crow::response r(403, "Path outside basePath"); return r;
            }
            if (!std::filesystem::exists(full)) {
                crow::response r(404, "File not found: " + rel); return r;
            }
            std::ifstream f(full, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
            auto ext = full.extension().string();
            std::string mime = "audio/mpeg";
            if      (ext == ".wav")  mime = "audio/wav";
            else if (ext == ".ogg")  mime = "audio/ogg";
            else if (ext == ".m4a")  mime = "audio/mp4";
            else if (ext == ".flac") mime = "audio/flac";
            crow::response r(bytes);
            r.set_header("Content-Type", mime);
            return r;
        });

        // -----------------------------------------------------------------
        // GET /api/scene_image?file=<basename>  →  serve cached scene image
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/scene_image")([&](const crow::request& req) {
            json result;
            std::string file = req.url_params.get("file") ? req.url_params.get("file") : "";
            if (file.empty()
                || file.find('/') != std::string::npos
                || file.find('\\') != std::string::npos
                || file.find("..") != std::string::npos) {
                result["success"] = false;
                result["error"]   = "Invalid filename.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            std::string full_path = cfg.basePath + "images/scene_cache/" + file;
            if (!std::filesystem::exists(full_path)) {
                result["success"] = false;
                result["error"]   = "Not found: " + file;
                crow::response res(404, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            std::ifstream f(full_path, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
            result["success"] = true;
            result["image"]   = bytes_to_base64(bytes);
            auto ext = std::filesystem::path(full_path).extension().string();
            if      (ext == ".jpg" || ext == ".jpeg") result["mime"] = "image/jpeg";
            else if (ext == ".webp")                  result["mime"] = "image/webp";
            else                                      result["mime"] = "image/png";
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/visual_world
        // Returns JSON visual config if the script implements get_visual_world().
        // Used by rpgai-gui to load tilemap, rooms, and NPC sprite definitions.
        // Response: { "supported": bool, "config": {...} } | { "supported": false, "error": "..." }
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/visual_world")([&]() {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            // Always present, also without a visual world: the GUI needs it
            // to enable possess on text-only adventures too, and to locate
            // the catalogs/assets on disk (CatalogBrowser, AssetBrowser).
            result["debug_gui"] = cfg.debugGui;
            {
                std::error_code ec;
                std::filesystem::path root = cfg.assetRoot.empty()
                    ? std::filesystem::current_path(ec)
                    : std::filesystem::absolute(cfg.assetRoot, ec);
                result["asset_root"] = root.string();
            }
            sol::protected_function pf = lua["get_visual_world"];
            if (!pf.valid()) {
                result["supported"] = false;
            } else {
                try {
                    sol::protected_function_result r = pf();
                    if (!r.valid()) {
                        sol::error e = r;
                        result["supported"] = false;
                        result["error"]     = std::string(e.what());
                    } else {
                        std::string vw_str = r.get<std::string>(0);
                        result["supported"] = true;
                        result["config"]    = json::parse(vw_str);
                        // Tell the GUI whether possess/debug routes are enabled
                        result["config"]["debug_gui"] = cfg.debugGui;
                    }
                } catch (const std::exception& e) {
                    result["supported"] = false;
                    result["error"]     = std::string(e.what());
                }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/vn/scene
        // Visual-Novel mode: returns the current scene descriptor if the
        // script implements get_vn_scene() (flat background + foreground NPC
        // sprites, tag-selected from catalog/vn_scene.json). Mirrors
        // /api/visual_world. Assets are fetched by the GUI via /api/serve_file.
        // Response: { supported:bool, scene:{...}, asset_root } | { supported:false, error }
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/vn/scene")([&]() {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            {
                std::error_code ec;
                std::filesystem::path root = cfg.assetRoot.empty()
                    ? std::filesystem::current_path(ec)
                    : std::filesystem::absolute(cfg.assetRoot, ec);
                result["asset_root"] = root.string();
                // Always reload catalog from disk so Ricarica picks up any
                // changes saved via the VN Editor or manual JSON edits.
                std::string cat_path = vn_catalog_file();
                sol::protected_function rl = lua["vn_reload_catalog"];
                if (rl.valid()) { try { rl(cat_path); } catch (...) {} }
            }
            sol::protected_function pf = lua["get_vn_scene"];
            if (!pf.valid()) {
                result["supported"] = false;
            } else {
                try {
                    sol::protected_function_result r = pf();
                    if (!r.valid()) {
                        sol::error e = r;
                        result["supported"] = false;
                        result["error"]     = std::string(e.what());
                    } else {
                        std::string scene_str = r.get<std::string>(0);
                        result["supported"] = true;
                        result["scene"]     = json::parse(scene_str);
                    }
                } catch (const std::exception& e) {
                    result["supported"] = false;
                    result["error"]     = std::string(e.what());
                }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/vn/asset?path=<rel_path>  →  serve an asset as base64 JSON,
        // rooted at ASSET_ROOT (where asset/ and catalog/ live), unlike
        // /api/serve_file which is rooted at the script basePath. VN catalog
        // 'file' paths are relative to ASSET_ROOT.
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/vn/asset")([&](const crow::request& req) {
            json result;
            std::string rel = req.url_params.get("path") ? req.url_params.get("path") : "";
            if (rel.empty()) {
                result["success"] = false; result["error"] = "Missing path";
                crow::response r(400, result.dump());
                r.set_header("Content-Type", "application/json"); return r;
            }
            std::error_code ec;
            std::filesystem::path base = cfg.assetRoot.empty()
                ? std::filesystem::current_path(ec)
                : std::filesystem::absolute(cfg.assetRoot, ec);
            base = std::filesystem::weakly_canonical(base, ec);
            std::filesystem::path full;
            try { full = std::filesystem::weakly_canonical(base / rel); }
            catch (...) {
                result["success"] = false; result["error"] = "Invalid path";
                crow::response r(400, result.dump());
                r.set_header("Content-Type", "application/json"); return r;
            }
            auto [base_end, _] = std::mismatch(base.begin(), base.end(), full.begin());
            if (base_end != base.end()) {
                result["success"] = false; result["error"] = "Path outside asset root";
                crow::response r(403, result.dump());
                r.set_header("Content-Type", "application/json"); return r;
            }
            if (!std::filesystem::exists(full)) {
                result["success"] = false; result["error"] = "File not found: " + rel;
                crow::response r(404, result.dump());
                r.set_header("Content-Type", "application/json"); return r;
            }
            std::ifstream f(full, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
            result["success"] = true;
            result["image"]   = bytes_to_base64(bytes);
            auto ext = full.extension().string();
            if      (ext == ".jpg" || ext == ".jpeg") result["mime"] = "image/jpeg";
            else if (ext == ".webp")                  result["mime"] = "image/webp";
            else                                      result["mime"] = "image/png";
            crow::response r(result.dump());
            r.set_header("Content-Type", "application/json"); return r;
        });

        // -----------------------------------------------------------------
        // Catalog editor (Pillar 2 — designer-in-the-loop). Reads/writes
        // catalog/rules.json (placement rules) and patches catalog/objects.json
        // (per-object placement/footprint corrections). The furnisher reloads
        // rules at every apartment generation, so edits take effect on next
        // /image / visual regen — no restart.
        // catalog_path(name) resolves under ASSET_ROOT just like the Lua side.
        auto catalog_path = [&](const std::string& name) -> std::string {
            std::error_code ec;
            std::filesystem::path root = cfg.assetRoot.empty()
                ? std::filesystem::current_path(ec)
                : std::filesystem::absolute(cfg.assetRoot, ec);
            return (root / "catalog" / name).string();
        };
        auto write_json_atomic = [](const std::string& path, const std::string& body) -> bool {
            std::string tmp = path + ".tmp";
            std::ofstream f(tmp, std::ios::trunc);
            if (!f.good()) return false;
            f << body;
            f.close();
            if (!f.good()) return false;
            return std::rename(tmp.c_str(), path.c_str()) == 0;
        };

        // GET /api/catalog/rules → current rules.json (or an empty skeleton).
        CROW_ROUTE(app, "/api/catalog/rules")([&]() {
            std::string path = catalog_path("rules.json");
            std::ifstream f(path);
            crow::response res;
            res.set_header("Content-Type", "application/json");
            if (f.good()) {
                std::stringstream ss; ss << f.rdbuf();
                res.body = ss.str();
            } else {
                res.body = R"({"version":1,"category":{},"match":[]})";
            }
            return res;
        });

        // POST /api/catalog/rules  body = full rules.json → validate + write.
        CROW_ROUTE(app, "/api/catalog/rules").methods("POST"_method)
        ([&](const crow::request& req) {
            json out;
            try {
                json parsed = json::parse(req.body);   // reject malformed JSON
                if (!parsed.is_object()) throw std::runtime_error("root must be an object");
                if (!write_json_atomic(catalog_path("rules.json"), parsed.dump(2))) {
                    out["ok"] = false; out["error"] = "write failed";
                    return crow::response(500, out.dump());
                }
                out["ok"] = true;
            } catch (const std::exception& e) {
                out["ok"] = false; out["error"] = e.what();
                return crow::response(400, out.dump());
            }
            crow::response res(out.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/catalog/object  body = { id, placement?, footprint?[w,h] }
        // Patches one object in objects.json (placement/footprint corrections).
        CROW_ROUTE(app, "/api/catalog/object").methods("POST"_method)
        ([&](const crow::request& req) {
            json out;
            try {
                json b = json::parse(req.body);
                std::string id = b.value("id", "");
                if (id.empty()) throw std::runtime_error("id required");
                std::string path = catalog_path("objects.json");
                std::ifstream f(path);
                if (!f.good()) throw std::runtime_error("objects.json not found");
                json cat = json::parse(f); f.close();
                if (!cat.contains(id)) throw std::runtime_error("unknown object id: " + id);
                if (b.contains("placement") && b["placement"].is_string())
                    cat[id]["placement"] = b["placement"];
                if (b.contains("footprint") && b["footprint"].is_array()
                        && b["footprint"].size() == 2)
                    cat[id]["footprint"] = b["footprint"];
                // dump(1): match objects.json's 1-space indent so a one-field
                // patch produces a tiny diff, not a whole-file reformat.
                if (!write_json_atomic(path, cat.dump(1))) {
                    out["ok"] = false; out["error"] = "write failed";
                    return crow::response(500, out.dump());
                }
                out["ok"] = true; out["object"] = cat[id];
            } catch (const std::exception& e) {
                out["ok"] = false; out["error"] = e.what();
                return crow::response(400, out.dump());
            }
            crow::response res(out.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // VN authoring editor (designer-in-the-loop). Reads/writes the
        // Visual-Novel asset catalog (catalog/vn_scene.json): backgrounds +
        // their hotspots, and NPC sprites, with tags. The VnEditor window in
        // rpgai-gui binds files→tags and draws hotspots→object/exit links.
        // The catalog is reloaded on next get_vn_scene() via the Lua lib (the
        // adventure can call vn.init again, or just restart) — for live edits
        // the editor also POSTs and the GUI re-fetches the scene.
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/vn/catalog")([&]() {
            std::string path = vn_catalog_file();
            std::ifstream f(path);
            crow::response res;
            res.set_header("Content-Type", "application/json");
            if (f.good()) {
                std::stringstream ss; ss << f.rdbuf();
                res.body = ss.str();
            } else {
                res.body = R"({"backgrounds":[],"sprites":[]})";
            }
            return res;
        });

        // POST /api/vn/catalog  body = full vn_scene.json → validate + write.
        // Also hot-reloads the Lua VN catalog so edits take effect immediately.
        CROW_ROUTE(app, "/api/vn/catalog").methods("POST"_method)
        ([&](const crow::request& req) {
            json out;
            try {
                json parsed = json::parse(req.body);   // reject malformed JSON
                if (!parsed.is_object()) throw std::runtime_error("root must be an object");
                if (!parsed.contains("backgrounds")) parsed["backgrounds"] = json::array();
                if (!parsed.contains("sprites"))      parsed["sprites"]     = json::array();
                std::string path = vn_catalog_file();
                if (!write_json_atomic(path, parsed.dump(2))) {
                    out["ok"] = false; out["error"] = "write failed";
                    return crow::response(500, out.dump());
                }
                // Live reload: tell the Lua lib to re-read the catalog so the
                // next /api/vn/scene reflects the edit without a restart.
                {
                    std::lock_guard<std::mutex> lock(lua_mutex);
                    sol::protected_function rl = lua["vn_reload_catalog"];
                    if (rl.valid()) { try { rl(path); } catch (...) {} }
                }
                out["ok"] = true;
            } catch (const std::exception& e) {
                out["ok"] = false; out["error"] = e.what();
                return crow::response(400, out.dump());
            }
            crow::response res(out.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/npcs
        // NPC list for the GUI Cast window if the script implements
        // get_npc_list() → JSON string '[{"id":..,"name":..,"location":..},..]'.
        // Response: { "supported": bool, "npcs": [...] } | { "supported": false }
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/npcs")([&]() {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["get_npc_list"];
            if (!pf.valid()) {
                result["supported"] = false;
            } else {
                try {
                    sol::protected_function_result r = pf();
                    if (!r.valid()) {
                        sol::error e = r;
                        result["supported"] = false;
                        result["error"]     = std::string(e.what());
                    } else {
                        result["supported"] = true;
                        result["npcs"]      = json::parse(r.get<std::string>(0));
                    }
                } catch (const std::exception& e) {
                    result["supported"] = false;
                    result["error"]     = std::string(e.what());
                }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/location/<id>
        // Location inspector (exits/objects/npcs) if the script implements
        // get_location_info(id) → JSON string. "" / "current" = player's location.
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/location/<string>")([&](const std::string& loc_id) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["get_location_info"];
            if (!pf.valid()) {
                result["supported"] = false;
            } else {
                try {
                    sol::protected_function_result r = pf(loc_id == "current" ? "" : loc_id);
                    if (!r.valid()) {
                        sol::error e = r;
                        result["supported"] = false;
                        result["error"]     = std::string(e.what());
                    } else {
                        result["supported"] = true;
                        result["info"]      = json::parse(r.get<std::string>(0));
                    }
                } catch (const std::exception& e) {
                    result["supported"] = false;
                    result["error"]     = std::string(e.what());
                }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/npc/<id>
        // NPC inspector info if the script implements get_npc_info(id) → JSON string.
        // Response: { "supported": bool, "info": {...} } | { "supported": false, "error": "..." }
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/npc/<string>")([&](const std::string& npc_id) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["get_npc_info"];
            if (!pf.valid()) {
                result["supported"] = false;
            } else {
                try {
                    sol::protected_function_result r = pf(npc_id);
                    if (!r.valid()) {
                        sol::error e = r;
                        result["supported"] = false;
                        result["error"]     = std::string(e.what());
                    } else {
                        result["supported"] = true;
                        result["info"]      = json::parse(r.get<std::string>(0));
                    }
                } catch (const std::exception& e) {
                    result["supported"] = false;
                    result["error"]     = std::string(e.what());
                }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // ENTITY EDITORS (GUI design mode). Thin routes over Lua globals the
        // adventure installs via adv.enable_editors() — same shape as the
        // inspectors above (Lua returns a JSON string, we parse it). The
        // editors delegate to persona.patch/get/generate; no parallel store.
        // -----------------------------------------------------------------
        // GET /api/editor/npcs  → editor_npcs() → { ok, npcs:[{id,name,location}] }
        CROW_ROUTE(app, "/api/editor/npcs")([&]() {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_npcs"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile (manca adv.enable_editors)"; }
            else {
                try {
                    sol::protected_function_result r = pf();
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // GET /api/npc/<id>/full  → editor_npc_get(id) → { ok, id, path, data }
        CROW_ROUTE(app, "/api/npc/<string>/full")([&](const std::string& npc_id) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_npc_get"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile"; }
            else {
                try {
                    sol::protected_function_result r = pf(npc_id);
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/npc/<id>/patch  body: {...persona.patch keys...}
        //   → editor_npc_patch(id, patch_json) → { ok } | { ok:false, error }
        CROW_ROUTE(app, "/api/npc/<string>/patch").methods("POST"_method)([&](const crow::request& req, const std::string& npc_id) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_npc_patch"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile"; }
            else {
                try {
                    // pass the body straight through as the patch JSON string
                    std::string patch = req.body.empty() ? "{}" : req.body;
                    sol::protected_function_result r = pf(npc_id, patch);
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/npc/new  body: { id, context }
        //   → editor_npc_create(id, context) → { ok, id, name } | { ok:false, error }
        CROW_ROUTE(app, "/api/npc/new").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_npc_create"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile"; }
            else {
                try {
                    auto body = json::parse(req.body.empty() ? "{}" : req.body);
                    std::string id  = body.value("id", "");
                    std::string ctx = body.value("context", "");
                    bool blank      = body.value("blank", false);
                    sol::protected_function_result r = pf(id, ctx, blank);
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // GET /api/editor/locations → editor_locations() → {ok, locations:[{id,name,source}]}
        CROW_ROUTE(app, "/api/editor/locations")([&]() {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_locations"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile"; }
            else {
                try {
                    sol::protected_function_result r = pf();
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // GET /api/location/<id>/full → editor_location_get(id) → {ok,id,source,editable,data}
        CROW_ROUTE(app, "/api/location/<string>/full")([&](const std::string& loc_id) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_location_get"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile"; }
            else {
                try {
                    sol::protected_function_result r = pf(loc_id);
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/location/<id>/patch  body = world.location_patch keys
        CROW_ROUTE(app, "/api/location/<string>/patch").methods("POST"_method)([&](const crow::request& req, const std::string& loc_id) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_location_patch"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile"; }
            else {
                try {
                    std::string patch = req.body.empty() ? "{}" : req.body;
                    sol::protected_function_result r = pf(loc_id, patch);
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/location/new  body: { id, data:{name,description,connected_to,...} }
        CROW_ROUTE(app, "/api/location/new").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_location_create"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile"; }
            else {
                try {
                    auto body = json::parse(req.body.empty() ? "{}" : req.body);
                    std::string id   = body.value("id", "");
                    std::string data = body.contains("data") ? body["data"].dump() : "";
                    sol::protected_function_result r = pf(id, data);
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // ── Object editor routes ──────────────────────────────────────────────
        // GET /api/editor/objects → editor_objects() → {ok, objects:[{id,name,location,current_state}]}
        CROW_ROUTE(app, "/api/editor/objects")([&]() {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_objects"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile (world.lua non caricato?)"; }
            else {
                try {
                    sol::protected_function_result r = pf();
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // GET /api/object/<id>/full → editor_object_get(id) → {ok, id, data}
        CROW_ROUTE(app, "/api/object/<string>/full")([&](const std::string& obj_id) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_object_get"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile"; }
            else {
                try {
                    sol::protected_function_result r = pf(obj_id);
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/object/<id>/patch  body = object patch keys
        CROW_ROUTE(app, "/api/object/<string>/patch").methods("POST"_method)([&](const crow::request& req, const std::string& obj_id) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_object_patch"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile"; }
            else {
                try {
                    auto patch = json::parse(req.body.empty() ? "{}" : req.body);
                    sol::protected_function_result r = pf(obj_id, patch.dump());
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/object/new  body: { id, data:{name,description,states,current_state,holder,actions} }
        CROW_ROUTE(app, "/api/object/new").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            sol::protected_function pf = lua["editor_object_create"];
            if (!pf.valid()) { result["ok"] = false; result["error"] = "editor non disponibile"; }
            else {
                try {
                    auto body = json::parse(req.body.empty() ? "{}" : req.body);
                    std::string id   = body.value("id", "");
                    std::string data = body.contains("data") ? body["data"].dump() : "{}";
                    sol::protected_function_result r = pf(id, data);
                    if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                    else result = json::parse(r.get<std::string>(0));
                } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/scaffold  body: { mode:"vn" }
        //   One-click conversion: make the loaded adventure support a render mode
        //   it lacks. For "vn": ensure <ASSET_ROOT>/catalog/vn_scene.json exists
        //   (empty skeleton), then call the adventure's scaffold_vn(path) global
        //   (installed by adv.set_config) which installs a universal get_vn_scene
        //   + loads the catalog. The designer then fills the catalog via VN Editor.
        CROW_ROUTE(app, "/api/scaffold").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            try {
                auto body = json::parse(req.body.empty() ? "{}" : req.body);
                std::string mode = body.value("mode", "vn");
                if (mode != "vn") {
                    result["ok"] = false; result["error"] = "scaffold supporta solo mode=vn";
                } else {
                    sol::object sf = lua["scaffold_vn"];
                    if (!sf.valid() || sf.get_type() != sol::type::function) {
                        result["ok"] = false;
                        result["error"] = "scaffold non disponibile: avvia e inizializza un'avventura basata su adventure.lua";
                    } else {
                        // Ensure catalog/ dir + an empty vn_scene.json skeleton.
                        std::string path = vn_catalog_file();
                        std::error_code ec;
                        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
                        if (!std::filesystem::exists(path, ec)) {
                            std::ofstream f(path, std::ios::trunc);
                            if (f.good()) f << "{\"backgrounds\":[],\"sprites\":[]}";
                        }
                        sol::protected_function fn = sf;
                        auto r = fn(path);
                        if (!r.valid()) { sol::error e = r; result["ok"] = false; result["error"] = std::string(e.what()); }
                        else result = json::parse(r.get<std::string>(0));
                    }
                }
            } catch (const std::exception& e) { result["ok"] = false; result["error"] = std::string(e.what()); }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/debug/npc_action  (enabled only with --debug-gui)
        // Puppet control for the GUI "regia" mode.
        // Body: { "npc": "jenny", "action": "move", "args": {...} }
        // Calls Lua debug_npc_action(npc, action, args_json) → JSON string
        // { "success": bool, "output"?: str, "error"?: str }
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/debug/npc_action").methods("POST"_method)([&](const crow::request& req) {
            json result;
            if (!cfg.debugGui) {
                result["success"] = false;
                result["error"]   = "Debug routes disabled. Start the engine with --debug-gui.";
                crow::response res(403, result.dump());
                res.set_header("Content-Type", "application/json");
                return res;
            }
            std::lock_guard<std::mutex> lock(lua_mutex);
            try {
                auto body = json::parse(req.body);
                std::string npc    = body.value("npc", "");
                std::string action = body.value("action", "");
                std::string args   = body.value("args", json::object()).dump();
                sol::protected_function pf = lua["debug_npc_action"];
                if (!pf.valid()) {
                    result["success"] = false;
                    result["error"]   = "Script does not implement debug_npc_action().";
                } else {
                    sol::protected_function_result r = pf(npc, action, args);
                    if (!r.valid()) {
                        sol::error e = r;
                        result["success"] = false;
                        result["error"]   = std::string(e.what());
                    } else {
                        result = json::parse(r.get<std::string>(0));
                    }
                }
            } catch (const std::exception& e) {
                result["success"] = false;
                result["error"]   = std::string(e.what());
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/commands  →  engine commands + optional script get_commands()
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/commands")([&]() {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            if (session_state == SessionState::IDLE) {
                result["success"] = false;
                result["error"]   = "No active session.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // Always-present engine commands.
            // label (optional) = display text in sidebar; cmd = what gets filled on click.
            json cmds = json::array();
            auto eng = [&](const char* cmd, const char* desc,
                           bool exec = true, const char* label = nullptr) {
                json item = {{"cmd", cmd}, {"desc", desc}, {"exec", exec}, {"system", true}};
                if (label) item["label"] = label;
                cmds.push_back(item);
            };
            eng("/image",         "Generate scene image. Add 'lora' to apply LoRA via /image lora command. Modes: regen, refine, fix [--s N] <instruction>, compose. Combine: /image lora regen, /image lora fix <text>. Add --partial to allow missing assets.",
                false, "/image [lora] [regen|refine|fix [--s 0.9] <text>|compose [--s N]] [--partial]");
            // eng("/swap",          "Face-swap: replace detected faces left-to-right with NPC asset faces. Use 'null' to skip a slot, '--enhance' to run GFPGAN after swap. Requires --faceswap-url.",
            //     false, "/swap [--enhance] <id1> [null] <id2> ...");
            eng("/show_asset",    "Show a script asset. IDs: 'scene' = current scene image, 'pin' = pinned scene image for this location, any other ID = script-defined asset (use /show_asset without args to list). Usage: /show_asset <id|scene|pin>",
                false, "/show_asset <id|scene|pin>");
            eng("/generate_asset","Generate or regenerate a script asset. Usage: /generate_asset <id>",
                false, "/generate_asset <id>");
            eng("/cost",          "Show token usage per component (narrator, agent, gen, ambient, …).");
            eng("/undo",          "Undo the last turn — restores state and history. Use before /fix to correct a bad scene.");
            eng("/observe",       "Ask the AI to describe the current scene in detail");
            eng("/fix",           "Rewrite the last AI response. After /undo, rewrites the undone scene with a correction.");
            eng("/summary",       "Summarise the story so far");
            eng("/save",          "Save the game manually");
            eng("/sim",           "Simulate N NPC ticks without player input. Usage: /sim N (default 4). Advances time N×tick_minutes steps, logs NPC activity and movements. Output also written to game log if configured.",
                false, "/sim <N>");
            eng("/debug",         "Show internal state: NPC locations, tool call log, recent warnings.");
            eng("/validate",      "Run world linter: check travel edges, NPC placement, persona routines for gaps.");
            eng("/map",           "Show full location map with exits from current position.");
            eng("/setloc",        "Debug: teleport player or NPC to a location. Usage: /setloc <location_id> [npc_id]",
                false, "/setloc <location_id> [npc_id]");

            // Script-specific commands (optional)
            sol::protected_function pf = lua["get_commands"];
            if (pf.valid()) {
                try {
                    sol::protected_function_result pfr = pf();
                    if (pfr.valid() && pfr.get_type() == sol::type::table) {
                        sol::table tbl = pfr.get<sol::table>();
                        for (auto& kv : tbl) {
                            if (kv.second.get_type() != sol::type::table) continue;
                            sol::table c = kv.second.as<sol::table>();
                            std::string cmd = c.get_or<std::string>("cmd", "");
                            if (cmd.empty()) continue;
                            sol::object exec_val = c["exec"];
                            bool exec = (exec_val.get_type() == sol::type::boolean)
                                        ? exec_val.as<bool>() : false;
                            cmds.push_back({
                                {"cmd",  cmd},
                                {"desc", c.get_or<std::string>("desc", "")},
                                {"exec", exec}
                            });
                        }
                    }
                } catch (...) {}
            }

            result["success"]  = true;
            result["commands"] = cmds;
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/swap  →  face-swap via local Python server
        // Body: { "input": "/swap id1 null id2" }
        // Returns: { success, job_id } — poll /api/image/job/<id>
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/swap").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;

            if (cfg.faceswapUrl.empty()) {
                result["success"] = false;
                result["error"]   = "Face-swap not enabled. Use --faceswap-url.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }
            if (session_state != SessionState::PLAYING) {
                result["success"] = false;
                result["error"]   = "No active session.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            std::string cmd_input;
            try {
                auto body  = json::parse(req.body);
                cmd_input  = body.value("input", "");
            } catch (...) {}

            // Tokenise: "/swap [--enhance] id1 null id2 id3"
            // → tokens = ["id1","null","id2","id3"], enhance = false|true
            std::vector<std::string> tokens;
            bool swap_enhance = false;
            {
                std::istringstream iss(cmd_input);
                std::string tok;
                bool first = true;
                while (iss >> tok) {
                    if (first) { first = false; continue; } // skip "/swap"
                    if (tok == "--enhance") { swap_enhance = true; continue; }
                    tokens.push_back(tok);
                }
            }
            if (tokens.empty()) {
                result["success"] = false;
                result["error"]   = "Usage: /swap [--enhance] <id1> [null] <id2> ...";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // Resolve asset paths for non-null tokens — needs Lua under mutex
            // positions[] and asset_paths[] are aligned: null entries are skipped
            std::vector<int>         positions;
            std::vector<std::string> asset_paths;
            sol::protected_function  gap = lua["get_asset_path"];
            for (int i = 0; i < (int)tokens.size(); ++i) {
                if (tokens[i] == "null" || tokens[i] == "NULL") continue;
                std::string path;
                if (gap.valid()) {
                    auto r = gap(tokens[i]);
                    if (r.valid() && r.get_type() == sol::type::string) {
                        path = r.get<std::string>();
                        if (!std::filesystem::path(path).is_absolute())
                            path = cfg.basePath + path;
                    }
                }
                if (path.empty() || !std::filesystem::exists(path)) {
                    result["success"] = false;
                    result["error"]   = "Asset not found: " + tokens[i];
                    crow::response res(404, result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                positions.push_back(i);
                asset_paths.push_back(path);
            }
            if (positions.empty()) {
                result["success"] = false;
                result["error"]   = "No valid asset IDs provided.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // Find the last cached scene image
            sol::protected_function gsf = lua["get_scene_images"];
            std::vector<CollageEntry> scene_entries;
            if (gsf.valid()) {
                auto pfr = gsf();
                if (pfr.valid() && pfr.get_type() == sol::type::table) {
                    sol::table tbl = pfr;
                    sol::object af = tbl["assets"];
                    sol::table al  = (af.valid() && af.get_type() == sol::type::table)
                                     ? af.as<sol::table>() : tbl;
                    for (auto& kv : al) {
                        sol::table e = kv.second;
                        std::string id   = e.get_or<std::string>("id",   "");
                        std::string path = e.get_or<std::string>("path", "");
                        if (!path.empty()) scene_entries.push_back({path, id});
                    }
                }
            }
            std::string last_img = scene_cache::lookup_last(
                cfg.basePath, cfg.script, scene_entries, cfg.sessionStart);
            if (last_img.empty()) {
                result["success"] = false;
                result["error"]   = "No cached scene image found. Run /image first.";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            // Launch async job
            std::string job_id = new_job_id();
            {
                std::lock_guard<std::mutex> lk(img_jobs_mutex);
                img_jobs[job_id] = ImageJob{};
            }

            std::string swap_url_copy   = cfg.faceswapUrl;
            std::string base_copy       = cfg.basePath;
            std::string script_copy     = cfg.script;
            std::string swap_sess_copy  = cfg.sessionStart;
            bool        enhance_copy    = swap_enhance;
            std::vector<std::string> paths_copy = asset_paths;
            std::vector<int>         pos_copy   = positions;
            std::vector<CollageEntry> entries_copy = scene_entries;

            launch_image_job(job_id, [=]() -> std::pair<std::vector<uint8_t>, std::string> {
                // Read target image
                std::ifstream tf(last_img, std::ios::binary);
                std::vector<uint8_t> target_bytes(
                    (std::istreambuf_iterator<char>(tf)), {});
                if (target_bytes.empty())
                    throw std::runtime_error("Cannot read cached scene: " + last_img);

                // Read source images
                std::vector<std::vector<uint8_t>> sources;
                for (const auto& p : paths_copy) {
                    std::ifstream sf(p, std::ios::binary);
                    sources.push_back(std::vector<uint8_t>(
                        (std::istreambuf_iterator<char>(sf)), {}));
                }

                std::string url = swap_url_copy;
                if (url.size() < 5 || url.substr(url.size() - 5) != "/swap")
                    url += "/swap";

                std::cerr << "[SWAP] POST " << url
                          << " positions=" << json(pos_copy).dump() << "\n";

                std::vector<uint8_t> result_bytes =
                    img_detail::http_post_faceswap_raw(url, target_bytes, sources, pos_copy, enhance_copy);

                if (result_bytes.size() >= 4 &&
                    !(result_bytes[0] == 0x89 && result_bytes[1] == 'P' &&
                      result_bytes[2] == 'N'  && result_bytes[3] == 'G')) {
                    std::string body(result_bytes.begin(),
                                     result_bytes.begin() + std::min<size_t>(200, result_bytes.size()));
                    throw std::runtime_error("[SWAP] Response is not PNG: " + body);
                }

                // Save to scene cache with unique timestamped key
                if (!base_copy.empty()) {
                    scene_cache::ensure_dirs(base_copy);
                    long long   mt   = scene_cache::max_mtime(entries_copy);
                    std::string bkey = scene_cache::make_cache_key(script_copy, entries_copy, mt);
                    std::string ts   = scene_cache::timestamp_str();
                    std::string key  = bkey + "_swap_" + ts;

                    std::string result_path = scene_cache::save_result(base_copy, result_bytes, ts + "_swap");
                    scene_cache::CacheEntry ce;
                    ce.cache_key     = key;
                    ce.script        = script_copy;
                    for (const auto& e : entries_copy) ce.assets.push_back(e.tag);
                    ce.prompt        = "faceswap";
                    ce.image_path    = result_path;
                    ce.collage_path  = "";
                    ce.generated_at  = ts;
                    ce.utc_at        = scene_cache::utc_iso_now();
                    ce.session_start = swap_sess_copy;
                    scene_cache::upsert(base_copy, ce);
                }

                return {std::move(result_bytes), "faceswap"};
            });

            result["success"] = true;
            result["job_id"]  = job_id;
            result["warning"] = "";
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // -----------------------------------------------------------------
        // POST /api/scripts/upload  →  save uploaded .lua to basePath
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/scripts/upload").methods("POST"_method)([&](const crow::request& req) {
            json result;
            try {
                auto body    = json::parse(req.body);
                std::string name    = body.value("name", "");
                std::string content = body.value("content", "");
                if (name.empty() ||
                    name.find('/') != std::string::npos ||
                    name.find("..") != std::string::npos ||
                    name.size() < 5 ||
                    name.substr(name.size() - 4) != ".lua") {
                    result["success"] = false;
                    result["error"]   = "Invalid filename (must be *.lua, no path separators)";
                } else {
                    std::string dest = cfg.basePath + name;
                    std::ofstream out(dest);
                    if (!out.is_open()) throw std::runtime_error("Cannot write: " + dest);
                    out << content;
                    result["success"] = true;
                    result["path"]    = dest;
                }
            } catch (const std::exception& e) {
                result["success"] = false;
                result["error"]   = e.what();
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // GET /api/scripts/download?name=<file>  →  serve .lua as download
        CROW_ROUTE(app, "/api/scripts/download")([&](const crow::request& req) {
            const char* raw = req.url_params.get("name");
            std::string name = raw ? raw : "";
            if (name.empty() ||
                name.find('/') != std::string::npos ||
                name.find("..") != std::string::npos ||
                name.size() < 5 ||
                name.substr(name.size() - 4) != ".lua") {
                crow::response res(400, "Invalid filename");
                return res;
            }
            std::ifstream f(cfg.basePath + name, std::ios::binary);
            if (!f.is_open()) { crow::response res(404, "Not found"); return res; }
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            crow::response res(content);
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            res.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
            return res;
        });

        // -----------------------------------------------------------------
        // GET /api/settings  →  current config as JSON
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/settings")([&]() {
            json r = config_to_json();
            r["success"] = true;
            crow::response res(r.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/settings  →  update config and persist to file
        CROW_ROUTE(app, "/api/settings").methods("POST"_method)([&](const crow::request& req) {
            json result;
            try {
                auto body = json::parse(req.body);
                std::lock_guard<std::mutex> lk(lua_mutex);
                apply_settings_json(body);
                update_lua_path();
                if (cfg.imgEnabled) sync_img_cfg_from_config();
                save_settings_file();
                result["success"] = true;
                result["message"] = "Settings saved";
            } catch (const std::exception& e) {
                result["success"] = false;
                result["error"]   = e.what();
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // GET /api/servers/status  →  reachability of local helper servers
        CROW_ROUTE(app, "/api/servers/status")([&]() {
            json r;
            r["faceswap_locale"] = http_ping("http://localhost:8001/");
            r["qwen_locale"] = http_ping(cfg.imgI2iUrl.empty()
                                         ? "http://localhost:8000/health"
                                         : cfg.imgI2iUrl + "/health");
            r["t2i_locale"]  = http_ping("http://localhost:8003/");
            r["tts_locale"]  = http_ping((cfg.ttsUrl.empty() ? "http://localhost:8004" : cfg.ttsUrl) + "/health");
            r["rembg_locale"] = http_ping("http://localhost:8005/");
            r["success"] = true;
            crow::response res(r.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/servers/action  →  install/start/stop faceswap_locale, qwen_locale, t2i_locale
        CROW_ROUTE(app, "/api/servers/action").methods("POST"_method)([&](const crow::request& req) {
            json result;
            try {
                auto body   = json::parse(req.body);
                std::string server = body.value("server", "");
                std::string action = body.value("action", "");

                // Per-server env override wins over global env setting.
                // Fallback chain: server_env_type (request) → cfg per-server → global cfg.
                std::string srvEnvType = body.value("server_env_type", std::string{});
                std::string srvEnvPath = body.value("server_env_path", std::string{});
                if (srvEnvType.empty() && server == "tts_locale") {
                    srvEnvType = cfg.ttsLocaleEnvType;
                    srvEnvPath = cfg.ttsLocaleEnvPath;
                }
                std::string reqEnvType = srvEnvType.empty() ? body.value("py_env_type", cfg.pyEnvType) : srvEnvType;
                std::string reqEnvPath = srvEnvType.empty() ? body.value("py_env_path", cfg.pyEnvPath) : srvEnvPath;

                // Derive workspace root from basePath (scripts/ → workspace/)
                auto srv_base = [&]() -> std::string {
                    namespace fs = std::filesystem;
                    if (!cfg.basePath.empty()) {
                        fs::path bp(cfg.basePath);
                        if (bp.is_absolute())
                            return bp.parent_path().parent_path().string() + "/";
                    }
                    return "./";
                }();

                std::string script_path, log_file, pip_deps;
                if (server == "faceswap_locale") {
                    script_path = srv_base + "faceswap_locale/server.py";
                    log_file    = "/tmp/rpgai_faceswap_locale.log";
                    pip_deps    = "insightface onnxruntime fastapi uvicorn python-multipart pillow numpy opencv-python-headless";
                } else if (server == "qwen_locale") {
                    script_path = srv_base + "qwen_locale/server_locale.py";
                    log_file    = "/tmp/rpgai_qwen.log";
                    pip_deps    = "diffusers torch transformers accelerate peft bitsandbytes huggingface_hub fastapi uvicorn pillow python-multipart";
                } else if (server == "t2i_locale") {
                    script_path = srv_base + "t2i_locale/server.py";
                    log_file    = "/tmp/rpgai_t2i.log";
                    pip_deps    = "diffusers torch transformers accelerate fastapi uvicorn pillow pydantic";
                } else if (server == "rembg_locale") {
                    script_path = srv_base + "rembg_locale/server.py";
                    log_file    = "/tmp/rpgai_rembg.log";
                    pip_deps    = "transparent-background fastapi uvicorn python-multipart pillow numpy";
                } else if (server == "tts_locale") {
                    script_path = srv_base + "tts_locale/server.py";
                    log_file    = "/tmp/rpgai_tts.log";
                    // transformers pinned <4.37: BeamSearchScorer removed from public API in 4.37+.
                    // After install, run tts_locale/patch_tts_compat.py to fix torch.load weights_only
                    // and BeamSearchScorer import — Coqui TTS is unmaintained and incompatible with
                    // modern PyTorch/transformers without these patches.
                    pip_deps    = "transformers==4.36.2 TTS torchaudio soundfile numpy fastapi uvicorn python-multipart";
                } else {
                    result["success"] = false;
                    result["error"]   = "Unknown server: " + server;
                    crow::response res(result.dump());
                    res.set_header("Content-Type", "application/json");
                    return res;
                }

                // Build python/pip invocation from configured environment.
                auto py_cmd = [&]() -> std::string {
                    if (reqEnvType == "venv")  return reqEnvPath + "/bin/python3";
                    if (reqEnvType == "conda") {
                        std::string env = reqEnvPath.empty() ? "rpgai" : reqEnvPath;
                        return "conda run -n " + env + " python";
                    }
                    if (reqEnvType == "uv")    return "uv run python";
                    return "python3";
                };
                auto pip_cmd = [&]() -> std::string {
                    if (reqEnvType == "venv")  return reqEnvPath + "/bin/pip";
                    if (reqEnvType == "conda") {
                        std::string env = reqEnvPath.empty() ? "rpgai" : reqEnvPath;
                        return "conda run -n " + env + " pip";
                    }
                    if (reqEnvType == "uv")    return "uv pip";
                    return "pip3";
                };
                // On Linux, system Python is often "externally managed" — pass the override flag.
                auto pip_install_flags = [&]() -> std::string {
#ifdef __linux__
                    if (cfg.pyEnvType == "system") return " --break-system-packages";
#endif
                    return std::string{};
                }();

                if (action == "start") {
                    namespace fs = std::filesystem;
                    std::string cfg_args = (server == "qwen_locale") ? cfg.qwenLocaleArgs
                                         : (server == "tts_locale")  ? cfg.ttsLocaleArgs
                                         : std::string{};
                    std::string extra_args = body.value("extra_args", cfg_args);
                    // Strip shell metacharacters — args field is trusted (local admin UI)
                    // but prevent accidental injection from copy-paste.
                    for (char c : {'\'', '"', '`', ';', '&', '|', '(', ')', '<', '>'})
                        extra_args.erase(std::remove(extra_args.begin(), extra_args.end(), c), extra_args.end());

                    std::string srv_dir = fs::path(script_path).parent_path().string();
                    std::string srv_script = fs::path(script_path).filename().string();
                    std::string args_part = extra_args.empty() ? "" : " " + extra_args;
                    std::string cmd = "nohup bash -c 'cd \"" + srv_dir + "\" && " + py_cmd() + " " + srv_script + args_part + "' > " + log_file + " 2>&1 &";
                    system(cmd.c_str());
                    result["success"] = true;
                    result["message"] = "Starting " + server + " — log: " + log_file;
                } else if (action == "stop") {
                    std::string cmd = "pkill -f '" + script_path + "'";
                    system(cmd.c_str());
                    result["success"] = true;
                    result["message"] = "Stop signal sent to " + server;
                } else if (action == "install") {
                    std::string cmd;
                    if (reqEnvType == "conda") {
                        std::string env = reqEnvPath.empty() ? "rpgai" : reqEnvPath;
                        // Create env if missing, then install deps
                        cmd = "nohup bash -c \"(conda env list | grep -q '^" + env + " ' || conda create -n " + env + " python=3.10 -y) && conda run -n " + env + " pip install " + pip_deps + "\" > " + log_file + " 2>&1 &";
                    } else {
                        cmd = "nohup " + pip_cmd() + " install " + pip_deps + pip_install_flags + " > " + log_file + " 2>&1 &";
                    }
                    system(cmd.c_str());
                    result["success"] = true;
                    result["message"] = "Installing " + server + " deps — log: " + log_file;
                } else {
                    result["success"] = false;
                    result["error"]   = "Unknown action: " + action;
                }
            } catch (const std::exception& e) {
                result["success"] = false;
                result["error"]   = e.what();
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // GET /api/servers/models/t2i_locale  →  proxy model list from Python server
        CROW_ROUTE(app, "/api/servers/models/t2i_locale")([&]() {
            json result;
            try {
                std::string resp = img_detail::http_get("http://127.0.0.1:8003/models");
                auto j = json::parse(resp);
                result["success"] = true;
                result["models"]  = j.value("models",  json::array());
                result["loaded"]  = j.value("loaded",  json());
            } catch (const std::exception& e) {
                result["success"] = false;
                result["models"]  = json::array();
                result["loaded"]  = nullptr;
                result["error"]   = e.what();
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // =================================================================
        // CODERAI SESSION
        // =================================================================
        std::mutex coder_mutex;

        struct CoderPendingApproval {
            std::string tool_call_id;
            std::string tool_name;
            std::string tool_args;
            bool        is_danger      = false;
            json        messages_snapshot;
            int         remaining_iters = 0;
            std::string base_url;
            std::string api_key;
            std::string eff_model;
            std::string preview;
        };

        struct CodingSession {
            std::vector<Message>                  history;
            std::optional<CoderPendingApproval>   pending;
            std::string                           provider_override;
            std::string                           model_override;
            bool                                  active = false;
            // Async job completions to inject into the next user turn
            std::vector<std::string>              pending_notifications;
        };

        CodingSession coder_session;

        // CoderAI history persistence — without this, any server restart
        // (needed after every C++ rebuild, or a routine reload) silently wipes
        // the whole planning conversation (discussed NPCs, decisions, pending
        // context) since CodingSession only ever lived in memory. Persisted
        // as plain {role, content} pairs, not the transient CoderPendingApproval
        // (an in-flight tool call has no side effects yet if a restart hits
        // mid-approval, so it's fine for that one turn to be lost — the
        // conversation content is what matters).
        auto coder_history_path = [&]() -> std::string {
            return cfg.savePath + "coder_history.json";
        };
        // Cap: an unbounded conversation that now survives every restart (see
        // history persistence above) grows the context sent on every turn —
        // past a point that measurably degrades tool-calling reliability on
        // cheaper/faster models (they start replying in prose describing an
        // action instead of actually emitting a tool_call). Keep only the
        // most recent exchanges; older planning is still worth more than
        // nothing, but not at the cost of the model stopping using tools.
        static const size_t CODER_HISTORY_CAP = 40; // ~20 user/assistant turns
        auto save_coder_history = [&]() {
            json arr = json::array();
            size_t start = coder_session.history.size() > CODER_HISTORY_CAP
                ? coder_session.history.size() - CODER_HISTORY_CAP : 0;
            for (size_t i = start; i < coder_session.history.size(); ++i)
                arr.push_back({{"role", coder_session.history[i].role},
                                {"content", coder_session.history[i].content}});
            write_json_atomic(coder_history_path(), arr.dump());
        };
        auto load_coder_history = [&]() {
            std::ifstream f(coder_history_path());
            if (!f.good()) return;
            try {
                json arr; f >> arr;
                for (auto& m : arr) {
                    if (m.contains("role") && m.contains("content"))
                        coder_session.history.push_back(
                            {m["role"].get<std::string>(), m["content"].get<std::string>(), "", ""});
                }
                // Cap immediately on load too — a file grown large before this
                // cap existed (or from a long-running session) would otherwise
                // hand the model an oversized context on the very next turn.
                if (coder_session.history.size() > CODER_HISTORY_CAP)
                    coder_session.history.erase(coder_session.history.begin(),
                        coder_session.history.end() - CODER_HISTORY_CAP);
                if (!coder_session.history.empty()) coder_session.active = true;
            } catch (...) { /* corrupt file — start fresh, don't crash startup */ }
        };
        load_coder_history();

        // Resolves the CoderAI knowledge base directory. Explicit --coder-path
        // always wins. Otherwise the naive default (cfg.basePath + "coder_knowledge/")
        // is WRONG for the single most common launch pattern: private adventures
        // live under --path my_scripts/, but the shared knowledge base always
        // lives at scripts/coder_knowledge/ — the two are different directories.
        // Without this fallback, read_knowledge fails on every call for anyone
        // running with --path my_scripts/ and no explicit --coder-path, and
        // CoderAI is left improvising library APIs from memory for the entire
        // session (confirmed live: fabricated routine fields, wrong file
        // targets — all traced back to read_knowledge silently erroring out).
        auto resolve_coder_knowledge_path = [&]() -> std::string {
            if (!cfg.coderKnowledgePath.empty()) return cfg.coderKnowledgePath;
            std::string primary = cfg.basePath + "coder_knowledge/";
            std::error_code ec;
            if (std::filesystem::exists(primary, ec)) return primary;
            // Anchor the fallback on --asset-root (always an absolute repo path
            // when the caller sets it, as every launch script does) rather than
            // a bare relative "scripts/coder_knowledge/" — a relative path only
            // resolves if the process's CWD happens to be the repo root, which
            // is not guaranteed (e.g. launched from a GUI wrapper, cron, IDE).
            std::filesystem::path root = cfg.assetRoot.empty()
                ? std::filesystem::current_path(ec)
                : std::filesystem::absolute(cfg.assetRoot, ec);
            std::string fallback = (root / "scripts" / "coder_knowledge").string() + "/";
            if (std::filesystem::exists(fallback, ec)) return fallback;
            return primary; // neither exists — keep old path so the existing
                             // "directory NOT FOUND" diagnostics still fire
        };

        // Tool definitions (order: auto tier first, then confirm, then danger)
        std::vector<ToolDef> coder_tool_defs = {
            {"read_file",
             "Read a file. Path must start with scripts/, saves/, images/, my_scripts/, asset/, or catalog/.",
             R"({"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Relative path, e.g. scripts/my_adv.lua or catalog/vn_scene.json"}}})"},
            {"list_files",
             "List files matching a glob pattern. Example: scripts/lib/*.lua",
             R"({"type":"object","required":["pattern"],"properties":{"pattern":{"type":"string"}}})"},
            {"find_definition",
             "Find where a Lua function or variable is defined (grep in scripts/ and src/).",
             R"({"type":"object","required":["symbol"],"properties":{"symbol":{"type":"string"}}})"},
            {"find_usages",
             "Find all usages of a symbol across the codebase (recursive grep).",
             R"({"type":"object","required":["symbol"],"properties":{"symbol":{"type":"string"}}})"},
            {"check_lua_syntax",
             "Check a Lua code string for syntax errors using luajit.",
             R"({"type":"object","required":["code"],"properties":{"code":{"type":"string"}}})"},
            {"read_knowledge",
             "Read a documentation file from the knowledge base. Topics: quickstart (declarative adventures — default for new ones), lua_api, lib_adventure, lib_persona, lib_world, lib_agent, lib_memory, lib_tools, lib_visualnovel, lib_assets, patterns, template_ref, decisions_guide",
             R"({"type":"object","required":["topic"],"properties":{"topic":{"type":"string","description":"One of: lua_api, lib_adventure, lib_persona, lib_world, lib_agent, lib_memory, lib_tools, lib_visualnovel, lib_assets, patterns, template_ref, decisions_guide"}}})"},
            {"update_coder_memory",
             "Append a persistent note to coder_memory.md (survives session resets).",
             R"({"type":"object","required":["content"],"properties":{"content":{"type":"string"}}})"},
            {"write_file",
             "Create a new file. REQUIRES USER APPROVAL before execution.",
             R"({"type":"object","required":["path","content"],"properties":{"path":{"type":"string"},"content":{"type":"string"}}})"},
            {"str_replace",
             "Surgically modify an existing file by replacing exact text. REQUIRES USER APPROVAL. old_string must be unique in the file.",
             R"({"type":"object","required":["path","old_string","new_string"],"properties":{"path":{"type":"string"},"old_string":{"type":"string","description":"Exact text to replace — must be unique in file"},"new_string":{"type":"string"}}})"},
            {"delete_file",
             "Permanently delete a file. REQUIRES USER APPROVAL.",
             R"({"type":"object","required":["path"],"properties":{"path":{"type":"string"}}})"},
            // Game bridge tools (auto tier — read-only or safe)
            {"get_game_state",
             "Get the live game state (get_status_for_ai + get_state_snapshot + display). Requires an active PLAYING session.",
             R"({"type":"object","properties":{}})"},
            {"get_script_errors",
             "Get the last N Lua/script errors captured during the current session.",
             R"json({"type":"object","properties":{"limit":{"type":"integer","description":"Max errors to return (default 10)"}}})json"},
            {"reload_script",
             "Hot-reload the active Lua script without restarting the engine. Optionally preserves state via save/restore. Requires an active session.",
             R"json({"type":"object","properties":{"preserve_state":{"type":"boolean","description":"Save and restore state across reload (default false)"}}})json"},
            // NPC image helpers — read-only, no LLM, auto tier
            {"get_npc_description",
             "Return an NPC's physical appearance and current outfit (from persona.format_appearance). Use before generating NPC images to build an accurate prompt.",
             R"json({"type":"object","required":["id"],"properties":{"id":{"type":"string","description":"NPC id, e.g. 'federica'"}}})json"},
            {"get_adventure_style",
             "Return the current adventure's image style string (from get_image_style() Lua function). Use when building image prompts to stay consistent with the adventure's visual style.",
             R"({"type":"object","properties":{}})"},
            {"get_asset_path",
             "Return the filesystem path for an NPC or scene asset (from get_asset_path() Lua function). Use to know where to save or inject a generated image into the adventure.",
             R"json({"type":"object","required":["id"],"properties":{"id":{"type":"string","description":"Asset id, e.g. 'federica', 'bg_salotto'"}}})json"},
            // Phase 5 — Sandbox + state injection + deep undo
            {"run_lua",
             "Execute Lua code in a sandbox (separate lua_State, no access to live game state). Has access to standard libs, json, and query_llm. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["code"],"properties":{"code":{"type":"string"},"timeout_s":{"type":"integer","description":"Max seconds to run (default 30, max 60)"}}})json"},
            {"eval_lua",
             "Execute Lua code directly on the LIVE game state. Can read/modify state variables. Use for surgical state fixes. REQUIRES USER APPROVAL.",
             R"({"type":"object","required":["code"],"properties":{"code":{"type":"string"}}})"},
            {"call_undo",
             "Undo the last N game turns (uses in-memory undo stack, max 10). REQUIRES USER APPROVAL.",
             R"json({"type":"object","properties":{"steps":{"type":"integer","description":"Number of turns to undo (default 1, max 10)"}}})json"},
            {"load_save",
             "Load a save file and restore the game session. Use for deep undo (requires full save mode). REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["filename"],"properties":{"filename":{"type":"string","description":"Save filename from saves/ directory"}}})json"},
            // Phase 7 — Web search + image search
            {"web_search",
             "Search the web. Returns top 5 results (title, snippet, url). Default provider: DuckDuckGo (no key). Set --search-key for Brave Search.",
             R"({"type":"object","required":["query"],"properties":{"query":{"type":"string"}}})"},
            {"search_images",
             "Search for images. Default: DuckDuckGo images (no key, real web images). Use --pixabay-key for Pixabay. Use --search-provider openverse for Creative Commons only. Returns title, image_url, thumbnail_url, source_url.",
             R"json({"type":"object","required":["query"],"properties":{"query":{"type":"string"},"count":{"type":"integer","description":"Number of results (default 5, max 10)"}}})json"},
            {"download_asset",
             "Download an image from a URL and save it as a local asset. REQUIRES USER APPROVAL.",
             R"({"type":"object","required":["url","path"],"properties":{"url":{"type":"string","description":"https:// image URL"},"path":{"type":"string","description":"Save path, e.g. asset/vn/bg/cucina_giorno.jpg or my_scripts/images/bg_campagna.jpg"}}})"},
            {"copy_file",
             "Copy a file to another path (useful to reuse assets between adventures). REQUIRES USER APPROVAL.",
             R"({"type":"object","required":["src","dst"],"properties":{"src":{"type":"string","description":"Source path"},"dst":{"type":"string","description":"Destination path"}}})"},
            // Phase 6 — Image tools (native C++, use engine image config)
            {"analyze_image",
             "Describe or analyze an image using the vision LLM. Pass a local path (asset/, catalog/, scripts/, my_scripts/, images/) or an external https:// URL. Optional question to focus the analysis.",
             R"json({"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Local path (asset/, catalog/, scripts/, my_scripts/, images/) or https:// URL"},"question":{"type":"string","description":"What to look for or describe (default: general description)"}}})json"},
            {"ground_image",
             "Locate objects or regions in an image and return their bounding boxes as normalized [x1,y1,x2,y2] coordinates [0.0-1.0]. Use to find where something is before or after an edit, or to verify a modification happened at the expected position.",
             R"json({"type":"object","required":["path","query"],"properties":{"path":{"type":"string","description":"Local image path (asset/, my_scripts/, images/, saves/)"},"query":{"type":"string","description":"What to find, in English. E.g. 'windows', 'the person on the left', 'dark areas'"}}})json"},
            {"crop_image",
             "Crop a rectangular region from an image and save it as a new PNG. Combine with ground_image to extract a character or object by its bounding box. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["input_path","x","y","w","h"],"properties":{"input_path":{"type":"string","description":"Source image path (asset/, my_scripts/, images/, saves/)"},"x":{"type":"integer","description":"Left pixel coordinate"},"y":{"type":"integer","description":"Top pixel coordinate"},"w":{"type":"integer","description":"Width in pixels"},"h":{"type":"integer","description":"Height in pixels"},"output_path":{"type":"string","description":"Save path (default: input_stem_crop.png)"}}})json"},
            {"composite_image",
             "Alpha-blend an asset PNG (with transparent background) onto a background image at a given pixel position. Use after crop_image + background removal, or with a pre-cut asset. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["background_path","asset_path","x","y"],"properties":{"background_path":{"type":"string","description":"Background image path"},"asset_path":{"type":"string","description":"Asset PNG with alpha transparency (RGBA)"},"x":{"type":"integer","description":"Left pixel position on background"},"y":{"type":"integer","description":"Top pixel position on background"},"scale":{"type":"number","description":"Scale factor for asset before compositing (default 1.0)"},"alpha":{"type":"number","description":"Global opacity multiplier 0.0-1.0 (default 1.0)"},"output_path":{"type":"string","description":"Save path (default: background_stem_composite.png)"}}})json"},
            {"generate_image",
             "Generate an image from a text prompt using the configured t2i provider. Saves to save_path. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["prompt","save_path"],"properties":{"prompt":{"type":"string","description":"Image generation prompt"},"save_path":{"type":"string","description":"Where to save, e.g. asset/vn/bg/scene.png or my_scripts/images/owl.png"}}})json"},
            {"edit_image",
             "Edit an existing image using a natural language instruction (i2i via configured provider). REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["input_path","instruction"],"properties":{"input_path":{"type":"string","description":"Source image path"},"instruction":{"type":"string","description":"What to change, e.g. 'add a full moon in the sky'"},"output_path":{"type":"string","description":"Save path (default: input_stem_edited.ext)"},"lora_name":{"type":"string","description":"LoRA name to apply (qwen_local only, optional)"},"lora_scale":{"type":"number","description":"LoRA strength 0.0-1.0 (default 1.0)"}}})json"},
            // t2i_locale server tools (portrait/scene with face conditioning, reference management)
            {"generate_portrait",
             "Generate an NPC portrait via t2i_locale server (FLUX + IP-Adapter face conditioning). Requires --img-url pointing to t2i_locale. Use faceswap=true for hard face replacement instead of soft conditioning. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["prompt","save_path"],"properties":{"prompt":{"type":"string"},"save_path":{"type":"string","description":"e.g. asset/vn/npc/jenny_default.png or my_scripts/images/jenny.png"},"char_id":{"type":"string","description":"Character ID for face conditioning (must have a reference built)"},"id_scale":{"type":"number","description":"Face conditioning strength 0.0-1.0 (default 0.8)"},"save_as_ref":{"type":"boolean","description":"Save output as new reference for char_id (default false)"},"faceswap":{"type":"boolean","description":"Use hard face-swap instead of soft IP-Adapter conditioning (default false)"},"lora":{"type":"string","description":"LoRA name to apply during generation (optional)"},"steps":{"type":"integer","description":"Inference steps (default: server default ~20)"},"width":{"type":"integer","description":"Output width in pixels (default 512)"},"height":{"type":"integer","description":"Output height in pixels (default 768)"},"seed":{"type":"integer","description":"Random seed (-1 = random)"}}})json"},
            {"generate_scene",
             "Generate a scene with multiple NPCs via t2i_locale server (face conditioning per character). Requires --img-url pointing to t2i_locale. Use faceswap=true for hard face replacement. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["prompt","save_path"],"properties":{"prompt":{"type":"string"},"save_path":{"type":"string"},"chars":{"type":"array","items":{"type":"string"},"description":"Character IDs to include (must have references built)"},"id_scale":{"type":"number","description":"Face conditioning strength for all chars (default 0.8)"},"faceswap":{"type":"boolean","description":"Use hard face-swap instead of soft IP-Adapter conditioning (default false)"},"lora":{"type":"string","description":"LoRA name to apply during generation (optional)"},"steps":{"type":"integer","description":"Inference steps (default: server default)"},"width":{"type":"integer","description":"Output width in pixels"},"height":{"type":"integer","description":"Output height in pixels"},"seed":{"type":"integer","description":"Random seed (-1 = random)"}}})json"},
            {"t2i_reference",
             "Manage character face references on the t2i_locale server. Actions: list (show all refs), health (server status), add (upload reference image for char_id), build (compute embedding from uploaded set). Typical flow: crop_image to extract face → t2i_reference add → t2i_reference build → generate_portrait.",
             R"json({"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["list","health","add","build"]},"char_id":{"type":"string","description":"Required for add/build"},"file_path":{"type":"string","description":"Local image path for add action (asset/, my_scripts/, images/, saves/)"}}})json"},
            {"remove_background",
             "Remove the background from an image via rembg_locale server (port 8005), producing a transparent PNG (RGBA). Use for NPC portraits before adding them to the VN sprite catalog. Requires rembg_locale server running (rembg_locale/start.sh). REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["input_path","output_path"],"properties":{"input_path":{"type":"string","description":"Source image (asset/, my_scripts/, images/)"},"output_path":{"type":"string","description":"Output PNG with alpha transparency, e.g. asset/vn/npc/rossana_sprite.png"},"type":{"type":"string","enum":["rgba","map","white","green"],"description":"rgba=transparent bg (default), map=grayscale matte, white/green=flat bg"},"threshold":{"type":"number","description":"Hard-threshold matte 0..1 for crisper edges (optional, omit for soft matting)"}}})json"},
            // faceswap_locale tools (auto tier: health/detect/identify; confirm tier: segment/swap/restore/register)
            {"faceswap_health",
             "Check faceswap_locale server status and loaded models. Returns: registered (all NPC ids with .npy embedding), swap_ready (subset with .jpg crop, usable in faceswap_swap npc_ids). Requires --faceswap-url.",
             R"json({"type":"object","properties":{}})json"},
            {"faceswap_detect",
             "Detect all faces in an image. Returns list of faces with bounding box (bbox:[x1,y1,x2,y2]) and confidence. Useful before register/swap to see how many faces are present.",
             R"json({"type":"object","required":["input_path"],"properties":{"input_path":{"type":"string","description":"Source image path (asset/, my_scripts/, images/, saves/)"}}})json"},
            {"faceswap_identify",
             "Match detected faces to registered NPC embeddings. Returns matches with npc_id and similarity score. Use to verify a face was correctly registered.",
             R"json({"type":"object","required":["input_path"],"properties":{"input_path":{"type":"string","description":"Source image path (asset/, my_scripts/, images/, saves/)"},"threshold":{"type":"number","description":"Similarity threshold 0-1 (default 0.35)"}}})json"},
            {"faceswap_register",
             "Register an NPC face embedding from an image. The face is stored as <npc_id>.npy in faceswap_locale/faces/. Use a clear frontal face photo for best results. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["input_path","npc_id"],"properties":{"input_path":{"type":"string","description":"Source image with the NPC face (asset/, my_scripts/, images/, saves/)"},"npc_id":{"type":"string","description":"NPC identifier, e.g. 'jenny' or 'marco'"}}})json"},
            {"faceswap_segment",
             "Generate a face mask using FaceParser, XSeg, or CLIPSeg. Returns a PNG mask saved to output_path. Parts: face, skin, eye, left_eye, right_eye, brow, lips, upper_lip, lower_lip, inner_mouth, nose, ear, hair, neck. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["input_path","output_path"],"properties":{"input_path":{"type":"string","description":"Source image"},"output_path":{"type":"string","description":"Where to save the mask PNG"},"parts":{"type":"string","description":"Comma-separated parts: face, skin, eye, lips, hair, etc. (default: face)"},"text":{"type":"string","description":"Text-guided segmentation via CLIPSeg, e.g. 'mouth' or 'hair'"},"xseg":{"type":"boolean","description":"Use XSeg binary mask (default false)"},"xseg_amount":{"type":"number","description":"XSeg blend amount 0-1 (default 0)"},"threshold":{"type":"number","description":"Mask threshold 0-1 (default 0.4)"},"occlude":{"type":"boolean","description":"Exclude occluded regions (hands/objects blocking face, default false)"},"expand":{"type":"integer","description":"Dilate mask by N pixels (default 0)"},"blur":{"type":"integer","description":"Gaussian feather radius in pixels (default 0)"},"invert":{"type":"boolean","description":"Invert the mask (default false)"}}})json"},
            {"faceswap_swap",
             "Swap faces in a target image using registered NPC embeddings or source images. Output saved to output_path. Positions map detected faces (0=leftmost) to source faces. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["target_path","output_path"],"properties":{"target_path":{"type":"string","description":"Target image with faces to replace"},"output_path":{"type":"string","description":"Where to save the swapped image"},"npc_ids":{"type":"array","items":{"type":"string"},"description":"Ordered list of NPC ids to use (face 0→npc_ids[0], etc.). Must have registered embeddings."},"source_paths":{"type":"array","items":{"type":"string"},"description":"Alternative: source image paths instead of registered NPCs"},"positions":{"type":"string","description":"Comma-separated face indices to replace (default: all detected, left-to-right)"},"enhance":{"type":"boolean","description":"Run GFPGAN after swap for quality improvement (default false)"},"mask_path":{"type":"string","description":"Optional face mask PNG to limit swap region"}}})json"},
            {"faceswap_restore",
             "Restore/enhance face quality using CodeFormer or GFPGAN. CodeFormer is preferred (no torch, ONNX). Output saved to output_path. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["input_path","output_path"],"properties":{"input_path":{"type":"string","description":"Source image to restore"},"output_path":{"type":"string","description":"Where to save the restored image"},"restorer":{"type":"string","enum":["codeformer","gfpgan"],"description":"Which restorer to use (default: codeformer)"},"fidelity":{"type":"number","description":"CodeFormer fidelity 0-1: 0=max enhancement 1=max identity preservation (default 0.7)"},"only_center_face":{"type":"boolean","description":"Restore only the center/largest face (default false)"}}})json"},
            {"faceswap_upscale",
             "Upscale an image via faceswap_locale (Real-ESRGAN 4x if available, PIL Lanczos fallback). Output saved to output_path. Use after face swap to improve final resolution. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["input_path","output_path"],"properties":{"input_path":{"type":"string","description":"Source image to upscale"},"output_path":{"type":"string","description":"Where to save the upscaled image"},"scale":{"type":"integer","description":"Upscale factor (default 4, Real-ESRGAN supports 2 or 4)"}}})json"},
            // Video pipeline tools — multi-pass face swap on video clips
            // Output dirs go in video_work/ (whitelisted). Input video_path = any absolute path.
            {"video_extract",
             "Extract frames (PNG) and audio (AAC) from a video clip into output_dir/frames/ + output_dir/audio.aac. Optional start/end time to process only a segment. Requires ffmpeg. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["video_path","output_dir"],"properties":{"video_path":{"type":"string","description":"Absolute path to source video file"},"output_dir":{"type":"string","description":"Output directory, e.g. video_work/clip1 (created if missing)"},"start_time":{"type":"string","description":"Start time HH:MM:SS or seconds float (default: beginning)"},"end_time":{"type":"string","description":"End time HH:MM:SS or seconds float (default: end)"},"fps":{"type":"string","description":"Output frame rate, e.g. '30' (default: source fps)"}}})json"},
            {"video_analyze",
             "Detect faces in all extracted frames, cluster by identity, build manifest.json + cluster_review/ symlinks + per-cluster preview videos. Run AFTER video_extract. May take 30-90s for 1-minute clips. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["frames_dir","output_dir"],"properties":{"frames_dir":{"type":"string","description":"Path to frames directory (from video_extract), e.g. video_work/clip1/frames"},"output_dir":{"type":"string","description":"Where to save manifest.json and cluster_review/, e.g. video_work/clip1"},"cluster_threshold":{"type":"number","description":"Cosine similarity threshold for same identity (default 0.40 — lower = stricter)"},"make_previews":{"type":"boolean","description":"Generate per-cluster mini-videos for review (default true)"},"preview_fps":{"type":"string","description":"FPS for preview videos (default '30')"}}})json"},
            {"video_assign_npc",
             "Assign NPC ids to face clusters in manifest.json. Run AFTER video_analyze and reviewing cluster_review/ previews. Maps cluster_id→npc_id so swap knows which face to use. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["manifest_path","npc_assign"],"properties":{"manifest_path":{"type":"string","description":"Path to manifest.json (e.g. video_work/clip1/manifest.json)"},"npc_assign":{"type":"string","description":"JSON object mapping cluster_id to npc_id, e.g. {\"face_0\":\"jenny\",\"face_1\":\"marco\"}. NPCs must be registered (faceswap_register)."}}})json"},
            {"video_swap",
             "Swap faces in all pending frames according to manifest NPC assignments. Writes output to swapped_dir. Run after video_assign_npc. Use retry_only=true to reprocess only flagged frames after refinement. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["manifest_path","frames_dir","swapped_dir"],"properties":{"manifest_path":{"type":"string","description":"Path to manifest.json"},"frames_dir":{"type":"string","description":"Original frames directory (video_work/clip1/frames)"},"swapped_dir":{"type":"string","description":"Output directory for swapped frames, e.g. video_work/clip1/frames_swapped"},"retry_only":{"type":"boolean","description":"Only process frames with status=pending_retry (default false)"}}})json"},
            {"video_quality",
             "Analyze swapped frames: detection score, blur (Laplacian), inter-frame pose delta. Updates manifest with per-frame quality metrics and groups problematic frames into flagged_sequences. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["manifest_path","swapped_dir"],"properties":{"manifest_path":{"type":"string","description":"Path to manifest.json"},"swapped_dir":{"type":"string","description":"Directory containing swapped frames"},"det_threshold":{"type":"number","description":"Flag if face detection confidence below this (default 0.5)"},"blur_threshold":{"type":"number","description":"Flag if Laplacian variance below this (default 50.0 — lower = more blurry)"},"pose_threshold":{"type":"number","description":"Flag if inter-frame pose delta above this (default 30.0 degrees)"},"gap_frames":{"type":"integer","description":"Max gap between flagged frames to merge into one sequence (default 5)"}}})json"},
            {"video_patch",
             "Update swap settings or NPC assignments for a frame range in manifest.json, then mark as pending_retry. Use after video_quality to refine flagged sequences (add mask, expand, blur). REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["manifest_path"],"properties":{"manifest_path":{"type":"string","description":"Path to manifest.json"},"frame_start":{"type":"string","description":"First frame id e.g. '000043' (default: all frames)"},"frame_end":{"type":"string","description":"Last frame id inclusive (default: all frames)"},"npc_assign":{"type":"string","description":"JSON: reassign clusters e.g. {\"face_0\":\"jenny\"}"},"cluster_id":{"type":"string","description":"Only patch faces of this cluster_id"},"mask_parts":{"type":"string","description":"Comma-separated FaceParser parts: face,skin,eye,lips,hair,nose,ear,neck"},"expand":{"type":"integer","description":"Dilate mask by N pixels (default: unchanged)"},"blur":{"type":"integer","description":"Gaussian feather radius in pixels (default: unchanged)"},"skip":{"type":"boolean","description":"Mark frames as skip — use original in final video (default false)"},"status":{"type":"string","description":"Set frame status (default: pending_retry)"}}})json"},
            {"video_assemble",
             "Assemble final video from swapped frames + audio. Optionally apply CodeFormer/GFPGAN restoration and/or Real-ESRGAN upscale on each frame before encoding. Audio is muxed from output_dir/audio.aac or explicit audio_path. REQUIRES USER APPROVAL.",
             R"json({"type":"object","required":["manifest_path","swapped_dir","output_path"],"properties":{"manifest_path":{"type":"string","description":"Path to manifest.json"},"swapped_dir":{"type":"string","description":"Directory containing swapped frames"},"output_path":{"type":"string","description":"Output video file path, e.g. video_work/clip1/output.mp4"},"audio_path":{"type":"string","description":"Audio file to mux (default: auto-detect output_dir/audio.aac)"},"fps":{"type":"string","description":"Frame rate for output video (default '30')"},"restore":{"type":"string","enum":["codeformer","gfpgan",""],"description":"Face restoration to apply on each frame before encode (default: none)"},"fidelity":{"type":"number","description":"CodeFormer fidelity 0-1 (default 0.7)"},"upscale":{"type":"integer","description":"Upscale factor: 0=none, 2 or 4 (Real-ESRGAN if available, PIL Lanczos fallback)"}}})json"},
        };

        // 0=auto, 1=confirm, 2=danger
        auto coder_tool_tier = [](const std::string& name) -> int {
            if (name == "delete_file") return 2;
            if (name == "write_file" || name == "str_replace") return 1;
            if (name == "run_lua" || name == "eval_lua" ||
                name == "call_undo" || name == "load_save" ||
                name == "download_asset" || name == "copy_file" ||
                name == "generate_image" || name == "edit_image" ||
                name == "crop_image" || name == "composite_image" ||
                name == "generate_portrait" || name == "generate_scene" ||
                name == "t2i_reference" ||
                name == "remove_background" ||
                name == "faceswap_register" || name == "faceswap_segment" ||
                name == "faceswap_swap"     || name == "faceswap_restore" ||
                name == "faceswap_upscale"  ||
                name == "video_extract"     || name == "video_analyze"  ||
                name == "video_assign_npc"  || name == "video_swap"     ||
                name == "video_quality"     || name == "video_patch"    ||
                name == "video_assemble") return 1;
            return 0;
        };

        // Whitelist check: no absolute paths, no ".." traversal, must start with an
        // allowed prefix, AND the symlink-resolved real path must stay inside that
        // prefix root (defeats `images/link -> /etc` style symlink escapes).
        auto is_coder_path_allowed = [](const std::string& path) -> bool {
            namespace fs = std::filesystem;
            if (path.empty() || path[0] == '/') return false;
            if (path.find("..") != std::string::npos) return false;
            static const std::vector<std::string> prefixes =
                {"scripts/","saves/","images/","my_scripts/","asset/","catalog/","video_work/"};
            bool prefix_ok = false;
            std::string matched;
            for (auto& pfx : prefixes)
                if (path.size() >= pfx.size() && path.compare(0, pfx.size(), pfx) == 0) {
                    prefix_ok = true; matched = pfx; break;
                }
            if (!prefix_ok) return false;
            try {
                fs::path root = fs::weakly_canonical(fs::current_path() / matched);
                fs::path full = fs::weakly_canonical(fs::current_path() / path);
                auto [it, _] = std::mismatch(root.begin(), root.end(), full.begin(), full.end());
                return it == root.end();   // full must be at or below root
            } catch (...) {
                return false;
            }
        };

        // Append one line to coder_image_log.jsonl (persistent image prompt log).
        auto append_img_log = [&](const std::string& tool, const std::string& prompt, const std::string& path) {
            std::ofstream logf(cfg.basePath + "coder_image_log.jsonl", std::ios::app);
            if (!logf.good()) return;
            auto now = std::chrono::system_clock::now();
            auto tt  = std::chrono::system_clock::to_time_t(now);
            char ts[32]; std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", std::localtime(&tt));
            json entry = {{"ts",ts},{"tool",tool},{"prompt",prompt},{"path",path}};
            logf << entry.dump() << "\n";
        };

        // Human-readable preview string for approval modal
        auto make_tool_preview = [](const std::string& name, const json& a) -> std::string {
            if (name == "write_file") {
                std::string cnt = a.value("content","");
                std::string p = "Create: " + a.value("path","?") + "\n\n";
                p += cnt.size() > 800 ? cnt.substr(0,800) + "\n[...]" : cnt;
                return p;
            }
            if (name == "str_replace")
                return "Modify: " + a.value("path","?") +
                       "\n\n--- REMOVE ---\n" + a.value("old_string","") +
                       "\n\n+++ ADD +++\n"    + a.value("new_string","");
            if (name == "delete_file") return "DELETE: " + a.value("path","?");
            if (name == "run_lua") {
                std::string c = a.value("code","");
                return "run_lua (sandbox)\n\n" + (c.size() > 800 ? c.substr(0,800)+"\n[...]" : c);
            }
            if (name == "eval_lua") {
                std::string c = a.value("code","");
                return "eval_lua (LIVE STATE)\n\n" + (c.size() > 800 ? c.substr(0,800)+"\n[...]" : c);
            }
            if (name == "call_undo")
                return "Undo " + std::to_string(a.value("steps",1)) + " game turn(s) (in-memory)";
            if (name == "load_save")
                return "Load save file: " + a.value("filename","?") + "\n(replaces current session)";
            if (name == "download_asset")
                return "Download image\nFrom: " + a.value("url","?") + "\nTo:   " + a.value("path","?");
            if (name == "copy_file")
                return "Copy file\nFrom: " + a.value("src","?") + "\nTo:   " + a.value("dst","?");
            if (name == "crop_image")
                return "Crop image\nInput:  " + a.value("input_path","?") +
                       "\nRegion: x=" + std::to_string(a.value("x",0)) +
                       " y=" + std::to_string(a.value("y",0)) +
                       " w=" + std::to_string(a.value("w",0)) +
                       " h=" + std::to_string(a.value("h",0)) +
                       "\nOutput: " + a.value("output_path","(auto)");
            if (name == "composite_image")
                return "Composite image\nBackground: " + a.value("background_path","?") +
                       "\nAsset:      " + a.value("asset_path","?") +
                       "\nPosition:   x=" + std::to_string(a.value("x",0)) +
                       " y=" + std::to_string(a.value("y",0)) +
                       "\nScale:      " + std::to_string(a.contains("scale") ? (float)a["scale"].get<double>() : 1.0f) +
                       "\nOutput:     " + a.value("output_path","(auto)");
            if (name == "generate_image")
                return "Generate image\nPrompt: " + a.value("prompt","?") + "\nSave to: " + a.value("save_path","?");
            if (name == "edit_image")
                return "Edit image\nInput:  " + a.value("input_path","?") +
                       "\nChange: " + a.value("instruction","?") +
                       "\nOutput: " + a.value("output_path","(auto)");
            if (name == "generate_portrait") {
                std::string s = "Generate portrait\nPrompt: " + a.value("prompt","?") +
                                "\nSave:   " + a.value("save_path","?");
                if (!a.value("char_id","").empty()) s += "\nChar:   " + a["char_id"].get<std::string>();
                return s;
            }
            if (name == "generate_scene") {
                std::string chars_str;
                if (a.contains("chars")) for (auto& c : a["chars"]) chars_str += c.get<std::string>() + " ";
                return "Generate scene\nPrompt: " + a.value("prompt","?") +
                       "\nChars:  " + (chars_str.empty() ? "(none)" : chars_str) +
                       "\nSave:   " + a.value("save_path","?");
            }
            if (name == "t2i_reference")
                return "t2i_reference " + a.value("action","?") +
                       (a.value("char_id","").empty() ? "" : " [" + a["char_id"].get<std::string>() + "]");
            if (name == "remove_background")
                return "Remove background\nInput:  " + a.value("input_path","?") +
                       "\nOutput: " + a.value("output_path","?") +
                       "\nType:   " + a.value("type","rgba");
            if (name == "faceswap_register")
                return "Register face\nNPC:    " + a.value("npc_id","?") +
                       "\nSource: " + a.value("input_path","?");
            if (name == "faceswap_segment")
                return "Face segment\nInput:  " + a.value("input_path","?") +
                       "\nOutput: " + a.value("output_path","?") +
                       "\nParts:  " + a.value("parts", a.value("text","(default)"));
            if (name == "faceswap_swap") {
                std::string npcs;
                if (a.contains("npc_ids")) for (auto& n : a["npc_ids"]) npcs += n.get<std::string>() + " ";
                return "Face swap\nTarget: " + a.value("target_path","?") +
                       "\nNPCs:   " + (npcs.empty() ? "(source_paths)" : npcs) +
                       "\nOutput: " + a.value("output_path","?");
            }
            if (name == "faceswap_restore")
                return "Face restore\nInput:     " + a.value("input_path","?") +
                       "\nOutput:    " + a.value("output_path","?") +
                       "\nRestorer:  " + a.value("restorer","codeformer") +
                       "\nFidelity:  " + std::to_string((float)a.value("fidelity",0.7));
            if (name == "faceswap_upscale")
                return "Upscale " + std::to_string(a.value("scale",4)) + "x\nInput:  " +
                       a.value("input_path","?") + "\nOutput: " + a.value("output_path","?");
            if (name == "video_extract")
                return "Extract frames\nVideo:  " + a.value("video_path","?") +
                       "\nOutput: " + a.value("output_dir","?") +
                       (a.value("start_time","").empty() ? "" : "\nRange:  " + a["start_time"].get<std::string>() + " → " + a.value("end_time","end"));
            if (name == "video_analyze")
                return "Analyze frames\nInput:  " + a.value("frames_dir","?") +
                       "\nOutput: " + a.value("output_dir","?");
            if (name == "video_assign_npc")
                return "Assign NPCs\n" + a.value("npc_assign","?") +
                       "\nManifest: " + a.value("manifest_path","?");
            if (name == "video_swap")
                return "Swap frames\nManifest: " + a.value("manifest_path","?") +
                       "\nOutput:   " + a.value("swapped_dir","?") +
                       (a.value("retry_only",false) ? "\n[retry only]" : "");
            if (name == "video_quality")
                return "Quality analysis\nManifest: " + a.value("manifest_path","?") +
                       "\nSwapped:  " + a.value("swapped_dir","?");
            if (name == "video_patch")
                return "Patch manifest\n" + a.value("manifest_path","?") +
                       (a.value("frame_start","").empty() ? "" : "\nRange: " + a["frame_start"].get<std::string>() + "–" + a.value("frame_end","end")) +
                       (a.value("mask_parts","").empty() ? "" : "\nMask:  " + a["mask_parts"].get<std::string>());
            if (name == "video_assemble")
                return "Assemble video\nOutput:  " + a.value("output_path","?") +
                       "\nRestore: " + a.value("restore","none") +
                       "\nUpscale: " + std::to_string(a.value("upscale",0)) + "x";
            return name + " " + a.dump();
        };

        // Short description of a tool call (used in chat badges)
        auto make_tool_brief = [](const std::string& name, const json& a) -> std::string {
            if (name=="read_file"||name=="delete_file"||name=="copy_file")
                return a.value("path","");
            if (name=="write_file"||name=="str_replace")  return a.value("path","");
            if (name=="eval_lua")   return "(live state)";
            if (name=="run_lua")    return "(sandbox)";
            if (name=="find_definition"||name=="find_usages") return a.value("symbol","");
            if (name=="web_search"||name=="search_images")    return a.value("query","");
            if (name=="read_knowledge")  return a.value("topic","");
            if (name=="check_lua_syntax") return "(snippet)";
            if (name=="call_undo")  return std::to_string(a.value("steps",1))+" step(s)";
            if (name=="load_save")  return a.value("filename","");
            if (name=="reload_script") return a.value("preserve_state",false)?"(preserve)":"(reset)";
            if (name=="download_asset") return a.value("path","");
            if (name=="list_files") return a.value("pattern","");
            if (name=="analyze_image") return a.value("path","");
            if (name=="ground_image")  return a.value("path","");
            if (name=="crop_image")    return a.value("output_path", a.value("input_path","") + " (crop)");
            if (name=="composite_image") return a.value("output_path", a.value("background_path","") + " (composite)");
            if (name=="generate_image") return a.value("save_path","");
            if (name=="edit_image") return a.value("output_path", a.value("input_path","") + " (edited)");
            if (name=="generate_portrait") return a.value("char_id", a.value("save_path",""));
            if (name=="generate_scene")    return a.value("save_path","");
            if (name=="t2i_reference")     return a.value("action","") + " " + a.value("char_id","");
            if (name=="remove_background") return a.value("input_path","") + " → " + a.value("output_path","");
            if (name=="faceswap_health")   return "faceswap_locale";
            if (name=="faceswap_detect")   return a.value("input_path","");
            if (name=="faceswap_identify") return a.value("input_path","");
            if (name=="faceswap_register") return a.value("npc_id","") + " ← " + a.value("input_path","");
            if (name=="faceswap_segment")  return a.value("output_path","");
            if (name=="faceswap_swap")     return a.value("target_path","") + " → " + a.value("output_path","");
            if (name=="faceswap_restore")  return a.value("input_path","") + " → " + a.value("output_path","");
            if (name=="faceswap_upscale")  return a.value("input_path","") + " " + std::to_string(a.value("scale",4)) + "x";
            if (name=="video_extract")    return a.value("output_dir","");
            if (name=="video_analyze")    return a.value("output_dir","") + "/manifest.json";
            if (name=="video_assign_npc") return a.value("manifest_path","");
            if (name=="video_swap")       return a.value("swapped_dir","");
            if (name=="video_quality")    return a.value("manifest_path","");
            if (name=="video_patch")      return a.value("frame_start","all") + "–" + a.value("frame_end","all");
            if (name=="video_assemble")   return a.value("output_path","");
            return "";
        };

        // HTTP POST with multiple headers (used by vision queries)
        auto http_post_coder = [](const std::string& url,
                                   const std::string& body,
                                   const std::vector<std::string>& headers) -> std::string {
            CURL* c = curl_easy_init();
            if (!c) return "";
            std::string resp;
            struct curl_slist* hl = nullptr;
            for (auto& h : headers) hl = curl_slist_append(hl, h.c_str());
            if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
            curl_easy_setopt(c, CURLOPT_URL, url.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                +[](char* p, size_t s, size_t n, void* u) -> size_t {
                    static_cast<std::string*>(u)->append(p, s*n); return s*n; });
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
            curl_easy_perform(c);
            if (hl) curl_slist_free_all(hl);
            curl_easy_cleanup(c);
            return resp;
        };

        // Vision query: send an image + question to the coder LLM, return text description.
        // Falls back to main provider if coder provider not set.
        auto coder_vision_query = [&](const std::vector<uint8_t>& img_bytes,
                                       const std::string& mime_type,
                                       const std::string& question) -> std::string {
            std::string b64 = bytes_to_base64(img_bytes);
            std::string q = question.empty() ? "Describe this image in detail." : question;
            // Use dedicated vision model/provider if set, else fall back to coder model
            AIProvider prov  = cfg.coderVisionProvider;
            std::string model = cfg.coderVisionModel.empty() ? cfg.activeModel() : cfg.coderVisionModel;
            std::string key   = cfg.coderKey;

            if (prov == AIProvider::OPENROUTER || prov == AIProvider::OPENAI) {
                std::string url = (prov == AIProvider::OPENROUTER)
                    ? "https://openrouter.ai/api/v1/chat/completions"
                    : cfg.openai_baseUrl + "/v1/chat/completions";
                std::string eff_key = key.empty()
                    ? (prov == AIProvider::OPENROUTER ? cfg.openrouter_key : cfg.openai_key)
                    : key;
                json content = json::array({
                    {{"type","image_url"},{"image_url",{{"url","data:"+mime_type+";base64,"+b64}}}},
                    {{"type","text"},{"text",q}}
                });
                json req = {{"model",model},{"max_tokens",1024},
                            {"messages",json::array({{{"role","user"},{"content",content}}})}};
                std::string r = http_post_coder(url, req.dump(),
                    {"Content-Type: application/json","Authorization: Bearer "+eff_key});
                try { return json::parse(r)["choices"][0]["message"]["content"].get<std::string>(); }
                catch(...) { return "[analyze_image error] " + r.substr(0,300); }
            }
            if (prov == AIProvider::CLAUDE) {
                std::string eff_key = key.empty() ? cfg.claude_key : key;
                json content = json::array({
                    {{"type","image"},{"source",{{"type","base64"},{"media_type",mime_type},{"data",b64}}}},
                    {{"type","text"},{"text",q}}
                });
                json req = {{"model",model},{"max_tokens",1024},
                            {"messages",json::array({{{"role","user"},{"content",content}}})}};
                std::string r = http_post_coder("https://api.anthropic.com/v1/messages", req.dump(),
                    {"Content-Type: application/json",
                     "x-api-key: "+eff_key,
                     "anthropic-version: 2023-06-01"});
                try { return json::parse(r)["content"][0]["text"].get<std::string>(); }
                catch(...) { return "[analyze_image error] " + r.substr(0,300); }
            }
            if (prov == AIProvider::OLLAMA) {
                json req = {{"model",model},{"stream",false},
                            {"messages",json::array({{{"role","user"},{"content",q},
                                                      {"images",json::array({b64})}}})}};
                std::string r = http_post_coder(cfg.ollama_baseUrl+"/api/chat", req.dump(),
                    {"Content-Type: application/json"});
                try { return json::parse(r)["message"]["content"].get<std::string>(); }
                catch(...) { return "[analyze_image error] " + r.substr(0,300); }
            }
            if (prov == AIProvider::GEMINI) {
                std::string eff_key = key.empty() ? cfg.gemini_key : key;
                json parts = json::array({
                    {{"inline_data",{{"mime_type",mime_type},{"data",b64}}}},
                    {{"text",q}}
                });
                json req = {{"contents",json::array({{{"parts",parts}}})}};
                std::string r = http_post_coder(
                    "https://generativelanguage.googleapis.com/v1beta/models/"
                    + model + ":generateContent?key=" + eff_key, req.dump(),
                    {"Content-Type: application/json"});
                try { return json::parse(r)["candidates"][0]["content"]["parts"][0]["text"].get<std::string>(); }
                catch(...) { return "[analyze_image error] " + r.substr(0,300); }
            }
            return "[analyze_image: provider not supported for vision]";
        };

        // Structured visual grounding: image → [{label, bbox:[x1,y1,x2,y2], note}]
        // Uses vision model (coderVisionProvider/Model). bbox values normalized [0.0,1.0].
        auto coder_grounding_query = [&](const std::vector<uint8_t>& img_bytes,
                                         const std::string& mime_type,
                                         const std::string& query) -> std::string {
            std::string b64   = bytes_to_base64(img_bytes);
            AIProvider  prov  = cfg.coderVisionProvider;
            std::string model = cfg.coderVisionModel.empty() ? cfg.activeModel() : cfg.coderVisionModel;
            std::string key   = cfg.coderKey;

            // JSON schema for grounding output
            json grounding_schema = {
                {"type","object"},
                {"required", json::array({"objects"})},
                {"properties",{
                    {"objects",{
                        {"type","array"},
                        {"items",{
                            {"type","object"},
                            {"required", json::array({"label","bbox","note"})},
                            {"properties",{
                                {"label",{{"type","string"}}},
                                {"bbox",{{"type","array"},{"items",{{"type","number"}}},{"minItems",4},{"maxItems",4}}},
                                {"note",{{"type","string"}}}
                            }}
                        }}
                    }}
                }}
            };

            std::string sys_p =
                "You are a visual grounding assistant. Identify objects or regions in the image "
                "matching the query. Return bounding boxes as normalized [x1,y1,x2,y2] floats in [0.0,1.0] "
                "where (0,0)=top-left, (1,1)=bottom-right. Return ONLY valid JSON, no markdown.";
            std::string usr_p = "Query: " + query;

            // Helper: strip markdown code fences if model wraps JSON in ```
            auto strip_md = [](std::string s) -> std::string {
                auto st = s.find("```");
                if (st == std::string::npos) return s;
                st = s.find('\n', st); if (st == std::string::npos) return s;
                auto en = s.rfind("```"); if (en <= st) return s;
                return s.substr(st+1, en-st-1);
            };

            if (prov == AIProvider::OLLAMA) {
                // Use Ollama structured output (format field) for reliable JSON
                json req = {{"model",model},{"stream",false},
                            {"format", grounding_schema},
                            {"system", sys_p},
                            {"messages",json::array({{
                                {"role","user"},
                                {"content",usr_p},
                                {"images",json::array({b64})}
                            }})}};
                std::string r = http_post_coder(cfg.ollama_baseUrl+"/api/chat", req.dump(),
                    {"Content-Type: application/json"});
                try {
                    std::string content = json::parse(r)["message"]["content"].get<std::string>();
                    return json::parse(strip_md(content)).dump();
                }
                catch(...) { return json{{"error","grounding parse failed"},{"raw",r.substr(0,400)}}.dump(); }
            }
            if (prov == AIProvider::OPENROUTER || prov == AIProvider::OPENAI) {
                std::string url = (prov == AIProvider::OPENROUTER)
                    ? "https://openrouter.ai/api/v1/chat/completions"
                    : cfg.openai_baseUrl + "/v1/chat/completions";
                std::string eff_key = key.empty()
                    ? (prov == AIProvider::OPENROUTER ? cfg.openrouter_key : cfg.openai_key)
                    : key;
                json content_arr = json::array({
                    {{"type","image_url"},{"image_url",{{"url","data:"+mime_type+";base64,"+b64}}}},
                    {{"type","text"},{"text",usr_p}}
                });
                json req = {
                    {"model",model},{"max_tokens",1024},
                    {"response_format",{{"type","json_schema"},{"json_schema",{
                        {"name","grounding"},{"strict",true},{"schema",grounding_schema}
                    }}}},
                    {"messages",json::array({
                        {{"role","system"},{"content",sys_p}},
                        {{"role","user"},{"content",content_arr}}
                    })}
                };
                std::string r = http_post_coder(url, req.dump(),
                    {"Content-Type: application/json","Authorization: Bearer "+eff_key});
                try {
                    std::string txt = json::parse(r)["choices"][0]["message"]["content"].get<std::string>();
                    return json::parse(strip_md(txt)).dump();
                }
                catch(...) { return json{{"error","grounding parse failed"},{"raw",r.substr(0,400)}}.dump(); }
            }
            return json{{"error","provider not supported for grounding"}}.dump();
        };

        // Simple HTTP GET for web_search (10s timeout, no SSL verify for simplicity)
        auto http_get_coder = [](const std::string& url,
                                  const std::vector<std::string>& headers) -> std::string {
            CURL* c = curl_easy_init();
            if (!c) return "";
            std::string body;
            curl_easy_setopt(c, CURLOPT_URL, url.c_str());
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(c, CURLOPT_USERAGENT, "rpgai-coder/1.0");
            struct curl_slist* hl = nullptr;
            for (auto& h : headers) hl = curl_slist_append(hl, h.c_str());
            if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                +[](char* p, size_t s, size_t n, void* u) -> size_t {
                    static_cast<std::string*>(u)->append(p, s*n); return s*n; });
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
            curl_easy_perform(c);
            if (hl) curl_slist_free_all(hl);
            curl_easy_cleanup(c);
            return body;
        };

        // Reload active script. Caller must hold lua_mutex.
        auto do_script_reload = [&](bool preserve_state) -> json {
            if (active_script.empty())
                return {{"success",false},{"error","no active script"}};
            std::string snapshot;
            if (preserve_state && session_state == SessionState::PLAYING) {
                try { snapshot = lua["get_state_snapshot"]().get<std::string>(); } catch (...) {}
            }
            clear_optional_script_globals(lua);
            lua.script_file(cfg.basePath + active_script);
            load_script_tools(lua);
            bool state_restored = false;
            if (preserve_state && !snapshot.empty()) {
                try {
                    sol::protected_function rs = lua["restore_state"];
                    if (rs.valid()) { rs(snapshot); state_restored = true; }
                } catch (...) {}
            }
            return {{"success",true},{"script",active_script},{"state_preserved",state_restored}};
        };

        // Execute one coder tool. Returns JSON string result.
        auto execute_coder_tool = [&](const std::string& name, const std::string& args_json) -> std::string {
            json a;
            try { a = json::parse(args_json); } catch (...) { a = json::object(); }

            if (name == "read_file") {
                std::string path = a.value("path","");
                if (!is_coder_path_allowed(path))
                    return json{{"error","path not allowed: "+path}}.dump();
                std::ifstream f(path);
                if (!f.good()) return json{{"error","not found: "+path}}.dump();
                std::string c((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
                if (c.size() > 65536) c = c.substr(0,65536) + "\n[truncated at 64 KB]";
                return json{{"path",path},{"content",c},{"bytes",(int)c.size()}}.dump();
            }

            if (name == "list_files") {
                std::string pat = a.value("pattern","");
                if (pat.find("..") != std::string::npos)
                    return json{{"error","pattern not allowed"}}.dump();
                glob_t gl{}; int r = glob(pat.c_str(), GLOB_NOSORT, nullptr, &gl);
                json files = json::array();
                if (r == 0)
                    for (size_t i = 0; i < gl.gl_pathc && i < 200; ++i)
                        files.push_back(gl.gl_pathv[i]);
                globfree(&gl);
                return json{{"files",files},{"count",(int)files.size()}}.dump();
            }

            if (name == "find_definition" || name == "find_usages") {
                std::string sym = a.value("symbol","");
                std::string safe; for (char c : sym) if (isalnum(c)||c=='_'||c=='.') safe+=c;
                if (safe.empty()) return json{{"error","empty symbol"}}.dump();
                std::string pat = (name == "find_definition") ? ("function " + safe) : safe;
                std::string cmd = "grep -rn \"" + pat + "\" scripts/ src/ 2>/dev/null | head -30";
                FILE* p = popen(cmd.c_str(),"r");
                if (!p) return json{{"error","popen failed"}}.dump();
                std::string out; char buf[512];
                while (fgets(buf,sizeof(buf),p)) out += buf;
                pclose(p);
                return json{{"symbol",safe},{"results",out}}.dump();
            }

            if (name == "check_lua_syntax") {
                std::string code = a.value("code","");
                if (code.empty()) return json{{"ok",false},{"error","empty code"}}.dump();
                std::string tmp = "/tmp/rpgai_coder_" + std::to_string(getpid()) + ".lua";
                { std::ofstream tf(tmp); tf << code; }
                FILE* p = popen(("luajit -bl \""+tmp+"\" /dev/null 2>&1").c_str(),"r");
                std::string out;
                if (p) { char buf[512]; while (fgets(buf,sizeof(buf),p)) out+=buf; pclose(p); }
                std::remove(tmp.c_str());
                bool ok = out.empty() || out.find("error") == std::string::npos;
                return json{{"ok",ok},{"output",out}}.dump();
            }

            if (name == "read_knowledge") {
                std::string topic = a.value("topic","");
                // Sanitize: only allow alnum + _
                std::string safe; for (char c : topic) if (isalnum(c)||c=='_') safe+=c;
                if (safe.empty()) return json{{"error","empty topic"}}.dump();
                std::string kpath = resolve_coder_knowledge_path();
                std::string fpath = kpath + safe + ".md";
                std::ifstream f(fpath);
                if (!f.good()) {
                    std::error_code ec;
                    if (!std::filesystem::exists(kpath, ec)) {
                        // Self-describing failure: a missing DIRECTORY is a launch
                        // config problem, not a wrong topic — say so, or the model
                        // silently falls back to inventing library APIs from memory.
                        return json{{"error","knowledge base directory NOT FOUND: " + kpath +
                            " — the engine was launched without --coder-path (or with a wrong one). "
                            "STOP and tell the user to add --coder-path <repo>/scripts/coder_knowledge/ "
                            "to the launch command. Do NOT guess library APIs from memory."}}.dump();
                    }
                    return json{{"error","topic not found: "+safe+". Available: quickstart, lua_api, lib_adventure, lib_persona, lib_world, lib_agent, lib_memory, lib_tools, lib_visualnovel, lib_assets, patterns, template_ref, decisions_guide"}}.dump();
                }
                std::string c((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
                return json{{"topic",safe},{"content",c}}.dump();
            }

            if (name == "update_coder_memory") {
                std::string content = a.value("content","");
                if (content.empty()) return json{{"ok",false},{"error","empty content"}}.dump();
                std::ofstream f(cfg.basePath + "coder_memory.md", std::ios::app);
                if (!f.good()) return json{{"ok",false},{"error","cannot write coder_memory.md"}}.dump();
                f << "\n" << content;
                return json{{"ok",true}}.dump();
            }

            // A persona file written directly to disk (write_file/str_replace,
            // or run_lua's SEPARATE sandboxed Lua state) leaves the LIVE
            // game session's in-memory NPC registry stale — the running
            // adventure never learns the character exists until
            // persona.reload_all() runs. Relying on the model to remember
            // the documented "save → reload" step is not reliable (real live
            // bug: a freshly-created NPC was flatly denied by another
            // character because the running session never saw her). Auto-
            // reload instead, whenever the write lands inside the ACTIVE
            // adventure's persona directory (_PERSONA_BASE_PATH) and a game
            // is currently loaded.
            auto maybe_reload_after_persona_write = [&](const std::string& path, json& result) {
                std::lock_guard<std::mutex> llk(lua_mutex);
                if (session_state != SessionState::PLAYING) return;
                sol::object npc_path_obj = lua["_PERSONA_BASE_PATH"];
                if (!npc_path_obj.is<std::string>()) return;
                std::string npc_path = npc_path_obj.as<std::string>();
                if (npc_path.empty() || path.compare(0, npc_path.size(), npc_path) != 0) return;
                try {
                    result["auto_reloaded"] = do_script_reload(true);
                } catch (const std::exception& e) {
                    result["auto_reload_error"] = std::string(e.what());
                }
            };

            if (name == "write_file") {
                std::string path    = a.value("path","");
                std::string content = a.value("content","");
                if (!is_coder_path_allowed(path)) return json{{"error","path not allowed"}}.dump();
                size_t sl = path.rfind('/');
                if (sl != std::string::npos)
                    std::filesystem::create_directories(path.substr(0, sl));
                std::ofstream f(path);
                if (!f.good()) return json{{"error","cannot write: "+path}}.dump();
                f << content;
                json result = json{{"ok",true},{"path",path},{"bytes",(int)content.size()}};
                maybe_reload_after_persona_write(path, result);
                return result.dump();
            }

            if (name == "str_replace") {
                std::string path  = a.value("path","");
                std::string old_s = a.value("old_string","");
                std::string new_s = a.value("new_string","");
                if (!is_coder_path_allowed(path)) return json{{"error","path not allowed"}}.dump();
                std::ifstream fin(path);
                if (!fin.good()) return json{{"error","not found: "+path}}.dump();
                std::string c((std::istreambuf_iterator<char>(fin)),
                               std::istreambuf_iterator<char>()); fin.close();
                size_t pos = c.find(old_s);
                if (pos == std::string::npos) return json{{"error","old_string not found in file"}}.dump();
                if (c.find(old_s, pos+1) != std::string::npos)
                    return json{{"error","old_string not unique — add more surrounding context"}}.dump();
                c.replace(pos, old_s.size(), new_s);
                std::ofstream fout(path);
                if (!fout.good()) return json{{"error","cannot write: "+path}}.dump();
                fout << c;
                json result = json{{"ok",true},{"path",path}};
                maybe_reload_after_persona_write(path, result);
                return result.dump();
            }

            if (name == "delete_file") {
                std::string path = a.value("path","");
                if (!is_coder_path_allowed(path)) return json{{"error","path not allowed"}}.dump();
                if (std::remove(path.c_str()) != 0) return json{{"error","cannot delete: "+path}}.dump();
                return json{{"ok",true},{"path",path}}.dump();
            }

            // --- Game bridge tools ---

            if (name == "get_game_state") {
                std::lock_guard<std::mutex> llk(lua_mutex);
                if (session_state != SessionState::PLAYING)
                    return json{{"error","no active game session (must be PLAYING)"}}.dump();
                try {
                    std::string status   = lua["get_status_for_ai"]().get<std::string>();
                    std::string snapshot = lua["get_state_snapshot"]().get<std::string>();
                    std::string display;
                    sol::protected_function gds = lua["get_display_state"];
                    if (gds.valid()) { auto r = gds(); if (r.valid()) display = r.get<std::string>(); }
                    json out;
                    try { out["status"]   = json::parse(status); }   catch (...) { out["status"]   = status; }
                    try { out["snapshot"] = json::parse(snapshot); }  catch (...) { out["snapshot"] = snapshot; }
                    out["display"] = display;
                    out["script"]  = active_script;
                    // NPC directory set by persona.init() — available to CoderAI without reading script
                    sol::object npc_path = lua["_PERSONA_BASE_PATH"];
                    if (npc_path.is<std::string>())
                        out["npc_path"] = npc_path.as<std::string>();
                    return out.dump();
                } catch (const std::exception& e) {
                    return json{{"error",std::string(e.what())}}.dump();
                }
            }

            if (name == "get_script_errors") {
                int lim = a.value("limit", 10);
                std::lock_guard<std::mutex> g(script_error_mutex);
                json errs = json::array();
                int start = std::max(0, (int)script_error_ring.size() - lim);
                for (int i = start; i < (int)script_error_ring.size(); ++i)
                    errs.push_back(script_error_ring[i]);
                return json{{"errors",errs},{"count",(int)errs.size()}}.dump();
            }

            if (name == "reload_script") {
                bool preserve = a.value("preserve_state", false);
                std::lock_guard<std::mutex> llk(lua_mutex);
                try {
                    return do_script_reload(preserve).dump();
                } catch (const std::exception& e) {
                    push_script_error(std::string("reload: ") + e.what());
                    return json{{"error",std::string(e.what())}}.dump();
                }
            }

            // --- NPC image helpers ---

            if (name == "get_npc_description") {
                std::string id = a.value("id","");
                // Sanitize: only alphanumeric + underscore allowed (prevents Lua injection)
                for (char c : id)
                    if (!std::isalnum((unsigned char)c) && c != '_')
                        return json{{"error","invalid npc id (alphanumeric and _ only)"}}.dump();
                if (id.empty()) return json{{"error","id required"}}.dump();
                std::lock_guard<std::mutex> llk(lua_mutex);
                try {
                    std::string lua_code =
                        "local ok, p = pcall(require, 'lib/persona')\n"
                        "if ok and type(p)=='table' and p.format_appearance then\n"
                        "  return p.format_appearance('" + id + "', '12:00')\n"
                        "end\n"
                        "return nil";
                    auto res = lua.safe_script(lua_code);
                    if (res.valid()) {
                        sol::object val = res;
                        if (val.is<std::string>() && !val.as<std::string>().empty())
                            return json{{"description", val.as<std::string>()}}.dump();
                    }
                    // Fallback: read persona file directly
                    sol::object npc_path_obj = lua["_PERSONA_BASE_PATH"];
                    if (npc_path_obj.is<std::string>()) {
                        std::string fpath = npc_path_obj.as<std::string>() + id + ".lua";
                        std::ifstream f(fpath);
                        if (f.good()) {
                            std::string content((std::istreambuf_iterator<char>(f)), {});
                            // Extract appearance field with simple search
                            size_t pos = content.find("appearance");
                            if (pos != std::string::npos) {
                                size_t q1 = content.find('"', pos + 10);
                                size_t q2 = (q1 != std::string::npos) ? content.find('"', q1+1) : std::string::npos;
                                if (q2 != std::string::npos)
                                    return json{{"description", content.substr(q1+1, q2-q1-1)}, {"note","persona module not loaded; raw appearance field"}}.dump();
                            }
                        }
                    }
                    return json{{"error","NPC not found or persona module not loaded"}}.dump();
                } catch (const std::exception& e) {
                    return json{{"error",std::string(e.what())}}.dump();
                }
            }

            if (name == "get_adventure_style") {
                std::lock_guard<std::mutex> llk(lua_mutex);
                try {
                    sol::protected_function fn = lua["get_image_style"];
                    if (fn.valid()) {
                        auto r = fn();
                        if (r.valid()) {
                            sol::object val = r;
                            if (val.is<std::string>())
                                return json{{"style", val.as<std::string>()}}.dump();
                        }
                    }
                    return json{{"style",""},{"note","get_image_style() not defined in this adventure"}}.dump();
                } catch (const std::exception& e) {
                    return json{{"error",std::string(e.what())}}.dump();
                }
            }

            if (name == "get_asset_path") {
                std::string id = a.value("id","");
                for (char c : id)
                    if (!std::isalnum((unsigned char)c) && c != '_')
                        return json{{"error","invalid asset id (alphanumeric and _ only)"}}.dump();
                if (id.empty()) return json{{"error","id required"}}.dump();
                std::lock_guard<std::mutex> llk(lua_mutex);
                try {
                    sol::protected_function fn = lua["get_asset_path"];
                    if (fn.valid()) {
                        auto r = fn(id);
                        if (r.valid()) {
                            sol::object val = r;
                            if (val.is<std::string>() && !val.as<std::string>().empty())
                                return json{{"path", val.as<std::string>()}}.dump();
                        }
                    }
                    return json{{"error","get_asset_path() not defined or asset '" + id + "' not found"}}.dump();
                } catch (const std::exception& e) {
                    return json{{"error",std::string(e.what())}}.dump();
                }
            }

            // --- Phase 5: Sandbox Lua, eval_lua, call_undo, load_save ---

            if (name == "run_lua") {
                std::string code = a.value("code","");
                int timeout_s = std::min(a.value("timeout_s", 30), 60);
                if (code.empty()) return json{{"ok",false},{"error","empty code"}}.dump();

                sol::state sbox;
                // No io lib (io.open would bypass the path whitelist). os kept for
                // time/date only — every filesystem/process entry point is removed.
                sbox.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table,
                                    sol::lib::math, sol::lib::os, sol::lib::package);
                for (const char* osf : {"execute","exit","remove","rename",
                                         "tmpname","getenv","setlocale"})
                    sbox["os"][osf] = sol::lua_nil;
                // require can pull in arbitrary modules from disk; keep package.path
                // restricted (set below) but also drop loadfile/dofile file access.
                sbox["loadfile"] = sol::lua_nil;
                sbox["dofile"]   = sol::lua_nil;
                // Point package.path at scripts/lib/
                // scripts/?.lua  → require("lib/json") finds scripts/lib/json.lua
                // scripts/lib/?.lua → require("json") finds scripts/lib/json.lua
                sbox["package"]["path"] = cfg.basePath + "?.lua;" + cfg.basePath + "lib/?.lua";
                // query_llm bridge (uses engine's active provider/model by default)
                sbox.set_function("query_llm", [&](const std::string& sys,
                        const std::string& hist_json, const std::string& user,
                        sol::object schema_o, sol::object model_o, sol::object prov_o) -> std::string {
                    std::string schema = schema_o.is<std::string>() ? schema_o.as<std::string>() : "";
                    std::string model  = model_o.is<std::string>()  ? model_o.as<std::string>()  : cfg.activeModel();
                    std::string prov   = prov_o.is<std::string>()   ? prov_o.as<std::string>()   : cfg.providerName;
                    std::vector<Message> hist;
                    try { for (auto& m : json::parse(hist_json)) hist.push_back({m["role"],m["content"]}); } catch (...) {}
                    return query_llm(provider_from_string(prov), sys, hist, user, schema, model);
                });
                // get_tier bridge — lets run_lua discover gen/agent/ambient tier without hardcoding
                sbox.set_function("get_tier", [&](const std::string& name) -> sol::table {
                    sol::table t = sbox.create_table();
                    if (name == "gen") {
                        t["model"]    = cfg.genModel.empty()    ? cfg.activeModel() : cfg.genModel;
                        t["provider"] = cfg.genProvider.empty() ? cfg.providerName  : cfg.genProvider;
                    } else if (name == "agent") {
                        t["model"]    = cfg.agentModel.empty()    ? cfg.activeModel() : cfg.agentModel;
                        t["provider"] = cfg.agentProvider.empty() ? cfg.providerName  : cfg.agentProvider;
                    } else if (name == "ambient") {
                        t["model"]    = cfg.ambientModel.empty()    ? cfg.activeModel() : cfg.ambientModel;
                        t["provider"] = cfg.ambientProvider.empty() ? cfg.providerName  : cfg.ambientProvider;
                    } else {
                        t["model"]    = cfg.activeModel();
                        t["provider"] = cfg.providerName;
                    }
                    return t;
                });
                // Capture print output
                std::string output;
                sbox["print"] = [&](sol::variadic_args va, sol::this_state ts) {
                    sol::state_view sv(ts);
                    std::string line;
                    for (size_t i = 0; i < va.size(); ++i) {
                        if (i > 0) line += "\t";
                        try { line += va[i].as<std::string>(); }
                        catch (...) {
                            auto r = sv["tostring"](va[i]);
                            if (r.valid()) { try { line += r.get<std::string>(); } catch (...) { line += "(?)"; } }
                        }
                    }
                    line += "\n";
                    if (output.size() < 65536) output += line;
                };
                // Timeout via Lua debug hook reading a thread-local C++ deadline.
                // Not a Lua global — the executed script cannot reach or raise it.
                g_sandbox_deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()
                    + (long long)timeout_s * 1000;
                lua_State* sL = sbox.lua_state();
                lua_sethook(sL, [](lua_State* L, lua_Debug*) {
                    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (now > g_sandbox_deadline_ms)
                        luaL_error(L, "timeout: script exceeded time limit");
                }, LUA_MASKCOUNT, 10000);
                try {
                    auto res = sbox.safe_script(code, sol::script_pass_on_error);
                    lua_sethook(sL, nullptr, 0, 0);
                    if (!res.valid()) {
                        sol::error err = res;
                        return json{{"ok",false},{"error",std::string(err.what())},{"output",output}}.dump();
                    }
                    if (output.size() >= 65536) output += "\n[output truncated at 64KB]";
                    return json{{"ok",true},{"output",output}}.dump();
                } catch (const std::exception& e) {
                    lua_sethook(sL, nullptr, 0, 0);
                    return json{{"ok",false},{"error",std::string(e.what())},{"output",output}}.dump();
                }
            }

            if (name == "eval_lua") {
                std::string code = a.value("code","");
                if (code.empty()) return json{{"ok",false},{"error","empty code"}}.dump();
                std::lock_guard<std::mutex> llk(lua_mutex);
                if (session_state != SessionState::PLAYING)
                    return json{{"error","no active game session (must be PLAYING)"}}.dump();
                // Capture print, restore after
                sol::protected_function orig_print = lua["print"];
                std::string output;
                lua["print"] = [&](sol::variadic_args va, sol::this_state ts) {
                    sol::state_view sv(ts);
                    std::string line;
                    for (size_t i = 0; i < va.size(); ++i) {
                        if (i > 0) line += "\t";
                        try { line += va[i].as<std::string>(); }
                        catch (...) {
                            auto r = sv["tostring"](va[i]);
                            if (r.valid()) { try { line += r.get<std::string>(); } catch (...) { line += "(?)"; } }
                        }
                    }
                    line += "\n";
                    if (output.size() < 65536) output += line;
                };
                auto restore_print = [&]() {
                    if (orig_print.valid()) lua["print"] = orig_print;
                    else lua["print"] = sol::lua_nil;
                };
                try {
                    auto res = lua.safe_script(code, sol::script_pass_on_error);
                    restore_print();
                    if (!res.valid()) {
                        sol::error err = res;
                        push_script_error(std::string("eval_lua: ") + err.what());
                        return json{{"ok",false},{"error",std::string(err.what())},{"output",output}}.dump();
                    }
                    return json{{"ok",true},{"output",output}}.dump();
                } catch (const std::exception& e) {
                    restore_print();
                    push_script_error(std::string("eval_lua: ") + e.what());
                    return json{{"error",std::string(e.what())},{"output",output}}.dump();
                }
            }

            if (name == "call_undo") {
                int steps = std::max(1, std::min(a.value("steps", 1), 10));
                std::lock_guard<std::mutex> llk(lua_mutex);
                if (session_state != SessionState::PLAYING)
                    return json{{"error","no active game session"}}.dump();
                int done = 0;
                std::string err_msg;
                for (int i = 0; i < steps && !undo_stack.empty(); ++i) {
                    auto snap = undo_stack.back().first;
                    auto hist = undo_stack.back().second;
                    sol::protected_function rs = lua["restore_state"];
                    if (!rs.valid()) { err_msg = "restore_state not defined"; break; }
                    sol::protected_function_result rr = rs(snap);
                    if (!rr.valid()) {   // restore failed — keep checkpoint, stop
                        sol::error e = rr; err_msg = e.what(); break;
                    }
                    undo_stack.pop_back();
                    chat_history = hist;
                    ++done;
                }
                std::string display;
                try {
                    sol::protected_function gds = lua["get_display_state"];
                    if (gds.valid()) display = gds().get<std::string>();
                } catch (...) {}
                json out = {{"ok",done>0},{"steps_undone",done},
                            {"remaining_undo",(int)undo_stack.size()},{"display",display}};
                if (!err_msg.empty()) out["error"] = err_msg;
                return out.dump();
            }

            if (name == "load_save") {
                std::string save_name = a.value("filename","");
                if (save_name.empty()) return json{{"error","filename required"}}.dump();
                if (save_name.find('/') != std::string::npos ||
                    save_name.find('\\') != std::string::npos ||
                    save_name.find("..") != std::string::npos)
                    return json{{"error","invalid filename"}}.dump();
                std::lock_guard<std::mutex> llk(lua_mutex);
                std::string full_path = cfg.savePath + save_name;
                std::ifstream probe(full_path);
                if (!probe.is_open())
                    return json{{"error","file not found: " + save_name}}.dump();
                std::string line, last_ln;
                while (std::getline(probe, line)) if (!line.empty()) last_ln = line;
                probe.close();
                std::string script_to_load = cfg.script;
                try {
                    auto jl = json::parse(last_ln);
                    if (jl.contains("script") && jl["script"].is_string())
                        script_to_load = jl["script"].get<std::string>();
                } catch (...) {}
                try {
                    cfg.script = script_to_load;
                    clear_optional_script_globals(lua);
                    lua.script_file(cfg.basePath + cfg.script);
                    load_script_tools(lua);
                    chat_history.clear();
                    if (!load_session_from_jsonl(full_path, lua, chat_history))
                        return json{{"error","failed to restore session: " + save_name}}.dump();
                    cfg.saveFile = save_name;
                    if (full_stream.is_open()) full_stream.close();
                    if (cfg.saveMode == SaveMode::FULL)
                        full_stream.open(full_path, std::ios::app);
                    web_last_llm_reply.clear();
                    web_last_player_input.clear();
                    session_state = SessionState::PLAYING;
                    active_script = script_to_load;
                    g_active_script_stem = script_stem(script_to_load);
                    undo_stack.clear();
                    std::string display;
                    try { display = lua["get_display_state"]().get<std::string>(); } catch (...) {}
                    return json{{"ok",true},{"script",script_to_load},{"display",display}}.dump();
                } catch (const std::exception& e) {
                    push_script_error(std::string("load_save: ") + e.what());
                    return json{{"error",std::string(e.what())}}.dump();
                }
            }

            if (name == "copy_file") {
                std::string src = a.value("src","");
                std::string dst = a.value("dst","");
                if (!is_coder_path_allowed(src)) return json{{"error","src not allowed"}}.dump();
                if (!is_coder_path_allowed(dst))  return json{{"error","dst not allowed"}}.dump();
                try {
                    size_t sl = dst.rfind('/');
                    if (sl != std::string::npos) std::filesystem::create_directories(dst.substr(0,sl));
                    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
                    return json{{"ok",true},{"src",src},{"dst",dst}}.dump();
                } catch (const std::exception& e) {
                    return json{{"error",std::string(e.what())}}.dump();
                }
            }

            // Phase 6 — Image tools
            if (name == "analyze_image") {
                std::string path = a.value("path","");
                std::string question = a.value("question","");
                if (path.empty()) return json{{"error","path required"}}.dump();
                std::vector<uint8_t> img_bytes;
                std::string mime_type = "image/jpeg";
                if (path.substr(0,4) == "http") {
                    // Fetch external URL
                    CURL* c = curl_easy_init();
                    if (!c) return json{{"error","curl init failed"}}.dump();
                    curl_easy_setopt(c, CURLOPT_URL, path.c_str());
                    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
                    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
                    curl_easy_setopt(c, CURLOPT_USERAGENT,
                        "Mozilla/5.0 (X11; Linux x86_64; rv:120.0) Gecko/20100101 Firefox/120.0");
                    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                        +[](char* p, size_t s, size_t n, void* u) -> size_t {
                            auto* b = static_cast<std::vector<uint8_t>*>(u);
                            b->insert(b->end(), (uint8_t*)p, (uint8_t*)p+s*n); return s*n; });
                    curl_easy_setopt(c, CURLOPT_WRITEDATA, &img_bytes);
                    curl_easy_perform(c);
                    curl_easy_cleanup(c);
                } else {
                    if (!is_coder_path_allowed(path))
                        return json{{"error","path not allowed: "+path}}.dump();
                    std::ifstream f(path, std::ios::binary);
                    if (!f.good()) return json{{"error","file not found: "+path}}.dump();
                    img_bytes = std::vector<uint8_t>(
                        std::istreambuf_iterator<char>(f), {});
                }
                if (img_bytes.empty()) return json{{"error","failed to load image"}}.dump();
                // Sniff MIME
                auto ub = [&](int i) { return (unsigned char)img_bytes[i]; };
                if (img_bytes.size()>=4 && ub(0)==0x89 && img_bytes[1]=='P')
                    mime_type = "image/png";
                else if (img_bytes.size()>=12 &&
                         std::string(img_bytes.begin(), img_bytes.begin()+4) == "RIFF" &&
                         std::string(img_bytes.begin()+8, img_bytes.begin()+12) == "WEBP")
                    mime_type = "image/webp";
                std::string description = coder_vision_query(img_bytes, mime_type, question);
                json res = {{"description",description},{"bytes",(int)img_bytes.size()}};
                if (!question.empty()) res["question_asked"] = question;
                return res.dump();
            }

            if (name == "ground_image") {
                std::string path  = a.value("path","");
                std::string query = a.value("query","");
                if (path.empty())  return json{{"error","path required"}}.dump();
                if (query.empty()) return json{{"error","query required"}}.dump();
                if (!is_coder_path_allowed(path))
                    return json{{"error","path not allowed: "+path}}.dump();
                std::ifstream gf(path, std::ios::binary);
                if (!gf.good()) return json{{"error","file not found: "+path}}.dump();
                std::vector<uint8_t> gimg(std::istreambuf_iterator<char>(gf), {});
                if (gimg.empty()) return json{{"error","failed to load image"}}.dump();
                std::string gmime = "image/jpeg";
                if (gimg.size()>=4 && (unsigned char)gimg[0]==0x89 && gimg[1]=='P')
                    gmime = "image/png";
                else if (gimg.size()>=12 &&
                         std::string(gimg.begin(), gimg.begin()+4)=="RIFF" &&
                         std::string(gimg.begin()+8, gimg.begin()+12)=="WEBP")
                    gmime = "image/webp";
                return coder_grounding_query(gimg, gmime, query);
            }

            if (name == "crop_image") {
                std::string input_path  = a.value("input_path","");
                std::string output_path = a.value("output_path","");
                int cx = a.value("x", 0), cy = a.value("y", 0);
                int cw = a.value("w", 0), ch = a.value("h", 0);
                if (input_path.empty())  return json{{"error","input_path required"}}.dump();
                if (!is_coder_path_allowed(input_path))
                    return json{{"error","path not allowed: "+input_path}}.dump();
                if (cw <= 0 || ch <= 0) return json{{"error","w and h must be > 0"}}.dump();
                if (output_path.empty()) {
                    namespace fs = std::filesystem;
                    fs::path p(input_path);
                    output_path = (p.parent_path() / (p.stem().string() + "_crop.png")).string();
                }
                if (!is_coder_path_allowed(output_path))
                    return json{{"error","output path not allowed: "+output_path}}.dump();
                int src_w, src_h, src_ch;
                stbi_uc* src = stbi_load(input_path.c_str(), &src_w, &src_h, &src_ch, 4);
                if (!src) return json{{"error","cannot load image: "+input_path}}.dump();
                // clamp to bounds
                if (cx < 0) { cw += cx; cx = 0; }
                if (cy < 0) { ch += cy; cy = 0; }
                if (cx + cw > src_w) cw = src_w - cx;
                if (cy + ch > src_h) ch = src_h - cy;
                if (cw <= 0 || ch <= 0) { stbi_image_free(src); return json{{"error","crop region out of image bounds"}}.dump(); }
                std::vector<uint8_t> crop_buf(cw * ch * 4);
                for (int row = 0; row < ch; ++row)
                    std::memcpy(crop_buf.data() + row * cw * 4,
                                src + ((cy + row) * src_w + cx) * 4, cw * 4);
                stbi_image_free(src);
                namespace fs = std::filesystem;
                fs::create_directories(fs::path(output_path).parent_path());
                std::vector<uint8_t> png_bytes;
                auto wfn = [](void* ctx, void* data, int sz) {
                    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
                    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + sz);
                };
                if (!stbi_write_png_to_func(wfn, &png_bytes, cw, ch, 4, crop_buf.data(), cw * 4) || png_bytes.empty())
                    return json{{"error","PNG encode failed"}}.dump();
                { std::ofstream ofs(output_path, std::ios::binary);
                  if (!ofs.good()) return json{{"error","cannot write: "+output_path}}.dump();
                  ofs.write((char*)png_bytes.data(), png_bytes.size()); }
                return json{{"ok",true},{"path",output_path},{"width",cw},{"height",ch},{"bytes",(int)png_bytes.size()}}.dump();
            }

            if (name == "composite_image") {
                std::string bg_path     = a.value("background_path","");
                std::string asset_path  = a.value("asset_path","");
                std::string output_path = a.value("output_path","");
                int px = a.value("x", 0), py = a.value("y", 0);
                float scale = a.contains("scale") ? (float)a["scale"].get<double>() : 1.0f;
                float alpha = a.contains("alpha") ? (float)a["alpha"].get<double>() : 1.0f;
                if (bg_path.empty())    return json{{"error","background_path required"}}.dump();
                if (asset_path.empty()) return json{{"error","asset_path required"}}.dump();
                if (!is_coder_path_allowed(bg_path))
                    return json{{"error","path not allowed: "+bg_path}}.dump();
                if (!is_coder_path_allowed(asset_path))
                    return json{{"error","path not allowed: "+asset_path}}.dump();
                if (output_path.empty()) {
                    namespace fs = std::filesystem;
                    fs::path p(bg_path);
                    output_path = (p.parent_path() / (p.stem().string() + "_composite.png")).string();
                }
                if (!is_coder_path_allowed(output_path))
                    return json{{"error","output path not allowed: "+output_path}}.dump();
                // Load background
                int bg_w, bg_h, bg_ch;
                stbi_uc* bg_raw = stbi_load(bg_path.c_str(), &bg_w, &bg_h, &bg_ch, 4);
                if (!bg_raw) return json{{"error","cannot load background: "+bg_path}}.dump();
                std::vector<uint8_t> canvas(bg_raw, bg_raw + bg_w * bg_h * 4);
                stbi_image_free(bg_raw);
                // Load asset
                int as_w, as_h, as_ch;
                stbi_uc* as_raw = stbi_load(asset_path.c_str(), &as_w, &as_h, &as_ch, 4);
                if (!as_raw) return json{{"error","cannot load asset: "+asset_path}}.dump();
                int dst_w = as_w, dst_h = as_h;
                std::vector<uint8_t> as_scaled;
                const uint8_t* as_ptr = as_raw;
                if (scale != 1.0f && scale > 0.0f) {
                    dst_w = std::max(1, (int)(as_w * scale));
                    dst_h = std::max(1, (int)(as_h * scale));
                    as_scaled.resize(dst_w * dst_h * 4);
                    stbir_resize_uint8_linear(as_raw, as_w, as_h, 0,
                                              as_scaled.data(), dst_w, dst_h, 0, STBIR_RGBA);
                    as_ptr = as_scaled.data();
                }
                // Alpha-blend
                for (int row = 0; row < dst_h; ++row) {
                    int dy = py + row;
                    if (dy < 0 || dy >= bg_h) continue;
                    for (int col = 0; col < dst_w; ++col) {
                        int dx = px + col;
                        if (dx < 0 || dx >= bg_w) continue;
                        int si = (row * dst_w + col) * 4;
                        int di = (dy * bg_w + dx) * 4;
                        float sa = (as_ptr[si+3] / 255.0f) * alpha;
                        float da = canvas[di+3] / 255.0f;
                        float oa = sa + da * (1.0f - sa);
                        if (oa > 0.0f)
                            for (int c = 0; c < 3; ++c)
                                canvas[di+c] = (uint8_t)((as_ptr[si+c]*sa + canvas[di+c]*da*(1.0f-sa)) / oa);
                        canvas[di+3] = (uint8_t)(oa * 255.0f);
                    }
                }
                stbi_image_free(as_raw);
                // Save
                namespace fs = std::filesystem;
                fs::create_directories(fs::path(output_path).parent_path());
                std::vector<uint8_t> png_bytes;
                auto wfn = [](void* ctx, void* data, int sz) {
                    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
                    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + sz);
                };
                if (!stbi_write_png_to_func(wfn, &png_bytes, bg_w, bg_h, 4, canvas.data(), bg_w * 4) || png_bytes.empty())
                    return json{{"error","PNG encode failed"}}.dump();
                { std::ofstream ofs(output_path, std::ios::binary);
                  if (!ofs.good()) return json{{"error","cannot write: "+output_path}}.dump();
                  ofs.write((char*)png_bytes.data(), png_bytes.size()); }
                return json{{"ok",true},{"path",output_path},{"width",bg_w},{"height",bg_h},
                            {"asset_placed",json{{"x",px},{"y",py},{"w",dst_w},{"h",dst_h}}}}.dump();
            }

            if (name == "generate_image") {
                std::string prompt    = a.value("prompt","");
                std::string save_path = a.value("save_path","");
                if (prompt.empty())    return json{{"error","prompt required"}}.dump();
                if (save_path.empty()) return json{{"error","save_path required"}}.dump();
                if (!is_coder_path_allowed(save_path))
                    return json{{"error","path not allowed: "+save_path}}.dump();
                if (img_cfg.provider == ImageProvider::SDCPP_LOCAL &&
                    img_cfg.url.find("localhost") != std::string::npos) {
                    // SDCPP_LOCAL is the fallback — only warn if nothing is actually configured
                    // but let it try anyway; the error will come from the provider itself
                }
                size_t sl = save_path.rfind('/');
                if (sl != std::string::npos)
                    std::filesystem::create_directories(save_path.substr(0,sl));
                try {
                    std::vector<uint8_t> result = text_to_image(prompt, save_path);
                    if (result.empty()) return json{{"error","t2i returned empty result"}}.dump();
                    append_img_log("generate_image", prompt, save_path);
                    return json{{"ok",true},{"path",save_path},{"bytes",(int)result.size()}}.dump();
                } catch (const std::exception& e) {
                    return json{{"error",std::string(e.what())}}.dump();
                }
            }

            if (name == "edit_image") {
                std::string input_path   = a.value("input_path","");
                std::string instruction  = a.value("instruction","");
                std::string output_path  = a.value("output_path","");
                std::string lora_name    = a.value("lora_name","");
                float       lora_scale   = a.contains("lora_scale") ? (float)a["lora_scale"].get<double>() : 1.0f;
                if (input_path.empty())  return json{{"error","input_path required"}}.dump();
                if (instruction.empty()) return json{{"error","instruction required"}}.dump();
                if (!is_coder_path_allowed(input_path))
                    return json{{"error","path not allowed: "+input_path}}.dump();
                // Compute default output path: stem + "_edited" + ext
                if (output_path.empty()) {
                    size_t dot = input_path.rfind('.');
                    output_path = (dot != std::string::npos)
                        ? input_path.substr(0,dot) + "_edited" + input_path.substr(dot)
                        : input_path + "_edited";
                }
                if (!is_coder_path_allowed(output_path))
                    return json{{"error","output_path not allowed: "+output_path}}.dump();
                std::ifstream f(input_path, std::ios::binary);
                if (!f.good()) return json{{"error","file not found: "+input_path}}.dump();
                std::vector<uint8_t> img_bytes(std::istreambuf_iterator<char>(f), {});
                if (img_bytes.empty()) return json{{"error","failed to load image"}}.dump();
                size_t sl = output_path.rfind('/');
                if (sl != std::string::npos)
                    std::filesystem::create_directories(output_path.substr(0,sl));
                // Async: start detached thread, return job_id immediately.
                // Use qwen_local::img2img overload with explicit lora params (thread-safe,
                // does not touch img_cfg globals). Falls back to generic image_to_image
                // when provider is not qwen_local (lora_name ignored in that case).
                append_img_log("edit_image", instruction, output_path);
                std::string job_id = "cij_" + std::to_string(++coder_img_counter);
                {
                    std::lock_guard<std::mutex> lk(coder_img_mutex);
                    coder_img_jobs[job_id] = CoderImgJob{CoderImgJob::St::RUNNING, output_path, ""};
                }
                bool use_lora = !lora_name.empty() && (img_cfg.i2i_provider == ImageProvider::QWEN_LOCAL);
                std::thread([job_id, img_bytes = std::move(img_bytes), instruction, output_path,
                             lora_name, lora_scale, use_lora]() {
                    try {
                        std::vector<uint8_t> result;
                        if (use_lora)
                            result = qwen_local::img2img(img_bytes, instruction, lora_name, lora_scale);
                        else
                            result = image_to_image(img_bytes, instruction);
                        if (result.empty()) throw std::runtime_error("i2i returned empty result");
                        save_image(result, output_path);
                        std::lock_guard<std::mutex> lk(coder_img_mutex);
                        coder_img_jobs[job_id].status = CoderImgJob::St::DONE;
                    } catch (const std::exception& e) {
                        std::lock_guard<std::mutex> lk(coder_img_mutex);
                        coder_img_jobs[job_id].status = CoderImgJob::St::ERROR;
                        coder_img_jobs[job_id].error  = e.what();
                    }
                }).detach();
                json resp = {{"status","processing"},{"job_id",job_id},{"output_path",output_path},
                             {"message","Image editing started in background. The UI will notify when done."}};
                if (use_lora) resp["lora"] = lora_name;
                return resp.dump();
            }

            // t2i_locale server tools
            // Helper: POST JSON body to t2i_locale, receive binary PNG response
            auto t2i_post_image = [&](const std::string& endpoint,
                                       const std::string& body,
                                       const std::string& save_path) -> std::string {
                if (img_cfg.url.empty())
                    return json{{"error","t2i server URL not configured (--img-url)"}}.dump();
                std::string url = img_cfg.url + endpoint;
                CURL* c = curl_easy_init();
                if (!c) return json{{"error","curl init"}}.dump();
                std::vector<uint8_t> result;
                struct curl_slist* hl = nullptr;
                hl = curl_slist_append(hl, "Content-Type: application/json");
                curl_easy_setopt(c, CURLOPT_URL,        url.c_str());
                curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
                curl_easy_setopt(c, CURLOPT_POSTFIELDS,     body.c_str());
                curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE,  (long)body.size());
                curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                    +[](char* p, size_t s, size_t n, void* u) -> size_t {
                        auto* b = static_cast<std::vector<uint8_t>*>(u);
                        b->insert(b->end(),(uint8_t*)p,(uint8_t*)p+s*n); return s*n; });
                curl_easy_setopt(c, CURLOPT_WRITEDATA, &result);
                CURLcode rc = curl_easy_perform(c);
                long http_code = 0;
                curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
                curl_slist_free_all(hl);
                curl_easy_cleanup(c);
                if (rc != CURLE_OK)
                    return json{{"error",std::string(curl_easy_strerror(rc))}}.dump();
                if (http_code >= 400) {
                    std::string err(result.begin(), result.end());
                    return json{{"error","server "+std::to_string(http_code)+": "+err.substr(0,200)}}.dump();
                }
                if (result.empty()) return json{{"error","empty response from server"}}.dump();
                size_t sl = save_path.rfind('/');
                if (sl != std::string::npos) std::filesystem::create_directories(save_path.substr(0,sl));
                std::ofstream f(save_path, std::ios::binary);
                f.write((char*)result.data(), result.size());
                return json{{"ok",true},{"path",save_path},{"bytes",(int)result.size()}}.dump();
            };

            if (name == "generate_portrait") {
                std::string prompt    = a.value("prompt","");
                std::string save_path = a.value("save_path","");
                std::string char_id   = a.value("char_id","");
                double id_scale       = a.value("id_scale", 0.8);
                bool save_as_ref      = a.value("save_as_ref", false);
                bool faceswap         = a.value("faceswap", false);
                std::string lora      = a.value("lora","");
                if (prompt.empty())    return json{{"error","prompt required"}}.dump();
                if (save_path.empty()) return json{{"error","save_path required"}}.dump();
                if (!is_coder_path_allowed(save_path))
                    return json{{"error","path not allowed: "+save_path}}.dump();
                json payload = {{"prompt",prompt},{"character_id",char_id},
                                {"id_scale",id_scale},{"save_as_reference",save_as_ref},
                                {"faceswap",faceswap}};
                if (!lora.empty())           payload["lora"]    = lora;
                if (a.contains("steps"))     payload["steps"]   = a["steps"].get<int>();
                if (a.contains("width"))     payload["width"]   = a["width"].get<int>();
                if (a.contains("height"))    payload["height"]  = a["height"].get<int>();
                if (a.contains("seed"))      payload["seed"]    = a["seed"].get<int>();
                { std::string r = t2i_post_image("/generate_portrait", payload.dump(), save_path);
                  try { if (json::parse(r).value("ok",false)) append_img_log("generate_portrait", prompt, save_path); } catch(...) {}
                  return r; }
            }

            if (name == "generate_scene") {
                std::string prompt    = a.value("prompt","");
                std::string save_path = a.value("save_path","");
                double id_scale       = a.value("id_scale", 0.8);
                bool faceswap         = a.value("faceswap", false);
                std::string lora      = a.value("lora","");
                if (prompt.empty())    return json{{"error","prompt required"}}.dump();
                if (save_path.empty()) return json{{"error","save_path required"}}.dump();
                if (!is_coder_path_allowed(save_path))
                    return json{{"error","path not allowed: "+save_path}}.dump();
                json chars_arr = json::array();
                if (a.contains("chars"))
                    for (auto& c : a["chars"])
                        chars_arr.push_back({{"id",c.get<std::string>()},{"id_scale",id_scale}});
                json payload = {{"prompt",prompt},{"characters",chars_arr},{"faceswap",faceswap}};
                if (!lora.empty())           payload["lora"]    = lora;
                if (a.contains("steps"))     payload["steps"]   = a["steps"].get<int>();
                if (a.contains("width"))     payload["width"]   = a["width"].get<int>();
                if (a.contains("height"))    payload["height"]  = a["height"].get<int>();
                if (a.contains("seed"))      payload["seed"]    = a["seed"].get<int>();
                { std::string r = t2i_post_image("/generate_scene", payload.dump(), save_path);
                  try { if (json::parse(r).value("ok",false)) append_img_log("generate_scene", prompt, save_path); } catch(...) {}
                  return r; }
            }

            if (name == "t2i_reference") {
                std::string action   = a.value("action","");
                std::string char_id  = a.value("char_id","");
                std::string file_path = a.value("file_path","");
                if (img_cfg.url.empty())
                    return json{{"error","t2i server URL not configured (--img-url)"}}.dump();
                std::string base = img_cfg.url;

                if (action == "list" || action == "health") {
                    std::string endpoint = (action == "health") ? "/health" : "/references";
                    std::string resp = http_get_coder(base + endpoint, {"Accept: application/json"});
                    // Return raw JSON from server
                    try { (void)json::parse(resp); return resp; } catch(...) { return json{{"raw",resp}}.dump(); }
                }
                if (action == "build") {
                    if (char_id.empty()) return json{{"error","char_id required for build"}}.dump();
                    return http_post_coder(base + "/references/build",
                        json{{"character_id",char_id}}.dump(),
                        {"Content-Type: application/json"});
                }
                if (action == "add") {
                    if (char_id.empty())   return json{{"error","char_id required for add"}}.dump();
                    if (file_path.empty()) return json{{"error","file_path required for add"}}.dump();
                    if (!is_coder_path_allowed(file_path))
                        return json{{"error","file_path not allowed"}}.dump();
                    if (!std::filesystem::exists(file_path))
                        return json{{"error","file not found: "+file_path}}.dump();
                    CURL* c = curl_easy_init();
                    if (!c) return json{{"error","curl init"}}.dump();
                    std::string resp;
                    curl_mime* mime = curl_mime_init(c);
                    // character_id field
                    curl_mimepart* part = curl_mime_addpart(mime);
                    curl_mime_name(part, "character_id");
                    curl_mime_data(part, char_id.c_str(), CURL_ZERO_TERMINATED);
                    // save_to_set field
                    part = curl_mime_addpart(mime);
                    curl_mime_name(part, "save_to_set");
                    curl_mime_data(part, "true", CURL_ZERO_TERMINATED);
                    // file field
                    part = curl_mime_addpart(mime);
                    curl_mime_name(part, "file");
                    curl_mime_filedata(part, file_path.c_str());
                    std::string fname = file_path.substr(file_path.rfind('/')+1);
                    (void)curl_mime_filename(part, fname.c_str());
                    (void)curl_mime_type(part, "image/jpeg");
                    curl_easy_setopt(c, CURLOPT_URL, (base + "/references/add").c_str());
                    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
                    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
                    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                        +[](char* p, size_t s, size_t n, void* u) -> size_t {
                            static_cast<std::string*>(u)->append(p,s*n); return s*n; });
                    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
                    curl_easy_perform(c);
                    curl_mime_free(mime);
                    curl_easy_cleanup(c);
                    try { (void)json::parse(resp); return resp; } catch(...) { return json{{"raw",resp}}.dump(); }
                }
                return json{{"error","unknown action: "+action+" (list|health|add|build)"}}.dump();
            }

            if (name == "remove_background") {
                std::string input_path  = a.value("input_path","");
                std::string output_path = a.value("output_path","");
                std::string bg_type     = a.value("type","rgba");
                double threshold        = a.value("threshold", -1.0);
                if (input_path.empty())  return json{{"error","input_path required"}}.dump();
                if (output_path.empty()) return json{{"error","output_path required"}}.dump();
                if (!is_coder_path_allowed(input_path))
                    return json{{"error","input_path not allowed: "+input_path}}.dump();
                if (!is_coder_path_allowed(output_path))
                    return json{{"error","output_path not allowed: "+output_path}}.dump();
                if (!std::filesystem::exists(input_path))
                    return json{{"error","file not found: "+input_path}}.dump();
                // Read source image bytes
                std::ifstream ifs(input_path, std::ios::binary);
                std::vector<uint8_t> img_bytes((std::istreambuf_iterator<char>(ifs)),
                                                std::istreambuf_iterator<char>());
                if (img_bytes.empty())
                    return json{{"error","could not read input file"}}.dump();
                // POST multipart to rembg_locale
                static const std::string REMBG_URL = "http://127.0.0.1:8005/remove";
                CURL* c = curl_easy_init();
                if (!c) return json{{"error","curl init failed"}}.dump();
                curl_mime* mime = curl_mime_init(c);
                // field: image (file bytes)
                curl_mimepart* part = curl_mime_addpart(mime);
                curl_mime_name(part, "image");
                curl_mime_data(part, (char*)img_bytes.data(), img_bytes.size());
                // derive a filename for mime content-disposition
                std::string fname = std::filesystem::path(input_path).filename().string();
                curl_mime_filename(part, fname.c_str());
                std::string mime_type_hdr = "image/" +
                    (fname.rfind(".png") != std::string::npos ? std::string("png")
                   : fname.rfind(".jpg") != std::string::npos || fname.rfind(".jpeg") != std::string::npos
                        ? std::string("jpeg") : std::string("octet-stream"));
                curl_mime_type(part, mime_type_hdr.c_str());
                // field: type
                curl_mimepart* tp = curl_mime_addpart(mime);
                curl_mime_name(tp, "type");
                curl_mime_data(tp, bg_type.c_str(), CURL_ZERO_TERMINATED);
                // field: threshold (optional)
                std::string thr_str;
                if (threshold >= 0.0) {
                    thr_str = std::to_string(threshold);
                    curl_mimepart* thp = curl_mime_addpart(mime);
                    curl_mime_name(thp, "threshold");
                    curl_mime_data(thp, thr_str.c_str(), CURL_ZERO_TERMINATED);
                }
                std::vector<uint8_t> result;
                long http_code = 0;
                curl_easy_setopt(c, CURLOPT_URL,      REMBG_URL.c_str());
                curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
                curl_easy_setopt(c, CURLOPT_TIMEOUT,  120L);
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                    +[](char* p, size_t s, size_t n, void* u) -> size_t {
                        auto* b = static_cast<std::vector<uint8_t>*>(u);
                        b->insert(b->end(),(uint8_t*)p,(uint8_t*)p+s*n); return s*n; });
                curl_easy_setopt(c, CURLOPT_WRITEDATA, &result);
                CURLcode rc = curl_easy_perform(c);
                curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
                curl_mime_free(mime);
                curl_easy_cleanup(c);
                if (rc != CURLE_OK)
                    return json{{"error",std::string("curl: ")+curl_easy_strerror(rc)+
                        " — is rembg_locale running on port 8005? (rembg_locale/start.sh)"}}.dump();
                if (http_code >= 400) {
                    std::string err(result.begin(), result.end());
                    return json{{"error","server "+std::to_string(http_code)+": "+err.substr(0,300)}}.dump();
                }
                if (result.empty()) return json{{"error","empty response from rembg_locale"}}.dump();
                // Save PNG
                size_t sl = output_path.rfind('/');
                if (sl != std::string::npos)
                    std::filesystem::create_directories(output_path.substr(0, sl));
                std::ofstream ofs(output_path, std::ios::binary);
                ofs.write((char*)result.data(), result.size());
                return json{{"ok",true},{"path",output_path},{"bytes",(int)result.size()},
                            {"type",bg_type}}.dump();
            }

            // ── faceswap_locale tools ─────────────────────────────────────────────
            // Helper: POST multipart to faceswap_locale, collect raw bytes.
            // Returns {ok, error, http_code} on failure; raw bytes on success.
            auto faceswap_post_raw = [&](const std::string& endpoint,
                                          const std::function<void(CURL*, curl_mime*)>& add_parts,
                                          std::vector<uint8_t>& out_bytes,
                                          long timeout_s = 60L) -> std::string /* error or "" */ {
                if (cfg.faceswapUrl.empty())
                    return "faceswap_locale not configured. Use --faceswap-url.";
                std::string url = cfg.faceswapUrl + endpoint;
                CURL* c = curl_easy_init();
                if (!c) return "curl init failed";
                curl_mime* mime = curl_mime_init(c);
                add_parts(c, mime);
                long http_code = 0;
                curl_easy_setopt(c, CURLOPT_URL,      url.c_str());
                curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
                curl_easy_setopt(c, CURLOPT_TIMEOUT,  timeout_s);
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                    +[](char* p, size_t s, size_t n, void* u) -> size_t {
                        auto* b = static_cast<std::vector<uint8_t>*>(u);
                        b->insert(b->end(),(uint8_t*)p,(uint8_t*)p+s*n); return s*n; });
                curl_easy_setopt(c, CURLOPT_WRITEDATA, &out_bytes);
                CURLcode rc = curl_easy_perform(c);
                curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
                curl_mime_free(mime);
                curl_easy_cleanup(c);
                if (rc != CURLE_OK)
                    return std::string("curl error: ") + curl_easy_strerror(rc);
                if (http_code >= 400) {
                    std::string body(out_bytes.begin(), out_bytes.end());
                    return "server " + std::to_string(http_code) + ": " + body.substr(0,300);
                }
                return "";
            };

            // Helper: add a string field to a curl_mime form.
            auto mime_str = [](curl_mime* m, const char* name, const std::string& val) {
                curl_mimepart* p = curl_mime_addpart(m);
                curl_mime_name(p, name);
                curl_mime_data(p, val.c_str(), CURL_ZERO_TERMINATED);
            };

            // Helper: add a file field to a curl_mime form (reads from disk).
            auto mime_file = [](curl_mime* m, const char* name, const std::string& path) {
                curl_mimepart* p = curl_mime_addpart(m);
                curl_mime_name(p, name);
                curl_mime_filedata(p, path.c_str());
                std::string fname = std::filesystem::path(path).filename().string();
                curl_mime_filename(p, fname.c_str());
                std::string ext = fname.rfind(".png") != std::string::npos ? "image/png"
                                : fname.rfind(".jpg") != std::string::npos ||
                                  fname.rfind(".jpeg") != std::string::npos ? "image/jpeg"
                                : "application/octet-stream";
                curl_mime_type(p, ext.c_str());
            };

            if (name == "faceswap_health") {
                if (cfg.faceswapUrl.empty())
                    return json{{"error","faceswap_locale not configured. Use --faceswap-url."}}.dump();
                std::string resp = http_get_coder(cfg.faceswapUrl + "/health",
                                                  {"Accept: application/json"});
                try { (void)json::parse(resp); return resp; }
                catch (...) { return json{{"raw",resp}}.dump(); }
            }

            if (name == "faceswap_detect") {
                std::string input_path = a.value("input_path","");
                if (input_path.empty()) return json{{"error","input_path required"}}.dump();
                if (!is_coder_path_allowed(input_path))
                    return json{{"error","path not allowed: "+input_path}}.dump();
                if (!std::filesystem::exists(input_path))
                    return json{{"error","file not found: "+input_path}}.dump();
                std::vector<uint8_t> out;
                std::string err = faceswap_post_raw("/detect",
                    [&](CURL*, curl_mime* m){ mime_file(m, "image", input_path); },
                    out);
                if (!err.empty()) return json{{"error",err}}.dump();
                std::string body(out.begin(), out.end());
                try { (void)json::parse(body); return body; }
                catch (...) { return json{{"raw",body}}.dump(); }
            }

            if (name == "faceswap_identify") {
                std::string input_path = a.value("input_path","");
                double threshold       = a.value("threshold", 0.35);
                if (input_path.empty()) return json{{"error","input_path required"}}.dump();
                if (!is_coder_path_allowed(input_path))
                    return json{{"error","path not allowed: "+input_path}}.dump();
                if (!std::filesystem::exists(input_path))
                    return json{{"error","file not found: "+input_path}}.dump();
                std::string thr_str = std::to_string(threshold);
                std::vector<uint8_t> out;
                std::string err = faceswap_post_raw("/identify",
                    [&](CURL*, curl_mime* m){
                        mime_file(m, "image", input_path);
                        mime_str(m, "threshold", thr_str);
                    }, out);
                if (!err.empty()) return json{{"error",err}}.dump();
                std::string body(out.begin(), out.end());
                try { (void)json::parse(body); return body; }
                catch (...) { return json{{"raw",body}}.dump(); }
            }

            if (name == "faceswap_register") {
                std::string input_path = a.value("input_path","");
                std::string npc_id     = a.value("npc_id","");
                if (input_path.empty()) return json{{"error","input_path required"}}.dump();
                if (npc_id.empty())     return json{{"error","npc_id required"}}.dump();
                if (!is_coder_path_allowed(input_path))
                    return json{{"error","path not allowed: "+input_path}}.dump();
                if (!std::filesystem::exists(input_path))
                    return json{{"error","file not found: "+input_path}}.dump();
                std::vector<uint8_t> out;
                std::string err = faceswap_post_raw("/register",
                    [&](CURL*, curl_mime* m){
                        mime_file(m, "image", input_path);
                        mime_str(m, "npc_id", npc_id);
                    }, out);
                if (!err.empty()) return json{{"error",err}}.dump();
                std::string body(out.begin(), out.end());
                try { (void)json::parse(body); return body; }
                catch (...) { return json{{"raw",body}}.dump(); }
            }

            if (name == "faceswap_segment") {
                std::string input_path  = a.value("input_path","");
                std::string output_path = a.value("output_path","");
                if (input_path.empty())  return json{{"error","input_path required"}}.dump();
                if (output_path.empty()) return json{{"error","output_path required"}}.dump();
                if (!is_coder_path_allowed(input_path))
                    return json{{"error","input_path not allowed: "+input_path}}.dump();
                if (!is_coder_path_allowed(output_path))
                    return json{{"error","output_path not allowed: "+output_path}}.dump();
                if (!std::filesystem::exists(input_path))
                    return json{{"error","file not found: "+input_path}}.dump();
                std::string parts        = a.value("parts","");
                std::string text         = a.value("text","");
                bool   xseg             = a.value("xseg", false);
                double xseg_amount      = a.value("xseg_amount", 0.0);
                double threshold        = a.value("threshold", 0.4);
                bool   occlude          = a.value("occlude", false);
                int    expand           = a.value("expand", 0);
                int    blur             = a.value("blur", 0);
                bool   invert           = a.value("invert", false);
                std::vector<uint8_t> out;
                std::string err = faceswap_post_raw("/segment",
                    [&](CURL*, curl_mime* m){
                        mime_file(m, "image", input_path);
                        mime_str(m, "parts",       parts);
                        mime_str(m, "text",        text);
                        mime_str(m, "xseg",        xseg ? "true" : "false");
                        mime_str(m, "xseg_amount", std::to_string(xseg_amount));
                        mime_str(m, "threshold",   std::to_string(threshold));
                        mime_str(m, "occlude",     occlude ? "true" : "false");
                        mime_str(m, "expand",      std::to_string(expand));
                        mime_str(m, "blur",        std::to_string(blur));
                        mime_str(m, "invert",      invert ? "true" : "false");
                    }, out, 120L);
                if (!err.empty()) return json{{"error",err}}.dump();
                if (out.empty()) return json{{"error","empty response"}}.dump();
                // Validate image magic bytes
                bool is_image = (out.size() >= 4) &&
                    ((out[0]==0x89&&out[1]=='P'&&out[2]=='N'&&out[3]=='G') ||  // PNG
                     (out[0]==0xFF&&out[1]==0xD8));                             // JPEG
                if (!is_image) {
                    std::string body(out.begin(), out.end());
                    return json{{"error","server returned non-image: "+body.substr(0,200)}}.dump();
                }
                size_t sl = output_path.rfind('/');
                if (sl != std::string::npos)
                    std::filesystem::create_directories(output_path.substr(0, sl));
                std::ofstream ofs(output_path, std::ios::binary);
                ofs.write((char*)out.data(), out.size());
                return json{{"ok",true},{"path",output_path},{"bytes",(int)out.size()}}.dump();
            }

            if (name == "faceswap_swap") {
                std::string target_path = a.value("target_path","");
                std::string output_path = a.value("output_path","");
                if (target_path.empty()) return json{{"error","target_path required"}}.dump();
                if (output_path.empty()) return json{{"error","output_path required"}}.dump();
                if (!is_coder_path_allowed(target_path))
                    return json{{"error","target_path not allowed: "+target_path}}.dump();
                if (!is_coder_path_allowed(output_path))
                    return json{{"error","output_path not allowed: "+output_path}}.dump();
                if (!std::filesystem::exists(target_path))
                    return json{{"error","file not found: "+target_path}}.dump();
                bool enhance = a.value("enhance", false);
                std::string positions = a.value("positions","");

                // Collect source_paths (explicit files) or npc_ids (registered embeddings).
                // The faceswap_locale /swap endpoint accepts:
                //   - sources: one or more image files (multipart "sources" fields)
                //   - npc_ids: JSON array string of NPC ids (server looks up .npy embeddings)
                std::vector<std::string> source_paths;
                std::string npc_ids_json;
                if (a.contains("source_paths"))
                    for (auto& p : a["source_paths"]) source_paths.push_back(p.get<std::string>());
                if (a.contains("npc_ids"))
                    npc_ids_json = json(a["npc_ids"]).dump();

                // Validate source_paths
                for (auto& sp : source_paths) {
                    if (!is_coder_path_allowed(sp))
                        return json{{"error","source_path not allowed: "+sp}}.dump();
                    if (!std::filesystem::exists(sp))
                        return json{{"error","source file not found: "+sp}}.dump();
                }

                // Build positions string from npc_ids if not specified
                if (positions.empty() && !npc_ids_json.empty()) {
                    json nids = json::parse(npc_ids_json);
                    std::string pos_str;
                    for (size_t i = 0; i < nids.size(); ++i) {
                        if (i) pos_str += ",";
                        pos_str += std::to_string(i);
                    }
                    positions = pos_str;
                }

                std::string mask_path = a.value("mask_path","");
                if (!mask_path.empty() && !is_coder_path_allowed(mask_path))
                    return json{{"error","mask_path not allowed: "+mask_path}}.dump();

                std::vector<uint8_t> out;
                std::string err = faceswap_post_raw("/swap",
                    [&](CURL*, curl_mime* m){
                        mime_file(m, "target", target_path);
                        if (!npc_ids_json.empty())
                            mime_str(m, "npc_ids", npc_ids_json);
                        for (auto& sp : source_paths)
                            mime_file(m, "sources", sp);
                        if (!positions.empty())
                            mime_str(m, "positions", "[" + positions + "]");
                        mime_str(m, "enhance", enhance ? "true" : "false");
                        if (!mask_path.empty() && std::filesystem::exists(mask_path))
                            mime_file(m, "mask", mask_path);
                    }, out, 120L);
                if (!err.empty()) return json{{"error",err}}.dump();
                if (out.empty()) return json{{"error","empty response"}}.dump();
                bool is_image = (out.size() >= 4) &&
                    ((out[0]==0x89&&out[1]=='P'&&out[2]=='N'&&out[3]=='G') ||
                     (out[0]==0xFF&&out[1]==0xD8));
                if (!is_image) {
                    std::string body(out.begin(), out.end());
                    return json{{"error","server returned non-image: "+body.substr(0,200)}}.dump();
                }
                size_t sl = output_path.rfind('/');
                if (sl != std::string::npos)
                    std::filesystem::create_directories(output_path.substr(0, sl));
                std::ofstream ofs(output_path, std::ios::binary);
                ofs.write((char*)out.data(), out.size());
                return json{{"ok",true},{"path",output_path},{"bytes",(int)out.size()}}.dump();
            }

            if (name == "faceswap_restore") {
                std::string input_path  = a.value("input_path","");
                std::string output_path = a.value("output_path","");
                std::string restorer    = a.value("restorer","codeformer");
                double fidelity         = a.value("fidelity", 0.7);
                bool only_center        = a.value("only_center_face", false);
                if (input_path.empty())  return json{{"error","input_path required"}}.dump();
                if (output_path.empty()) return json{{"error","output_path required"}}.dump();
                if (!is_coder_path_allowed(input_path))
                    return json{{"error","input_path not allowed: "+input_path}}.dump();
                if (!is_coder_path_allowed(output_path))
                    return json{{"error","output_path not allowed: "+output_path}}.dump();
                if (!std::filesystem::exists(input_path))
                    return json{{"error","file not found: "+input_path}}.dump();
                std::vector<uint8_t> out;
                std::string err = faceswap_post_raw("/restore",
                    [&](CURL*, curl_mime* m){
                        mime_file(m, "image", input_path);
                        mime_str(m, "restorer",         restorer);
                        mime_str(m, "fidelity",         std::to_string(fidelity));
                        mime_str(m, "only_center_face", only_center ? "true" : "false");
                    }, out, 120L);
                if (!err.empty()) return json{{"error",err}}.dump();
                if (out.empty()) return json{{"error","empty response"}}.dump();
                bool is_image = (out.size() >= 4) &&
                    ((out[0]==0x89&&out[1]=='P'&&out[2]=='N'&&out[3]=='G') ||
                     (out[0]==0xFF&&out[1]==0xD8));
                if (!is_image) {
                    std::string body(out.begin(), out.end());
                    return json{{"error","server returned non-image: "+body.substr(0,200)}}.dump();
                }
                size_t sl = output_path.rfind('/');
                if (sl != std::string::npos)
                    std::filesystem::create_directories(output_path.substr(0, sl));
                std::ofstream ofs(output_path, std::ios::binary);
                ofs.write((char*)out.data(), out.size());
                return json{{"ok",true},{"path",output_path},{"bytes",(int)out.size()},
                            {"restorer",restorer},{"fidelity",fidelity}}.dump();
            }

            if (name == "faceswap_upscale") {
                std::string input_path  = a.value("input_path","");
                std::string output_path = a.value("output_path","");
                int scale               = a.value("scale", 4);
                if (input_path.empty())  return json{{"error","input_path required"}}.dump();
                if (output_path.empty()) return json{{"error","output_path required"}}.dump();
                if (!is_coder_path_allowed(input_path))
                    return json{{"error","input_path not allowed: "+input_path}}.dump();
                if (!is_coder_path_allowed(output_path))
                    return json{{"error","output_path not allowed: "+output_path}}.dump();
                if (!std::filesystem::exists(input_path))
                    return json{{"error","file not found: "+input_path}}.dump();
                std::string scale_str = std::to_string(scale);
                std::vector<uint8_t> out;
                std::string err = faceswap_post_raw("/upscale",
                    [&](CURL*, curl_mime* m){
                        mime_file(m, "image", input_path);
                        mime_str(m, "scale", scale_str);
                    }, out, 180L);
                if (!err.empty()) return json{{"error",err}}.dump();
                if (out.empty()) return json{{"error","empty response"}}.dump();
                bool is_image = (out.size() >= 4) &&
                    ((out[0]==0x89&&out[1]=='P'&&out[2]=='N'&&out[3]=='G') ||
                     (out[0]==0xFF&&out[1]==0xD8));
                if (!is_image) {
                    std::string body(out.begin(), out.end());
                    return json{{"error","server returned non-image: "+body.substr(0,200)}}.dump();
                }
                size_t sl = output_path.rfind('/');
                if (sl != std::string::npos)
                    std::filesystem::create_directories(output_path.substr(0, sl));
                std::ofstream ofs(output_path, std::ios::binary);
                ofs.write((char*)out.data(), out.size());
                return json{{"ok",true},{"path",output_path},{"bytes",(int)out.size()},
                            {"scale",scale}}.dump();
            }
            // ── video pipeline tools ──────────────────────────────────────────────
            // All video tools POST form data (path strings) to faceswap_locale server
            // and return JSON. We reuse the faceswap_post_raw helper but treat the
            // binary response as a UTF-8 JSON string.
            auto video_post = [&](const std::string& endpoint,
                                   const std::function<void(CURL*, curl_mime*)>& add_parts,
                                   long timeout_s = 300L) -> std::string {
                std::vector<uint8_t> out;
                std::string err = faceswap_post_raw(endpoint, add_parts, out, timeout_s);
                if (!err.empty()) return json{{"error",err}}.dump();
                std::string body(out.begin(), out.end());
                try { (void)json::parse(body); return body; }
                catch (...) { return json{{"raw",body.substr(0,500)}}.dump(); }
            };

            if (name == "video_extract") {
                std::string video_path = a.value("video_path","");
                std::string output_dir = a.value("output_dir","");
                std::string start_time = a.value("start_time","");
                std::string end_time   = a.value("end_time","");
                std::string fps        = a.value("fps","");
                if (video_path.empty()) return json{{"error","video_path required"}}.dump();
                if (output_dir.empty()) return json{{"error","output_dir required"}}.dump();
                // output_dir: whitelist check; video_path: any absolute (user's file)
                if (!std::filesystem::path(output_dir).is_absolute() &&
                    !is_coder_path_allowed(output_dir))
                    return json{{"error","output_dir not allowed: "+output_dir+
                                 " — use video_work/ or saves/"}}.dump();
                return video_post("/video/extract",
                    [&](CURL*, curl_mime* m){
                        mime_str(m, "video_path",  video_path);
                        mime_str(m, "output_dir",  output_dir);
                        mime_str(m, "start_time",  start_time);
                        mime_str(m, "end_time",    end_time);
                        mime_str(m, "fps",         fps);
                    }, 600L);
            }

            if (name == "video_analyze") {
                std::string frames_dir = a.value("frames_dir","");
                std::string output_dir = a.value("output_dir","");
                if (frames_dir.empty()) return json{{"error","frames_dir required"}}.dump();
                if (output_dir.empty()) return json{{"error","output_dir required"}}.dump();
                std::string thr     = std::to_string(a.value("cluster_threshold",0.40));
                std::string prev    = a.value("make_previews",true) ? "true" : "false";
                std::string pfps    = a.value("preview_fps",std::string("30"));
                return video_post("/video/analyze",
                    [&](CURL*, curl_mime* m){
                        mime_str(m, "frames_dir",        frames_dir);
                        mime_str(m, "output_dir",        output_dir);
                        mime_str(m, "cluster_threshold", thr);
                        mime_str(m, "make_previews",     prev);
                        mime_str(m, "preview_fps",       pfps);
                    }, 600L);
            }

            if (name == "video_assign_npc") {
                std::string manifest_path = a.value("manifest_path","");
                std::string npc_assign    = a.value("npc_assign","");
                if (manifest_path.empty()) return json{{"error","manifest_path required"}}.dump();
                if (npc_assign.empty())    return json{{"error","npc_assign required (JSON object)"}}.dump();
                return video_post("/video/patch_manifest",
                    [&](CURL*, curl_mime* m){
                        mime_str(m, "manifest_path", manifest_path);
                        mime_str(m, "npc_assign",    npc_assign);
                        mime_str(m, "frame_start",   "");
                        mime_str(m, "frame_end",     "");
                        mime_str(m, "status",        "pending");
                    });
            }

            if (name == "video_swap") {
                std::string manifest_path = a.value("manifest_path","");
                std::string frames_dir    = a.value("frames_dir","");
                std::string swapped_dir   = a.value("swapped_dir","");
                bool retry_only           = a.value("retry_only",false);
                if (manifest_path.empty()) return json{{"error","manifest_path required"}}.dump();
                if (frames_dir.empty())    return json{{"error","frames_dir required"}}.dump();
                if (swapped_dir.empty())   return json{{"error","swapped_dir required"}}.dump();
                return video_post("/video/swap_frames",
                    [&](CURL*, curl_mime* m){
                        mime_str(m, "manifest_path", manifest_path);
                        mime_str(m, "frames_dir",    frames_dir);
                        mime_str(m, "output_dir",    swapped_dir);
                        mime_str(m, "retry_only",    retry_only ? "true" : "false");
                    }, 600L);
            }

            if (name == "video_quality") {
                std::string manifest_path = a.value("manifest_path","");
                std::string swapped_dir   = a.value("swapped_dir","");
                if (manifest_path.empty()) return json{{"error","manifest_path required"}}.dump();
                if (swapped_dir.empty())   return json{{"error","swapped_dir required"}}.dump();
                return video_post("/video/quality",
                    [&](CURL*, curl_mime* m){
                        mime_str(m, "manifest_path",  manifest_path);
                        mime_str(m, "swapped_dir",    swapped_dir);
                        mime_str(m, "det_threshold",  std::to_string(a.value("det_threshold",0.50)));
                        mime_str(m, "blur_threshold", std::to_string(a.value("blur_threshold",50.0)));
                        mime_str(m, "pose_threshold", std::to_string(a.value("pose_threshold",30.0)));
                        mime_str(m, "gap_frames",     std::to_string(a.value("gap_frames",5)));
                    }, 300L);
            }

            if (name == "video_patch") {
                std::string manifest_path = a.value("manifest_path","");
                if (manifest_path.empty()) return json{{"error","manifest_path required"}}.dump();
                std::string expand_s = a.contains("expand") ? std::to_string(a["expand"].get<int>()) : "-1";
                std::string blur_s   = a.contains("blur")   ? std::to_string(a["blur"].get<int>())   : "-1";
                return video_post("/video/patch_manifest",
                    [&](CURL*, curl_mime* m){
                        mime_str(m, "manifest_path", manifest_path);
                        mime_str(m, "frame_start",   a.value("frame_start",""));
                        mime_str(m, "frame_end",     a.value("frame_end",""));
                        mime_str(m, "npc_assign",    a.value("npc_assign",""));
                        mime_str(m, "cluster_id",    a.value("cluster_id",""));
                        mime_str(m, "mask_parts",    a.value("mask_parts",""));
                        mime_str(m, "expand",        expand_s);
                        mime_str(m, "blur",          blur_s);
                        mime_str(m, "skip",          a.value("skip",false) ? "true" : "false");
                        mime_str(m, "status",        a.value("status",std::string("pending_retry")));
                    });
            }

            if (name == "video_assemble") {
                std::string manifest_path = a.value("manifest_path","");
                std::string swapped_dir   = a.value("swapped_dir","");
                std::string output_path   = a.value("output_path","");
                if (manifest_path.empty()) return json{{"error","manifest_path required"}}.dump();
                if (swapped_dir.empty())   return json{{"error","swapped_dir required"}}.dump();
                if (output_path.empty())   return json{{"error","output_path required"}}.dump();
                return video_post("/video/assemble",
                    [&](CURL*, curl_mime* m){
                        mime_str(m, "manifest_path", manifest_path);
                        mime_str(m, "swapped_dir",   swapped_dir);
                        mime_str(m, "audio_path",    a.value("audio_path",""));
                        mime_str(m, "output_path",   output_path);
                        mime_str(m, "fps",           a.value("fps",std::string("30")));
                        mime_str(m, "restore",       a.value("restore",std::string("")));
                        mime_str(m, "fidelity",      std::to_string(a.value("fidelity",0.7)));
                        mime_str(m, "upscale",       std::to_string(a.value("upscale",0)));
                    }, 600L);
            }
            // ── end video pipeline tools ──────────────────────────────────────────

            // ── end faceswap_locale tools ─────────────────────────────────────────

            if (name == "search_images") {
                std::string query = a.value("query","");
                int count = std::min(a.value("count", 5), 10);
                if (query.empty()) return json{{"error","query required"}}.dump();
                CURL* ce = curl_easy_init();
                char* enc = ce ? curl_easy_escape(ce, query.c_str(), (int)query.size()) : nullptr;
                std::string q = enc ? enc : query;
                if (enc) curl_free(enc);
                if (ce) curl_easy_cleanup(ce);

                json results = json::array();
                std::string provider_used;

                if (!cfg.pixabayKey.empty()) {
                    provider_used = "pixabay";
                    std::string url = "https://pixabay.com/api/?key=" + cfg.pixabayKey
                                    + "&q=" + q + "&image_type=photo&per_page="
                                    + std::to_string(count) + "&safesearch=true";
                    std::string resp = http_get_coder(url, {"Accept: application/json"});
                    try {
                        auto r = json::parse(resp);
                        for (auto& h : r["hits"]) {
                            results.push_back({
                                {"title",    h.value("tags","")},
                                {"image_url",h.value("largeImageURL", h.value("webformatURL",""))},
                                {"thumbnail_url", h.value("webformatURL","")},
                                {"source_url",    h.value("pageURL","")},
                                {"author",        h.value("user","")}
                            });
                            if ((int)results.size() >= count) break;
                        }
                    } catch (...) {}
                } else if (cfg.searchProvider == "openverse") {
                    // Openverse — Creative Commons only (opt-in via --search-provider openverse)
                    provider_used = "openverse";
                    std::string url = "https://api.openverse.org/v1/images/?q=" + q
                                    + "&page_size=" + std::to_string(count)
                                    + "&license_type=commercial,modification";
                    std::string resp = http_get_coder(url, {
                        "Accept: application/json",
                        "User-Agent: rpgai-coder/1.0 (contact: rpgai)"
                    });
                    try {
                        auto r = json::parse(resp);
                        for (auto& item : r["results"]) {
                            results.push_back({
                                {"title",         item.value("title","")},
                                {"image_url",     item.value("url","")},
                                {"thumbnail_url", item.value("thumbnail","")},
                                {"source_url",    item.value("foreign_landing_url","")},
                                {"author",        item.value("creator","")}
                            });
                            if ((int)results.size() >= count) break;
                        }
                    } catch (...) {}
                } else {
                    // DuckDuckGo images — default, no key, real web images
                    provider_used = "duckduckgo_images";
                    static const std::vector<std::string> ddg_ua = {
                        "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:120.0) Gecko/20100101 Firefox/120.0",
                        "Accept: text/html,application/xhtml+xml"
                    };
                    // Step 1: obtain vqd token
                    std::string init_resp = http_get_coder(
                        "https://duckduckgo.com/?q=" + q + "&ia=images", ddg_ua);
                    std::string vqd;
                    // Try pattern: vqd=4-... or vqd":"4-...
                    for (auto& pat : std::vector<std::string>{"vqd=","\"vqd\":\""}) {
                        auto pos = init_resp.find(pat);
                        if (pos != std::string::npos) {
                            size_t s = pos + pat.size();
                            size_t e = init_resp.find_first_of("&\"'\\", s);
                            if (e != std::string::npos) { vqd = init_resp.substr(s, e-s); break; }
                        }
                    }
                    if (!vqd.empty()) {
                        CURL* ce2 = curl_easy_init();
                        char* ve = ce2 ? curl_easy_escape(ce2, vqd.c_str(), (int)vqd.size()) : nullptr;
                        std::string vqd_enc = ve ? ve : vqd;
                        if (ve) curl_free(ve);
                        if (ce2) curl_easy_cleanup(ce2);
                        std::string img_url = "https://duckduckgo.com/i.js?q=" + q
                                            + "&vqd=" + vqd_enc + "&o=json&p=1&s=0";
                        std::string img_resp = http_get_coder(img_url, {
                            "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:120.0) Gecko/20100101 Firefox/120.0",
                            "Referer: https://duckduckgo.com/",
                            "Accept: application/json, text/javascript"
                        });
                        try {
                            auto r = json::parse(img_resp);
                            for (auto& item : r["results"]) {
                                results.push_back({
                                    {"title",         item.value("title","")},
                                    {"image_url",     item.value("image","")},
                                    {"thumbnail_url", item.value("thumbnail","")},
                                    {"source_url",    item.value("url","")},
                                    {"author",        ""}
                                });
                                if ((int)results.size() >= count) break;
                            }
                        } catch (...) {}
                    }
                    // Fallback to Openverse if DDG returned nothing
                    if (results.empty()) {
                        provider_used = "openverse_fallback";
                        std::string url = "https://api.openverse.org/v1/images/?q=" + q
                                        + "&page_size=" + std::to_string(count);
                        std::string resp = http_get_coder(url, {"Accept: application/json","User-Agent: rpgai-coder/1.0"});
                        try {
                            auto r = json::parse(resp);
                            for (auto& item : r["results"]) {
                                results.push_back({
                                    {"title",         item.value("title","")},
                                    {"image_url",     item.value("url","")},
                                    {"thumbnail_url", item.value("thumbnail","")},
                                    {"source_url",    item.value("foreign_landing_url","")},
                                    {"author",        item.value("creator","")}
                                });
                                if ((int)results.size() >= count) break;
                            }
                        } catch (...) {}
                    }
                }
                return json{{"provider",provider_used},{"query",query},
                            {"count",(int)results.size()},{"results",results}}.dump();
            }

            if (name == "download_asset") {
                std::string url  = a.value("url","");
                std::string path = a.value("path","");
                if (url.empty())  return json{{"error","url required"}}.dump();
                if (path.empty()) return json{{"error","path required"}}.dump();
                if (url.substr(0,4) != "http")
                    return json{{"error","url must start with http"}}.dump();
                if (!is_coder_path_allowed(path))
                    return json{{"error","path not allowed: "+path}}.dump();
                // Create parent dirs
                size_t sl = path.rfind('/');
                if (sl != std::string::npos)
                    std::filesystem::create_directories(path.substr(0, sl));
                // Download via curl
                CURL* c = curl_easy_init();
                if (!c) return json{{"error","curl init failed"}}.dump();
                FILE* fp = fopen(path.c_str(), "wb");
                if (!fp) { curl_easy_cleanup(c); return json{{"error","cannot create: "+path}}.dump(); }
                curl_easy_setopt(c, CURLOPT_URL, url.c_str());
                curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
                curl_easy_setopt(c, CURLOPT_USERAGENT, "rpgai-coder/1.0");
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                    +[](char* p, size_t s, size_t n, void* u) -> size_t {
                        fwrite(p, s, n, static_cast<FILE*>(u)); return s*n; });
                curl_easy_setopt(c, CURLOPT_WRITEDATA, fp);
                CURLcode res = curl_easy_perform(c);
                long http_code = 0;
                curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
                curl_easy_cleanup(c);
                fclose(fp);
                if (res != CURLE_OK || http_code >= 400) {
                    std::remove(path.c_str());
                    return json{{"error","download failed (http "+std::to_string(http_code)+")"}}.dump();
                }
                // File size
                std::error_code ec;
                auto sz = std::filesystem::file_size(path, ec);
                return json{{"ok",true},{"path",path},{"bytes",(int)(ec ? 0 : sz)}}.dump();
            }

            if (name == "web_search") {
                std::string query = a.value("query","");
                if (query.empty()) return json{{"error","query required"}}.dump();
                // URL-encode query
                CURL* ce = curl_easy_init();
                char* enc = ce ? curl_easy_escape(ce, query.c_str(), (int)query.size()) : nullptr;
                std::string q = enc ? enc : query;
                if (enc) curl_free(enc);
                if (ce) curl_easy_cleanup(ce);

                json results = json::array();
                std::string provider_used;

                if (!cfg.searchKey.empty() && cfg.searchProvider == "brave") {
                    // Brave Search API
                    provider_used = "brave";
                    std::string url = "https://api.search.brave.com/res/v1/web/search?q=" + q + "&count=5";
                    std::string resp = http_get_coder(url, {
                        "X-Subscription-Token: " + cfg.searchKey,
                        "Accept: application/json"
                    });
                    try {
                        auto r = json::parse(resp);
                        for (auto& item : r["web"]["results"]) {
                            results.push_back({{"title",item.value("title","")},
                                               {"snippet",item.value("description","")},
                                               {"url",item.value("url","")}});
                            if ((int)results.size() >= 5) break;
                        }
                    } catch (...) {}
                } else {
                    // DuckDuckGo Instant Answer API (no key required)
                    provider_used = "duckduckgo";
                    std::string url = "https://api.duckduckgo.com/?q=" + q
                                    + "&format=json&no_html=1&skip_disambig=1&t=rpgai";
                    std::string resp = http_get_coder(url, {});
                    try {
                        auto r = json::parse(resp);
                        std::string abs     = r.value("AbstractText","");
                        std::string abs_url = r.value("AbstractURL","");
                        if (!abs.empty())
                            results.push_back({{"title","Abstract"},{"snippet",abs},{"url",abs_url}});
                        for (auto& t : r.value("RelatedTopics", json::array())) {
                            if (!t.is_object() || !t.contains("Text")) continue;
                            std::string txt = t.value("Text","");
                            results.push_back({{"title",txt.substr(0, std::min((int)txt.size(),80))},
                                               {"snippet",txt},
                                               {"url",t.value("FirstURL","")}});
                            if ((int)results.size() >= 5) break;
                        }
                    } catch (...) {}
                }
                return json{{"provider",provider_used},{"query",query},
                            {"count",(int)results.size()},{"results",results}}.dump();
            }

            return json{{"error","unknown tool: "+name}}.dump();
        };

        // Resolve LLM endpoint for a provider (OpenAI-compatible for tool calling)
        // run_coder_loop speaks only the OpenAI tool-calling wire format. Claude and
        // Gemini are NOT compatible — return an empty base_url to signal "unsupported"
        // rather than silently POSTing to OpenRouter with the wrong model/key.
        auto get_coder_api_config = [&](AIProvider prov) -> std::pair<std::string,std::string> {
            switch (prov) {
                case AIProvider::OPENROUTER: return {cfg.openrouter_baseUrl, cfg.openrouter_key};
                case AIProvider::OPENAI:     return {cfg.openai_baseUrl,     cfg.openai_key};
                case AIProvider::OLLAMA:     return {cfg.ollama_baseUrl + "/v1/chat/completions", ""};
                default:                     return {"", ""};
            }
        };

        // Pauseable tool loop on an OpenAI-format messages array.
        // Returns {done:true, reply:"..."} or {done:false, pending:{...}}
        auto run_coder_loop = [&](
                json& messages,
                const std::string& base_url,
                const std::string& api_key,
                const std::string& eff_model,
                int max_iter) -> json
        {
            json jtools = json::array();
            for (auto& td : coder_tool_defs) {
                json params;
                try { params = json::parse(td.params_schema); }
                catch (...) { params = {{"type","object"},{"properties",json::object()}}; }
                jtools.push_back({{"type","function"},{"function",{
                    {"name",td.name},{"description",td.description},{"parameters",params}
                }}});
            }

            json tools_used = json::array();
            const auto loop_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(120);
            for (int iter = 0; iter < max_iter; ++iter) {
                if (std::chrono::steady_clock::now() >= loop_deadline)
                    return {{"done",true},{"reply","[coder loop timed out after 120s]"},
                            {"tools_used",tools_used}};
                bool force_final = (iter == max_iter - 1);
                json body;
                body["model"]    = eff_model;
                body["messages"] = messages;
                if (!force_final) { body["tools"] = jtools; body["tool_choice"] = "auto"; }
                else              { body["tool_choice"] = "none"; }

                std::string readBuffer;
                CURL* curl = make_curl();
                if (!curl) return {{"done",true},{"reply","[curl init failed]"}};
                std::string jsonStr = body.dump();
                // Log outgoing request
                {
                    std::string last_role, last_content;
                    if (!messages.empty()) {
                        auto& lm = messages.back();
                        last_role    = lm.value("role","?");
                        last_content = lm.value("content","");
                        if (last_content.size() > 300) last_content = last_content.substr(0,300) + "...";
                    }
                    std::cerr << "[CODER iter=" << iter << "] model=" << eff_model
                              << " msgs=" << messages.size()
                              << " tools=" << (!force_final ? std::to_string(jtools.size()) : "none(final)")
                              << "\n  last[" << last_role << "]: " << last_content << "\n";
                }
                struct curl_slist* hdrs = nullptr;
                hdrs = curl_slist_append(hdrs,"Content-Type: application/json");
                if (!api_key.empty()) {
                    std::string auth = "Authorization: Bearer " + api_key;
                    hdrs = curl_slist_append(hdrs, auth.c_str());
                }
                curl_easy_setopt(curl, CURLOPT_URL,           base_url.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    jsonStr.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &readBuffer);
                CURLcode rc = curl_easy_perform(curl);
                curl_slist_free_all(hdrs);
                curl_easy_cleanup(curl);

                if (rc != CURLE_OK)
                    return {{"done",true},{"reply","[network: "+std::string(curl_easy_strerror(rc))+"]"}};

                try {
                    auto jRes = json::parse(readBuffer);
                    if (jRes.contains("error")) {
                        std::cerr << "[CODER error] " << jRes["error"].dump() << "\n";
                        return {{"done",true},{"reply","[API error: "+jRes["error"].dump()+"]"}};
                    }
                    if (!jRes.contains("choices") || jRes["choices"].empty()) {
                        std::cerr << "[CODER error] no choices in response: " << readBuffer.substr(0,200) << "\n";
                        return {{"done",true},{"reply","[no response from LLM]"}};
                    }

                    auto& msg = jRes["choices"][0]["message"];
                    messages.push_back(msg);

                    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()
                        && !msg["tool_calls"].empty()) {
                        std::cerr << "[CODER] model called " << msg["tool_calls"].size() << " tool(s)\n";
                        for (size_t ti = 0; ti < msg["tool_calls"].size(); ++ti) {
                            auto& tc      = msg["tool_calls"][ti];
                            if (!tc.contains("function") || tc["function"].is_null()) continue;
                            std::string tc_id   = tc.value("id","");
                            std::string fn_name = tc["function"].value("name","");
                            std::string fn_args = tc["function"].value("arguments","{}");

                            json tc_args_j;
                            try { tc_args_j = json::parse(fn_args); } catch(...) {}
                            int tier = coder_tool_tier(fn_name);
                            if (tier == 0) {
                                std::cerr << "[CODER tool] " << fn_name << " args=" << fn_args << "\n";
                                std::string res = execute_coder_tool(fn_name, fn_args);
                                std::string res_log = res.size() > 200 ? res.substr(0,200)+"..." : res;
                                std::cerr << "[CODER tool result] " << fn_name << " -> " << res_log << "\n";
                                messages.push_back({{"role","tool"},{"tool_call_id",tc_id},{"content",res}});
                                json t = {{"name",fn_name},{"brief",make_tool_brief(fn_name,tc_args_j)}};
                                if (fn_name == "search_images") {
                                    try { t["image_results"] = json::parse(res); } catch(...) {}
                                }
                                tools_used.push_back(t);
                            } else {
                                // Pause — push placeholder results for remaining batch items.
                                // Tell the model they were cancelled (not pending re-try) so
                                // it does not call the same tool again after approval.
                                for (size_t tj = ti+1; tj < msg["tool_calls"].size(); ++tj) {
                                    std::string skip_name = msg["tool_calls"][tj].contains("function")
                                        ? msg["tool_calls"][tj]["function"].value("name","tool")
                                        : "tool";
                                    std::string skip_id = msg["tool_calls"][tj].value("id","");
                                    messages.push_back({{"role","tool"},{"tool_call_id",skip_id},
                                        {"content","[cancelled: only one approval-required action per turn. Do not retry "+skip_name+" — ask the user if they want to continue.]"}});
                                }
                                tools_used.push_back({{"name",fn_name},{"brief",make_tool_brief(fn_name,tc_args_j)},{"pending",true}});
                                return {{"done",false},{"pending",{
                                    {"tool_call_id",    tc_id},
                                    {"tool_name",       fn_name},
                                    {"tool_args",       fn_args},
                                    {"is_danger",       tier == 2},
                                    {"messages_snapshot", messages},
                                    {"remaining_iters", max_iter - iter - 1},
                                    {"base_url",        base_url},
                                    {"api_key",         api_key},
                                    {"eff_model",       eff_model},
                                    {"preview",         make_tool_preview(fn_name, tc_args_j)}
                                }},{"tools_used",tools_used}};
                            }
                        }
                        // All tools in batch were auto — loop continues
                    } else {
                        std::string reply = (msg.contains("content") && msg["content"].is_string())
                            ? msg["content"].get<std::string>() : "";
                        return {{"done",true},{"reply",reply},{"tools_used",tools_used}};
                    }
                } catch (const std::exception& e) {
                    return {{"done",true},{"reply","[parse error: "+std::string(e.what())+"]"}};
                }
            }
            return {{"done",true},{"reply","[max iterations reached]"},{"tools_used",tools_used}};
        };

        // Translate loop result into HTTP response JSON; update session history.
        // user_msg_first is non-empty only on the initial /chat call (not on approve/deny).
        auto handle_loop_result = [&](const json& loop_res, const std::string& user_msg_first) -> json {
            json tools_used = loop_res.value("tools_used", json::array());
            if (loop_res.value("done", true)) {
                std::string reply = loop_res.value("reply","");
                if (!user_msg_first.empty())
                    coder_session.history.push_back({"user", user_msg_first});
                coder_session.history.push_back({"assistant", reply});
                coder_session.active = true;
                if (coder_session.history.size() > CODER_HISTORY_CAP)
                    coder_session.history.erase(coder_session.history.begin(),
                        coder_session.history.end() - CODER_HISTORY_CAP);
                save_coder_history();
                return {{"success",true},{"reply",reply},
                        {"history_length",(int)coder_session.history.size()},
                        {"tools_used",tools_used}};
            }
            const auto& p = loop_res["pending"];
            coder_session.pending = CoderPendingApproval{
                p.value("tool_call_id",""), p.value("tool_name",""), p.value("tool_args",""),
                p.value("is_danger",false), p.value("messages_snapshot",json::array()),
                p.value("remaining_iters",0), p.value("base_url",""),
                p.value("api_key",""), p.value("eff_model",""), p.value("preview","")
            };
            if (!user_msg_first.empty()) {
                coder_session.history.push_back({"user", user_msg_first});
                coder_session.active = true;
                if (coder_session.history.size() > CODER_HISTORY_CAP)
                    coder_session.history.erase(coder_session.history.begin(),
                        coder_session.history.end() - CODER_HISTORY_CAP);
                save_coder_history();
            }
            return {{"success",true},{"status","pending_approval"},
                    {"tool_name", p.value("tool_name","")},
                    {"tool_args", p.value("tool_args","")},
                    {"is_danger", p.value("is_danger",false)},
                    {"preview",   p.value("preview","")},
                    {"tools_used",tools_used}};
        };

        // Builds the CoderAI system prompt, injecting coder_memory.md if present.
        auto build_coder_system_prompt = [&]() -> std::string {
            std::string knowledge_path = resolve_coder_knowledge_path();
            std::string persona = cfg.coderPersona.empty()
                ? std::string(DEFAULT_CODER_PERSONA) : cfg.coderPersona;
            std::string prompt =
                persona + "\n\n"
                "RpgAi is a C++17 engine that bridges Lua game scripts and LLMs to create text-based RPG adventures. "
                "The engine handles I/O, HTTP, and LLM calls. All game logic lives in Lua scripts.\n\n"
                "## Available tools\n"
                "Auto: read_file, list_files, find_definition, find_usages, check_lua_syntax, update_coder_memory,\n"
                "      get_game_state, get_script_errors, reload_script, read_knowledge,\n"
                "      web_search, search_images, analyze_image,\n"
                "      get_npc_description, get_adventure_style, get_asset_path\n"
                "Requires approval: write_file, str_replace, download_asset, copy_file, call_undo, load_save,\n"
                "                   run_lua, eval_lua, generate_image, edit_image, generate_portrait, generate_scene,\n"
                "                   remove_background\n"
                "Danger (explicit confirmation): delete_file\n\n"
                "## Rules\n"
                "- NEVER describe or promise to call a tool — call it directly. If you intend to edit/generate/analyze an image, emit the tool call immediately, do not write text saying you will do it.\n"
                "- Call read_file or read_knowledge before writing code that uses a library you have not seen yet\n"
                "- Use str_replace (not write_file) to modify existing files\n"
                "- Never write to src/, vendor/, or build/ directories\n"
                "- New adventures: DEFAULT = declarative quickstart — read_knowledge('quickstart') + read_file('scripts/template_min.lua'), then write one quick.define{...} spec. Use scripts/template.lua only for advanced features quickstart does not wire (world.lua, npc.lua routines, adventure events)\n"
                "- Ask the user the DECISIONS questions (genre, mode, features, NPC list, provider) before writing any adventure script\n"
                "- When asked to search for images or find pictures: ALWAYS call search_images tool\n"
                "- When mentioning a local image path in your reply, write it as-is (e.g. my_scripts/images/foo.png) — it will render automatically in the UI\n"
                "- When asked to show or display a local image: just write its full relative path in your reply\n"
                "- When asked to analyze, describe, or inspect an image (local or URL): call analyze_image, then synthesize the result to DIRECTLY answer the user's original question — do not paste the raw description verbatim\n"
                "- When asked to generate or create an image from text: call generate_image (uses configured t2i provider)\n"
                "- When asked to edit, modify, or change an existing image: call edit_image (uses configured i2i provider). NEVER call generate_image for editing — generate_image creates from scratch, edit_image modifies an existing file.\n"
                "- When asked to cut/extract/crop a region or character from an image: call crop_image. Use ground_image first to get pixel coordinates if needed.\n"
                "- When asked to place/paste/composite an asset onto a background: call composite_image. The asset must be a PNG with transparent background (RGBA). Typical pipeline: ground_image → crop_image → remove_background → composite_image → edit_image.\n"
                "- When asked to remove the background from an image (for VN sprites, cut-outs): call remove_background (port 8005, rembg_locale). If the tool returns a connection error, tell the user to start rembg_locale: cd rembg_locale && ./start.sh\n"
                "- NPC VN sprite pipeline: generate_portrait → remove_background(output: asset/vn/npc/<id>.png) → add to catalog via VN Editor.\n"
                "- To create a face reference from an existing image ('memorizza il volto come X'): ground_image to find face bbox → crop_image to extract → t2i_reference(add, char_id=X) → t2i_reference(build, char_id=X). Then use generate_portrait(char_id=X) or generate_scene(chars=[X,...]) to generate with that face.\n"
                "- generate_portrait / generate_scene require --img-url pointing to t2i_locale server. If the tool returns 't2i server URL not configured', inform the user to add --img-url to their launch command.\n"
                "- faceswap=true in generate_portrait/scene uses hard face replacement (best for photorealistic consistency); omit or faceswap=false uses soft IP-Adapter conditioning (better for stylized results).\n"
                "- edit_image / generate_image: ALWAYS write the prompt/instruction in English — image models do not understand other languages\n"
                "- Call edit_image at most ONCE per user request. Do not batch multiple edit_image calls in a single turn.\n"
                "- edit_image: NEVER set output_path equal to input_path (never overwrite the original). Default _edited suffix is fine; omit output_path to use it.\n"
                "- edit_image takes 1-10 minutes to complete (GPU inference). ALWAYS warn the user BEFORE calling it: tell them to wait and that the UI will show a loading indicator.\n"
                "- If edit_image or generate_image returns an error, report the exact error to the user. NEVER claim success when the tool returned an error field.\n"
                "- When performing image tasks (analyze_image, edit_image, generate_image, ground_image, crop_image, composite_image) without an active game session: do NOT mention game database, scene cache, asset registry, or any game-specific concepts. Just describe the image result plainly.\n"
                "- When generating an image of a specific NPC: FIRST call get_npc_description(id) + get_adventure_style(), THEN compose the prompt, SHOW IT TO THE USER for review/edit, THEN call generate_image. Never skip the review step.\n"
                "- To replace an NPC asset with a newly generated image: call get_asset_path(id) to find the target path, then copy_file(generated_path, asset_path) after user approval.\n\n"
                "## NPC files — always find the correct directory first\n"
                "Before reading or editing any NPC persona file, call get_game_state().\n"
                "The response includes \"npc_path\": the exact directory persona.init() registered for this adventure.\n"
                "Example: npc_path = \"my_scripts/npcs_villa3/\" → NPC file = \"my_scripts/npcs_villa3/gaia.lua\"\n"
                "NEVER assume scripts/npcs/ — that is only the persona.lua default, almost always wrong.\n"
                "If npc_path is absent (session not PLAYING), read_file the adventure script and grep persona.init.\n"
                "BEFORE any persona file edit (add behaviour, routine, needs, goals): MANDATORY\n"
                "read_knowledge('lib_persona') for the file format and read_knowledge('patterns')\n"
                "(section 'Editing persona files') for the hard rules: exact field names (needs/sequences,\n"
                "NEVER npc_needs/npc_sequences), section order, str_replace-only, no whole-file rewrites.\n"
                "If read_knowledge fails, STOP and report it to the user — never improvise the format.\n"
                "Note: with session-isolated adventures npc_path points to npcs_<name>_sessions/<ts>/ —\n"
                "editing there changes THIS game only; edit the template dir npcs_<name>/ for permanent changes.\n\n"
                "## Structural change pattern (save → reload)\n"
                "For any change that modifies script structure (add/remove NPC in NPC_DATA, add location,\n"
                "add tool, change get_tools/get_json_schema/get_system_prompt) use this exact flow:\n"
                "1. check_lua_syntax on the new code snippet before applying\n"
                "2. str_replace to apply it\n"
                "3. Disk backup: call /api/save via a brief note to the user — or just proceed if a save was\n"
                "   made recently (the in-memory snapshot in reload_script IS the safety net)\n"
                "4. reload_script(preserve_state=true) — hot-swaps the .lua; restore_state() inside will\n"
                "   also run persona.reload_all() picking up any persona file changes\n"
                "5. Check the result: if success=false, revert the file with str_replace (restore old content)\n"
                "   then reload_script(preserve_state=true) again to get back to working state\n"
                "LIMIT: libs (adventure.lua, persona.lua, world.lua, agent.lua, etc.) are NOT reloaded by\n"
                "reload_script — they are cached in package.loaded. Changes to lib files require engine restart.\n\n"
                "## Persona files auto-reload — do NOT call reload_script yourself for these\n"
                "write_file/str_replace targeting a path inside the active adventure's npc_path (a persona\n"
                "file: scripts/npcs/<id>.lua or my_scripts/npcs*/<id>.lua) AUTOMATICALLY triggers an engine-side\n"
                "reload_script(preserve_state=true) the moment the write succeeds — the tool result includes\n"
                "auto_reloaded (or auto_reload_error). This exists because forgetting the manual reload step\n"
                "left a freshly-created NPC invisible to the running game (another character flatly denied she\n"
                "existed). Do NOT call reload_script yourself right after creating/editing a persona file — it\n"
                "already happened; check auto_reloaded in the write_file/str_replace result instead. Still call\n"
                "reload_script MANUALLY for the OTHER kind of structural change (NPC_DATA, tools, schema, prompt\n"
                "— i.e. edits to the ADVENTURE SCRIPT itself, not a persona file), which is not auto-reloaded.\n\n"
                "## Before registering a new NPC — check it doesn't already exist\n"
                "Before adding a character to NPC_DATA/spec.npcs or writing a NEW persona file, list_files the\n"
                "npc_path directory and read_file any persona whose name/role sounds similar (e.g. \"the old\n"
                "priest\", \"the vicar\", \"the sagrestana\"). A character can already exist as a GENERATED\n"
                "persona (created earlier by the in-game generate_npc tool, filename does not match the id you'd\n"
                "expect) even if the adventure script's spec.npcs never mentions them. Registering a second, new\n"
                "id for a role that already has a persona file creates two characters with the same name living\n"
                "in parallel (confirmed to happen — one had 10+ turns of real interaction history, the other was\n"
                "a bare duplicate quietly dreaming on its own). If in doubt, ask the user which id is canonical\n"
                "before creating a new one.\n\n"
                "## Using the gen tier from run_lua\n"
                "To call the strong gen model (--gen-model) from a run_lua snippet:\n"
                "  local t = get_tier('gen')\n"
                "  local result = query_llm(sys, '[]', user, schema, t.model, t.provider)\n"
                "get_tier('gen'|'agent'|'ambient') is available in the run_lua sandbox.\n"
                "eval_lua also has get_tier (runs on live game state, needs active session).\n\n"
                "## Knowledge base: " + knowledge_path + "\n"
                + ([&]() -> std::string {
                    std::error_code ec;
                    if (!std::filesystem::exists(knowledge_path, ec))
                        return "WARNING: this directory DOES NOT EXIST — read_knowledge will fail. "
                               "Tell the user to relaunch with --coder-path <repo>/scripts/coder_knowledge/ "
                               "before doing any scripting work. Do not guess library APIs from memory.\n";
                    return "";
                })() +
                "Call read_knowledge before working with: quickstart (declarative adventures — DEFAULT for new ones),\n"
                "lua_api (C++ Lua bindings), lib_adventure (adv.* framework),\n"
                "lib_persona (NPC files/agents), lib_world (procedural world), lib_agent (NPC agents),\n"
                "lib_memory (cross-session memory), lib_tools (tool system), lib_visualnovel (VN mode, catalog,\n"
                "sprites, backgrounds, verb-coin), lib_assets (image tools, asset paths, generation workflows),\n"
                "patterns (design patterns), template_ref (advanced adventure template), decisions_guide (§DECISIONS).\n";
            if (!cfg.langInstruction.empty())
                prompt += "\n## Language\n" + cfg.langInstruction +
                    " EXCEPTION: the `instruction` field in edit_image calls and the `prompt` field in generate_image calls MUST always be written in English regardless of language setting — image models only understand English.\n";
            std::ifstream mf(cfg.basePath + "coder_memory.md");
            if (mf.good()) {
                std::string mc((std::istreambuf_iterator<char>(mf)),
                                std::istreambuf_iterator<char>());
                if (!mc.empty())
                    prompt += "\n## Your persistent memory\n" + mc + "\n";
            }
            // Recent image operations log (last 10 entries from coder_image_log.jsonl)
            {
                std::ifstream imglog(cfg.basePath + "coder_image_log.jsonl");
                if (imglog.good()) {
                    std::vector<std::string> lines;
                    std::string ln;
                    while (std::getline(imglog, ln))
                        if (!ln.empty()) lines.push_back(ln);
                    if (!lines.empty()) {
                        prompt += "\n## Recent image operations (last sessions)\n";
                        size_t start = lines.size() > 10 ? lines.size() - 10 : 0;
                        for (size_t i = start; i < lines.size(); i++) {
                            try {
                                auto e = json::parse(lines[i]);
                                std::string p = e.value("prompt","");
                                if (p.size() > 120) p = p.substr(0,120) + "...";
                                prompt += "[" + e.value("ts","") + "] " + e.value("tool","") +
                                          " → " + e.value("path","") + "\n  prompt: " + p + "\n";
                            } catch(...) {}
                        }
                    }
                }
            }
            // Active session context (injected only when a game is running)
            if (session_state == SessionState::PLAYING && !active_script.empty()) {
                std::string npc_p, log_p;
                bool has_vn = false;
                {
                    std::lock_guard<std::mutex> llk(lua_mutex);
                    sol::object o1 = lua["_PERSONA_BASE_PATH"];
                    if (o1.is<std::string>()) npc_p = o1.as<std::string>();
                    sol::object o2 = lua["_GAMELOG_PATH"];
                    if (o2.is<std::string>()) log_p = o2.as<std::string>();
                    has_vn = lua["get_vn_scene"].valid();
                }
                prompt += "\n## Active game session\n";
                prompt += "Script: " + active_script + "\n";
                if (!npc_p.empty()) prompt += "NPC path: " + npc_p + "\n";
                if (!log_p.empty()) prompt += "Game log: " + log_p + "\n";
                prompt += "The user can prefix their message with @state (current game state JSON), "
                          "@log or @log:N (last N lines of the game log, default 10), "
                          "@errors (recent Lua errors) — these are expanded server-side before you see them.\n";
                if (has_vn) {
                    std::string cat = g_active_script_stem.empty()
                        ? "catalog/vn_scene.json"
                        : ("catalog/" + g_active_script_stem + "_vn.json");
                    prompt +=
                        "\n## Visual Novel mode is active\n"
                        "Catalog path: " + cat + "\n"
                        "Schema (backgrounds): {id, file, tags, location, conditions:[{time_from,time_to}|{flag}|{object,state}], hotspots:[{type,object|location,rect:[x,y,w,h]}]}\n"
                        "Schema (sprites):     {id, npc, file, tags, conditions:[{outfit}|{stat,min,max}|{flag}]}\n"
                        "Schema (verb_options): {\"esamina\":[\"vestiti\",\"espressione\"], \"chiedi\":[\"nome\"]} — per-verb L2 sub-options shown in the GUI verb coin\n"
                        "NPC persona files carry vn_verbs (L1 verbs) and topics (L2 for 'parla').\n"
                        "After editing the catalog tell the user to press 'Ricarica' in the VN window.\n"
                        "Inline image preview: write any local .png/.jpg path in your reply and the GUI will render it automatically.\n";
                }
            }
            return prompt;
        };

        // GET /api/coder/image?path=<path>  →  serve local image file (CoderAI use only)
        // GET /api/coder/image?url=<url>    →  proxy external image (avoids referer/hotlink blocks)
        CROW_ROUTE(app, "/api/coder/image")([&](const crow::request& req) {
            // External URL proxy
            std::string ext_url = req.url_params.get("url") ? std::string(req.url_params.get("url")) : "";
            if (!ext_url.empty()) {
                if (ext_url.size() < 8 ||
                    (ext_url.substr(0,7) != "http://" && ext_url.substr(0,8) != "https://"))
                { crow::response res(400,"bad url"); return res; }
                CURL* c = curl_easy_init();
                if (!c) { crow::response res(502,"curl init"); return res; }
                std::string body;
                curl_easy_setopt(c, CURLOPT_URL, ext_url.c_str());
                curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
                curl_easy_setopt(c, CURLOPT_USERAGENT,
                    "Mozilla/5.0 (X11; Linux x86_64; rv:120.0) Gecko/20100101 Firefox/120.0");
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                    +[](char* p, size_t s, size_t n, void* u) -> size_t {
                        auto* b = static_cast<std::string*>(u);
                        size_t add = s*n;
                        if (b->size() + add > 8*1024*1024) return 0; // 8MB limit
                        b->append(p, add); return add; });
                curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
                curl_easy_perform(c);
                curl_easy_cleanup(c);
                if (body.empty()) { crow::response res(502,"fetch failed"); return res; }
                // Sniff MIME from magic bytes
                std::string mime = "image/jpeg";
                auto ub = [&](int i) { return (unsigned char)body[i]; };
                if (body.size()>=4 && ub(0)==0x89 && body[1]=='P' && body[2]=='N' && body[3]=='G')
                    mime = "image/png";
                else if (body.size()>=12 && body.substr(0,4)=="RIFF" && body.substr(8,4)=="WEBP")
                    mime = "image/webp";
                else if (body.size()>=6 && (body.substr(0,6)=="GIF87a" || body.substr(0,6)=="GIF89a"))
                    mime = "image/gif";
                crow::response res(body);
                res.set_header("Content-Type", mime);
                res.set_header("Cache-Control", "max-age=3600");
                return res;
            }
            // Local file serving
            std::string path = req.url_params.get("path") ? std::string(req.url_params.get("path")) : "";
            if (path.empty() || path.find("..") != std::string::npos || path[0] == '/') {
                crow::response res(400, "bad path"); return res;
            }
            if (!is_coder_path_allowed(path)) { crow::response res(403, "forbidden"); return res; }
            std::ifstream f(path, std::ios::binary);
            if (!f.good()) { crow::response res(404, "not found"); return res; }
            std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string ext = path.substr(path.rfind('.')+1);
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            std::string mime = (ext=="jpg"||ext=="jpeg") ? "image/jpeg"
                             : (ext=="webp") ? "image/webp"
                             : "image/png";
            crow::response res(std::string(buf.begin(), buf.end()));
            res.set_header("Content-Type", mime);
            res.set_header("Cache-Control", "max-age=3600");
            return res;
        });

        // GET /api/coder/files?path=scripts/  →  directory listing for CoderAI file tree
        CROW_ROUTE(app, "/api/coder/files")([&](const crow::request& req) {
            std::string path = req.url_params.get("path") ? std::string(req.url_params.get("path")) : "";
            // Default: list top-level sections
            if (path.empty()) {
                json sections = json::array();
                for (auto& p : std::vector<std::string>{"scripts/","saves/","images/","my_scripts/","asset/","catalog/"}) {
                    if (std::filesystem::exists(p))
                        sections.push_back({{"name",p},{"type","dir"},{"path",p},{"is_image",false}});
                }
                json r = {{"path",""},{"items",sections}};
                crow::response res(r.dump()); res.set_header("Content-Type","application/json"); return res;
            }
            if (path.find("..") != std::string::npos || path[0] == '/' ||
                !is_coder_path_allowed(path)) {
                crow::response res(400, json{{"error","invalid path"}}.dump());
                res.set_header("Content-Type","application/json"); return res;
            }
            json items = json::array();
            try {
                if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
                    for (auto& e : std::filesystem::directory_iterator(path)) {
                        std::string name = e.path().filename().string();
                        if (!name.empty() && name[0] == '.') continue;
                        bool is_dir = e.is_directory();
                        std::string ext = e.path().extension().string();
                        // lowercase ext
                        for (auto& c : ext) c = (char)tolower((unsigned char)c);
                        bool is_img = (ext==".png"||ext==".jpg"||ext==".jpeg"||ext==".webp");
                        items.push_back({{"name",name},{"type",is_dir?"dir":"file"},
                                         {"path",e.path().string()},{"is_image",is_img}});
                    }
                    std::sort(items.begin(), items.end(), [](const json& a, const json& b){
                        bool ad = a["type"]=="dir", bd = b["type"]=="dir";
                        if (ad!=bd) return ad>bd;
                        return a["name"].get<std::string>() < b["name"].get<std::string>();
                    });
                }
            } catch (...) {}
            json r = {{"path",path},{"items",items}};
            crow::response res(r.dump()); res.set_header("Content-Type","application/json"); return res;
        });

        // -----------------------------------------------------------------
        // Tool-mods (GUI_VISION.md §8): user-written Lua tools in gui_tools/.
        // Each file returns { name, params = { {id,type,label,default,options}.. },
        // run = function(p) -> { ok, text?, image? } | string }.
        // Files are re-loaded on every call → hot edit, no restart.
        // Bridges available to tool-mods: coder_tool(name, args_json) → the
        // whole CoderAI toolset (generate_image, edit_image, analyze_image,
        // read_file, web_search, ...), plus query_llm(...).
        // -----------------------------------------------------------------
        static std::mutex tools_mutex;
        auto load_tool_mod = [&](sol::state& tl, const std::string& path) -> sol::table {
            tl.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table,
                              sol::lib::math, sol::lib::package, sol::lib::io,
                              sol::lib::os);
            tl["package"]["path"] = cfg.basePath + "?.lua;" + cfg.basePath + "lib/?.lua";
            tl.set_function("coder_tool", [&](const std::string& name,
                                              const std::string& args_json) -> std::string {
                return execute_coder_tool(name, args_json);
            });
            tl.set_function("query_llm", [&](const std::string& sys,
                    const std::string& hist_json, const std::string& user,
                    sol::object schema_o, sol::object model_o, sol::object prov_o) -> std::string {
                std::string schema = schema_o.is<std::string>() ? schema_o.as<std::string>() : "";
                std::string model  = model_o.is<std::string>()  ? model_o.as<std::string>()  : cfg.activeModel();
                std::string prov   = prov_o.is<std::string>()   ? prov_o.as<std::string>()   : cfg.providerName;
                std::vector<Message> hist;
                try { for (auto& m : json::parse(hist_json)) hist.push_back({m["role"],m["content"]}); } catch (...) {}
                return query_llm(provider_from_string(prov), sys, hist, user, schema, model);
            });
            sol::protected_function_result r = tl.safe_script_file(path, sol::script_pass_on_error);
            if (!r.valid()) { sol::error e = r; throw std::runtime_error(e.what()); }
            sol::object o = r;
            if (o.get_type() != sol::type::table)
                throw std::runtime_error("tool-mod must return a table");
            return o.as<sol::table>();
        };

        // GET /api/tools/list → scan gui_tools/*.lua
        CROW_ROUTE(app, "/api/tools/list")([&]() {
            std::lock_guard<std::mutex> lock(tools_mutex);
            json out = json::array();
            std::error_code ec;
            for (auto& e : std::filesystem::directory_iterator("gui_tools", ec)) {
                if (e.path().extension() != ".lua") continue;
                json t;
                std::string tid = e.path().stem().string();
                t["id"] = tid;
                try {
                    sol::state tl;
                    sol::table mod = load_tool_mod(tl, e.path().string());
                    t["name"] = mod.get_or<std::string>("name", tid);
                    json params = json::array();
                    sol::object po = mod["params"];
                    if (po.is<sol::table>()) {
                        for (auto& kv : po.as<sol::table>()) {
                            sol::table p = kv.second.as<sol::table>();
                            json pj;
                            std::string pid = p.get_or<std::string>("id", "");
                            pj["id"]      = pid;
                            pj["type"]    = p.get_or<std::string>("type", "text");
                            pj["label"]   = p.get_or<std::string>("label", pid);
                            pj["default"] = p.get_or<std::string>("default", "");
                            sol::object opts = p["options"];
                            if (opts.is<sol::table>()) {
                                json oj = json::array();
                                for (auto& ov : opts.as<sol::table>())
                                    oj.push_back(ov.second.as<std::string>());
                                pj["options"] = oj;
                            }
                            params.push_back(pj);
                        }
                    }
                    t["params"] = params;
                } catch (const std::exception& ex) {
                    t["error"] = std::string(ex.what());
                }
                out.push_back(t);
            }
            json result = {{"tools", out}};
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/tools/run  Body: { "id": "...", "params": {k:v,...} }
        // Synchronous (the GUI calls it from a worker thread).
        CROW_ROUTE(app, "/api/tools/run").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(tools_mutex);
            json result;
            try {
                json body = json::parse(req.body);
                std::string id = body.value("id", "");
                std::string path = "gui_tools/" + id + ".lua";
                if (id.empty() || id.find('/') != std::string::npos ||
                    id.find("..") != std::string::npos || !std::filesystem::exists(path))
                    throw std::runtime_error("unknown tool: " + id);
                sol::state tl;
                sol::table mod = load_tool_mod(tl, path);
                sol::protected_function run = mod["run"];
                if (!run.valid()) throw std::runtime_error("tool has no run()");
                sol::table p = tl.create_table();
                for (auto& [k, v] : body.value("params", json::object()).items()) {
                    if      (v.is_string())  p[k] = v.get<std::string>();
                    else if (v.is_number())  p[k] = v.get<double>();
                    else if (v.is_boolean()) p[k] = v.get<bool>();
                }
                sol::protected_function_result r = run(p);
                if (!r.valid()) { sol::error e = r; throw std::runtime_error(e.what()); }
                sol::object o = r;
                result["success"] = true;
                if (o.is<std::string>()) {
                    result["text"] = o.as<std::string>();
                } else if (o.is<sol::table>()) {
                    sol::table rt = o.as<sol::table>();
                    result["success"] = rt.get_or("ok", true);
                    std::string txt = rt.get_or<std::string>("text", "");
                    std::string img = rt.get_or<std::string>("image", "");
                    std::string err = rt.get_or<std::string>("error", "");
                    if (!txt.empty()) result["text"]  = txt;
                    if (!img.empty()) result["image"] = img;
                    if (!err.empty()) result["error"] = err;
                }
            } catch (const std::exception& ex) {
                result["success"] = false;
                result["error"]   = std::string(ex.what());
            }
            crow::response res(result.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // POST /api/reload  →  hot-reload active Lua script
        // Body (optional): { "preserve_state": true }
        CROW_ROUTE(app, "/api/reload").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            try {
                json body = req.body.empty() ? json::object() : json::parse(req.body);
                bool preserve = body.value("preserve_state", false);
                result = do_script_reload(preserve);
            } catch (const std::exception& ex) {
                push_script_error(std::string("reload: ") + ex.what());
                result = {{"success",false},{"error",std::string(ex.what())}};
            }
            crow::response res(result.dump()); res.set_header("Content-Type","application/json"); return res;
        });

        // POST /api/coder/chat
        CROW_ROUTE(app, "/api/coder/chat").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(coder_mutex);
            json result;
            try {
                auto body = json::parse(req.body);
                std::string user_msg = body.value("message","");
                if (body.contains("provider")) coder_session.provider_override = body["provider"].get<std::string>();
                if (body.contains("model"))    coder_session.model_override    = body["model"].get<std::string>();
                if (user_msg.empty()) {
                    result = {{"success",false},{"error","message is required"}};
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type","application/json"); return res;
                }
                // A tool approval is outstanding. Starting a new chat now would
                // overwrite the pending snapshot/history, orphaning that approval
                // and letting it execute against the wrong context. Block it.
                if (coder_session.pending.has_value()) {
                    result = {{"success",false},{"status","pending_approval"},
                              {"error","An action is awaiting approval — approve or deny it before sending a new message."}};
                    crow::response res(409, result.dump());
                    res.set_header("Content-Type","application/json"); return res;
                }
                // ── @mention expansion ──────────────────────────────────────────
                // @state      → current game state JSON
                // @log[:N]    → last N lines of game log (default 10)
                // @errors     → recent Lua script errors
                // Expanded server-side; CoderAI sees the content, not the tag.
                auto read_last_lines = [](const std::string& path, int n) -> std::string {
                    std::ifstream f(path);
                    if (!f.good()) return "(log non trovato: " + path + ")";
                    std::vector<std::string> lines;
                    std::string line;
                    while (std::getline(f, line)) lines.push_back(line);
                    if (lines.empty()) return "(log vuoto)";
                    int start = std::max(0, (int)lines.size() - n);
                    std::string out;
                    for (int i = start; i < (int)lines.size(); ++i)
                        out += lines[i] + "\n";
                    return out;
                };
                // @state
                {
                    const std::string tag = "@state";
                    size_t pos;
                    while ((pos = user_msg.find(tag)) != std::string::npos) {
                        std::string exp = "[STATO NON DISPONIBILE (sessione non attiva)]";
                        if (session_state == SessionState::PLAYING) {
                            try {
                                std::lock_guard<std::mutex> llk(lua_mutex);
                                std::string st = lua["get_status_for_ai"]().get<std::string>();
                                std::string sn = lua["get_state_snapshot"]().get<std::string>();
                                std::string dp;
                                sol::protected_function gds = lua["get_display_state"];
                                if (gds.valid()) { auto r = gds(); if (r.valid()) dp = r.get<std::string>(); }
                                json out;
                                try { out["status"]   = json::parse(st); } catch (...) { out["status"]   = st; }
                                try { out["snapshot"] = json::parse(sn); } catch (...) { out["snapshot"] = sn; }
                                out["display"] = dp;
                                out["script"]  = active_script;
                                sol::object np = lua["_PERSONA_BASE_PATH"];
                                if (np.is<std::string>()) out["npc_path"] = np.as<std::string>();
                                exp = "[GAME STATE]\n" + out.dump(2) + "\n[/GAME STATE]";
                            } catch (...) { exp = "[ERRORE lettura stato]"; }
                        }
                        user_msg.replace(pos, tag.size(), exp);
                    }
                }
                // @log[:N]
                {
                    size_t pos = 0;
                    while ((pos = user_msg.find("@log", pos)) != std::string::npos) {
                        int n = 10;
                        size_t end = pos + 4;
                        if (end < user_msg.size() && user_msg[end] == ':') {
                            size_t ns = end + 1, ne = ns;
                            while (ne < user_msg.size() && std::isdigit((unsigned char)user_msg[ne])) ++ne;
                            if (ne > ns) { n = std::stoi(user_msg.substr(ns, ne - ns)); end = ne; }
                        }
                        std::string log_path;
                        { std::lock_guard<std::mutex> llk(lua_mutex);
                          sol::object lp = lua["_GAMELOG_PATH"];
                          if (lp.is<std::string>()) log_path = lp.as<std::string>(); }
                        std::string exp;
                        if (log_path.empty()) {
                            exp = "[GAME LOG non configurato per questa avventura]";
                        } else {
                            exp = "[GAME LOG ultime " + std::to_string(n) + " righe — " + log_path + "]\n"
                                + read_last_lines(log_path, n) + "[/GAME LOG]";
                        }
                        user_msg.replace(pos, end - pos, exp);
                        pos += exp.size();
                    }
                }
                // @errors
                {
                    const std::string tag = "@errors";
                    size_t pos;
                    while ((pos = user_msg.find(tag)) != std::string::npos) {
                        std::string exp = "[ERRORI SCRIPT]\n";
                        { std::lock_guard<std::mutex> g(script_error_mutex);
                          int start = std::max(0, (int)script_error_ring.size() - 10);
                          if (start >= (int)script_error_ring.size()) exp += "(nessun errore recente)\n";
                          else for (int i = start; i < (int)script_error_ring.size(); ++i)
                              exp += script_error_ring[i] + "\n"; }
                        exp += "[/ERRORI SCRIPT]";
                        user_msg.replace(pos, tag.size(), exp);
                    }
                }
                // ── end @mention expansion ───────────────────────────────────────

                AIProvider  use_provider = cfg.coderProvider;
                std::string use_model    = cfg.coderModel;
                if (!coder_session.provider_override.empty())
                    use_provider = provider_from_string(coder_session.provider_override);
                if (!coder_session.model_override.empty())
                    use_model = coder_session.model_override;
                auto [base_url, api_key] = get_coder_api_config(use_provider);
                if (base_url.empty()) {
                    result = {{"success",false},{"error",
                        "CoderAI supports only openrouter, openai, or ollama providers "
                        "(tool calling uses the OpenAI wire format). Set --coder-provider "
                        "to one of those."}};
                    crow::response res(400, result.dump());
                    res.set_header("Content-Type","application/json"); return res;
                }

                // Prepend any async job completion notifications to the user message
                if (!coder_session.pending_notifications.empty()) {
                    std::string note_prefix;
                    for (auto& n : coder_session.pending_notifications)
                        note_prefix += "[System: " + n + "]\n";
                    coder_session.pending_notifications.clear();
                    user_msg = note_prefix + user_msg;
                }

                std::string sys = build_coder_system_prompt();
                json messages = json::array();
                messages.push_back({{"role","system"},{"content",sys}});
                for (auto& m : coder_session.history)
                    messages.push_back({{"role",m.role},{"content",m.content}});
                messages.push_back({{"role","user"},{"content",user_msg}});

                auto loop_res = run_coder_loop(messages, base_url, api_key, use_model, 20);
                result = handle_loop_result(loop_res, user_msg);
            } catch (const std::exception& e) {
                result = {{"success",false},{"error",std::string(e.what())}};
            }
            crow::response res(result.dump());
            res.set_header("Content-Type","application/json"); return res;
        });

        // POST /api/coder/reset
        CROW_ROUTE(app, "/api/coder/reset").methods("POST"_method)([&]() {
            std::lock_guard<std::mutex> lock(coder_mutex);
            coder_session.history.clear();
            coder_session.pending.reset();
            coder_session.pending_notifications.clear();
            coder_session.active = false;
            std::remove(coder_history_path().c_str());
            json result = {{"success",true}};
            crow::response res(result.dump());
            res.set_header("Content-Type","application/json"); return res;
        });

        // GET /api/coder/capabilities — live tools+vision test on the configured coder model
        CROW_ROUTE(app, "/api/coder/capabilities")([&]() {
            std::string prov  = cfg.coderProviderName.empty() ? cfg.providerName : cfg.coderProviderName;
            std::string model = cfg.coderModel.empty()        ? cfg.activeModel()  : cfg.coderModel;
            std::string key   = cfg.coderKey.empty()          ? cfg.openrouter_key : cfg.coderKey;
            std::string base_url;
            bool is_ollama = (prov == "ollama");
            if (is_ollama) {
                base_url = cfg.ollama_baseUrl;
                key = "";
            } else if (prov == "openrouter") {
                base_url = "https://openrouter.ai/api/v1";
                if (key.empty()) key = cfg.openrouter_key;
            } else { // openai
                base_url = "https://api.openai.com/v1";
                if (key.empty()) key = cfg.openai_key;
            }

            // Helper: POST json, return response body string
            auto http_post = [&](const std::string& url, const std::string& body,
                                  const std::vector<std::string>& hdrs) -> std::string {
                CURL* c = curl_easy_init();
                if (!c) return "";
                std::string out;
                struct curl_slist* hl = nullptr;
                for (auto& h : hdrs) hl = curl_slist_append(hl, h.c_str());
                curl_easy_setopt(c, CURLOPT_URL,        url.c_str());
                curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
                curl_easy_setopt(c, CURLOPT_POSTFIELDS,    body.c_str());
                curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
                curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                    +[](char* p, size_t s, size_t n, void* u)->size_t{
                        ((std::string*)u)->append(p,s*n); return s*n; });
                curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
                curl_easy_perform(c);
                curl_slist_free_all(hl); curl_easy_cleanup(c);
                return out;
            };

            // Valid 4x4 red PNG (pre-generated, checksum correct)
            static const char* TEST_PNG_B64 =
                "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAIAAAAmkwkpAAAAD0lEQVR4nGP4z8"
                "DAQAEGAA5OD/G11Ln9AAAAAElFTkSuQmCC";

            json tools_def = json::array();
            tools_def.push_back({
                {"type","function"},
                {"function",{
                    {"name","get_color"},
                    {"description","Returns a color name"},
                    {"parameters",{
                        {"type","object"},
                        {"properties",{{"color",{{"type","string"}}}}},
                        {"required",json::array({"color"})}
                    }}
                }}
            });

            bool tools_ok = false, vision_ok = false;
            std::string tools_err, vision_err;

            // ── TOOLS TEST ────────────────────────────────────────────────────
            try {
                std::string resp;
                if (is_ollama) {
                    json body = {
                        {"model", model}, {"stream", false},
                        {"messages", json::array({{{"role","user"},{"content","What color is the sky? Use get_color."}}})} ,
                        {"tools", tools_def}
                    };
                    resp = http_post(base_url + "/api/chat", body.dump(),
                                     {"Content-Type: application/json"});
                    auto j = json::parse(resp);
                    if (j.contains("error")) tools_err = j["error"].get<std::string>();
                    else tools_ok = j.value("message", json{}).contains("tool_calls");
                } else {
                    json body = {
                        {"model", model}, {"max_tokens", 50},
                        {"messages", json::array({{{"role","user"},{"content","What color is the sky? Use get_color."}}})} ,
                        {"tools", tools_def}, {"tool_choice","auto"}
                    };
                    std::vector<std::string> hdrs = {"Content-Type: application/json",
                        "Authorization: Bearer " + key};
                    resp = http_post(base_url + "/chat/completions", body.dump(), hdrs);
                    auto j = json::parse(resp);
                    if (j.contains("error")) tools_err = j["error"].dump().substr(0,100);
                    else {
                        auto& ch = j["choices"][0]["message"];
                        tools_ok = ch.contains("tool_calls") && !ch["tool_calls"].empty();
                    }
                }
            } catch (const std::exception& e) { tools_err = e.what(); }

            // ── VISION TEST ───────────────────────────────────────────────────
            try {
                std::string resp;
                if (is_ollama) {
                    json body = {
                        {"model", model}, {"stream", false},
                        {"messages", json::array({{
                            {"role","user"},
                            {"content","What color is this image? One word."},
                            {"images", json::array({std::string(TEST_PNG_B64)})}
                        }})}
                    };
                    resp = http_post(base_url + "/api/chat", body.dump(),
                                     {"Content-Type: application/json"});
                    auto j = json::parse(resp);
                    if (j.contains("error")) vision_err = j["error"].get<std::string>();
                    else {
                        std::string content = j.value("message",json{}).value("content","");
                        vision_ok = !content.empty();
                    }
                } else {
                    std::string data_url = std::string("data:image/png;base64,") + TEST_PNG_B64;
                    json body = {
                        {"model", model}, {"max_tokens", 20},
                        {"messages", json::array({{
                            {"role","user"},
                            {"content", json::array({
                                {{"type","text"},{"text","What color is this image? One word."}},
                                {{"type","image_url"},{"image_url",{{"url",data_url}}}}
                            })}
                        }})}
                    };
                    std::vector<std::string> hdrs = {"Content-Type: application/json",
                        "Authorization: Bearer " + key};
                    resp = http_post(base_url + "/chat/completions", body.dump(), hdrs);
                    auto j = json::parse(resp);
                    if (j.contains("error")) vision_err = j["error"].dump().substr(0,100);
                    else {
                        std::string content = j["choices"][0]["message"].value("content","");
                        vision_ok = !content.empty();
                    }
                }
            } catch (const std::exception& e) { vision_err = e.what(); }

            json result = {
                {"model",   model}, {"provider", prov},
                {"tools",   tools_ok},  {"tools_error",  tools_err},
                {"vision",  vision_ok}, {"vision_error", vision_err}
            };
            crow::response res(result.dump());
            res.set_header("Content-Type","application/json"); return res;
        });

        // GET /api/coder/status
        CROW_ROUTE(app, "/api/coder/status")([&]() {
            std::lock_guard<std::mutex> lock(coder_mutex);
            json result = {
                {"active",           coder_session.active},
                {"history_length",   (int)coder_session.history.size()},
                {"provider",         coder_session.provider_override.empty() ? cfg.coderProviderName : coder_session.provider_override},
                {"model",            coder_session.model_override.empty()    ? cfg.coderModel         : coder_session.model_override},
                {"pending_approval", coder_session.pending.has_value()}
            };
            crow::response res(result.dump());
            res.set_header("Content-Type","application/json"); return res;
        });

        // POST /api/coder/approve  — execute pending tool and resume loop
        CROW_ROUTE(app, "/api/coder/approve").methods("POST"_method)([&]() {
            std::lock_guard<std::mutex> lock(coder_mutex);
            json result;
            try {
                if (!coder_session.pending.has_value()) {
                    result = {{"success",false},{"error","no pending approval"}};
                    crow::response res(result.dump());
                    res.set_header("Content-Type","application/json"); return res;
                }
                auto p = *coder_session.pending;
                coder_session.pending.reset();
                std::string tool_result = execute_coder_tool(p.tool_name, p.tool_args);
                // Check if the tool started an async image job
                json async_job;
                try {
                    json tr = json::parse(tool_result);
                    if (tr.contains("job_id") && tr.value("status","") == "processing")
                        async_job = {{"job_id", tr["job_id"]}, {"output_path", tr.value("output_path","")}, {"tool", p.tool_name}};
                } catch (...) {}
                json messages = p.messages_snapshot;
                messages.push_back({{"role","tool"},{"tool_call_id",p.tool_call_id},{"content",tool_result}});
                int rem = std::max(1, p.remaining_iters);
                auto loop_res = run_coder_loop(messages, p.base_url, p.api_key, p.eff_model, rem);
                result = handle_loop_result(loop_res, "");
                if (!async_job.is_null()) result["async_job"] = async_job;
            } catch (const std::exception& e) {
                result = {{"success",false},{"error",std::string(e.what())}};
            }
            crow::response res(result.dump());
            res.set_header("Content-Type","application/json"); return res;
        });

        // POST /api/coder/deny  — inject denial and resume loop
        CROW_ROUTE(app, "/api/coder/deny").methods("POST"_method)([&]() {
            std::lock_guard<std::mutex> lock(coder_mutex);
            json result;
            try {
                if (!coder_session.pending.has_value()) {
                    result = {{"success",true}};
                    crow::response res(result.dump());
                    res.set_header("Content-Type","application/json"); return res;
                }
                auto p = *coder_session.pending;
                coder_session.pending.reset();
                json messages = p.messages_snapshot;
                // Strong refusal: tool result + system-level note that stops the model from
                // re-proposing the same approval-gated action.
                messages.push_back({{"role","tool"},{"tool_call_id",p.tool_call_id},
                    {"content","[USER DENIED] The user refused the action '" + p.tool_name +
                     "'. Do NOT retry this action or any variant of it. "
                     "Acknowledge the refusal and ask the user how to proceed."}});
                // Force exactly 1 iteration — just enough for a final text reply.
                // More iterations would let the model loop back into another tier-1 request.
                auto loop_res = run_coder_loop(messages, p.base_url, p.api_key, p.eff_model, 1);
                result = handle_loop_result(loop_res, "");
            } catch (const std::exception& e) {
                result = {{"success",false},{"error",std::string(e.what())}};
            }
            crow::response res(result.dump());
            res.set_header("Content-Type","application/json"); return res;
        });

        // GET /api/coder/image_job/<id>  — poll async CoderAI image job status
        CROW_ROUTE(app, "/api/coder/image_job/<string>").methods("GET"_method)(
            [](const crow::request&, const std::string& id) {
                std::lock_guard<std::mutex> lk(coder_img_mutex);
                auto it = coder_img_jobs.find(id);
                if (it == coder_img_jobs.end()) {
                    crow::response res(json{{"error","job not found"}}.dump());
                    res.set_header("Content-Type","application/json"); return res;
                }
                const auto& j = it->second;
                json r;
                switch (j.status) {
                    case CoderImgJob::St::RUNNING: r = {{"status","running"}}; break;
                    case CoderImgJob::St::DONE:    r = {{"status","done"},{"path",j.output_path}}; break;
                    case CoderImgJob::St::ERROR:   r = {{"status","error"},{"error",j.error}}; break;
                }
                crow::response res(r.dump());
                res.set_header("Content-Type","application/json"); return res;
            });

        // POST /api/coder/job_done  — JS calls this when async image job completes.
        // Injects a completion note into pending_notifications so the next /chat turn
        // carries it as context, preventing the model from re-reporting "processing".
        CROW_ROUTE(app, "/api/coder/job_done").methods("POST"_method)([&](const crow::request& req) {
            std::lock_guard<std::mutex> lock(coder_mutex);
            json result;
            try {
                json body = json::parse(req.body);
                std::string job_id    = body.value("job_id","");
                std::string out_path  = body.value("output_path","");
                std::string status    = body.value("status","done");
                std::string note;
                if (status == "done")
                    note = "Background edit_image job " + job_id + " completed successfully. "
                           "Image saved at: " + out_path + ". "
                           "The edit is done — do not tell the user it is still processing.";
                else
                    note = "Background edit_image job " + job_id + " failed: " + body.value("error","unknown error") + ".";
                coder_session.pending_notifications.push_back(note);
                result = {{"success",true}};
            } catch (const std::exception& e) {
                result = {{"success",false},{"error",std::string(e.what())}};
            }
            crow::response res(result.dump());
            res.set_header("Content-Type","application/json"); return res;
        });

        // -----------------------------------------------------------------
        // Start server
        // -----------------------------------------------------------------
        int port = cfg.webPort;
        CsrfGuard::port = port;   // allow same-origin POSTs on the chosen port
        std::string url = "http://localhost:" + std::to_string(port);
        // OSC 8 hyperlink sequences
        static const std::string ESC_ST = "\033\\"; // ESC + backslash (string terminator)
        std::string link_open  = "\033]8;;" + url + ESC_ST;
        std::string link_close = "\033]8;;" + ESC_ST;
        std::string clickable  = link_open + url + link_close;
        print_system((cfg.openBrowser ? "Web mode -> " : "REST mode -> ") + clickable);
        // Open browser after Crow binds the port (only --web; --rest stays headless)
        if (cfg.openBrowser) {
            std::thread([url]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
#ifdef __APPLE__
                std::system(("open " + url).c_str());
#elif defined(__linux__)
                std::system(("xdg-open " + url).c_str());
#elif defined(_WIN32)
                std::system(("start " + url).c_str());
#endif
            }).detach();
        }
        print_system("Routes: GET /  /api/scripts  /api/saves  /api/status  /api/show_asset  /api/scene_image  /api/commands");
        print_system("        POST /api/start  /api/init  /api/load  /api/chat  /api/command  /api/save");
        print_system("        POST /api/image  /api/generate_asset  /api/swap  GET /api/image/job/<id>");
        print_system("        GET|POST /api/settings  GET /api/servers/status  POST /api/servers/action");
        print_system("CoderAI: POST /api/coder/chat  /api/coder/reset  /api/coder/approve  /api/coder/deny  GET /api/coder/status");
        print_system("        POST /api/reload  (hot-reload active script)");
        if (cfg.imgEnabled)
            print_system("Image:  provider=" + cfg.imgProvider + " url=" + cfg.imgUrl);
        if (!cfg.faceswapUrl.empty())
            print_system("FaceSwap: " + cfg.faceswapUrl);
        app.loglevel(crow::LogLevel::Warning);
        app.port(port).multithreaded().run();
    }

    // Clear sol::protected_function references before sol::state goes out of scope.
    // active_tool_fns is a static global destroyed AFTER sol::state lua (local),
    // so without this the sol::protected_function destructors call luaL_unref()
    // on an already-closed Lua state → SIGSEGV on Ctrl+C.
    active_tools.clear();
    active_tool_fns.clear();

    curl_global_cleanup();
            return 1;
        }
   