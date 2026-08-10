#include <map>
#include <mutex>
#include <string>

// Default per-request timeout for LLM calls (seconds). Configurable via
// --llm-timeout so a stalled provider can't freeze a player's turn for minutes.
// Image calls override this per-request (they may legitimately take longer).
inline long g_llm_timeout_s = 120;

// Cap on completion tokens for every text LLM call. Without it a degenerate
// generation (e.g. an endless run of whitespace/padding from a flaky model)
// streams to the provider's default max — minutes of latency, chat/log filled
// with "void", and a large completion-token bill. Configurable via
// --max-output-tokens. Claude paths already cap at 1024; this brings the
// OpenAI/OpenRouter paths in line. 0 = no cap (use provider default).
inline long g_llm_max_tokens = 1024;

// ── Token accounting (dev): per-component token usage ────────────────────────
// Every OpenAI-compatible response carries a "usage" object; we read it (no extra
// calls) and accumulate by LABEL so we can see how many tokens each component
// spends (narrator vs agent vs gen vs ambient). The label is a thread_local the
// caller sets before invoking (so async/worker-thread calls attribute correctly).
struct LlmTokenStat { long long prompt = 0, completion = 0, total = 0, calls = 0; };
inline std::map<std::string, LlmTokenStat> g_llm_token_usage;
inline std::mutex                          g_llm_token_mutex;
inline thread_local std::string            g_llm_label = "other";

// Read usage from an LLM response (OpenAI style prompt/completion_tokens, or
// Anthropic input/output_tokens) and add it to the current label's bucket.
static void record_llm_usage(const json& resp) {
    if (!resp.is_object() || !resp.contains("usage") || !resp["usage"].is_object())
        return;
    const auto& u = resp["usage"];
    long long p = u.value("prompt_tokens",     u.value("input_tokens",  0));
    long long c = u.value("completion_tokens", u.value("output_tokens", 0));
    long long t = u.value("total_tokens",      p + c);
    std::lock_guard<std::mutex> lk(g_llm_token_mutex);
    auto& s = g_llm_token_usage[g_llm_label];
    s.prompt += p; s.completion += c; s.total += t; s.calls += 1;
}

// curl_easy_init with CURLOPT_NOSIGNAL — required for safe multithreaded use and
// to prevent libcurl from touching SIGALRM/SIGPIPE on SIGINT.
static inline CURL* make_curl() {
    CURL* c = curl_easy_init();
    if (c) {
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        // Bound every request so a stalled endpoint can never hang a worker
        // (or a mutex-holding coder loop) forever.
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, g_llm_timeout_s);
    }
    return c;
}

// Gemini (Google AI Studio) rejects schemas that contain "additionalProperties".
// Strip it recursively before forwarding to any Google model via OpenRouter.
static void strip_additional_properties(json& node) {
    if (!node.is_object()) return;
    node.erase("additionalProperties");
    if (node.contains("properties") && node["properties"].is_object()) {
        for (auto& [k, v] : node["properties"].items()) {
            strip_additional_properties(v);
        }
    }
    for (const char* key : {"items", "then", "else", "not"}) {
        if (node.contains(key)) strip_additional_properties(node[key]);
    }
    for (const char* key : {"anyOf", "oneOf", "allOf"}) {
        if (node.contains(key) && node[key].is_array()) {
            for (auto& item : node[key]) strip_additional_properties(item);
        }
    }
}

// Anthropic (Claude) structured output rejects several advisory JSON-Schema
// constraints: numeric minimum/maximum, and array minItems/maxItems other than
// 0/1. They are non-structural validation hints, so stripping them only loosens
// validation (never breaks parsing). Recurses the whole tree, erasing the keys
// wherever they appear. Lets scripts keep these constraints for other providers.
static void strip_unsupported_constraints(json& node) {
    if (node.is_object()) {
        for (const char* k : {"minimum", "maximum", "exclusiveMinimum",
                              "exclusiveMaximum", "multipleOf",
                              "minItems", "maxItems"}) {
            node.erase(k);
        }
        for (auto& el : node.items()) strip_unsupported_constraints(el.value());
    } else if (node.is_array()) {
        for (auto& item : node) strip_unsupported_constraints(item);
    }
}

// Parse a JSON-Schema string and adapt it to the target model:
// Google rejects "additionalProperties"; Anthropic (incl. via OpenRouter, model
// strings like "anthropic/..." or containing "claude") rejects min/max and
// minItems/maxItems. Central place so every response_format site stays consistent.
static json schema_for_model(const std::string& schema_str, const std::string& model) {
    json s = json::parse(schema_str);
    if (model.rfind("google/", 0) == 0) strip_additional_properties(s);
    bool is_anthropic = model.find("claude")    != std::string::npos
                     || model.find("anthropic") != std::string::npos
                     || model.find("sonnet")    != std::string::npos
                     || model.find("opus")      != std::string::npos
                     || model.find("haiku")     != std::string::npos;
    if (is_anthropic) strip_unsupported_constraints(s);
    return s;
}

std::string ollama_query(const std::string& system,
                         const std::vector<Message>& history, 
                         const std::string& prompt,
                         const std::string& format,
                         const std::string& model) {
    
    ollama::setServerURL(ollama_baseUrl);  
    ollama::setReadTimeout(300);

    ollama::messages messages;

    // 1. System Prompt
    if (!system.empty()) {
        messages.push_back(ollama::message("system", system));
    }

    // 2. Chat History
    for (const auto& msg : history) {
        messages.push_back(ollama::message(msg.role, msg.content));
    }

    // 3. Current User Prompt
    if (!prompt.empty()) {
        messages.push_back(ollama::message("user", prompt));
    }

    std::string ret;
    try {
        ollama::request request(ollama::message_type::chat);

        request["model"] = model;
        request["messages"] = messages.to_json();
        request["stream"] = false;
        
        request["options"]["temperature"] = 0.1;
        if (g_llm_max_tokens > 0) request["options"]["num_predict"] = g_llm_max_tokens;

        if (!format.empty()) {
            // Using standard std::exception catch will also handle json::parse_error here
            request["format"] = json::parse(format); 
        }

        ret = ollama::chat(request);

    } catch (const std::exception& e) {
        // Catches BOTH ollama::exception and nlohmann::json exceptions
        std::cerr << "[OLLAMA ERROR] " << e.what() << "\n";
        return "{\"error\": \"Ollama request failed\"}";
    }

    return ret;
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string makePostRequest(const std::string& url, const json& body) {
    CURL* curl = make_curl();
    std::string readBuffer;

    if (curl) {
        std::string jsonStr = body.dump();
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "[CURL ERROR] " << curl_easy_strerror(res) << std::endl;
        }

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    }
    return readBuffer;
}

std::string gemini_query(const std::string& system, 
                         const std::vector<Message>& history, 
                         const std::string& prompt, 
                         const std::string& format,
                         const std::string& model) {
    
    std::string url = gemini_baseUrl + model + ":generateContent?key=" + gemini_key;
    json body;
    
    // 1. System instructions
    if (!system.empty()) {
        body["system_instruction"]["parts"]["text"] = system;
    }

    // 2. Chat history and current prompt
    json contents = json::array();
    for (const auto& msg : history) {
        std::string role = (msg.role == "assistant") ? "model" : msg.role;
        contents.push_back({
            {"role", role},
            {"parts", {{{"text", msg.content}}}}
        });
    }
    if (!prompt.empty()) {
        contents.push_back({
            {"role", "user"},
            {"parts", {{{"text", prompt}}}}
        });
    }
    body["contents"] = contents;
    if (g_llm_max_tokens > 0)
        body["generationConfig"]["maxOutputTokens"] = g_llm_max_tokens;

    // 3. Structured output (JSON Schema)
    if (!format.empty()) {
        body["generationConfig"]["response_mime_type"] = "application/json";
        try {
            body["generationConfig"]["response_schema"] = json::parse(format);
        } catch (const std::exception& e) {
            std::cerr << "[WARNING] Format string is not a valid JSON Schema: " << e.what() << "\n";
        }
    }

    // 4. Safety Settings
    body["safetySettings"] = json::array({
        {{"category", "HARM_CATEGORY_SEXUALLY_EXPLICIT"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_HATE_SPEECH"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_HARASSMENT"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_DANGEROUS_CONTENT"}, {"threshold", "BLOCK_NONE"}}
    });
    
    // 4. Perform the HTTP request
    std::string response = makePostRequest(url, body);
    
    // 5. Safely parse the response
    try {
        auto jRes = json::parse(response); record_llm_usage(jRes);
        
        // Handle successful response
        if (jRes.contains("candidates") && !jRes["candidates"].empty()) {
            auto& parts = jRes["candidates"][0]["content"]["parts"];
            if (!parts.empty() && parts[0].contains("text")) {
                return parts[0]["text"].get<std::string>();
            }
        } 
        // Handle API-level errors (e.g., quota exceeded, bad request)
        else if (jRes.contains("error")) {
            std::cerr << "[GEMINI API ERROR] " << jRes["error"].dump() << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[GEMINI PARSE ERROR] " << e.what() << "\nRaw Response: " << response << "\n";
        return "{\"error\": \"Parse failed\"}";
    }
    
    return response;
}

std::string openai_query(const std::string& system,
                         const std::vector<Message>& history,
                         const std::string& prompt,
                         const std::string& format,
                         const std::string& model) {
    json body;
    body["model"] = model;

    json messages = json::array();
    if (!system.empty()) {
        messages.push_back({{"role", "system"}, {"content", system}});
    }
    for (const auto& msg : history) {
        messages.push_back({{"role", msg.role}, {"content", msg.content}});
    }
    if (!prompt.empty()) {
        messages.push_back({{"role", "user"}, {"content", prompt}});
    }
    body["messages"] = messages;
    if (g_llm_max_tokens > 0) body["max_tokens"] = g_llm_max_tokens;

    if (!format.empty()) {
        try {
            body["response_format"] = {
                {"type", "json_schema"},
                {"json_schema", {
                    {"name", "response"},
                    {"strict", true},
                    {"schema", schema_for_model(format, model)}
                }}
            };
        } catch (const std::exception& e) {
            std::cerr << "[WARNING] OpenAI format is not a valid JSON Schema: " << e.what() << "\n";
        }
    }

    std::string readBuffer;
    CURL* curl = make_curl();
    
    if (curl) {
        std::string jsonStr = body.dump();
        struct curl_slist* headers = NULL;
        
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth = "Authorization: Bearer " + openai_api_key;
        headers = curl_slist_append(headers, auth.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, openai_baseUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "[CURL ERROR] " << curl_easy_strerror(res) << "\n";
        }

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    }

    try {
        if (!readBuffer.empty()) {
            auto jRes = json::parse(readBuffer); record_llm_usage(jRes);
            if (jRes.contains("choices") && !jRes["choices"].empty()) {
                return jRes["choices"][0]["message"]["content"].get<std::string>();
            } else if (jRes.contains("error")) {
                std::cerr << "[OPENAI API ERROR] " << jRes["error"].dump() << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[OPENAI PARSE ERROR] " << e.what() << "\nRaw Response: " << readBuffer << "\n";
        return "{\"error\": \"Parse failed\"}";
    }

    return readBuffer;
}

std::string claude_query(const std::string& system,
                         const std::vector<Message>& history,
                         const std::string& prompt,
                         const std::string& format,
                         const std::string& model) {
    json body;
    body["model"]      = model;
    body["max_tokens"] = (g_llm_max_tokens > 0 ? g_llm_max_tokens : 1024);

    // 1. System prompt — separate field, not inside the messages array
    if (!system.empty()) {
        body["system"] = system;
    }

    // 2. Chat history + current prompt
    // NOTE: Claude requires strict user/assistant alternation.
    // "system" role messages in history (e.g. human moves) must be
    // normalised to "user" to avoid a 400 error.
    json messages = json::array();
    for (const auto& msg : history) {
        std::string role = msg.role;
        if (role == "system") role = "user"; // normalise
        messages.push_back({{"role", role}, {"content", msg.content}});
    }
    if (!prompt.empty()) {
        messages.push_back({{"role", "user"}, {"content", prompt}});
    }
    body["messages"] = messages;

    // 3. Structured output (JSON Schema via beta header)
    if (!format.empty()) {
        try {
            body["output_format"] = {
                {"type", "json_schema"},
                {"schema", schema_for_model(format, model)}
            };
        } catch (const std::exception& e) {
            std::cerr << "[WARNING] Claude format is not a valid JSON Schema: " << e.what() << "\n";
        }
    }

    // 4. HTTP request — note different auth header vs OpenAI
    std::string readBuffer;
    CURL* curl = make_curl();

    if (curl) {
        std::string jsonStr = body.dump();
        struct curl_slist* headers = NULL;

        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth = "x-api-key: " + claude_api_key; // different from OpenAI Bearer token
        headers = curl_slist_append(headers, auth.c_str());
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
        headers = curl_slist_append(headers, "anthropic-beta: structured-outputs-2025-11-13");

        curl_easy_setopt(curl, CURLOPT_URL, claude_baseUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "[CURL ERROR] " << curl_easy_strerror(res) << "\n";
        }

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    }

    // 5. Parse response — different structure from OpenAI:
    //    Claude: response.content[0].text
    //    OpenAI: response.choices[0].message.content
    try {
        if (!readBuffer.empty()) {
            auto jRes = json::parse(readBuffer); record_llm_usage(jRes);
            if (jRes.contains("content") && !jRes["content"].empty()) {
                return jRes["content"][0]["text"].get<std::string>();
            } else if (jRes.contains("error")) {
                std::cerr << "[CLAUDE API ERROR] " << jRes["error"].dump() << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[CLAUDE PARSE ERROR] " << e.what() << "\nRaw Response: " << readBuffer << "\n";
        return "{\"error\": \"Parse failed\"}";
    }

    return readBuffer;
}

std::string openrouter_query(const std::string& system,
                             const std::vector<Message>& history,
                             const std::string& prompt,
                             const std::string& format,
                             const std::string& model) {
    json body;
    body["model"] = model; // e.g. "qwen/qwen3-32b", "google/gemini-flash-1.5"

    json messages = json::array();
    if (!system.empty()) {
        messages.push_back({{"role", "system"}, {"content", system}});
    }
    for (const auto& msg : history) {
        messages.push_back({{"role", msg.role}, {"content", msg.content}});
    }
    if (!prompt.empty()) {
        messages.push_back({{"role", "user"}, {"content", prompt}});
    }
    body["messages"] = messages;
    body["safe_prompt"] = false;
    if (g_llm_max_tokens > 0) body["max_tokens"] = g_llm_max_tokens;

    // Structured output — same schema format as OpenAI.
    // Google models via OpenRouter reject "additionalProperties", so strip it.
    if (!format.empty()) {
        try {
            bool is_google = (model.rfind("google/", 0) == 0);
            json schema = schema_for_model(format, model);
            body["response_format"] = {
                {"type", "json_schema"},
                {"json_schema", {
                    {"name", "response"},
                    {"strict", !is_google},
                    {"schema", schema}
                }}
            };
        } catch (const std::exception& e) {
            std::cerr << "[WARNING] OpenRouter format is not a valid JSON Schema: " << e.what() << "\n";
        }
    }

    std::string readBuffer;
    CURL* curl = make_curl();

    if (curl) {
        std::string jsonStr = body.dump();
        struct curl_slist* headers = NULL;

        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth = "Authorization: Bearer " + openrouter_api_key;
        headers = curl_slist_append(headers, auth.c_str());

        // Optional: identifies your app on openrouter.ai stats
        if (!openrouter_app_url.empty()) {
            std::string referer = "HTTP-Referer: " + openrouter_app_url;
            headers = curl_slist_append(headers, referer.c_str());
        }
        if (!openrouter_app_title.empty()) {
            std::string title = "X-Title: " + openrouter_app_title;
            headers = curl_slist_append(headers, title.c_str());
        }

headers = curl_slist_append(headers, "X-Safe-Prompt: false");

        curl_easy_setopt(curl, CURLOPT_URL, openrouter_baseUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "[CURL ERROR] " << curl_easy_strerror(res) << "\n";
        }

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    }

    // Response structure is identical to OpenAI
    try {
        if (!readBuffer.empty()) {
            auto jRes = json::parse(readBuffer); record_llm_usage(jRes);
            if (jRes.contains("choices") && !jRes["choices"].empty()) {
                return jRes["choices"][0]["message"]["content"].get<std::string>();
            } else if (jRes.contains("error")) {
                std::cerr << "[OPENROUTER API ERROR] " << jRes["error"].dump() << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[OPENROUTER PARSE ERROR] " << e.what() << "\nRaw Response: " << readBuffer << "\n";
        return "{\"error\": \"Parse failed\"}";
    }

    return readBuffer;
}

// ===========================================================================
// EMBEDDING ENGINE
//
// get_embedding() calls an HTTP endpoint and returns a vector of floats.
// Supports two formats:
//
//   "ollama"  → POST {ollama_baseUrl}/api/embeddings
//               body: {"model":"...", "prompt":"..."}
//               response: {"embedding": [...]}
//
//   "openai"  → POST {embedUrl}/v1/embeddings  (any compatible endpoint)
//               body: {"model":"...", "input":"..."}
//               response: {"data":[{"embedding":[...]}]}
//
// The function is thread-safe (no mutable global state).
// Returns an empty vector on error.
// ===========================================================================

// Accumulates HTTP response data into a string
static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// Executes a JSON POST request and returns the response body.
// headers is a list of "Key: Value" strings.
static std::string http_post(const std::string& url,
                              const std::string& body,
                              const std::vector<std::string>& headers) {
    CURL* curl = make_curl();
    if (!curl) return {};

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    struct curl_slist* hlist = nullptr;
    hlist = curl_slist_append(hlist, "Content-Type: application/json");
    for (const auto& h : headers)
        hlist = curl_slist_append(hlist, h.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

    curl_easy_perform(curl);
    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return response;
}

// Determines which embedding endpoint to use based on configuration.
// Priority: --embed-provider > main provider.
static std::string effective_embed_provider() {
    if (!cfg.embedProvider.empty()) return cfg.embedProvider;
    // OpenAI and OpenRouter use the /v1/embeddings compatible endpoint
    if (cfg.provider == AIProvider::OPENAI || cfg.provider == AIProvider::OPENROUTER)
        return "openai";
    return "ollama";
}

static std::vector<float> get_embedding(const std::string& text) {
    std::string ep = effective_embed_provider();

    try {
        if (ep == "ollama") {
            // Ollama endpoint: POST baseUrl/api/embeddings
            std::string model = cfg.embedModel.empty() ? cfg.ollama_model : cfg.embedModel;
            std::string url   = (cfg.embedUrl.empty() ? cfg.ollama_baseUrl : cfg.embedUrl)
                                + "/api/embeddings";
            json req = { {"model", model}, {"prompt", text} };
            std::string resp = http_post(url, req.dump(), {});
            auto j = json::parse(resp);
            std::vector<float> vec;
            for (auto& v : j.at("embedding")) vec.push_back(v.get<float>());
            return vec;

        } else {
            // OpenAI-compatible endpoint: POST baseUrl/v1/embeddings
            std::string model = cfg.embedModel.empty() ? "text-embedding-3-small" : cfg.embedModel;
            std::string base  = cfg.embedUrl.empty()
                                ? (cfg.provider == AIProvider::OPENROUTER
                                   ? cfg.openrouter_baseUrl
                                   : cfg.openai_baseUrl)
                                : cfg.embedUrl;
            // Normalize base URL: strip /chat/completions if present
            auto pos = base.rfind("/chat/completions");
            if (pos != std::string::npos) base = base.substr(0, pos);
            std::string url = base + "/embeddings";

            std::string key = cfg.embedKey.empty()
                              ? (cfg.provider == AIProvider::OPENROUTER
                                 ? cfg.openrouter_key : cfg.openai_key)
                              : cfg.embedKey;

            json req = { {"model", model}, {"input", text} };
            std::string resp = http_post(url, req.dump(),
                                         {"Authorization: Bearer " + key});
            auto j = json::parse(resp);
            std::vector<float> vec;
            for (auto& v : j.at("data").at(0).at("embedding"))
                vec.push_back(v.get<float>());
            return vec;
        }
    } catch (const std::exception& e) {
        std::cerr << "[EMBED] Error: " << e.what() << "\n";
        return {};
    }
}

// Cosine similarity between two vectors. Returns 0 if either is empty.
static float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0f;
    float dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    float denom = std::sqrt(na) * std::sqrt(nb);
    return denom > 0 ? dot / denom : 0.0f;
}

// ===========================================================================
// TOOL CALLING — OpenAI / OpenRouter
//
// Runs one complete tool-calling session. Sends messages + tools JSON to the
// API, executes any tool_calls returned by the model via the executor callback,
// appends tool results, and repeats until the model produces a terminal
// response (finish_reason != "tool_calls") or max_iter is reached.
//
// json_schema — if non-empty, also sent as response_format so the terminal
//               response is still structured JSON (compatible with existing
//               process_ai_response usage).
// ===========================================================================
static std::string openai_tool_loop(
        const std::string& base_url,
        const std::string& api_key,
        const std::string& model,
        const std::string& system,
        const std::vector<Message>& history,
        const std::string& user_prompt,
        const std::string& json_schema,
        const std::vector<ToolDef>& tools,
        std::function<std::string(const std::string&, const std::string&)> executor,
        int max_iter = 8)
{
    // Build tools JSON array
    json jtools = json::array();
    for (auto& td : tools) {
        json params;
        try { params = json::parse(td.params_schema); }
        catch (...) { params = {{"type","object"},{"properties",json::object()}}; }
        jtools.push_back({{"type","function"},{"function",{
            {"name",td.name},{"description",td.description},{"parameters",params}
        }}});
    }

    // Build initial messages array
    json messages = json::array();
    if (!system.empty())
        messages.push_back({{"role","system"},{"content",system}});
    for (auto& m : history)
        messages.push_back({{"role",m.role},{"content",m.content}});
    if (!user_prompt.empty())
        messages.push_back({{"role","user"},{"content",user_prompt}});

    // one_shot_call: single API call with given messages, no tools, tool_choice:none.
    // Returns content string, or "" on failure.
    auto one_shot_call = [&](const json& msgs) -> std::string {
        json rb;
        rb["model"]       = model;
        rb["messages"]    = msgs;
        rb["tool_choice"] = "none";
        if (g_llm_max_tokens > 0) rb["max_tokens"] = g_llm_max_tokens;
        if (!json_schema.empty()) {
            try {
                bool is_google = (model.rfind("google/", 0) == 0);
                json sc = schema_for_model(json_schema, model);
                rb["response_format"] = {{"type","json_schema"},{"json_schema",{
                    {"name","response"},{"strict",!is_google},{"schema",sc}
                }}};
            } catch (...) {}
        }
        std::string buf;
        CURL* c2 = make_curl();
        if (!c2) return "";
        std::string js = rb.dump();
        struct curl_slist* h2 = nullptr;
        h2 = curl_slist_append(h2, "Content-Type: application/json");
        if (!api_key.empty()) {
            std::string auth = "Authorization: Bearer " + api_key;
            h2 = curl_slist_append(h2, auth.c_str());
        }
        curl_easy_setopt(c2, CURLOPT_URL,           base_url.c_str());
        curl_easy_setopt(c2, CURLOPT_POSTFIELDS,    js.c_str());
        curl_easy_setopt(c2, CURLOPT_HTTPHEADER,    h2);
        curl_easy_setopt(c2, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(c2, CURLOPT_WRITEDATA,     &buf);
        CURLcode rc2 = curl_easy_perform(c2);
        curl_slist_free_all(h2);
        curl_easy_cleanup(c2);
        if (rc2 != CURLE_OK) return "";
        try {
            auto jr = json::parse(buf); record_llm_usage(jr);
            if (jr.contains("error")) return "";          // API error → signal failure
            if (jr.contains("choices") && !jr["choices"].empty()) {
                auto& m = jr["choices"][0]["message"];
                if (m.contains("content") && m["content"].is_string())
                    return m["content"].get<std::string>();
            }
        } catch (...) {}
        return "";
    };

    // rescue_call: 2-stage recovery when the tool loop exits early.
    // Stage 1: accumulated context + no tools (preserves what the LLM already processed).
    // Stage 2: fresh context  + no tools (guaranteed small payload).
    auto rescue_call = [&]() -> std::string {
        std::cerr << "[TOOL LOOP] rescue stage 1 — accumulated context, no tools\n";
        std::string r1 = one_shot_call(messages);
        if (!r1.empty()) return r1;
        std::cerr << "[TOOL LOOP] rescue stage 2 — fresh context, no tools\n";
        json fresh = json::array();
        if (!system.empty())
            fresh.push_back({{"role","system"},{"content",system}});
        for (auto& m : history)
            fresh.push_back({{"role",m.role},{"content",m.content}});
        if (!user_prompt.empty())
            fresh.push_back({{"role","user"},{"content",user_prompt +
                "\n\n[I tool sono stati eseguiti. Scrivi la narrazione adesso nel formato JSON richiesto.]"}});
        std::string r2 = one_shot_call(fresh);
        return r2.empty() ? "{\"error\":\"rescue failed\"}" : r2;
    };

    // The executor (tool calls into Lua, e.g. agents) may change g_llm_label;
    // re-pin it each iteration so the loop's own API calls stay attributed here.
    const std::string loop_label = g_llm_label;
    for (int iter = 0; iter < max_iter; ++iter) {
        g_llm_label = loop_label;
        bool force_final = (iter == max_iter - 1);
        if (force_final)
            std::cerr << "[TOOL LOOP] last iteration — forcing tool_choice:none\n";
        json body;
        body["model"]    = model;
        body["messages"] = messages;
        if (g_llm_max_tokens > 0) body["max_tokens"] = g_llm_max_tokens;
        if (!force_final) {
            body["tools"]       = jtools;
            body["tool_choice"] = "auto";
        } else {
            body["tool_choice"] = "none";
        }

        if (!json_schema.empty()) {
            try {
                bool is_google = (model.rfind("google/", 0) == 0);
                json schema = schema_for_model(json_schema, model);
                body["response_format"] = {{"type","json_schema"},{"json_schema",{
                    {"name","response"},{"strict",!is_google},{"schema",schema}
                }}};
            } catch (...) {}
        }

        std::string readBuffer;
        CURL* curl = make_curl();
        if (!curl) return "{\"error\":\"curl init failed\"}";

        std::string jsonStr = body.dump();
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
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

        if (rc != CURLE_OK) {
            std::cerr << "[TOOL LOOP CURL] " << curl_easy_strerror(rc) << "\n";
            return "{\"error\":\"curl failed\"}";
        }

        try {
            auto jRes = json::parse(readBuffer); record_llm_usage(jRes);
            if (jRes.contains("error")) {
                std::cerr << "[TOOL LOOP API ERROR] " << jRes["error"].dump() << "\n";
                return rescue_call();
            }
            if (!jRes.contains("choices") || jRes["choices"].empty())
                return rescue_call();

            auto& choice = jRes["choices"][0];
            auto& msg    = choice["message"];
            std::string finish = choice.value("finish_reason", "");

            // Append assistant turn for continuity
            messages.push_back(msg);

            // Some models (e.g. Ollama) return finish_reason="stop" but still include tool_calls
            if (msg.contains("tool_calls") && msg["tool_calls"].is_array() && !msg["tool_calls"].empty()) {
                for (auto& tc : msg["tool_calls"]) {
                    if (!tc.contains("function") || tc["function"].is_null()) continue;
                    auto& fn = tc["function"];
                    std::string tc_id   = tc.value("id", "");
                    std::string fn_name = fn.contains("name") && fn["name"].is_string()
                                         ? fn["name"].get<std::string>() : "";
                    std::string fn_args = fn.contains("arguments") && fn["arguments"].is_string()
                                         ? fn["arguments"].get<std::string>() : "{}";
                    std::cerr << "[TOOL] " << fn_name << "(" << fn_args << ")\n";
                    std::string result  = executor(fn_name, fn_args);
                    std::cerr << "[TOOL] → " << result << "\n";
                    messages.push_back({
                        {"role","tool"},{"tool_call_id",tc_id},{"content",result}
                    });
                }
                // Loop: ask LLM again with tool results
            } else {
                // Terminal response
                if (msg.contains("content") && msg["content"].is_string())
                    return msg["content"].get<std::string>();
                return "";
            }
        } catch (const std::exception& e) {
            std::cerr << "[TOOL LOOP PARSE] " << e.what() << "\n";
            return "{\"error\":\"parse failed\"}";
        }
    }
    std::cerr << "[TOOL LOOP] max iterations (" << max_iter << ") reached\n";
    return rescue_call();
}

// ===========================================================================
// TOOL CALLING — Anthropic Claude
//
// Same semantics as openai_tool_loop but uses Claude's tool_use / tool_result
// message format. json_schema is ignored here (Claude structured output and
// tool use are mutually exclusive in the same request).
// ===========================================================================
static std::string claude_tool_loop(
        const std::string& system,
        const std::vector<Message>& history,
        const std::string& user_prompt,
        const std::string& model,
        const std::vector<ToolDef>& tools,
        std::function<std::string(const std::string&, const std::string&)> executor,
        int max_iter = 8)
{
    json jtools = json::array();
    for (auto& td : tools) {
        json schema;
        try { schema = json::parse(td.params_schema); }
        catch (...) { schema = {{"type","object"},{"properties",json::object()}}; }
        jtools.push_back({
            {"name",td.name},{"description",td.description},{"input_schema",schema}
        });
    }

    json messages = json::array();
    for (auto& m : history) {
        std::string role = (m.role == "system") ? "user" : m.role;
        messages.push_back({{"role",role},{"content",m.content}});
    }
    if (!user_prompt.empty())
        messages.push_back({{"role","user"},{"content",user_prompt}});

    // Final-text rescue: one no-tools request to extract narration when the loop
    // exits without a terminal text response. Returns "" on failure.
    auto final_text_call = [&](const json& msgs) -> std::string {
        json body;
        body["model"]      = model;
        body["max_tokens"] = (g_llm_max_tokens > 0 ? g_llm_max_tokens : 1024);
        body["messages"]   = msgs;
        if (!system.empty()) body["system"] = system;
        std::string buf;
        CURL* c = make_curl();
        if (!c) return "";
        std::string js = body.dump();
        struct curl_slist* h = nullptr;
        h = curl_slist_append(h, "Content-Type: application/json");
        std::string auth = "x-api-key: " + claude_api_key;
        h = curl_slist_append(h, auth.c_str());
        h = curl_slist_append(h, "anthropic-version: 2023-06-01");
        curl_easy_setopt(c, CURLOPT_URL,           claude_baseUrl.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDS,    js.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER,    h);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(c, CURLOPT_WRITEDATA,     &buf);
        CURLcode rc = curl_easy_perform(c);
        curl_slist_free_all(h);
        curl_easy_cleanup(c);
        if (rc != CURLE_OK) return "";
        try {
            auto jr = json::parse(buf); record_llm_usage(jr);
            if (jr.contains("error")) return "";
            for (auto& block : jr["content"])
                if (block.value("type","") == "text") return block.value("text","");
        } catch (...) {}
        return "";
    };

    // The executor (tool calls into Lua, e.g. agents) may change g_llm_label;
    // re-pin it each iteration so the loop's own API calls stay attributed here.
    const std::string loop_label = g_llm_label;
    for (int iter = 0; iter < max_iter; ++iter) {
        g_llm_label = loop_label;
        bool force_final = (iter == max_iter - 1);
        if (force_final)
            std::cerr << "[CLAUDE TOOL LOOP] last iteration — forcing no-tools response\n";
        json body;
        body["model"]      = model;
        body["max_tokens"] = (g_llm_max_tokens > 0 ? g_llm_max_tokens : 1024);
        body["messages"]   = messages;
        if (!system.empty()) body["system"] = system;
        if (!force_final) body["tools"] = jtools;

        std::string readBuffer;
        CURL* curl = make_curl();
        if (!curl) return "{\"error\":\"curl init\"}";

        std::string jsonStr = body.dump();
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        std::string auth = "x-api-key: " + claude_api_key;
        hdrs = curl_slist_append(hdrs, auth.c_str());
        hdrs = curl_slist_append(hdrs, "anthropic-version: 2023-06-01");

        curl_easy_setopt(curl, CURLOPT_URL,           claude_baseUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    jsonStr.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &readBuffer);
        CURLcode rc = curl_easy_perform(curl);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) return "{\"error\":\"curl failed\"}";

        try {
            auto jRes = json::parse(readBuffer); record_llm_usage(jRes);
            if (jRes.contains("error")) {
                std::cerr << "[CLAUDE TOOL ERROR] " << jRes["error"].dump() << "\n";
                return "{\"error\":\"api error\"}";
            }
            std::string stop_reason = jRes.value("stop_reason", "");
            auto& content = jRes["content"];

            if (stop_reason == "tool_use") {
                messages.push_back({{"role","assistant"},{"content",content}});
                json tool_results = json::array();
                for (auto& block : content) {
                    if (block.value("type","") != "tool_use") continue;
                    std::string tool_id = block.value("id","");
                    std::string fn_name = block.value("name","");
                    std::string fn_args = block.contains("input") ? block["input"].dump() : "{}";
                    std::cerr << "[TOOL] " << fn_name << "(" << fn_args << ")\n";
                    std::string result  = executor(fn_name, fn_args);
                    std::cerr << "[TOOL] → " << result << "\n";
                    tool_results.push_back({
                        {"type","tool_result"},{"tool_use_id",tool_id},{"content",result}
                    });
                }
                messages.push_back({{"role","user"},{"content",tool_results}});
            } else {
                for (auto& block : content) {
                    if (block.value("type","") == "text")
                        return block.value("text","");
                }
                return "";
            }
        } catch (const std::exception& e) {
            std::cerr << "[CLAUDE TOOL PARSE] " << e.what() << "\n";
            return "{\"error\":\"parse failed\"}";
        }
    }
    std::cerr << "[CLAUDE TOOL LOOP] max iterations reached — final-text rescue\n";
    {
        std::string r = final_text_call(messages);
        if (!r.empty()) return r;
    }
    return "{\"error\":\"max tool iterations reached\"}";
}
