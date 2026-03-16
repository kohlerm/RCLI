// =============================================================================
// RCLI Proxy Mode — Unix Socket Server Implementation (macOS/Linux)
// =============================================================================

#include "cmd_proxy.h"
#include "../api/rcli_api.h"
#include "../engines/vad_engine.h"
#include "../engines/stt_engine.h"
#include "../engines/tts_engine.h"
#include "../audio/audio_io.h"
#include "../core/log.h"
#include "cli_common.h"
#include "help.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/event.h>  // kqueue on macOS
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <condition_variable>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <chrono>

// Base64 decoder (no external deps)
namespace base64 {
    static const unsigned char table[256] = {
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
        52,53,54,55,56,57,58,59,60,61,64,64,64,65,64,64,
        64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
        64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64
    };

    std::vector<unsigned char> decode(const std::string& input) {
        size_t len = input.size();
        if (len == 0) return {};
        size_t padding = 0;
        if (len >= 1 && input[len - 1] == '=') padding++;
        if (len >= 2 && input[len - 2] == '=') padding++;
        size_t out_len = (len / 4) * 3 - padding;
        std::vector<unsigned char> out(out_len);
        size_t j = 0;
        uint32_t buf = 0;
        int bits = 0;
        for (size_t i = 0; i < len; ++i) {
            unsigned char c = table[(unsigned char)input[i]];
            if (c == 64) continue;
            if (c == 65) break; // '=' padding
            buf = (buf << 6) | c;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (j < out_len) out[j++] = (buf >> bits) & 0xFF;
            }
        }
        out.resize(j);
        return out;
    }

    std::string encode(const unsigned char* data, size_t len) {
        static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((len + 2) / 3) * 4);
        for (size_t i = 0; i < len; i += 3) {
            uint32_t n = static_cast<uint32_t>(data[i]) << 16;
            if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
            if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
            out.push_back(tbl[(n >> 18) & 0x3F]);
            out.push_back(tbl[(n >> 12) & 0x3F]);
            out.push_back((i + 1 < len) ? tbl[(n >> 6) & 0x3F] : '=');
            out.push_back((i + 2 < len) ? tbl[n & 0x3F] : '=');
        }
        return out;
    }
}

// Simple JSON builder (no external deps)
namespace json {
    std::string escape(const std::string& s) {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\b': result += "\\b"; break;
                case '\f': result += "\\f"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c;
            }
        }
        return result;
    }
    
    std::string object(const std::vector<std::pair<std::string, std::string>>& pairs) {
        std::string result = "{";
        for (size_t i = 0; i < pairs.size(); ++i) {
            if (i > 0) result += ",";
            result += "\"" + pairs[i].first + "\":\"" + pairs[i].second + "\"";
        }
        result += "}";
        return result;
    }
    
    std::string object_with_bool(const std::vector<std::pair<std::string, std::string>>& pairs,
                                  const std::vector<std::pair<std::string, bool>>& bools) {
        std::string result = "{";
        bool first = true;
        
        for (const auto& p : pairs) {
            if (!first) result += ",";
            first = false;
            result += "\"" + p.first + "\":\"" + p.second + "\"";
        }
        
        for (const auto& p : bools) {
            if (!first) result += ",";
            first = false;
            result += "\"" + p.first + "\":" + (p.second ? "true" : "false");
        }
        
        result += "}";
        return result;
    }
}

namespace rcli {

// =============================================================================
// Client Implementation
// =============================================================================

class ProxyClientImpl : public ProxyClient {
public:
    ProxyClientImpl(int fd, class ProxyServerImpl* server) : fd_(fd), server_(server), connected_(true) {}
    
    ~ProxyClientImpl() {
        close();
    }
    
    int fd() const override { return fd_; }
    
    void send(const std::string& json_line) override {
        if (!connected_) return;
        
        std::string msg = json_line + "\n";
        ssize_t sent = ::send(fd_, msg.c_str(), msg.length(), 0);  // No MSG_NOSIGNAL on macOS
        if (sent < 0) {
            if (errno == EPIPE || errno == ECONNRESET) {
                connected_ = false;
            }
        }
    }
    
    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
            connected_ = false;
        }
    }
    
    bool is_connected() const override {
        return connected_ && fd_ >= 0;
    }
    
    void on_message(const std::string& line);
    
private:
    int fd_;
    class ProxyServerImpl* server_;
    std::atomic<bool> connected_;
    std::string read_buffer_;
    
    friend class ProxyServerImpl;
};

// =============================================================================
// Server Implementation
// =============================================================================

class ProxyServerImpl : public ProxyServer {
public:
    ProxyServerImpl(const ProxyConfig& config) 
        : config_(config), running_(false), server_fd_(-1), kqueue_fd_(-1), engine_(nullptr) {}
    
    ~ProxyServerImpl() {
        stop();
    }
    
    int run() override {
        // Expand ~ in socket path
        std::string socket_path = config_.socket_path;
        if (socket_path[0] == '~') {
            const char* home = getenv("HOME");
            if (home) {
                socket_path = std::string(home) + socket_path.substr(1);
            }
        }
        
        // Remove existing socket file
        unlink(socket_path.c_str());
        
        // Create server socket
        server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            perror("socket");
            return 1;
        }
        
        // Set non-blocking
        int flags = fcntl(server_fd_, F_GETFL, 0);
        fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);
        
        // Bind
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
        
        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind");
            close(server_fd_);
            return 1;
        }
        
        // Listen
        if (listen(server_fd_, 10) < 0) {
            perror("listen");
            close(server_fd_);
            return 1;
        }
        
        printf("RCLI Proxy listening on %s\n", socket_path.c_str());
        fflush(stdout);
        
        // Create kqueue
        kqueue_fd_ = kqueue();
        if (kqueue_fd_ < 0) {
            perror("kqueue");
            close(server_fd_);
            return 1;
        }
        
        // Add server socket to kqueue
        struct kevent ev;
        EV_SET(&ev, server_fd_, EVFILT_READ, EV_ADD, 0, 0, nullptr);
        if (kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr) < 0) {
            perror("kevent (server)");
            close(server_fd_);
            return 1;
        }
        
        // Initialize RCLI engine for proxy mode (STT + TTS only, no LLM)
        if (!init_engine_proxy()) {
            fprintf(stderr, "Failed to initialize RCLI engine for proxy mode\n");
            close(server_fd_);
            return 1;
        }
        
        running_ = true;
        
        // Main event loop
        struct kevent events[64];
        while (running_) {
            struct timespec timeout = {0, 100000000}; // 100ms
            int nfds = kevent(kqueue_fd_, nullptr, 0, events, 64, &timeout);
            
            if (nfds < 0) {
                if (errno == EINTR) continue;
                perror("kevent");
                break;
            }
            
            for (int i = 0; i < nfds; ++i) {
                if (events[i].ident == (uintptr_t)server_fd_) {
                    // New connection
                    accept_client();
                } else {
                    // Client data
                    auto it = clients_.find(events[i].ident);
                    if (it != clients_.end()) {
                        if (events[i].flags & EV_EOF) {
                            // Client disconnected
                            it->second->connected_ = false;
                        } else {
                            handle_client_data(it->second.get());
                        }
                    }
                }
            }
            
            // Clean up disconnected clients
            cleanup_clients();
        }
        
        stop();
        return 0;
    }
    
    void stop() override {
        running_ = false;
        
        // Close all clients
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (auto& [fd, client] : clients_) {
                client->close();
            }
            clients_.clear();
        }
        
        // Close server socket
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
        
        // Close kqueue
        if (kqueue_fd_ >= 0) {
            close(kqueue_fd_);
            kqueue_fd_ = -1;
        }
        
        // Cleanup engine
        if (engine_) {
            rcli_destroy(engine_);
            engine_ = nullptr;
        }
        
        // Remove socket file
        std::string socket_path = config_.socket_path;
        if (socket_path[0] == '~') {
            const char* home = getenv("HOME");
            if (home) {
                socket_path = std::string(home) + socket_path.substr(1);
            }
        }
        unlink(socket_path.c_str());
    }
    
    void broadcast(const std::string& json_line) override {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& [fd, client] : clients_) {
            if (client->is_connected()) {
                client->send(json_line);
            }
        }
    }
    
    bool is_running() const override {
        return running_;
    }
    
    size_t client_count() const override {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        return clients_.size();
    }
    
private:
    bool init_engine_proxy() {
        // Create engine with streaming STT model config
        std::string config_json = "{\"streaming_stt_model\":\"" + config_.stt_model + "\"}";
        engine_ = rcli_create(config_json.c_str());
        if (!engine_) {
            fprintf(stderr, "Failed to create RCLI engine\n");
            return false;
        }
        
        // Set up callbacks BEFORE rcli_init_proxy (init wires them to the pipeline)
        rcli_set_transcript_callback(engine_, [](const char* text, int is_final, void* user_data) {
            auto* server = static_cast<ProxyServerImpl*>(user_data);
            
            // Normalize transcript casing for display consistency in OpenCode.
            // Zipformer tends to output uppercase-like text; sentence case is easier to read.
            std::string t(text);
            if (!t.empty()) {
                for (char& c : t) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                bool cap_next = true;
                for (char& c : t) {
                    if (cap_next && std::isalpha(static_cast<unsigned char>(c))) {
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        cap_next = false;
                    }
                    if (c == '.' || c == '!' || c == '?') cap_next = true;
                }

                // Trim leading/trailing whitespace from recognizer output
                size_t start = t.find_first_not_of(" \t\r\n");
                if (start == std::string::npos) {
                    t.clear();
                } else {
                    size_t end = t.find_last_not_of(" \t\r\n");
                    t = t.substr(start, end - start + 1);
                }
            }

            // Strip whitespace to check real content length
            size_t real_len = 0;
            for (char c : t) {
                if (c != ' ' && c != '\t') real_len++;
            }

            // Filter phantom single-char transcripts from background noise.
            if (real_len <= 1) {
                printf("[Proxy] Filtered phantom transcript: \"%s\" (final=%d, len=%zu)\n",
                       t.c_str(), is_final, real_len);
                fflush(stdout);
                return;
            }

            // Filter very short partials (< 3 real chars) to reduce noise.
            if (!is_final && real_len < 3) {
                return;
            }
            
            printf("[Proxy] Transcript: \"%s\" (final=%d)\n", t.c_str(), is_final);
            fflush(stdout);
            std::string msg = json::object_with_bool(
                {{"type", "transcript"}, {"text", json::escape(t)}},
                {{"isFinal", is_final != 0}}
            );
            server->broadcast(msg);
        }, this);
        
        rcli_set_state_callback(engine_, [](int old_state, int new_state, void* user_data) {
            auto* server = static_cast<ProxyServerImpl*>(user_data);
            const char* state_str = "idle";
            switch (new_state) {
                case 0: state_str = "idle"; break;
                case 1: state_str = "listening"; break;
                case 2: state_str = "processing"; break;
                case 3: state_str = "speaking"; break;
                case 4: state_str = "interrupted"; break;
            }
            printf("[Proxy] State change: %d -> %d (%s)\n", old_state, new_state, state_str);
            fflush(stdout);
            std::string msg = json::object({{"type", "state"}, {"state", state_str}});
            server->broadcast(msg);
        }, this);
        
        // Initialize for proxy mode (STT + TTS only, no LLM)
        if (rcli_init_proxy(engine_, config_.models_dir.c_str()) != 0) {
            fprintf(stderr, "Failed to initialize RCLI engine for proxy mode\n");
            rcli_destroy(engine_);
            engine_ = nullptr;
            return false;
        }
        
        return true;
    }
    
    void accept_client() {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept");
            }
            return;
        }
        
        // Set non-blocking
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        
        // Add to kqueue
        struct kevent ev;
        EV_SET(&ev, client_fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
        kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);
        
        // Create client
        auto client = std::make_unique<ProxyClientImpl>(client_fd, this);
        
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_[client_fd] = std::move(client);
        }
        
        if (config_.verbose) {
            printf("Client connected (fd=%d, total=%zu)\n", client_fd, client_count());
        }
        printf("[Proxy] Client connected (fd=%d, total=%zu)\n", client_fd, client_count());
        fflush(stdout);
    }
    
    void handle_client_data(ProxyClientImpl* client) {
        char buffer[4096];
        ssize_t n = recv(client->fd(), buffer, sizeof(buffer) - 1, 0);
        
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return; // No data available
            }
            // Client disconnected
            client->connected_ = false;
            return;
        }
        
        buffer[n] = '\0';
        client->read_buffer_ += buffer;
        
        // Process complete lines
        size_t pos;
        while ((pos = client->read_buffer_.find('\n')) != std::string::npos) {
            std::string line = client->read_buffer_.substr(0, pos);
            client->read_buffer_.erase(0, pos + 1);
            
            if (!line.empty()) {
                process_client_message(client, line);
            }
        }
    }
    
    void process_client_message(ProxyClientImpl* client, const std::string& line) {
        // Don't log audio messages (too frequent and large)
        if (line.find("\"audio\"") == std::string::npos && line.find("\"audio_final\"") == std::string::npos) {
            printf("[Proxy] Received message from client fd=%d: %s\n", client->fd(), line.c_str());
            fflush(stdout);
        }
        
        // Simple JSON parsing (extract type field)
        size_t type_pos = line.find("\"type\"");
        if (type_pos == std::string::npos) return;
        
        size_t colon_pos = line.find(':', type_pos);
        if (colon_pos == std::string::npos) return;
        
        size_t quote_pos = line.find('"', colon_pos);
        if (quote_pos == std::string::npos) return;
        
        size_t end_quote = line.find('"', quote_pos + 1);
        if (end_quote == std::string::npos) return;
        
        std::string msg_type = line.substr(quote_pos + 1, end_quote - quote_pos - 1);
        
        if (msg_type == "toggle") {
            // Parse enabled field
            bool enabled = line.find("\"enabled\":true") != std::string::npos ||
                          line.find("\"enabled\": true") != std::string::npos;
            
            // Parse clientAudioCapture field
            bool client_audio = line.find("\"clientAudioCapture\":true") != std::string::npos ||
                               line.find("\"clientAudioCapture\": true") != std::string::npos;
            
            printf("[Proxy] Toggle received: enabled=%s, clientAudio=%s\n", 
                   enabled ? "true" : "false", client_audio ? "true" : "false");
            fflush(stdout);
            
            if (enabled) {
                // Stop existing STT if already running
                if (stt_running_) {
                    printf("[Proxy] Stopping existing STT session before restart...\n");
                    fflush(stdout);
                    rcli_stop_listening(engine_);
                    stt_running_ = false;
                }
                
                client_audio_mode_ = client_audio;
                
                if (client_audio) {
                    // Client handles VAD — RCLI opens CoreAudio mic and broadcasts
                    // raw audio chunks to the client over the socket.  Client runs
                    // EnergyVad, then sends audio_final back for offline STT.
                    printf("[Proxy] Client VAD mode — starting mic + audio broadcast\n");
                    fflush(stdout);
                    
                    int rc = rcli_start_capture(engine_);
                    if (rc != 0) {
                        printf("[Proxy] Failed to start capture: %d\n", rc);
                        fflush(stdout);
                        return;
                    }
                    stt_running_ = true;
                    
                    // Start audio broadcast thread
                    start_audio_broadcast();
                    
                    // Broadcast listening state so client knows we're ready
                    std::string msg = json::object({{"type", "state"}, {"state", "listening"}});
                    broadcast(msg);
                } else {
                    // Normal mode — RCLI opens CoreAudio mic + runs VAD + STT
                    printf("[Proxy] Calling rcli_start_stt_only...\n");
                    fflush(stdout);
                    int rc = rcli_start_stt_only(engine_);
                    printf("[Proxy] rcli_start_stt_only returned %d\n", rc);
                    fflush(stdout);
                    if (rc == 0) stt_running_ = true;
                }
            } else {
                // Stop listening
                if (stt_running_) {
                    stop_audio_broadcast();
                    if (client_audio_mode_) {
                        rcli_stop_capture(engine_);
                    } else {
                        rcli_stop_listening(engine_);
                    }
                    stt_running_ = false;
                    client_audio_mode_ = false;
                }
            }
        } else if (msg_type == "audio_final") {
            // Complete speech segment from client — run Parakeet TDT for final transcript
            if (!stt_running_ || !client_audio_mode_) {
                return;
            }
            handle_audio_final(line);
        } else if (msg_type == "speak") {
            // Parse text field
            size_t text_pos = line.find("\"text\"");
            if (text_pos != std::string::npos) {
                size_t text_colon = line.find('"', text_pos + 6);
                if (text_colon != std::string::npos) {
                    size_t text_end = line.find('"', text_colon + 1);
                    if (text_end != std::string::npos) {
                        std::string text = line.substr(text_colon + 1, text_end - text_colon - 1);
                        // Unescape JSON
                        text = json_unescape(text);
                        rcli_speak(engine_, text.c_str());
                    }
                }
            }
        } else if (msg_type == "interrupt") {
            rcli_stop_speaking(engine_);
        } else if (msg_type == "config") {
            // Parse config updates (optional)
        }
    }
    
    // Extract a string value from a JSON line for a given key
    std::string extract_json_string(const std::string& line, const std::string& key) {
        std::string search = "\"" + key + "\"";
        size_t pos = line.find(search);
        if (pos == std::string::npos) return "";
        size_t colon = line.find(':', pos + search.size());
        if (colon == std::string::npos) return "";
        size_t q1 = line.find('"', colon + 1);
        if (q1 == std::string::npos) return "";
        size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) return "";
        return line.substr(q1 + 1, q2 - q1 - 1);
    }

    // Decode base64 PCM16 to float32 samples (normalized -1.0 to 1.0)
    std::vector<float> decode_audio(const std::string& b64) {
        auto bytes = base64::decode(b64);
        size_t num_samples = bytes.size() / 2;
        std::vector<float> samples(num_samples);
        for (size_t i = 0; i < num_samples; ++i) {
            int16_t s = static_cast<int16_t>(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
            samples[i] = s / 32768.0f;
        }
        return samples;
    }

    // Handle complete speech segment — run Parakeet TDT / Whisper for accurate final
    void handle_audio_final(const std::string& line) {
        std::string data = extract_json_string(line, "data");
        if (data.empty()) return;

        auto samples = decode_audio(data);
        if (samples.empty()) return;

        printf("[Proxy] audio_final: %zu samples (%.1fs)\n", 
               samples.size(), samples.size() / 16000.0f);
        fflush(stdout);

        // Reset streaming STT (we're about to emit a final from offline)
        rcli_stt_reset(engine_);
        last_client_partial_.clear();

        std::string text;

        // Try offline STT (Parakeet TDT or Whisper) first — higher accuracy
        if (rcli_has_offline_stt(engine_)) {
            const char* result = rcli_offline_transcribe(
                engine_, samples.data(), static_cast<int>(samples.size()));
            if (result && result[0] != '\0') {
                text = result;
            }
            printf("[Proxy] Offline STT result: \"%s\"\n", text.c_str());
            fflush(stdout);
        }

        // Fallback: run Zipformer on the full buffer
        if (text.empty()) {
            rcli_stt_feed_audio(engine_, samples.data(), static_cast<int>(samples.size()));
            rcli_stt_process_tick(engine_);
            const char* result_text = nullptr;
            int is_final = 0;
            if (rcli_stt_get_result(engine_, &result_text, &is_final) && result_text) {
                text = result_text;
            }
            rcli_stt_reset(engine_);
            printf("[Proxy] Streaming STT fallback result: \"%s\"\n", text.c_str());
            fflush(stdout);
        }

        // Trim whitespace (preserve original casing from the STT model)
        if (!text.empty()) {
            size_t start = text.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) {
                text.clear();
            } else {
                size_t end = text.find_last_not_of(" \t\r\n");
                text = text.substr(start, end - start + 1);
            }
        }

        // Filter phantom results — but always notify client so it exits "processing"
        size_t real_len = 0;
        for (char c : text) {
            if (c != ' ' && c != '\t') real_len++;
        }
        if (real_len <= 1) {
            printf("[Proxy] audio_final filtered (too short: \"%s\"), sending empty final\n", text.c_str());
            fflush(stdout);
            // Send empty final so bridge exits "processing" state
            std::string msg = json::object_with_bool(
                {{"type", "transcript"}, {"text", ""}},
                {{"isFinal", true}}
            );
            broadcast(msg);
            return;
        }

        printf("[Proxy] audio_final transcript: \"%s\"\n", text.c_str());
        fflush(stdout);

        std::string msg = json::object_with_bool(
            {{"type", "transcript"}, {"text", json::escape(text)}},
            {{"isFinal", true}}
        );
        broadcast(msg);
    }

    // --- Audio broadcast thread: reads CoreAudio ring buffer, sends to clients ---
    
    void start_audio_broadcast() {
        if (audio_broadcast_running_.load()) return;
        audio_broadcast_running_.store(true, std::memory_order_release);
        audio_broadcast_thread_ = std::thread([this]() {
            // Read buffer: 1600 float samples = 100ms at 16kHz
            constexpr int CHUNK = 1600;
            std::vector<float> buf(CHUNK);
            // PCM16 encoding buffer (2 bytes per sample)
            std::vector<unsigned char> pcm16(CHUNK * 2);
            
            printf("[Proxy] Audio broadcast thread started\n");
            fflush(stdout);
            
            while (audio_broadcast_running_.load(std::memory_order_relaxed)) {
                int n = rcli_read_capture_audio(engine_, buf.data(), CHUNK);
                if (n > 0) {
                    // Convert float32 [-1,1] to PCM16 little-endian
                    for (int i = 0; i < n; ++i) {
                        float s = buf[i];
                        if (s > 1.0f) s = 1.0f;
                        if (s < -1.0f) s = -1.0f;
                        int16_t v = static_cast<int16_t>(s * 32767.0f);
                        pcm16[i * 2]     = static_cast<unsigned char>(v & 0xFF);
                        pcm16[i * 2 + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
                    }
                    
                    std::string b64 = base64::encode(pcm16.data(), n * 2);
                    
                    // Build JSON manually (avoid escaping overhead for data field)
                    std::string msg = "{\"type\":\"audio_chunk\",\"data\":\"" + b64 + 
                                      "\",\"samples\":" + std::to_string(n) + "}";
                    broadcast(msg);
                } else {
                    // No data — sleep briefly to avoid busy-spinning
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            
            printf("[Proxy] Audio broadcast thread stopped\n");
            fflush(stdout);
        });
    }
    
    void stop_audio_broadcast() {
        if (!audio_broadcast_running_.load()) return;
        audio_broadcast_running_.store(false, std::memory_order_release);
        if (audio_broadcast_thread_.joinable()) {
            audio_broadcast_thread_.join();
        }
    }

    std::string json_unescape(const std::string& s) {
        std::string result;
        for (size_t i = 0; i < s.length(); ++i) {
            if (s[i] == '\\' && i + 1 < s.length()) {
                switch (s[i + 1]) {
                    case '"': result += '"'; ++i; break;
                    case '\\': result += '\\'; ++i; break;
                    case 'n': result += '\n'; ++i; break;
                    case 'r': result += '\r'; ++i; break;
                    case 't': result += '\t'; ++i; break;
                    default: result += s[i];
                }
            } else {
                result += s[i];
            }
        }
        return result;
    }
    
    void cleanup_clients() {
        bool should_stop_stt = false;
        
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (auto it = clients_.begin(); it != clients_.end();) {
                if (!it->second->is_connected()) {
                    int cfd = it->second->fd();
                    printf("[Proxy] Client disconnected (fd=%d)\n", cfd);
                    fflush(stdout);
                    if (cfd >= 0) {
                        struct kevent ev;
                        EV_SET(&ev, cfd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
                        kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);
                    }
                    it->second->close();
                    it = clients_.erase(it);
                } else {
                    ++it;
                }
            }
            
            // Check if we need to stop STT (but don't call it under the lock!)
            if (clients_.empty() && stt_running_) {
                should_stop_stt = true;
            }
        }
        // Mutex is now released — safe to call rcli_stop_listening
        // (which triggers state callback → broadcast → re-acquires mutex)
        if (should_stop_stt) {
            printf("[Proxy] No clients remaining, stopping STT...\n");
            fflush(stdout);
            stop_audio_broadcast();
            if (client_audio_mode_) {
                rcli_stop_capture(engine_);
            } else {
                auto t0 = std::chrono::steady_clock::now();
                rcli_stop_listening(engine_);
                auto t1 = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                printf("[Proxy] rcli_stop_listening took %lld ms\n", ms);
                fflush(stdout);
            }
            stt_running_ = false;
            client_audio_mode_ = false;
        }
    }
    
    ProxyConfig config_;
    std::atomic<bool> running_;
    bool stt_running_ = false;
    bool client_audio_mode_ = false;  // true when client handles VAD
    std::string last_client_partial_;  // last streaming partial for dedup
    std::thread audio_broadcast_thread_;
    std::atomic<bool> audio_broadcast_running_{false};
    int server_fd_;
    int kqueue_fd_;
    RCLIHandle engine_;
    
    mutable std::mutex clients_mutex_;
    std::unordered_map<int, std::unique_ptr<ProxyClientImpl>> clients_;
};

std::unique_ptr<ProxyServer> create_proxy_server(const ProxyConfig& config) {
    return std::make_unique<ProxyServerImpl>(config);
}

// =============================================================================
// Command Entry Point
// =============================================================================

int cmd_proxy(const ProxyConfig& config) {
    printf("Starting RCLI Proxy...\n");
    printf("  Socket: %s\n", config.socket_path.c_str());
    printf("  Models: %s\n", config.models_dir.c_str());
    printf("  TTS: %s\n", config.tts_model.c_str());
    printf("  STT: %s\n", config.stt_model.c_str());
    fflush(stdout);
    
    auto server = create_proxy_server(config);
    
    // Handle signals
    signal(SIGINT, [](int) {
        printf("\nShutting down...\n");
        exit(0);
    });
    
    return server->run();
}

} // namespace rcli
