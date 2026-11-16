// HTTP surface for the Nexus media engines (image, video, speech, voice) used in media mode.
// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <thread>

namespace ps5lm {
constexpr unsigned short kMediaDefaultPort = 8083;
constexpr std::size_t kMediaMaxHeader = 8192;
constexpr std::size_t kMediaMaxBody = 64u << 20;

struct MediaRequest {
    std::string method; // "GET" or "POST"
    std::string path;   // without the query string
    std::string body;   // Content-Length framed bytes; empty when none was sent
};

// Runs on the serving thread after the bearer check. The handler sends exactly one
// response (JSON or SSE) on fd and returns. The server closes fd afterwards.
using MediaHandler = void (*)(int fd, const MediaRequest & request);

// Peer filter for the accept path, on the host-order IPv4 address. Loopback 127/8 and
// Tailscale CGNAT 100.64.0.0/10 are always admitted; 10/8, 172.16/12 and 192.168/16 only
// when allow_lan. Everything else is refused.
bool media_peer_allowed(uint32_t ipv4_host_order, bool allow_lan);
// Complete JSON response with Connection: close.
void media_send_json(int fd, int code, const std::string & body);
// Complete response with the given Content-Type (for example audio/wav) and Connection: close.
void media_send_bytes(int fd, int code, const char * content_type, const std::string & body);
// PSllama error shape: {"error":{"message":...,"type":...}}.
void media_send_error(int fd, int code, const std::string & type, const std::string & message);
// Quoted JSON string with escapes applied.
std::string json_quote(const std::string & text);

// Server-sent events on a chunked 200 response: begin() once, event() per payload, end().
// A payload is JSON text or the literal "[DONE]". After a failed write open() turns false.
class MediaSse {
  public:
    explicit MediaSse(int fd) : fd_(fd) {}
    bool begin();
    bool event(const std::string & payload);
    void end();
    bool open() const { return open_; }
  private:
    int fd_;
    bool open_ = false;
};

class MediaServer {
  public:
    MediaServer();
    ~MediaServer();
    MediaServer(const MediaServer &) = delete;
    MediaServer & operator=(const MediaServer &) = delete;

    // Call before start(). Returns false if the route exists or the server is running.
    // Keys are "METHOD /path"; GET /health is registered by the constructor.
    bool route(const std::string & method, const std::string & path, MediaHandler handler);
    // token_file: the nexus-control.token file (sha256:<hex>). allow_lan: the caller's
    // nexus-control.lan marker check. Without it the server binds 127.0.0.1 and admits
    // loopback and 100.64.0.0/10 peers. With it, it binds 0.0.0.0 and also admits RFC1918.
    bool start(const std::string & token_file, bool allow_lan, unsigned short port = kMediaDefaultPort);
    void stop();
    unsigned short port() const { return port_; }

  private:
    void serve();
    void handle(int fd);
    std::map<std::string, MediaHandler> routes_;
    std::array<unsigned char, 32> token_hash_{};
    bool allow_lan_ = false;
    std::atomic<bool> stopped_{true};
    int listener_ = -1;
    unsigned short port_ = 0;
    std::thread thread_;
};

// Handlers and route registration for the media units. Each is defined in its own
// file; the media boot calls the three register functions on a MediaServer.
// U-38: native/nexus_media_image.cpp
void handle_image_generation(int fd, const MediaRequest & request);
void handle_video_generation(int fd, const MediaRequest & request);
void register_media_image_routes(MediaServer & server);
// U-39: native/nexus_media_audio.cpp
void handle_transcription(int fd, const MediaRequest & request);
void handle_speech(int fd, const MediaRequest & request);
void register_media_audio_routes(MediaServer & server);
// U-40: native/nexus_media_voice.cpp
void handle_voice_chat(int fd, const MediaRequest & request);
void register_media_voice_routes(MediaServer & server);

// GPU coordination: only one engine set may hold GPU memory at a time. Engines are
// named by MediaGpuUse bits. media_gpu_acquire unloads each loaded engine not in
// keep_mask and returns the mask of the engines it unloaded. Call it with no service
// lock held (the server is single-threaded, so no extra lock is needed).
// Derived from PSllama's free_gpu_for, see native/nexus_media_gpu.cpp.
enum MediaGpuUse : unsigned { kGpuSd = 1, kGpuStt = 2, kGpuTts = 4, kGpuVoiceLlm = 8 };
class SdService;
unsigned media_gpu_acquire(unsigned keep_mask);
// The media process's SdService, defined in nexus_media_image.cpp.
SdService & media_image_service();
// Frees the voice chat LLM if loaded. Idempotent. Returns true when it freed a model.
bool media_voice_unload();
} // namespace ps5lm
