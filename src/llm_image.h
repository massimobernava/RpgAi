// =============================================================================
//  llm_image.h  —  Image generation for RpgAi
//
//  Header-only dependencies (place in the same directory):
//    stb_image.h        https://github.com/nothings/stb
//    stb_image_write.h  https://github.com/nothings/stb
//    stb_image_resize2.h https://github.com/nothings/stb
//
//  Define in exactly one .cpp before including:
//    #define STB_IMAGE_IMPLEMENTATION
//    #define STB_IMAGE_WRITE_IMPLEMENTATION
//    #define STB_IMAGE_RESIZE_IMPLEMENTATION
//  Or add image_impl.cpp to the project (see bottom of file).
//
//  Supported providers:
//    SDCPP_LOCAL   stable-diffusion.cpp server  (native async)
//    OPENAI_IMAGE  DALL-E / GPT-image-1         (sync, multipart for edits)
//    OPENROUTER_IMG openrouter.ai image models  (sync, JSON)
//
//  Exposed operations:
//    build_collage(entries)                    → PNG bytes
//    text_to_image(prompt)                     → PNG bytes
//    image_to_image(collage_bytes, prompt)     → PNG bytes   (async for SDCPP)
//    save_image(bytes, path)                   → bool
//    bytes_to_base64(bytes)                    → string
//    base64_to_bytes(string)                   → bytes
// =============================================================================

#pragma once

#include <string>
#include <vector>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <ctime>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

// stb headers — require the implementation defines (see above)
#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"

using nlohmann::json;

// =============================================================================
// Image provider configuration — mirrors the Config struct in main.cpp
// =============================================================================

enum class ImageProvider {
    SDCPP_LOCAL,     // stable-diffusion.cpp local server (t2i + i2i)
    OPENAI_IMAGE,    // OpenAI DALL-E / GPT-image-1
    OPENROUTER_IMG,  // OpenRouter image models
    FAL_AI,          // fal.ai — supporta Qwen-Image-Edit-2511 e altri
    DASHSCOPE,       // Alibaba Cloud DashScope — modelli Qwen-Image nativi
    AIMLAPI,         // AI/ML API — wrapper OpenAI-compatible per Qwen-Image-Edit
    WAVESPEED,       // wavespeed.ai — Qwen-Image-Edit-2511, polling asincrono
};

// These fields are read from cfg in main.cpp — same global variable aliases
extern std::string& openai_api_key;
extern std::string& openrouter_api_key;

// Image configuration — populated by parse_args via CLI flags:
//   --img-provider  sdcpp_local|openai|openrouter
//   --img-url       http://localhost:7860
//   --img-key       <api key>  (for cloud providers, otherwise uses openai/or key)
//   --img-t2i-model <text-to-image model>
//   --img-i2i-model <image-to-image / editing model>
//   --img-width     output width  (default 1024)
//   --img-height    output height (default 1024)
//   --img-steps     sampling steps (default 28)
//   --img-strength  denoising strength for i2i (default 0.75)
struct ImageConfig {
    ImageProvider provider     = ImageProvider::SDCPP_LOCAL;
    std::string   providerName = "sdcpp_local";
    std::string   url          = "http://localhost:7860";
    std::string   key;            // API key — empty means use the main LLM provider key
    std::string   t2i_model;      // text-to-image model
    std::string   i2i_model;      // image-to-image / editing model

    // Separate provider for i2i (if different from t2i)
    // If i2i_provider_name is empty, uses the same provider as t2i.
    // Allows e.g.: t2i=openrouter (flux), i2i=fal_ai (qwen-edit-2511)
    ImageProvider i2i_provider   = ImageProvider::SDCPP_LOCAL;
    std::string   i2i_provider_name;   // "" = follows main provider
    std::string   i2i_url;             // i2i server URL (if different from url)
    std::string   i2i_key;             // i2i API key (if different from key)

    int           width          = 1024;
    int           height         = 1024;
    int           steps          = 28;
    float         strength       = 0.75f;
    int           poll_interval_ms = 2000;   // ms tra un poll e l'altro per SDCPP
    int           poll_timeout_s   = 120;    // timeout totale generazione
};

static ImageConfig img_cfg;

// =============================================================================
// Collage entry structure
// =============================================================================

struct CollageEntry {
    std::string path;   // absolute or relative path to the image file
    std::string tag;    // e.g. "environment: kitchen", "character: Elena"
};

// =============================================================================
// Helpers interni
// =============================================================================

namespace img_detail {

// Curl callback to accumulate the response
static size_t write_cb(void* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// Curl callback to accumulate binary bytes
static size_t write_bytes_cb(void* ptr, size_t size, size_t nmemb, std::vector<uint8_t>* out) {
    auto* p = static_cast<uint8_t*>(ptr);
    out->insert(out->end(), p, p + size * nmemb);
    return size * nmemb;
}

// Performs an HTTP POST with JSON body, returns response body
static std::string http_post_json(const std::string& url,
                                   const std::string& body,
                                   const std::string& auth_header = "") {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("[IMG] curl_easy_init failed");

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!auth_header.empty())
        headers = curl_slist_append(headers, auth_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("[IMG] curl error: ") + curl_easy_strerror(rc));
    return response;
}

// Performs an HTTP GET, returns response body
static std::string http_get(const std::string& url,
                              const std::string& auth_header = "") {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("[IMG] curl_easy_init failed");

    std::string response;
    struct curl_slist* headers = nullptr;
    if (!auth_header.empty())
        headers = curl_slist_append(headers, auth_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       30L);

    CURLcode rc = curl_easy_perform(curl);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("[IMG] curl GET error: ") + curl_easy_strerror(rc));
    return response;
}

// Multipart POST per OpenAI /v1/images/edits
static std::string http_post_multipart(const std::string& url,
                                        const std::string& auth_header,
                                        const std::string& prompt,
                                        const std::vector<uint8_t>& image_bytes,
                                        const std::string& model,
                                        int n = 1,
                                        const std::string& size_str = "1024x1024") {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("[IMG] curl_easy_init failed");

    curl_mime* form   = curl_mime_init(curl);
    curl_mimepart* p;

    // prompt
    p = curl_mime_addpart(form);
    curl_mime_name(p, "prompt");
    curl_mime_data(p, prompt.c_str(), CURL_ZERO_TERMINATED);

    // image (PNG bytes)
    p = curl_mime_addpart(form);
    curl_mime_name(p, "image[]");
    curl_mime_filename(p, "collage.png");
    curl_mime_type(p, "image/png");
    curl_mime_data(p, reinterpret_cast<const char*>(image_bytes.data()), image_bytes.size());

    // n
    p = curl_mime_addpart(form);
    curl_mime_name(p, "n");
    curl_mime_data(p, std::to_string(n).c_str(), CURL_ZERO_TERMINATED);

    // size
    p = curl_mime_addpart(form);
    curl_mime_name(p, "size");
    curl_mime_data(p, size_str.c_str(), CURL_ZERO_TERMINATED);

    // model (opzionale)
    if (!model.empty()) {
        p = curl_mime_addpart(form);
        curl_mime_name(p, "model");
        curl_mime_data(p, model.c_str(), CURL_ZERO_TERMINATED);
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, auth_header.c_str());

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST,      form);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       120L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_mime_free(form);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("[IMG] multipart error: ") + curl_easy_strerror(rc));
    return response;
}

// Base64 alphabet
static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace img_detail

// =============================================================================
// bytes_to_base64 / base64_to_bytes
// =============================================================================

inline std::string bytes_to_base64(const std::vector<uint8_t>& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t b = (uint32_t)in[i] << 16;
        if (i + 1 < in.size()) b |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in.size()) b |= (uint32_t)in[i + 2];
        out += img_detail::B64_TABLE[(b >> 18) & 0x3F];
        out += img_detail::B64_TABLE[(b >> 12) & 0x3F];
        out += (i + 1 < in.size()) ? img_detail::B64_TABLE[(b >> 6) & 0x3F] : '=';
        out += (i + 2 < in.size()) ? img_detail::B64_TABLE[b & 0x3F]        : '=';
    }
    return out;
}

inline std::vector<uint8_t> base64_to_bytes(const std::string& in) {
    static const int8_t DEC[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        int v = DEC[c];
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((buf >> bits) & 0xFF);
        }
    }
    return out;
}

// =============================================================================
// save_image  —  writes bytes to disk
// =============================================================================

inline bool save_image(const std::vector<uint8_t>& bytes, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return f.good();
}

// =============================================================================
// scene_cache — directory layout, lightweight hash, JSON database, cache lookup
// =============================================================================

namespace scene_cache {

// ---------------------------------------------------------------------------
// Lightweight hash — 64-bit integer-only implementation (no OpenSSL).
// Sufficient for the cache key; not cryptographically critical.
// ---------------------------------------------------------------------------

// Note: to avoid an OpenSSL dependency we use an FNV-1a 64-bit hash
// based on the string content. Simple, no external dependencies.
inline std::string hash_key(const std::string& data) {
    // FNV-1a 64-bit, then hex
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : data) {
        h ^= (uint64_t)c;
        h *= 1099511628211ULL;
    }
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << h;
    return ss.str();
}

// ---------------------------------------------------------------------------
// Returns the most recent modification timestamp among asset files.
// Returns 0 if a file does not exist.
// ---------------------------------------------------------------------------
inline std::time_t max_mtime(const std::vector<CollageEntry>& entries) {
    std::time_t latest = 0;
    for (const auto& e : entries) {
        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(e.path, ec);
        if (ec) continue;
        // Convert to time_t — C++17 compatible
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - std::filesystem::file_time_type::clock::now()
            + std::chrono::system_clock::now());
        std::time_t t = std::chrono::system_clock::to_time_t(sctp);
        if (t > latest) latest = t;
    }
    return latest;
}

// ---------------------------------------------------------------------------
// Computes the cache key:
//   hash( script_name + "|" + sorted_asset_ids + "|" + max_mtime )
// ---------------------------------------------------------------------------
inline std::string make_cache_key(const std::string& script,
                                   const std::vector<CollageEntry>& entries,
                                   std::time_t mtime) {
    // Sort asset ids for determinism
    std::vector<std::string> ids;
    for (const auto& e : entries) ids.push_back(e.tag);
    std::sort(ids.begin(), ids.end());

    std::ostringstream ss;
    ss << script << "|";
    for (const auto& id : ids) ss << id << ",";
    ss << "|" << mtime;

    return hash_key(ss.str());
}

// ---------------------------------------------------------------------------
// Current timestamp as string "YYYYMMDD_HHMMSS"
// ---------------------------------------------------------------------------
inline std::string timestamp_str() {
    auto now   = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");
    return ss.str();
}

// ---------------------------------------------------------------------------
// Describes a single cache entry
// ---------------------------------------------------------------------------
struct CacheEntry {
    std::string cache_key;
    std::string script;
    std::vector<std::string> assets;  // asset ids
    std::string prompt;
    std::string image_path;           // risultato finale
    std::string collage_path;         // collage temporaneo
    std::string generated_at;
};

// ---------------------------------------------------------------------------
// Percorsi delle directory — basate su base_path (stessa dir dello script Lua)
// ---------------------------------------------------------------------------
inline std::string scene_dir(const std::string& base_path) {
    return base_path + "images/scene_cache/";
}
inline std::string collage_dir(const std::string& base_path) {
    return base_path + "images/collage_tmp/";
}
inline std::string db_path(const std::string& base_path) {
    return scene_dir(base_path) + "cache_db.json";
}

// ---------------------------------------------------------------------------
// Crea le directory se non esistono
// ---------------------------------------------------------------------------
inline void ensure_dirs(const std::string& base_path) {
    std::filesystem::create_directories(scene_dir(base_path));
    std::filesystem::create_directories(collage_dir(base_path));
}

// ---------------------------------------------------------------------------
// Loads the JSON database from the cache directory.
// Returns an empty JSON array if the file does not exist.
// ---------------------------------------------------------------------------
inline json load_db(const std::string& base_path) {
    std::string path = db_path(base_path);
    if (!std::filesystem::exists(path)) return json::array();
    std::ifstream f(path);
    if (!f.is_open()) return json::array();
    try {
        json j; f >> j;
        return j.is_array() ? j : json::array();
    } catch (...) {
        return json::array();
    }
}

// ---------------------------------------------------------------------------
// Writes the JSON database to disk.
// ---------------------------------------------------------------------------
inline void save_db(const std::string& base_path, const json& db) {
    std::ofstream f(db_path(base_path));
    if (f.is_open()) f << db.dump(2);
}

// ---------------------------------------------------------------------------
// Cerca nella cache una voce con la stessa chiave.
// Restituisce il path dell'immagine se trovata E il file esiste ancora su disco.
// Altrimenti restituisce stringa vuota.
// ---------------------------------------------------------------------------
inline std::string lookup(const std::string& base_path,
                           const std::string& cache_key) {
    json db = load_db(base_path);
    for (const auto& entry : db) {
        if (entry.value("cache_key", "") == cache_key) {
            std::string img = entry.value("image_path", "");
            if (!img.empty() && std::filesystem::exists(img))
                return img;
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// Finds the most recent cached image that matches the given asset ids,
// regardless of the cache key timestamp.
// Returns the image path if found and the file still exists on disk.
// Returns empty string otherwise.
// Used when base_image="last": reuse the previous render as i2i base
// instead of rebuilding the collage from scratch.
// ---------------------------------------------------------------------------
inline std::string lookup_last(const std::string& base_path,
                                const std::string& script,
                                const std::vector<CollageEntry>& entries) {
    json db = load_db(base_path);
    if (db.empty()) return "";

    // Build sorted asset id set for comparison
    std::vector<std::string> want_ids;
    for (const auto& e : entries) want_ids.push_back(e.tag);
    std::sort(want_ids.begin(), want_ids.end());

    // Walk entries in reverse (last inserted = most recent)
    for (int i = (int)db.size() - 1; i >= 0; --i) {
        const auto& entry = db[i];
        if (entry.value("script", "") != script) continue;

        std::vector<std::string> have_ids;
        if (entry.contains("assets") && entry["assets"].is_array())
            for (const auto& a : entry["assets"])
                have_ids.push_back(a.get<std::string>());
        std::sort(have_ids.begin(), have_ids.end());

        if (have_ids != want_ids) continue;

        std::string img = entry.value("image_path", "");
        if (!img.empty() && std::filesystem::exists(img))
            return img;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Inserts or updates an entry in the database.
// ---------------------------------------------------------------------------
inline void upsert(const std::string& base_path, const CacheEntry& ce) {
    json db = load_db(base_path);

    // Remove any existing entries with the same key
    json new_db = json::array();
    for (const auto& entry : db) {
        if (entry.value("cache_key", "") != ce.cache_key)
            new_db.push_back(entry);
    }

    json item;
    item["cache_key"]    = ce.cache_key;
    item["script"]       = ce.script;
    item["assets"]       = ce.assets;
    item["prompt"]       = ce.prompt;
    item["image_path"]   = ce.image_path;
    item["collage_path"] = ce.collage_path;
    item["generated_at"] = ce.generated_at;
    new_db.push_back(item);

    save_db(base_path, new_db);
}

// ---------------------------------------------------------------------------
// Saves the collage to the temporary directory and returns its path
// ---------------------------------------------------------------------------
inline std::string save_collage(const std::string& base_path,
                                  const std::vector<uint8_t>& bytes,
                                  const std::string& ts) {
    std::string path = collage_dir(base_path) + ts + "_collage.png";
    save_image(bytes, path);
    return path;
}

// ---------------------------------------------------------------------------
// Saves the final scene image to the cache directory and returns its path
// ---------------------------------------------------------------------------
inline std::string save_result(const std::string& base_path,
                                 const std::vector<uint8_t>& bytes,
                                 const std::string& ts) {
    // Use .jpg because WaveSpeed returns jpeg
    std::string path = scene_dir(base_path) + ts + "_scene.jpg";
    save_image(bytes, path);
    return path;
}

} // namespace scene_cache

// =============================================================================
// build_collage
//
// Loads images from entries, resizes them to the same height (collage_h)
// preserving aspect ratio, lays them out horizontally with a small gap,
// encodes the result as PNG and returns it as bytes.
//
// The collage order mirrors the entries order — callers are responsible
// for placing the background first.
//
// Missing or unreadable files are skipped with a stderr warning.
// Returns an empty vector if no image could be loaded.
// =============================================================================

inline std::vector<uint8_t> build_collage(const std::vector<CollageEntry>& entries,
                                           int collage_h = 768,
                                           int gap_px    = 8) {
    if (entries.empty()) return {};

    // Holds data for each loaded and resized image
    struct LoadedImg {
        std::vector<uint8_t> data;  // pixel RGB, row-major
        int w, h;
        std::string tag;
    };

    std::vector<LoadedImg> loaded;
    int total_w = 0;

    for (const auto& entry : entries) {
        int orig_w, orig_h, channels;
        unsigned char* raw = stbi_load(entry.path.c_str(), &orig_w, &orig_h, &channels, 3);
        if (!raw) {
            std::cerr << "[IMG] Cannot load image: " << entry.path
                      << " (" << stbi_failure_reason() << ")\n";
            continue;
        }

        // Compute proportional width for the target height
        int new_w = static_cast<int>(orig_w * (float)collage_h / orig_h);
        if (new_w < 1) new_w = 1;

        std::vector<uint8_t> resized(new_w * collage_h * 3);
        stbir_resize_uint8_linear(
            raw,     orig_w, orig_h, 0,
            resized.data(), new_w, collage_h, 0,
            STBIR_RGB);

        stbi_image_free(raw);

        total_w += new_w + gap_px;
        loaded.push_back({ std::move(resized), new_w, collage_h, entry.tag });
    }

    if (loaded.empty()) return {};
    total_w -= gap_px;  // remove the trailing gap

    // Compose the RGB canvas
    std::vector<uint8_t> canvas(total_w * collage_h * 3, 30);  // near-black background

    int x_offset = 0;
    for (const auto& img : loaded) {
        for (int row = 0; row < collage_h; ++row) {
            const uint8_t* src = img.data.data() + row * img.w * 3;
            uint8_t*       dst = canvas.data() + (row * total_w + x_offset) * 3;
            std::memcpy(dst, src, img.w * 3);
        }
        x_offset += img.w + gap_px;
    }

    // Codifica PNG in memoria
    std::vector<uint8_t> png_bytes;
    auto write_fn = [](void* ctx, void* data, int size) {
        auto* vec = static_cast<std::vector<uint8_t>*>(ctx);
        uint8_t* p = static_cast<uint8_t*>(data);
        vec->insert(vec->end(), p, p + size);
    };

    int ok = stbi_write_png_to_func(write_fn, &png_bytes,
                                     total_w, collage_h, 3,
                                     canvas.data(), total_w * 3);
    if (!ok) {
        std::cerr << "[IMG] PNG encoding failed\n";
        return {};
    }

    std::cerr << "[IMG] Collage built: " << total_w << "x" << collage_h
              << " px, " << loaded.size() << "/" << entries.size()
              << " images, " << png_bytes.size() << " bytes\n";
    return png_bytes;
}

// =============================================================================
// Implementazioni provider
// =============================================================================

// ---------------------------------------------------------------------------
// SDCPP_LOCAL — stable-diffusion.cpp server
//
// text-to-image: POST /sdapi/v1/txt2img  (sincrono, risponde con base64)
// image-to-image: POST /sdcpp/v1/img_gen (asincrono, polling su /jobs/{id})
// ---------------------------------------------------------------------------

namespace sdcpp {

inline std::vector<uint8_t> txt2img(const std::string& prompt) {
    json req;
    req["prompt"]   = prompt;
    req["width"]    = img_cfg.width;
    req["height"]   = img_cfg.height;
    req["steps"]    = img_cfg.steps;
    req["seed"]     = -1;
    req["batch_size"] = 1;
    if (!img_cfg.t2i_model.empty())
        req["override_settings"]["sd_model_checkpoint"] = img_cfg.t2i_model;

    std::string resp = img_detail::http_post_json(
        img_cfg.url + "/sdapi/v1/txt2img", req.dump());

    auto j = json::parse(resp);
    if (!j.contains("images") || j["images"].empty())
        throw std::runtime_error("[IMG] sdcpp txt2img: no images in response");

    return base64_to_bytes(j["images"][0].get<std::string>());
}

inline std::vector<uint8_t> img2img(const std::vector<uint8_t>& collage_bytes,
                                     const std::string& prompt) {
    // Uses the native async API /sdcpp/v1/img_gen
    std::string b64 = bytes_to_base64(collage_bytes);

    json req;
    req["prompt"]       = prompt;
    req["width"]        = img_cfg.width;
    req["height"]       = img_cfg.height;
    req["strength"]     = img_cfg.strength;
    req["seed"]         = -1;
    req["init_image"]   = b64;
    req["batch_count"]  = 1;
    req["output_format"] = "png";
    req["sample_params"]["sample_steps"] = img_cfg.steps;
    if (!img_cfg.i2i_model.empty())
        req["model"] = img_cfg.i2i_model;

    // Submit the job
    std::string submit_resp = img_detail::http_post_json(
        img_cfg.url + "/sdcpp/v1/img_gen", req.dump());

    auto jsubmit = json::parse(submit_resp);
    if (!jsubmit.contains("id"))
        throw std::runtime_error("[IMG] sdcpp img2img: no job id in response: " + submit_resp);

    std::string job_id   = jsubmit["id"].get<std::string>();
    std::string poll_url = img_cfg.url + "/sdcpp/v1/jobs/" + job_id;

    std::cerr << "[IMG] SDCPP job submitted: " << job_id << "\n";

    // Polling loop
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(img_cfg.poll_timeout_s);

    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(img_cfg.poll_interval_ms));

        std::string poll_resp = img_detail::http_get(poll_url);
        auto jpoll = json::parse(poll_resp);

        std::string status = jpoll.value("status", "unknown");
        std::cerr << "[IMG] job " << job_id << " status: " << status << "\n";

        if (status == "completed") {
            if (!jpoll.contains("result") || !jpoll["result"].contains("images") ||
                jpoll["result"]["images"].empty())
                throw std::runtime_error("[IMG] sdcpp: completed but no images in result");
            std::string b64_result = jpoll["result"]["images"][0]["b64_json"].get<std::string>();
            return base64_to_bytes(b64_result);
        }
        if (status == "failed") {
            std::string msg = jpoll.contains("error")
                ? jpoll["error"].value("message", "unknown error")
                : "unknown error";
            throw std::runtime_error("[IMG] sdcpp job failed: " + msg);
        }
        if (status == "cancelled") {
            throw std::runtime_error("[IMG] sdcpp job was cancelled");
        }
        // queued / generating → continue polling
    }

    throw std::runtime_error("[IMG] sdcpp job timed out after "
                             + std::to_string(img_cfg.poll_timeout_s) + "s");
}

} // namespace sdcpp

// ---------------------------------------------------------------------------
// OPENAI_IMAGE — DALL-E 3 / GPT-image-1
//
// text-to-image: POST /v1/images/generations  (JSON, responds with b64_json)
// image-to-image: POST /v1/images/edits       (multipart)
// ---------------------------------------------------------------------------

namespace openai_img {

static std::string auth_header() {
    const std::string& key = img_cfg.key.empty() ? openai_api_key : img_cfg.key;
    return "Authorization: Bearer " + key;
}

inline std::vector<uint8_t> txt2img(const std::string& prompt) {
    json req;
    req["prompt"]           = prompt;
    req["n"]                = 1;
    req["size"]             = std::to_string(img_cfg.width) + "x"
                              + std::to_string(img_cfg.height);
    req["response_format"]  = "b64_json";
    if (!img_cfg.t2i_model.empty()) req["model"] = img_cfg.t2i_model;

    std::string resp = img_detail::http_post_json(
        img_cfg.url + "/v1/images/generations", req.dump(), auth_header());

    auto j = json::parse(resp);
    if (!j.contains("data") || j["data"].empty())
        throw std::runtime_error("[IMG] openai txt2img: no data in response");

    return base64_to_bytes(j["data"][0]["b64_json"].get<std::string>());
}

inline std::vector<uint8_t> img2img(const std::vector<uint8_t>& collage_bytes,
                                     const std::string& prompt) {
    std::string size_str = std::to_string(img_cfg.width) + "x"
                         + std::to_string(img_cfg.height);
    std::string model = img_cfg.i2i_model.empty() ? "" : img_cfg.i2i_model;

    std::string resp = img_detail::http_post_multipart(
        img_cfg.url + "/v1/images/edits",
        auth_header(), prompt, collage_bytes, model, 1, size_str);

    auto j = json::parse(resp);
    if (!j.contains("data") || j["data"].empty())
        throw std::runtime_error("[IMG] openai img2img: no data in response: " + resp);

    return base64_to_bytes(j["data"][0]["b64_json"].get<std::string>());
}

} // namespace openai_img

// ---------------------------------------------------------------------------
// OPENROUTER_IMG — OpenRouter image models
//
// Usa lo stesso schema OpenAI ma con base URL openrouter.ai.
// text-to-image: POST /v1/images/generations
// image-to-image: POST /v1/images/edits  (multipart)
// ---------------------------------------------------------------------------

namespace openrouter_img {

// ---------------------------------------------------------------------------
// OpenRouter usa POST /v1/chat/completions con "modalities": ["image"]
// Response is in choices[0].message.images[].image_url.url  (base64 data URL)
// Stesso endpoint per txt2img e img2img — la differenza è nel contenuto
// del messaggio: solo testo per txt2img, testo + immagine base64 per img2img.
// ---------------------------------------------------------------------------

static std::string auth_header() {
    const std::string& key = img_cfg.key.empty() ? openrouter_api_key : img_cfg.key;
    return "Authorization: Bearer " + key;
}

// Extracts PNG bytes from the OpenRouter response.
// Response has: choices[0].message.images[0].image_url.url
// which is a data URL like "data:image/png;base64,iVBOR..."
static std::vector<uint8_t> extract_image(const std::string& resp) {
    auto j = json::parse(resp);

    // Check for explicit errors
    if (j.contains("error")) {
        std::string msg = j["error"].is_object()
            ? j["error"].value("message", resp)
            : j["error"].get<std::string>();
        throw std::runtime_error("[IMG] OpenRouter error: " + msg);
    }

    if (!j.contains("choices") || j["choices"].empty())
        throw std::runtime_error("[IMG] OpenRouter: no choices in response. Body: "
                                 + resp.substr(0, 200));

    auto& msg = j["choices"][0]["message"];
    if (!msg.contains("images") || msg["images"].empty())
        throw std::runtime_error("[IMG] OpenRouter: no images in message. Body: "
                                 + resp.substr(0, 200));

    std::string data_url = msg["images"][0]["image_url"]["url"].get<std::string>();

    // Strip the "data:image/...;base64," prefix
    auto comma = data_url.find(',');
    if (comma == std::string::npos)
        throw std::runtime_error("[IMG] OpenRouter: malformed data URL");

    return base64_to_bytes(data_url.substr(comma + 1));
}

inline std::vector<uint8_t> txt2img(const std::string& prompt) {
    std::string model = img_cfg.t2i_model.empty()
        ? "black-forest-labs/flux-1.1-pro"   // sensible default for txt2img
        : img_cfg.t2i_model;

    json req;
    req["model"]      = model;
    req["modalities"] = json::array({"image"});
    req["messages"]   = json::array({
        {{"role", "user"}, {"content", prompt}}
    });

    std::string resp = img_detail::http_post_json(
        "https://openrouter.ai/api/v1/chat/completions",
        req.dump(), auth_header());

    return extract_image(resp);
}

inline std::vector<uint8_t> img2img(const std::vector<uint8_t>& collage_bytes,
                                     const std::string& prompt) {
    std::string model = img_cfg.i2i_model.empty()
        ? "qwen/qwen2.5-vl-72b-instruct"    // default for image editing
        : img_cfg.i2i_model;

    // The collage is passed as an inline image in the user message
    std::string b64     = bytes_to_base64(collage_bytes);
    std::string img_url = "data:image/png;base64," + b64;

    // Message with image + text in OpenRouter vision format
    json content = json::array({
        {
            {"type", "image_url"},
            {"image_url", {{"url", img_url}}}
        },
        {
            {"type", "text"},
            {"text", prompt}
        }
    });

    json req;
    req["model"]      = model;
    req["modalities"] = json::array({"image"});
    req["messages"]   = json::array({
        {{"role", "user"}, {"content", content}}
    });

    std::string resp = img_detail::http_post_json(
        "https://openrouter.ai/api/v1/chat/completions",
        req.dump(), auth_header());

    return extract_image(resp);
}

} // namespace openrouter_img

// ---------------------------------------------------------------------------
// FAL_AI — fal.ai serverless
//
// Supporta Qwen-Image-Edit-2511 e altri modelli di editing.
// API REST semplice: POST con JSON, risponde con images[].url (data URL o HTTP URL)
//
// Endpoint: https://fal.run/fal-ai/<model>
// Autenticazione: "Authorization: Key <fal_key>"
// ---------------------------------------------------------------------------

namespace fal_ai {

static std::string auth_header() {
    const std::string& key = img_cfg.i2i_key.empty()
        ? (img_cfg.key.empty() ? openrouter_api_key : img_cfg.key)
        : img_cfg.i2i_key;
    return "Authorization: Key " + key;
}

// fal.ai supports image upload via public URL or base64 data URL.
// Pass the collage as a data URL directly in the image_url field.
inline std::vector<uint8_t> img2img(const std::vector<uint8_t>& collage_bytes,
                                     const std::string& prompt) {
    std::string model = img_cfg.i2i_model.empty()
        ? "fal-ai/qwen-image-edit-2511"
        : img_cfg.i2i_model;

    std::string base_url = img_cfg.i2i_url.empty()
        ? "https://fal.run"
        : img_cfg.i2i_url;

    std::string url = base_url + "/" + model;

    std::string b64      = bytes_to_base64(collage_bytes);
    std::string data_url = "data:image/png;base64," + b64;

    json req;
    req["prompt"]              = prompt;
    req["image_url"]           = data_url;
    req["num_inference_steps"] = img_cfg.steps;
    req["guidance_scale"]      = 4.5;

    std::string resp = img_detail::http_post_json(url, req.dump(), auth_header());
    auto j = json::parse(resp);

    if (j.contains("detail")) {
        std::string msg = j["detail"].is_string()
            ? j["detail"].get<std::string>()
            : j["detail"].dump();
        throw std::runtime_error("[IMG] fal.ai error: " + msg);
    }
    if (!j.contains("images") || j["images"].empty())
        throw std::runtime_error("[IMG] fal.ai: no images in response. Body: "
                                 + resp.substr(0, 300));

    std::string img_url = j["images"][0]["url"].get<std::string>();

    // If it is a data URL, decode directly
    if (img_url.substr(0, 5) == "data:") {
        auto comma = img_url.find(',');
        if (comma == std::string::npos)
            throw std::runtime_error("[IMG] fal.ai: malformed data URL");
        return base64_to_bytes(img_url.substr(comma + 1));
    }

    // Otherwise it is an HTTP URL — download the bytes
    std::vector<uint8_t> result;
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("[IMG] curl_easy_init failed");
    curl_easy_setopt(curl, CURLOPT_URL,           img_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, img_detail::write_bytes_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &result);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("[IMG] fal.ai download error: ")
                                 + curl_easy_strerror(rc));
    return result;
}

} // namespace fal_ai

// ---------------------------------------------------------------------------
// DASHSCOPE — Alibaba Cloud DashScope
//
// Endpoint (Singapore): https://dashscope-intl.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation
// Supports both t2i and i2i with the same endpoint.
// Input images are passed as base64 in the content array.
// Response contains output.choices[0].message.content with images.
// ---------------------------------------------------------------------------

namespace dashscope {

static std::string auth_header() {
    const std::string& key = img_cfg.i2i_key.empty()
        ? (img_cfg.key.empty() ? openai_api_key : img_cfg.key)
        : img_cfg.i2i_key;
    return "Authorization: Bearer " + key;
}

static const std::string DS_URL =
    "https://dashscope-intl.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";

static std::vector<uint8_t> extract_dashscope_image(const std::string& resp) {
    auto j = json::parse(resp);
    if (j.contains("code") && j["code"] != "Success") {
        std::string msg = j.value("message", resp.substr(0, 200));
        throw std::runtime_error("[IMG] DashScope error: " + msg);
    }
    // output.choices[0].message.content is an array of {image: "base64..."} objects
    auto& choices = j["output"]["choices"];
    if (choices.empty())
        throw std::runtime_error("[IMG] DashScope: no choices. Body: " + resp.substr(0, 300));
    auto& content_arr = choices[0]["message"]["content"];
    for (auto& item : content_arr) {
        if (item.contains("image")) {
            std::string b64 = item["image"].get<std::string>();
            // May be a data URL or raw base64
            auto comma = b64.find(',');
            if (comma != std::string::npos) b64 = b64.substr(comma + 1);
            return base64_to_bytes(b64);
        }
        if (item.contains("image_url")) {
            std::string url = item["image_url"]["url"].get<std::string>();
            auto comma = url.find(',');
            if (comma != std::string::npos)
                return base64_to_bytes(url.substr(comma + 1));
            // HTTP URL — download
            std::vector<uint8_t> result;
            CURL* curl = curl_easy_init();
            curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  img_detail::write_bytes_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &result);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);
            CURLcode rc = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            if (rc != CURLE_OK)
                throw std::runtime_error("[IMG] DashScope download error: "
                                         + std::string(curl_easy_strerror(rc)));
            return result;
        }
    }
    throw std::runtime_error("[IMG] DashScope: no image in content. Body: " + resp.substr(0, 300));
}

inline std::vector<uint8_t> txt2img(const std::string& prompt) {
    std::string model = img_cfg.t2i_model.empty() ? "qwen-image-plus" : img_cfg.t2i_model;
    std::string ds_url = img_cfg.url.empty() ? DS_URL : img_cfg.url;

    json req;
    req["model"] = model;
    req["input"]["messages"] = json::array({
        {{"role","user"}, {"content", json::array({
            {{"text", prompt}}
        })}}
    });
    req["parameters"]["size"] = std::to_string(img_cfg.width) + "*"
                                + std::to_string(img_cfg.height);

    std::string resp = img_detail::http_post_json(ds_url, req.dump(), auth_header());
    return extract_dashscope_image(resp);
}

inline std::vector<uint8_t> img2img(const std::vector<uint8_t>& collage_bytes,
                                     const std::string& prompt) {
    std::string model = img_cfg.i2i_model.empty()
        ? "qwen-image-edit-plus" : img_cfg.i2i_model;
    std::string ds_url = img_cfg.i2i_url.empty()
        ? (img_cfg.url.empty() ? DS_URL : img_cfg.url)
        : img_cfg.i2i_url;

    std::string b64      = bytes_to_base64(collage_bytes);
    std::string data_url = "data:image/png;base64," + b64;

    json req;
    req["model"] = model;
    req["input"]["messages"] = json::array({
        {{"role","user"}, {"content", json::array({
            {{"image", data_url}},
            {{"text",  prompt}}
        })}}
    });
    req["parameters"]["watermark"]      = false;
    req["parameters"]["prompt_extend"]  = true;
    req["parameters"]["size"] = std::to_string(img_cfg.width) + "*"
                                + std::to_string(img_cfg.height);

    std::string resp = img_detail::http_post_json(ds_url, req.dump(), auth_header());
    return extract_dashscope_image(resp);
}

} // namespace dashscope

// ---------------------------------------------------------------------------
// AIMLAPI — AI/ML API (OpenAI-compatible wrapper for models like Qwen-Image-Edit)
// Endpoint: https://api.aimlapi.com/v1/images/generations
// Formato: {"model":"alibaba/qwen-image-edit","prompt":"...","image":"<base64>"}
// Response: {"data":[{"url":"https://..."}]}
// ---------------------------------------------------------------------------

namespace aimlapi {

static std::string auth_header() {
    const std::string& key = img_cfg.i2i_key.empty()
        ? (img_cfg.key.empty() ? openai_api_key : img_cfg.key)
        : img_cfg.i2i_key;
    return "Authorization: Bearer " + key;
}

inline std::vector<uint8_t> txt2img(const std::string& prompt) {
    std::string model = img_cfg.t2i_model.empty()
        ? "black-forest-labs/flux-1.1-pro" : img_cfg.t2i_model;

    json req;
    req["model"]  = model;
    req["prompt"] = prompt;

    std::string resp = img_detail::http_post_json(
        "https://api.aimlapi.com/v1/images/generations", req.dump(), auth_header());

    auto j = json::parse(resp);
    if (!j.contains("data") || j["data"].empty())
        throw std::runtime_error("[IMG] aimlapi txt2img: no data. Body: " + resp.substr(0, 200));

    std::string url = j["data"][0]["url"].get<std::string>();
    // data URL → decode, HTTP URL → download
    if (url.substr(0, 5) == "data:") {
        auto comma = url.find(',');
        return base64_to_bytes(url.substr(comma + 1));
    }
    std::vector<uint8_t> result;
    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  img_detail::write_bytes_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &result);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("[IMG] aimlapi download: ") + curl_easy_strerror(rc));
    return result;
}

inline std::vector<uint8_t> img2img(const std::vector<uint8_t>& collage_bytes,
                                     const std::string& prompt) {
    std::string model = img_cfg.i2i_model.empty()
        ? "alibaba/qwen-image-edit" : img_cfg.i2i_model;

    std::string b64 = bytes_to_base64(collage_bytes);

    json req;
    req["model"]  = model;
    req["prompt"] = prompt;
    req["image"]  = b64;   // raw base64 or data URL — both accepted

    std::string resp = img_detail::http_post_json(
        "https://api.aimlapi.com/v1/images/generations", req.dump(), auth_header());

    auto j = json::parse(resp);
    if (!j.contains("data") || j["data"].empty())
        throw std::runtime_error("[IMG] aimlapi img2img: no data. Body: " + resp.substr(0, 200));

    std::string url = j["data"][0]["url"].get<std::string>();
    if (url.substr(0, 5) == "data:") {
        auto comma = url.find(',');
        return base64_to_bytes(url.substr(comma + 1));
    }
    std::vector<uint8_t> result;
    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  img_detail::write_bytes_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &result);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("[IMG] aimlapi download: ") + curl_easy_strerror(rc));
    return result;
}

} // namespace aimlapi

// ---------------------------------------------------------------------------
// WAVESPEED — wavespeed.ai
//
// Async API with polling — two steps:
//   1. POST /api/v3/<model>  → { "data": { "id": "..." } }
//   2. GET  /api/v3/predictions/{id}/result  → poll finché status=completed
//
// Authentication: "Authorization: Bearer <key>"
// The input image is sent as base64 in the "image" field.
// Final response has: data.outputs[0] which is base64 or HTTP URL.
// ---------------------------------------------------------------------------

namespace wavespeed {

static const std::string WS_BASE = "https://api.wavespeed.ai";

static std::string auth_header() {
    const std::string& key = img_cfg.i2i_key.empty()
        ? (img_cfg.key.empty() ? openai_api_key : img_cfg.key)
        : img_cfg.i2i_key;
    return "Authorization: Bearer " + key;
}

// Downloads an HTTP URL as bytes via curl
static std::vector<uint8_t> download_url(const std::string& url) {
    std::vector<uint8_t> result;
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("[IMG] wavespeed: curl_easy_init failed");
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  img_detail::write_bytes_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &result);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("[IMG] wavespeed download: ")
                                 + curl_easy_strerror(rc));
    return result;
}

// Polls the result endpoint — waits up to 120s at 3s intervals
static std::vector<uint8_t> poll_result(const std::string& prediction_id) {
    const std::string poll_url = WS_BASE + "/api/v3/predictions/"
                                 + prediction_id + "/result";
    const int max_attempts = 40;  // 40 × 3s = 120s

    for (int i = 0; i < max_attempts; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(3));

        std::string resp = img_detail::http_get(poll_url, auth_header());
        auto j = json::parse(resp);

        // Check for errors
        if (j.contains("error") && !j["error"].is_null()) {
            std::string msg = j["error"].is_string()
                ? j["error"].get<std::string>()
                : j["error"].dump();
            throw std::runtime_error("[IMG] WaveSpeed error: " + msg);
        }

        auto& data = j["data"];
        std::string status = data.value("status", "");

        if (status == "failed") {
            std::string reason = data.value("error", "unknown");
            throw std::runtime_error("[IMG] WaveSpeed failed: " + reason);
        }

        if (status == "completed") {
            auto& outputs = data["outputs"];
            if (outputs.empty())
                throw std::runtime_error("[IMG] WaveSpeed: no outputs");

            std::string out = outputs[0].get<std::string>();

            // If it is a data URL
            if (out.substr(0, 5) == "data:") {
                auto comma = out.find(',');
                if (comma == std::string::npos)
                    throw std::runtime_error("[IMG] WaveSpeed: malformed data URL");
                return base64_to_bytes(out.substr(comma + 1));
            }
            // If it is raw base64 (no prefix)
            if (out.find("http") != 0) {
                return base64_to_bytes(out);
            }
            // Otherwise it is an HTTP URL — download
            return download_url(out);
        }
        // status pending/processing — continue polling
    }
    throw std::runtime_error("[IMG] WaveSpeed: timeout after 120s");
}

inline std::vector<uint8_t> img2img(const std::vector<uint8_t>& collage_bytes,
                                     const std::string& prompt) {
    std::string model = img_cfg.i2i_model.empty()
        ? "wavespeed-ai/qwen-image/edit-2511"
        : img_cfg.i2i_model;

    // Strip any leading slash or duplicate "wavespeed-ai/" prefix
    if (!model.empty() && model[0] == '/') model = model.substr(1);

    std::string url = WS_BASE + "/api/v3/" + model;

    std::string b64 = bytes_to_base64(collage_bytes);

    json req;
    req["prompt"]               = prompt;
    req["images"]               = json::array({b64});   // WaveSpeed expects an array
    req["seed"]                 = -1;
    req["output_format"]        = "jpeg";
    req["enable_base64_output"] = true;
    req["enable_sync_mode"]     = false;

    std::string resp = img_detail::http_post_json(url, req.dump(), auth_header());
    auto j = json::parse(resp);

    if (j.contains("error") && !j["error"].is_null()) {
        std::string msg = j["error"].is_string()
            ? j["error"].get<std::string>()
            : j["error"].dump();
        throw std::runtime_error("[IMG] WaveSpeed submit error: " + msg);
    }

    // Extract prediction ID
    std::string pred_id;
    if (j.contains("data") && j["data"].contains("id")) {
        pred_id = j["data"]["id"].get<std::string>();
    } else {
        throw std::runtime_error("[IMG] WaveSpeed: no prediction id in response: "
                                 + resp.substr(0, 200));
    }

    std::cerr << "[IMG] WaveSpeed job started: " << pred_id << "\n";
    return poll_result(pred_id);
}

} // namespace wavespeed

// =============================================================================
// Public API — text_to_image and image_to_image
// Dispatch based on img_cfg.provider
// =============================================================================

// Generates an image from text. Used to create missing NPC/location images.
// Saves the result to disk_path if non-empty.
inline std::vector<uint8_t> text_to_image(const std::string& prompt,
                                           const std::string& disk_path = "") {
    std::vector<uint8_t> result;
    switch (img_cfg.provider) {
        case ImageProvider::OPENAI_IMAGE:    result = openai_img::txt2img(prompt);     break;
        case ImageProvider::OPENROUTER_IMG:  result = openrouter_img::txt2img(prompt); break;
        case ImageProvider::DASHSCOPE:       result = dashscope::txt2img(prompt);      break;
        case ImageProvider::AIMLAPI:         result = aimlapi::txt2img(prompt);        break;
        // FAL_AI has no native t2i model — fallback to local sdcpp
        default:                             result = sdcpp::txt2img(prompt);          break;
    }
    if (!disk_path.empty() && !result.empty())
        save_image(result, disk_path);
    return result;
}

// Generates the scene image with cache management.
//
// Optional cache parameters:
//   base_path    — base directory of the Lua script (e.g. "../scripts/")
//   script_name  — Lua script filename (e.g. "magic_daze_v2.lua")
//   entries      — asset list used for the cache key and collage
//
// Behaviour:
//   1. Computes the cache key (script + asset ids + max mtime)
//   2. Cache hit (file still on disk) → returns cached bytes without regenerating
//   3. Cache miss → generate → save temp collage → save final result
//      → update cache_db.json → return bytes
inline std::vector<uint8_t> image_to_image(const std::vector<uint8_t>& collage_bytes,
                                            const std::string& prompt,
                                            const std::string& base_path        = "",
                                            const std::string& script_name      = "",
                                            const std::vector<CollageEntry>& entries = {},
                                            const std::string& base_image_path  = "") {
    // --- Cache lookup ---
    std::string cache_key;
    if (!base_path.empty() && !script_name.empty() && !entries.empty()) {
        scene_cache::ensure_dirs(base_path);
        std::time_t mtime = scene_cache::max_mtime(entries);
        cache_key = scene_cache::make_cache_key(script_name, entries, mtime);

        std::string cached = scene_cache::lookup(base_path, cache_key);
        if (!cached.empty()) {
            std::cerr << "[IMG] Cache hit: " << cached << "\n";
            std::ifstream f(cached, std::ios::binary);
            return std::vector<uint8_t>(
                std::istreambuf_iterator<char>(f), {});
        }
        std::cerr << "[IMG] Cache miss, generating...\n";
    }

    // --- Generation ---
    // If base_image_path points to an existing file, load it and use it as the
    // i2i source instead of the collage. collage_bytes remain the fallback.
    std::vector<uint8_t> i2i_source = collage_bytes;
    if (!base_image_path.empty() && std::filesystem::exists(base_image_path)) {
        std::ifstream bf(base_image_path, std::ios::binary);
        std::vector<uint8_t> loaded(
            std::istreambuf_iterator<char>(bf), {});
        if (!loaded.empty()) {
            i2i_source = std::move(loaded);
            std::cerr << "[IMG] Using cached image as i2i base: "
                      << base_image_path << "\n";
        }
    }

    ImageProvider p = img_cfg.i2i_provider_name.empty()
        ? img_cfg.provider
        : img_cfg.i2i_provider;

    std::vector<uint8_t> result;
    switch (p) {
        case ImageProvider::OPENAI_IMAGE:    result = openai_img::img2img(i2i_source, prompt);     break;
        case ImageProvider::OPENROUTER_IMG:  result = openrouter_img::img2img(i2i_source, prompt); break;
        case ImageProvider::FAL_AI:          result = fal_ai::img2img(i2i_source, prompt);          break;
        case ImageProvider::DASHSCOPE:       result = dashscope::img2img(i2i_source, prompt);       break;
        case ImageProvider::AIMLAPI:         result = aimlapi::img2img(i2i_source, prompt);         break;
        case ImageProvider::WAVESPEED:       result = wavespeed::img2img(i2i_source, prompt);       break;
        default:                             result = sdcpp::img2img(i2i_source, prompt);            break;
    }

    // --- Save result + update database ---
    if (!base_path.empty() && !cache_key.empty() && !result.empty()) {
        std::string ts          = scene_cache::timestamp_str();
        std::string coll_path   = scene_cache::save_collage(base_path, collage_bytes, ts);
        std::string result_path = scene_cache::save_result(base_path, result, ts);

        scene_cache::CacheEntry ce;
        ce.cache_key    = cache_key;
        ce.script       = script_name;
        ce.prompt       = prompt;
        ce.image_path   = result_path;
        ce.collage_path = coll_path;
        ce.generated_at = ts;
        for (const auto& e : entries) ce.assets.push_back(e.tag);
        scene_cache::upsert(base_path, ce);

        std::cerr << "[IMG] Scene saved: " << result_path << "\n";
    }

    return result;
}

// =============================================================================
// CLI parsing — add to parse_args in main.cpp
//
//   else if (arg == "--img-provider") { ... }
//   else if (arg == "--img-url")      { ... }
//   ...
// =============================================================================

inline ImageProvider img_provider_from_string(const std::string& s) {
    if (s == "openai")     return ImageProvider::OPENAI_IMAGE;
    if (s == "openrouter") return ImageProvider::OPENROUTER_IMG;
    if (s == "fal")        return ImageProvider::FAL_AI;
    if (s == "fal_ai")     return ImageProvider::FAL_AI;
    if (s == "dashscope")  return ImageProvider::DASHSCOPE;
    if (s == "aimlapi")    return ImageProvider::AIMLAPI;
    if (s == "wavespeed")  return ImageProvider::WAVESPEED;
    return ImageProvider::SDCPP_LOCAL;
}

// Call this from print_help to display image options
inline void print_image_help() {
    auto opt = [](const std::string& flag, const std::string& desc, const std::string& def = "") {
        std::string line = "  " + flag;
        if (line.size() < 36) line += std::string(36 - line.size(), ' ');
        line += desc;
        if (!def.empty()) line += " (default: " + def + ")";
        std::cout << line << "\n";
    };
    std::cout << "\nIMAGE GENERATION\n";
    opt("--img-provider <n>",    "sdcpp_local|openai|openrouter|fal|dashscope|aimlapi", "sdcpp_local");
    opt("--img-i2i-provider <n>",  "dedicated i2i provider (if different from --img-provider)", "");
    opt("--img-url <url>",       "local image server URL",         img_cfg.url);
    opt("--img-key <key>",       "API key (default: LLM provider key)", "");
    opt("--img-t2i-model <n>",   "text-to-image model",            "");
    opt("--img-i2i-model <n>",   "image-to-image/editing model",   "");
    opt("--img-width <n>",       "output image width",             "1024");
    opt("--img-height <n>",      "output image height",            "1024");
    opt("--img-steps <n>",       "sampling steps",                 "28");
    opt("--img-strength <f>",    "denoising strength (i2i)",       "0.75");
    opt("--img-i2i-url <url>",  "i2i server URL (if different from --img-url)",  "");
    opt("--img-i2i-key <key>",  "i2i API key (if different from --img-key)",     "");
}

// =============================================================================
// image_impl.cpp — create this file in the project with these lines:
//
//   #define STB_IMAGE_IMPLEMENTATION
//   #define STB_IMAGE_WRITE_IMPLEMENTATION
//   #define STB_IMAGE_RESIZE_IMPLEMENTATION
//   #include "stb_image.h"
//   #include "stb_image_write.h"
//   #include "stb_image_resize2.h"
//
// Add image_impl.cpp to the target sources in CMakeLists.txt.
// =============================================================================
