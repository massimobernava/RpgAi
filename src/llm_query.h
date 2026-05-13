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
    CURL* curl = curl_easy_init();
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
        auto jRes = json::parse(response);
        
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

    if (!format.empty()) {
        try {
            body["response_format"] = {
                {"type", "json_schema"},
                {"json_schema", {
                    {"name", "response"},
                    {"strict", true},
                    {"schema", json::parse(format)}
                }}
            };
        } catch (const std::exception& e) {
            std::cerr << "[WARNING] OpenAI format is not a valid JSON Schema: " << e.what() << "\n";
        }
    }

    std::string readBuffer;
    CURL* curl = curl_easy_init();
    
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
            auto jRes = json::parse(readBuffer);
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
    body["max_tokens"] = 1024;

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
                {"schema", json::parse(format)}
            };
        } catch (const std::exception& e) {
            std::cerr << "[WARNING] Claude format is not a valid JSON Schema: " << e.what() << "\n";
        }
    }

    // 4. HTTP request — note different auth header vs OpenAI
    std::string readBuffer;
    CURL* curl = curl_easy_init();

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
            auto jRes = json::parse(readBuffer);
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

    // Structured output — same schema format as OpenAI.
    // Google models via OpenRouter reject "additionalProperties", so strip it.
    if (!format.empty()) {
        try {
            json schema = json::parse(format);
            bool is_google = (model.rfind("google/", 0) == 0);
            if (is_google) strip_additional_properties(schema);
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
    CURL* curl = curl_easy_init();

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
            auto jRes = json::parse(readBuffer);
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
    CURL* curl = curl_easy_init();
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

    for (int iter = 0; iter < max_iter; ++iter) {
        json body;
        body["model"]    = model;
        body["messages"] = messages;
        body["tools"]    = jtools;
        body["tool_choice"] = "auto";

        if (!json_schema.empty()) {
            try {
                bool is_google = (model.rfind("google/", 0) == 0);
                json schema = json::parse(json_schema);
                if (is_google) strip_additional_properties(schema);
                body["response_format"] = {{"type","json_schema"},{"json_schema",{
                    {"name","response"},{"strict",!is_google},{"schema",schema}
                }}};
            } catch (...) {}
        }

        std::string readBuffer;
        CURL* curl = curl_easy_init();
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
            auto jRes = json::parse(readBuffer);
            if (jRes.contains("error")) {
                std::cerr << "[TOOL LOOP API ERROR] " << jRes["error"].dump() << "\n";
                return "{\"error\":\"api error\"}";
            }
            if (!jRes.contains("choices") || jRes["choices"].empty())
                return "{\"error\":\"no choices\"}";

            auto& choice = jRes["choices"][0];
            auto& msg    = choice["message"];
            std::string finish = choice.value("finish_reason", "");

            // Append assistant turn for continuity
            messages.push_back(msg);

            if (finish == "tool_calls" && msg.contains("tool_calls")) {
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
    return "{\"error\":\"max tool iterations reached\"}";
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

    for (int iter = 0; iter < max_iter; ++iter) {
        json body;
        body["model"]      = model;
        body["max_tokens"] = 1024;
        body["tools"]      = jtools;
        body["messages"]   = messages;
        if (!system.empty()) body["system"] = system;

        std::string readBuffer;
        CURL* curl = curl_easy_init();
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
            auto jRes = json::parse(readBuffer);
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
    std::cerr << "[CLAUDE TOOL LOOP] max iterations reached\n";
    return "{\"error\":\"max tool iterations reached\"}";
}
