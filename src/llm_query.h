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

    // Structured output — same schema format as OpenAI
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
