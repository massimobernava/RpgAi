
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
    float       imgStrength  = 0.75f;  // --save-mode last|full
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

struct Message { std::string role; std::string content; std::string player_id; };

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
    j["timestamp"]    = utc_timestamp();
    j["player_input"] = player_input;
    j["llm_response"] = llm_response;
    j["narration"]    = narration;
    j["state_after"]  = state_after;
    json j_hist = json::array();
    for (const auto& m : history)
        j_hist.push_back({{"role", m.role}, {"content", m.content}, {"player_id", m.player_id}});
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
        sol::table res = lua["restore_state"](j["state_after"].get<std::string>());
        if (!res["success"]) { std::cerr << "[ERROR] Lua restore failed.\n"; return false; }
        history.clear();
        for (const auto& item : j["chat_history"])
            history.push_back({item["role"], item["content"], item["player_id"]});
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
        else if (arg == "--img-i2i-key")   { cfg.imgI2iKey    = next(); }
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

    if (!parse_args(argc, argv)) return 1;

    std::string key_error = cfg.validate();
    if (!key_error.empty()) { print_error(key_error); return 1; }

    std::string active_model = cfg.activeModel();
    std::string save_mode_str = (cfg.saveMode == SaveMode::FULL) ? "full" : "last";

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
    lua["package"]["path"] = cfg.basePath + "lib/?.lua;" + lua["package"]["path"].get<std::string>();

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

    lua.script_file(cfg.basePath + cfg.script);

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
    // (scripts like estate_italiana recompute them on every call)
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
        sol::table cmd_result = lua["process_player_input"](player_input);

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
            std::string llm_reply = query_llm(cfg.provider, with_lang(effective_sys_prompt), trimmed,
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
                std::string reply = query_llm(cfg.provider, with_lang(sys_prompt), trimmed,
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

                hist.push_back({"user",      player_input, "player"});
                hist.push_back({"assistant", reply,        "gm"});

                write_turn(cfg.saveFile, fstream, cfg.saveMode,
                           player_input, reply, narration, snap, hist);

                web_last_llm_reply    = reply;
                web_last_player_input = player_input;

                result_json["success"]         = true;
                result_json["narration"]        = narration;
                result_json["display"]          = display;
                result_json["game_over"]        = game_over;
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
                for (const auto& e : std::filesystem::directory_iterator(cfg.basePath)) {
                    if (e.is_regular_file() && e.path().extension() == ".lua") {
                        std::string fn = e.path().filename().string();
                        if (fn[0] != '_') arr.push_back(fn);
                    }
                }
                std::sort(arr.begin(), arr.end());
                result["success"] = true;
                result["scripts"] = arr;
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
        // Body: { "script": "estate_italiana_v4X.lua" }
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
                lua.script_file(cfg.basePath + cfg.script);

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
                lua.script_file(cfg.basePath + cfg.script);

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

                result["success"] = true;
                result["script"]  = script_to_load;
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

        struct ImageJob {
            enum class State { PENDING, DONE, ERROR };
            State       state   = State::PENDING;
            std::string image_b64;   // risultato (base64 PNG)
            std::string error;
            std::string asset_id;    // id asset coinvolto (per /generate_asset)
        };

        std::mutex                          img_jobs_mutex;
        std::map<std::string, ImageJob>     img_jobs;
        std::atomic<int>                    img_job_counter{0};

        auto new_job_id = [&]() -> std::string {
            return "imgjob_" + std::to_string(++img_job_counter);
        };

        // Lancia la generazione su un thread separato e aggiorna il job
        auto launch_image_job = [&](const std::string& job_id,
                                    std::function<std::vector<uint8_t>()> fn) {
            std::thread([&img_jobs, &img_jobs_mutex, job_id, fn = std::move(fn)]() {
                try {
                    auto bytes = fn();
                    std::string b64 = bytes_to_base64(bytes);
                    std::lock_guard<std::mutex> lk(img_jobs_mutex);
                    img_jobs[job_id].image_b64 = std::move(b64);
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
            try {
                if (!req.body.empty()) {
                    auto body = json::parse(req.body);
                    partial = body.value("partial", false);
                }
            } catch (...) {}

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
            int collage_h           = img_cfg.height;
            std::string script_copy = cfg.script;
            std::string base_copy   = cfg.basePath;
            std::string hint_copy   = base_image_hint;

            launch_image_job(job_id, [=]() -> std::vector<uint8_t> {
                // Resolve base_image_hint into a concrete file path (if any)
                std::string base_image_path;
                if (hint_copy == "last") {
                    base_image_path = scene_cache::lookup_last(
                        base_copy, script_copy, entries_copy);
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

                // Generate visual prompt via main LLM
                std::string tags;
                for (const auto& e : entries_copy)
                    tags += "[" + e.tag + "] ";

                std::string prompt_sys =
                    "You are a visual prompt engineer for an image editing model. "
                    "Given a reference image and scene context, write a concise image prompt "
                    "describing how to render the scene. "
                    "Max 120 words. Visual description only, no JSON, no lists.";

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
                prompt_user += "\n\nWrite the image composition prompt in English.";

                std::string img_prompt = ::query_llm(
                    ::cfg.provider, prompt_sys, {}, prompt_user, "", ::cfg.activeModel());

                std::cerr << "[IMG] Scene prompt: " << img_prompt.substr(0, 120) << "...\n";

                // image-to-image — uses base_image_path if set, collage otherwise
                return image_to_image(collage, img_prompt,
                                      base_copy, script_copy, entries_copy,
                                      base_image_path);
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

            launch_image_job(job_id, [=]() -> std::vector<uint8_t> {
                auto bytes = text_to_image(prompt_copy, full_path_copy);
                if (bytes.empty())
                    throw std::runtime_error("text_to_image returned empty result");
                std::cerr << "[IMG] Asset '" << asset_id << "' saved to: " << full_path_copy << "\n";
                return bytes;
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
        // Start server
        // -----------------------------------------------------------------
        int port = 8080;
        print_system("Web mode — http://localhost:" + std::to_string(port));
        print_system("Routes: GET /  /api/scripts  /api/saves  /api/status  /api/show_asset");
        print_system("        POST /api/start  /api/init  /api/load  /api/chat  /api/command  /api/save");
        print_system("        POST /api/image  /api/generate_asset  GET /api/image/job/<id>");
        if (cfg.imgEnabled)
            print_system("Image:  provider=" + cfg.imgProvider + " url=" + cfg.imgUrl);
        app.loglevel(crow::LogLevel::Warning);
        app.port(port).multithreaded().run();
    }

    curl_global_cleanup();
            return 1;
        }
   