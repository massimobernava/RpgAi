
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

using nlohmann::json;

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
    return j;
}

static void apply_settings_json(const json& j) {
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
    for (const auto& w : tok_cur) {
        if (tokenize(entry.player_input).count(w))  score += 1;
        if (tokenize(entry.narration).count(w))      score += 1;
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
                std::string narr = j.value("narration", "");
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
                        try {
                            auto jc = json::parse(msg.value("content", "{}"));
                            narr = jc.value("narration", "");
                        } catch (...) {}
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
    opt("--web",                  "Enable web mode",                         "");
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
        if      (arg == "--help" || arg == "-h") { print_help(argv[0]); std::exit(0); }
        else if (arg == "--provider")      { cfg.providerName = next(); cfg.provider = provider_from_string(cfg.providerName); }
        else if (arg == "--script")        { cfg.script      = next(); }
        else if (arg == "--path")          { cfg.basePath    = next(); if (cfg.basePath.back() != '/') cfg.basePath += '/'; }
        else if (arg == "--load")          { cfg.loadFile    = next(); }
        else if (arg == "--save")          { cfg.saveFile    = next(); }
        else if (arg == "--save-mode")     { auto m = next(); cfg.saveMode = (m == "full") ? SaveMode::FULL : SaveMode::LAST; }
        else if (arg == "--save-path")     { cfg.savePath = next(); if (!cfg.savePath.empty() && cfg.savePath.back() != '/') cfg.savePath += '/'; }
        else if (arg == "--img-provider")  { cfg.imgProvider = next(); cfg.imgEnabled = true; }
        else if (arg == "--img-url")       { cfg.imgUrl      = next(); }
        else if (arg == "--img-key")       { cfg.imgKey      = next(); }
        else if (arg == "--img-t2i-model") { cfg.imgT2iModel = next(); }
        else if (arg == "--img-i2i-model") { cfg.imgI2iModel = next(); }
        else if (arg == "--img-width")     { cfg.imgWidth    = std::stoi(next()); }
        else if (arg == "--img-height")    { cfg.imgHeight   = std::stoi(next()); }
        else if (arg == "--img-steps")     { cfg.imgSteps    = std::stoi(next()); }
        else if (arg == "--img-strength")  { cfg.imgStrength  = std::stof(next()); }
        else if (arg == "--img-i2i-provider") { cfg.imgI2iProvider = next(); }
        else if (arg == "--img-i2i-url")   { cfg.imgI2iUrl    = next(); }
        else if (arg == "--tts-url")       { cfg.ttsUrl       = next(); }
        else if (arg == "--img-i2i-key")   { cfg.imgI2iKey    = next(); }
        else if (arg == "--img-lora")       { cfg.imgLora       = next(); }
        else if (arg == "--img-lora-scale") { cfg.imgLoraScale   = std::stof(next()); }
        else if (arg == "--i2i-model-lora")       { cfg.imgLoraModel      = next(); }
        else if (arg == "--img-i2i-steps")        { cfg.imgI2iSteps       = std::stoi(next()); }
        else if (arg == "--img-guidance-scale")   { cfg.imgGuidanceScale  = std::stof(next()); }
        else if (arg == "--faceswap-url")  { cfg.faceswapUrl   = next(); }
        else if (arg == "--web")           { cfg.webMode     = true; }
        else if (arg == "--rag")           { cfg.ragFile     = next(); }
        else if (arg == "--rag-examples")  { cfg.ragExamples = std::stoi(next()); }
        else if (arg == "--max-history")   { cfg.maxHistory  = std::stoi(next()); }
        else if (arg == "--max-retries")   { cfg.maxRetries  = std::stoi(next()); }
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

    print_system("RpgAi Engine starting...");
    print_system("Provider:   " + cfg.providerName + " | Model: " + active_model);
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

    curl_global_init(CURL_GLOBAL_ALL);

    // --- Lua setup ---
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::string,
                       sol::lib::table, sol::lib::math, sol::lib::os,sol::lib::debug);
    const std::string lua_default_path = lua["package"]["path"].get<std::string>();
    auto update_lua_path = [&]() {
        lua["package"]["path"] = cfg.basePath + "lib/?.lua;" + lua_default_path;
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
            const std::string& schema) -> std::string {

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

            return ::query_llm(cfg.provider, with_lang(sys_prompt), lua_history,
                               user_prompt, schema, cfg.activeModel());
        });

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

    // composite_images exposed to Lua — alpha-blend a stack of PNG layers into one file.
    // Lua signature:
    //   local ok, err = composite_images(layers_table, output_rel_path)
    //   Each entry in layers_table is either:
    //     - a string path (relative to --path): resized to canvas, placed at (0,0)
    //     - a table {path=string, x=int, y=int}: placed at pixel offset, natural size
    //   Canvas dimensions are taken from the first layer (must be a plain string path).
    //   output_rel_path: destination relative to --path base dir
    //   returns: true, "" on success | false, errmsg on failure
    lua.set_function("composite_images",
        [&](sol::table layers_tbl, const std::string& out_rel) -> std::tuple<bool, std::string> {
            namespace fs = std::filesystem;

            struct Layer { std::string path; int x = 0, y = 0; bool positioned = false; };

            try {
                fs::path base(cfg.basePath);
                fs::path out_abs = base / out_rel;
                fs::create_directories(out_abs.parent_path());

                std::vector<Layer> layers;
                for (auto& kv : layers_tbl) {
                    if (kv.second.get_type() == sol::type::string) {
                        layers.push_back({ (base / kv.second.as<std::string>()).string(), 0, 0, false });
                    } else if (kv.second.get_type() == sol::type::table) {
                        sol::table t = kv.second.as<sol::table>();
                        Layer l;
                        l.path       = (base / t.get_or<std::string>("path", "")).string();
                        sol::optional<int> ox = t["x"]; l.x = ox ? *ox : 0;
                        sol::optional<int> oy = t["y"]; l.y = oy ? *oy : 0;
                        l.positioned = true;
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

                auto blend_region = [&](const stbi_uc* src, int src_w, int src_h, int ox, int oy) {
                    for (int sy = 0; sy < src_h; ++sy) {
                        int dy = oy + sy;
                        if (dy < 0 || dy >= H) continue;
                        for (int sx = 0; sx < src_w; ++sx) {
                            int dx = ox + sx;
                            if (dx < 0 || dx >= W) continue;
                            int si = (sy * src_w + sx) * 4;
                            int di = (dy * W + dx) * 4;
                            float sa = src[si+3] / 255.0f;
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

                for (const auto& layer : layers) {
                    int w, h, ch;
                    stbi_uc* img = stbi_load(layer.path.c_str(), &w, &h, &ch, 4);
                    if (!img) { std::cerr << "[COMPOSITE] skip: " << layer.path << "\n"; continue; }

                    if (layer.positioned) {
                        // Natural size, blitted at (x,y)
                        blend_region(img, w, h, layer.x, layer.y);
                    } else {
                        // Full-canvas: resize to (W,H) then blit at (0,0)
                        if (w != W || h != H) {
                            std::vector<uint8_t> buf(W * H * 4);
                            stbir_resize_uint8_linear(img, w, h, 0, buf.data(), W, H, 0, STBIR_RGBA);
                            blend_region(buf.data(), W, H, 0, 0);
                        } else {
                            blend_region(img, W, H, 0, 0);
                        }
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

    while (running) {
        std::string player_input = read_input("❯ ");
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
                             || player_input.rfind("/summary ", 0) == 0
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
                    chat_history.push_back({"user",      last_player_input, "player"});
                    chat_history.push_back({"assistant", fix_reply,         "gm"});
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
        for (int attempt = 0; attempt < cfg.maxRetries && !turn_ok; ++attempt) {
            if (attempt > 0)
                print_warning("Retry " + std::to_string(attempt) +
                              "/" + std::to_string(cfg.maxRetries - 1) + "...");

            auto trimmed = trim_history(chat_history, cfg.maxHistory);
            std::string llm_reply = script_has_tools
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

                chat_history.push_back({"user",      player_input, "player"});
                chat_history.push_back({"assistant", llm_reply,    "gm"});

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

        // Three possible states for the web session
        enum class SessionState { IDLE, AWAITING_INIT, PLAYING };
        SessionState session_state = SessionState::IDLE;
        std::string  active_script;

        std::string web_last_llm_reply;
        std::string web_last_player_input;

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

            for (int attempt = 0; attempt < cfg.maxRetries; ++attempt) {
                auto trimmed  = trim_history(hist, cfg.maxHistory);
                std::string reply = script_has_tools
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

                std::string narration = res["narration"].get<std::string>();
                bool game_over        = res["game_over"].get<bool>();
                std::string go_reason = res["game_over_reason"].get<std::string>();
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

                hist.push_back({"user",      player_input, "player"});
                hist.push_back({"assistant", reply,        "gm"});

                write_turn(cfg.saveFile, fstream, cfg.saveMode,
                           player_input, reply, narration, snap, hist);

                web_last_llm_reply    = reply;
                web_last_player_input = player_input;

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

        crow::SimpleApp app;

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
        // GET /api/saves  →  lista .jsonl in savePath (o cwd)
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/saves")([&]() {
            json result;
            json arr = json::array();
            try {
                std::string dir = cfg.savePath.empty() ? "." : cfg.savePath;
                for (const auto& e : std::filesystem::directory_iterator(dir)) {
                    if (e.is_regular_file() && e.path().extension() == ".jsonl")
                        arr.push_back(e.path().filename().string());
                }
                std::sort(arr.begin(), arr.end());
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

                // Derive a save file name from the script so a new session never
                // overwrites a save that was loaded from a different script.
                // strip .lua suffix → append _session.jsonl
                {
                    std::string base = script_name;
                    auto dot = base.rfind(".lua");
                    if (dot != std::string::npos) base = base.substr(0, dot);
                    cfg.saveFile = base + "_session.jsonl";
                }
                // Close any previously open FULL-mode stream.
                if (full_stream.is_open()) full_stream.close();

                // Clear optional globals so they don't bleed from a previously loaded script.
                for (const char* fn : {"get_commands", "get_scene_images",
                                       "get_asset_path", "get_asset_prompt", "get_tools"}) {
                    lua[fn] = sol::lua_nil;
                }
                lua.script_file(cfg.basePath + cfg.script);
                load_script_tools(lua);

                chat_history.clear();
                web_last_llm_reply.clear();
                web_last_player_input.clear();
                session_state = SessionState::IDLE;

                std::string welcome = lua["get_welcome_message"]();
                session_state = SessionState::AWAITING_INIT;
                active_script = script_name;

                result["success"]    = true;
                result["welcome"]    = welcome;
                result["needs_init"] = true;
                result["display"]    = "";

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

                // Open full_stream if in FULL save mode
                if (cfg.saveMode == SaveMode::FULL && !full_stream.is_open()) {
                    full_stream.open(resolve_save_path(cfg.saveFile), std::ios::app);
                }

                session_state = SessionState::PLAYING;

                result["success"] = true;
                result["display"] = lua["get_display_state"]().get<std::string>();

            } catch (const std::exception& ex) {
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
                for (const char* fn : {"get_commands", "get_scene_images",
                                       "get_asset_path", "get_asset_prompt", "get_tools"}) {
                    lua[fn] = sol::lua_nil;
                }
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

                web_last_llm_reply.clear();
                web_last_player_input.clear();
                session_state = SessionState::PLAYING;
                active_script = script_to_load;

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
                result = run_turn(input_text, cmd_handled, chat_history, full_stream);
            } catch (const std::exception& ex) {
                result["success"] = false;
                result["error"]   = std::string(ex.what());
            }
            crow::response res(result.dump());
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
                lower_cmd = lower_cmd.substr(lower_cmd.find_first_not_of(" \t"));

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
                    write_turn(cfg.saveFile, full_stream, cfg.saveMode,
                               "[manual save]", "", "", lua["get_state_snapshot"](), chat_history);
                    result["success"] = true;
                    result["output"]  = "Session saved to: " + cfg.saveFile;
                    result["display"] = lua["get_display_state"]().get<std::string>();
                    crow::response res(result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }

                // ---- /summary [N] ----
                bool is_riassunto = (lower_cmd == "/summary" || lower_cmd == "/summarize"
                    || lower_cmd.rfind("/summary ", 0) == 0
                    || lower_cmd.rfind("/summary ",   0) == 0);

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
                        chat_history.push_back({"user",      web_last_player_input, "player"});
                        chat_history.push_back({"assistant", fix_reply,             "gm"});
                        web_last_llm_reply = fix_reply;
                        write_turn(cfg.saveFile, full_stream, cfg.saveMode,
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

                // ---- Delegate to Lua (script-specific commands) ----
                sol::table cmd_result = lua["process_player_input"](cmd_text);
                if (!cmd_result["success"].get<bool>()) {
                    result["success"] = false;
                    result["error"]   = cmd_result["error"].get<std::string>();
                    crow::response res(result.dump());
                    res.set_header("Content-Type", "application/json"); return res;
                }
                if (cmd_result["handled"].get<bool>()) {
                    sol::optional<std::string> out = cmd_result["output"];
                    result["success"] = true;
                    result["output"]  = out ? *out : "";
                    result["display"] = lua["get_display_state"]().get<std::string>();
                } else {
                    result = run_turn(cmd_text, false, chat_history, full_stream);
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
        // POST /api/save  →  manual save
        // -----------------------------------------------------------------
        CROW_ROUTE(app, "/api/save").methods("POST"_method)([&]() {
            std::lock_guard<std::mutex> lock(lua_mutex);
            json result;
            if (session_state == SessionState::IDLE) {
                result["success"] = false;
                result["error"]   = "No active session.";
            } else {
                write_turn(cfg.saveFile, full_stream, cfg.saveMode,
                           "[manual save]", "", "", lua["get_state_snapshot"](), chat_history);
                result["success"] = true;
                result["message"] = "Saved to: " + cfg.saveFile;
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
        };

        std::mutex                          img_jobs_mutex;
        std::map<std::string, ImageJob>     img_jobs;
        std::atomic<int>                    img_job_counter{0};

        auto new_job_id = [&]() -> std::string {
            return "imgjob_" + std::to_string(++img_job_counter);
        };

        // Lancia la generazione su un thread separato e aggiorna il job.
        // La lambda restituisce {bytes, prompt}: il prompt viene esposto nel job
        // per il tooltip nell'UI.
        auto launch_image_job = [&](const std::string& job_id,
                                    std::function<std::pair<std::vector<uint8_t>, std::string>()> fn) {
            std::thread([&img_jobs, &img_jobs_mutex, job_id, fn = std::move(fn)]() {
                try {
                    auto [bytes, prompt] = fn();
                    std::string b64 = bytes_to_base64(bytes);
                    std::lock_guard<std::mutex> lk(img_jobs_mutex);
                    img_jobs[job_id].image_b64 = std::move(b64);
                    img_jobs[job_id].prompt    = std::move(prompt);
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

            launch_image_job(job_id, [=]() -> std::pair<std::vector<uint8_t>, std::string> {
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
                    if (strength_copy <= 0.0f)
                        img_cfg.strength = 0.95f;
                    std::cerr << "[IMG] Mode: compose — strength=" << img_cfg.strength << "\n";

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

                // Apply strength override (--strength / --s flag from fix command)
                float saved_strength = img_cfg.strength;
                if (strength_copy > 0.0f) {
                    img_cfg.strength = strength_copy;
                    std::cerr << "[IMG] Strength override: " << strength_copy << "\n";
                }

                // image-to-image — base_image_bytes (compose) > base_image_path > collage
                auto img_bytes = image_to_image(collage, img_prompt,
                                                base_copy, script_copy, entries_copy,
                                                base_image_path, bypass_cache,
                                                sess_start_copy, base_image_bytes,
                                                lora_copy);
                img_cfg.strength = saved_strength;  // restore
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
            if (asset_id.empty()) {
                result["success"] = false;
                result["error"]   = "Parameter 'id' is required (?id=<asset_id>).";
                crow::response res(400, result.dump());
                res.set_header("Content-Type", "application/json"); return res;
            }

            if (session_state == SessionState::IDLE) {
                result["success"] = false;
                result["error"]   = "No active session.";
                crow::response res(400, result.dump());
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
                json item = {{"cmd", cmd}, {"desc", desc}, {"exec", exec}};
                if (label) item["label"] = label;
                cmds.push_back(item);
            };
            eng("/image",         "Generate scene image. Add 'lora' to apply LoRA via /image lora command. Modes: regen, refine, fix [--s N] <instruction>, compose. Combine: /image lora regen, /image lora fix <text>. Add --partial to allow missing assets.",
                false, "/image [lora] [regen|refine|fix [--s 0.9] <text>|compose [--s N]] [--partial]");
            eng("/swap",          "Face-swap: replace detected faces left-to-right with NPC asset faces. Use 'null' to skip a slot, '--enhance' to run GFPGAN after swap. Requires --faceswap-url.",
                false, "/swap [--enhance] <id1> [null] <id2> ...");
            eng("/show_asset",    "Show a script asset by ID. Usage: /show_asset <id>",
                false, "/show_asset <id>");
            eng("/generate_asset","Generate or regenerate a script asset. Usage: /generate_asset <id>",
                false, "/generate_asset <id>");
            eng("/observe",       "Ask the AI to describe the current scene in detail");
            eng("/fix",           "Rewrite the last AI response");
            eng("/summary",       "Summarise the story so far");
            eng("/save",          "Save the game manually");

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
                    std::time_t mt   = scene_cache::max_mtime(entries_copy);
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

        // -----------------------------------------------------------------
        // Start server
        // -----------------------------------------------------------------
        int port = 8080;
        std::string url = "http://localhost:" + std::to_string(port);
        // OSC 8 hyperlink sequences
        static const std::string ESC_ST = "\033\\"; // ESC + backslash (string terminator)
        std::string link_open  = "\033]8;;" + url + ESC_ST;
        std::string link_close = "\033]8;;" + ESC_ST;
        std::string clickable  = link_open + url + link_close;
        print_system("Web mode -> " + clickable);
        // Open browser after Crow binds the port
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
        print_system("Routes: GET /  /api/scripts  /api/saves  /api/status  /api/show_asset  /api/scene_image  /api/commands");
        print_system("        POST /api/start  /api/init  /api/load  /api/chat  /api/command  /api/save");
        print_system("        POST /api/image  /api/generate_asset  /api/swap  GET /api/image/job/<id>");
        print_system("        GET|POST /api/settings  GET /api/servers/status  POST /api/servers/action");
        if (cfg.imgEnabled)
            print_system("Image:  provider=" + cfg.imgProvider + " url=" + cfg.imgUrl);
        if (!cfg.faceswapUrl.empty())
            print_system("FaceSwap: " + cfg.faceswapUrl);
        app.loglevel(crow::LogLevel::Warning);
        app.port(port).multithreaded().run();
    }

    curl_global_cleanup();
            return 1;
        }
   