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
        
        // Initialize RCLI engine
        if (!init_engine()) {
            fprintf(stderr, "Failed to initialize RCLI engine\n");
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
    bool init_engine() {
        // Create engine
        engine_ = rcli_create(nullptr);
        if (!engine_) {
            fprintf(stderr, "Failed to create RCLI engine\n");
            return false;
        }
        
        // Set up callbacks BEFORE rcli_init (init wires them to the pipeline)
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

            // Filter phantom transcripts: single-character results from background
            // noise (e.g. "I", "U", "O") are almost always false positives.
            // Only apply to finals — partials will self-correct as more audio arrives.
            if (is_final) {
                // Strip whitespace to check real content length
                size_t real_len = 0;
                for (char c : t) {
                    if (c != ' ' && c != '\t') real_len++;
                }
                if (real_len <= 1) {
                    printf("[Proxy] Filtered phantom transcript: \"%s\" (final=%d, len=%zu)\n",
                           t.c_str(), is_final, real_len);
                    fflush(stdout);
                    return;  // Skip broadcasting phantom single-char finals
                }
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
        
        // Initialize with models (this wires the callbacks to the pipeline)
        if (rcli_init(engine_, config_.models_dir.c_str(), config_.gpu_layers) != 0) {
            fprintf(stderr, "Failed to initialize RCLI engine\n");
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
        printf("[Proxy] Received message from client fd=%d: %s\n", client->fd(), line.c_str());
        fflush(stdout);
        
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
            
            printf("[Proxy] Toggle received: enabled=%s\n", enabled ? "true" : "false");
            fflush(stdout);
            
            if (enabled) {
                // Stop existing STT if already running (e.g. from a previous client)
                if (stt_running_) {
                    printf("[Proxy] Stopping existing STT session before restart...\n");
                    fflush(stdout);
                    rcli_stop_listening(engine_);
                    stt_running_ = false;
                }
                // Start streaming STT only (no LLM thread) — for proxy mode
                printf("[Proxy] Calling rcli_start_stt_only...\n");
                fflush(stdout);
                int rc = rcli_start_stt_only(engine_);
                printf("[Proxy] rcli_start_stt_only returned %d\n", rc);
                fflush(stdout);
                if (rc == 0) stt_running_ = true;
            } else {
                // Stop listening
                if (stt_running_) {
                    rcli_stop_listening(engine_);
                    stt_running_ = false;
                }
            }
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
            auto t0 = std::chrono::steady_clock::now();
            rcli_stop_listening(engine_);
            auto t1 = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            printf("[Proxy] rcli_stop_listening took %lld ms\n", ms);
            fflush(stdout);
            stt_running_ = false;
        }
    }
    
    ProxyConfig config_;
    std::atomic<bool> running_;
    bool stt_running_ = false;
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
