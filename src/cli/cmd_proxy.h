// =============================================================================
// RCLI Proxy Mode — Unix Socket Server for Voice Integration
// =============================================================================
//
// This module implements a Unix domain socket server that exposes RCLI's
// voice pipeline (STT, TTS, VAD) to external clients (e.g., OpenCode).
// Supports multiple concurrent client connections.
//
// Protocol: JSON lines over Unix socket
//   Client -> Server: { "type": "toggle", "enabled": true }
//   Server -> Client: { "type": "transcript", "text": "hello", "isFinal": true }
//
// Usage:
//   rcli proxy --socket ~/.opencode/rcli-voice.sock --tts-model kokoro-en
//
// =============================================================================

#pragma once

#include <string>
#include <functional>
#include <memory>

namespace rcli {

// Configuration for the proxy server
struct ProxyConfig {
    std::string socket_path = "~/.opencode/rcli-voice.sock";
    std::string models_dir = "./models";
    std::string tts_model = "kokoro-en";
    std::string tts_voice = "";
    std::string stt_model = "zipformer";
    float vad_threshold = 0.5f;
    int gpu_layers = 99;
    bool verbose = false;
};

// Client connection handle
class ProxyClient {
public:
    virtual ~ProxyClient() = default;
    virtual int fd() const = 0;
    virtual void send(const std::string& json_line) = 0;
    virtual void close() = 0;
    virtual bool is_connected() const = 0;
};

// Server interface
class ProxyServer {
public:
    virtual ~ProxyServer() = default;
    
    // Start the server (blocking)
    virtual int run() = 0;
    
    // Stop the server (thread-safe)
    virtual void stop() = 0;
    
    // Broadcast a message to all connected clients
    virtual void broadcast(const std::string& json_line) = 0;
    
    // Check if server is running
    virtual bool is_running() const = 0;
    
    // Get number of connected clients
    virtual size_t client_count() const = 0;
};

// Factory function
std::unique_ptr<ProxyServer> create_proxy_server(const ProxyConfig& config);

// Main entry point for "rcli proxy" command
int cmd_proxy(const ProxyConfig& config);

} // namespace rcli
