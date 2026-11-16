// Listener, peer filter, bearer check and request reader for the media API.
// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexus_media.hpp"
#include "nexus_peer.hpp"
extern "C" {
#include "hash/sha256/sha256.h"
}
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

// Same pin guards and canonical-record check as native/nexus_control.cpp (keep in sync).
#if defined(NEXUS_CONTROL_REQUIRE_PROVISIONING_PIN)
#if !defined(__PROSPERO__) && !defined(NEXUS_CONTROL_TEST_PLATFORM)
#error Provisioning-pin policy is restricted to the PS5 title.
#endif
#ifndef NEXUS_CONTROL_PROVISIONING_DIGEST
#error A pin-enabled build requires an independently provisioned digest header.
#endif
namespace {
constexpr char provisioning_record[] = NEXUS_CONTROL_PROVISIONING_DIGEST;
constexpr bool canonical_provisioning_record() {
    if (sizeof(provisioning_record) != 72) return false;
    constexpr char prefix[] = "sha256:";
    for (size_t i = 0; i < 7; ++i)
        if (provisioning_record[i] != prefix[i]) return false;
    for (size_t i = 7; i < 71; ++i)
        if (!((provisioning_record[i] >= '0' && provisioning_record[i] <= '9') ||
              (provisioning_record[i] >= 'a' && provisioning_record[i] <= 'f'))) return false;
    return true;
}
static_assert(canonical_provisioning_record(), "Invalid provisioning digest configuration");
}
#endif

namespace ps5lm {
namespace {
std::string trim(std::string text) {
    const auto a = text.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = text.find_last_not_of(" \t\r\n");
    return text.substr(a, b - a + 1);
}

std::string lower(std::string text) {
    for (char & c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Same format and rules as NexusControl: "sha256:" followed by 64 hex digits.
bool parse_token_hash(const std::string & text, std::array<unsigned char, 32> & digest) {
    if (text.size() != 71 || text.compare(0, 7, "sha256:") != 0) return false;
    for (size_t i = 0; i < digest.size(); ++i) {
        const int high = hex_value(text[7 + i * 2]);
        const int low = hex_value(text[8 + i * 2]);
        if (high < 0 || low < 0) return false;
        digest[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

// Mirrors NexusControl::start() in native/nexus_control.cpp: the open/fstat/read
// sequence, the pinned sandbox-metadata rule, the pinned record format and the pinned
// digest comparison. Keep in sync. Only the diagnostic printf lines are omitted.
bool load_token_hash(const std::string & token_file, std::array<unsigned char, 32> & digest) {
    const int token_fd = open(token_file.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (token_fd < 0) return false;
    struct stat token_stat{};
    const int stat_result = fstat(token_fd, &token_stat);
    const uid_t process_euid = geteuid();
    bool pinned_sandbox_metadata = false;
#if defined(NEXUS_CONTROL_REQUIRE_PROVISIONING_PIN)
    pinned_sandbox_metadata = process_euid == 1 && token_stat.st_uid == 0 &&
        token_stat.st_nlink == 0 && (token_stat.st_mode & 07777) == 0600;
#endif
    const bool safe_file = stat_result == 0 && S_ISREG(token_stat.st_mode) &&
        (token_stat.st_uid == process_euid || pinned_sandbox_metadata) &&
        (token_stat.st_mode & (S_IRWXG | S_IRWXO)) == 0 && (token_stat.st_mode & S_IRUSR) != 0;
    char token_bytes[128];
    size_t token_length = 0;
    bool token_too_large = false;
    bool read_failed = false;
    while (safe_file) {
        const ssize_t count = read(token_fd, token_bytes + token_length, sizeof(token_bytes) - token_length);
        if (count == 0) break;
        if (count < 0) { read_failed = true; break; }
        token_length += static_cast<size_t>(count);
        if (token_length == sizeof(token_bytes)) {
            char extra;
            token_too_large = read(token_fd, &extra, 1) != 0;
            break;
        }
    }
    close(token_fd);
    if (!safe_file || read_failed || token_too_large || token_length == 0) return false;
    std::string encoded_hash(token_bytes, token_length);
#if defined(NEXUS_CONTROL_REQUIRE_PROVISIONING_PIN)
    if ((token_stat.st_mode & 07777) != 0600 || encoded_hash.size() != 72 ||
        encoded_hash.back() != '\n') return false;
    encoded_hash.pop_back();
    for (size_t i = 7; i < encoded_hash.size(); ++i)
        if (!((encoded_hash[i] >= '0' && encoded_hash[i] <= '9') ||
              (encoded_hash[i] >= 'a' && encoded_hash[i] <= 'f'))) return false;
#else
    encoded_hash = trim(encoded_hash);
#endif
    std::array<unsigned char, 32> expected{};
    if (!parse_token_hash(encoded_hash, expected)) return false;
#if defined(NEXUS_CONTROL_REQUIRE_PROVISIONING_PIN)
    std::array<unsigned char, 32> provisioned{};
    if (!parse_token_hash(provisioning_record, provisioned)) return false;
    unsigned int difference = 0;
    for (size_t i = 0; i < expected.size(); ++i) difference |= expected[i] ^ provisioned[i];
    if (difference != 0) return false;
#endif
    digest = expected;
    return true;
}

bool valid_bearer(const std::string & supplied) {
    return supplied.size() >= 32 && supplied.size() <= 128 &&
        std::all_of(supplied.begin(), supplied.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
}

// Constant-time comparison of SHA-256(supplied) with the stored digest.
bool token_matches(const std::array<unsigned char, 32> & expected, const std::string & supplied) {
    if (!valid_bearer(supplied)) return false;
    std::array<unsigned char, 32> actual{};
    sha256_hash(actual.data(), reinterpret_cast<const unsigned char *>(supplied.data()), supplied.size());
    unsigned int mismatch = 0;
    for (size_t i = 0; i < actual.size(); ++i) mismatch |= actual[i] ^ expected[i];
    return mismatch == 0;
}

const char * reason_phrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

bool send_all(int fd, const std::string & data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t count = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
        const ssize_t count = send(fd, data.data() + sent, data.size() - sent, 0);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

// Request head as read off the wire. The body is read only after the bearer check.
struct MediaHead {
    MediaRequest request;      // method and path; body holds the bytes that came with the head
    std::string authorization; // Authorization header value, or empty
    bool have_length = false;
    std::string length_text;   // Content-Length header value, trimmed
};

// Reads and parses the header block only. Returns 0 when it is complete, -1 when the peer
// sent nothing, or else the HTTP status to answer with (400 or 431). No body is read here.
int read_head(int fd, MediaHead & head) {
    std::string raw;
    char chunk[2048];
    std::size_t boundary = std::string::npos;
    while (boundary == std::string::npos) {
        const ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return raw.empty() ? -1 : 400;
        raw.append(chunk, static_cast<std::size_t>(got));
        boundary = raw.find("\r\n\r\n");
        if ((boundary == std::string::npos ? raw.size() : boundary) > kMediaMaxHeader) return 431;
    }

    const std::size_t line_end = raw.find("\r\n");
    const std::string line = raw.substr(0, line_end);
    const std::size_t first = line.find(' ');
    const std::size_t second = first == std::string::npos ? std::string::npos : line.find(' ', first + 1);
    if (second == std::string::npos) return 400;
    head.request.method = line.substr(0, first);
    head.request.path = line.substr(first + 1, second - first - 1);
    if (line.substr(second + 1) != "HTTP/1.1" || head.request.path.empty() || head.request.path[0] != '/') return 400;

    bool auth_seen = false;
    for (std::size_t at = line_end + 2; at < boundary;) {
        std::size_t eol = raw.find("\r\n", at);
        if (eol == std::string::npos || eol > boundary) eol = boundary;
        const std::string header = raw.substr(at, eol - at);
        at = eol + 2;
        const std::size_t colon = header.find(':');
        if (colon == std::string::npos) return 400;
        const std::string name = lower(header.substr(0, colon));
        const std::string value = trim(header.substr(colon + 1));
        if (name == "content-length") {
            if (head.have_length) return 400;
            head.have_length = true;
            head.length_text = value;
        } else if (name == "transfer-encoding") {
            if (lower(value) != "identity") return 400;
        } else if (name == "authorization") {
            if (auth_seen) return 400;
            auth_seen = true;
            head.authorization = value;
        }
    }
    head.request.body = raw.substr(boundary + 4);
    return 0;
}

// Checks Content-Length and reads the body. Runs only after the bearer check. Returns 0
// with request.body complete, or else the HTTP status to answer with (400, 411 or 413).
int read_body(int fd, MediaHead & head) {
    MediaRequest & request = head.request;
    std::size_t length = 0;
    if (head.have_length) {
        if (head.length_text.empty()) return 400;
        for (const char c : head.length_text) {
            if (c < '0' || c > '9') return 400;
            length = length * 10 + static_cast<std::size_t>(c - '0');
            if (length > kMediaMaxBody) return 413;
        }
        if (request.body.size() > length) return 400;
    }
    if (request.method == "POST" && !head.have_length) return 411;

    char chunk[2048];
    while (request.body.size() < length) {
        const std::size_t want = std::min(sizeof(chunk), length - request.body.size());
        const ssize_t got = recv(fd, chunk, want, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return 400;
        request.body.append(chunk, static_cast<std::size_t>(got));
    }
    return 0;
}

void reject_request(int fd, int code) {
    const char * message = code == 411 ? "Content-Length required" :
        code == 413 ? "request body too large" :
        code == 431 ? "request headers too large" : "invalid or oversized HTTP request";
    media_send_error(fd, code, "invalid_request_error", message);
}

void handle_health(int fd, const MediaRequest &) {
    media_send_json(fd, 200, "{\"status\":\"ok\",\"mode\":\"media\"}");
}
} // namespace

bool media_peer_allowed(uint32_t ipv4_host_order, bool allow_lan) {
    return peer_allowed_ipv4(ipv4_host_order, allow_lan);
}

void media_send_bytes(int fd, int code, const char * content_type, const std::string & body) {
    const std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason_phrase(code) +
        "\r\nContent-Type: " + content_type + "\r\nCache-Control: no-store\r\nConnection: close\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    if (send_all(fd, head)) send_all(fd, body);
}

void media_send_json(int fd, int code, const std::string & body) {
    media_send_bytes(fd, code, "application/json", body);
}

void media_send_error(int fd, int code, const std::string & type, const std::string & message) {
    media_send_json(fd, code, "{\"error\":{\"message\":" + json_quote(message) +
        ",\"type\":" + json_quote(type) + "}}");
}

std::string json_quote(const std::string & text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char encoded[7];
                    std::snprintf(encoded, sizeof(encoded), "\\u%04x", static_cast<unsigned>(c));
                    out += encoded;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out + "\"";
}

bool MediaSse::begin() {
    static const char head[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\nX-Accel-Buffering: no\r\n"
        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    open_ = send_all(fd_, std::string(head));
    return open_;
}

bool MediaSse::event(const std::string & payload) {
    if (!open_) return false;
    const std::string data = "data: " + payload + "\n\n";
    char size[32];
    std::snprintf(size, sizeof(size), "%lx\r\n", static_cast<unsigned long>(data.size()));
    open_ = send_all(fd_, std::string(size)) && send_all(fd_, data) && send_all(fd_, "\r\n");
    return open_;
}

void MediaSse::end() {
    if (open_) send_all(fd_, "0\r\n\r\n");
    open_ = false;
}

MediaServer::MediaServer() {
    routes_["GET /health"] = handle_health;
}

MediaServer::~MediaServer() { stop(); }

bool MediaServer::route(const std::string & method, const std::string & path, MediaHandler handler) {
    if (!stopped_ || handler == nullptr) return false;
    return routes_.emplace(method + " " + path, handler).second;
}

bool MediaServer::start(const std::string & token_file, bool allow_lan, unsigned short port) {
    if (!stopped_ || thread_.joinable()) return false;
    std::array<unsigned char, 32> token_hash{};
    if (!load_token_hash(token_file, token_hash)) {
        // Fixed message only: never the path contents, token or digest.
        std::printf("ps5lm-media: credential unavailable, media listener not started\n");
        return false;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    const int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(allow_lan ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(fd, 4) != 0) {
        const int error = errno;
        close(fd);
        std::printf("ps5lm-media: listener unavailable errno=%d\n", error);
        return false;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        close(fd);
        return false;
    }
    token_hash_ = token_hash;
    port_ = ntohs(address.sin_port);
    listener_ = fd;
    allow_lan_ = allow_lan;
    stopped_ = false;
    thread_ = std::thread(&MediaServer::serve, this);
    return true;
}

void MediaServer::stop() {
    stopped_ = true;
    if (thread_.joinable()) thread_.join();
    if (listener_ >= 0) close(listener_);
    listener_ = -1;
    token_hash_.fill(0);
}

void MediaServer::serve() {
    while (!stopped_) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener_, &set);
        timeval wait{};
        wait.tv_usec = 250000; // lets stop() take effect when no client connects
        if (select(listener_ + 1, &set, nullptr, nullptr, &wait) <= 0) continue;
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        const int client = accept(listener_, reinterpret_cast<sockaddr *>(&peer), &peer_length);
        if (client < 0) {
            // Persistent failures (for example, out of descriptors) must not spin the thread.
            if (errno != EINTR) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (!media_peer_allowed(ntohl(peer.sin_addr.s_addr), allow_lan_)) {
            close(client);
            continue;
        }
        timeval io{};
        io.tv_sec = 30;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof(io));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof(io));
        // A failed allocation in one request must not terminate the title.
        try {
            handle(client);
        } catch (...) {
            std::printf("ps5lm-media: request failed\n");
        }
        close(client);
    }
}

void MediaServer::handle(int fd) {
    MediaHead head;
    const int head_status = read_head(fd, head);
    if (head_status < 0) return;
    if (head_status > 0) {
        reject_request(fd, head_status);
        return;
    }
    // Bearer check before any body byte is read: an unauthenticated peer cannot make us buffer up to 64 MiB.
    if (head.authorization.compare(0, 7, "Bearer ") != 0 || !token_matches(token_hash_, head.authorization.substr(7))) {
        media_send_error(fd, 401, "authentication_error", "valid Bearer API key required");
        return;
    }
    const int body_status = read_body(fd, head);
    if (body_status > 0) {
        reject_request(fd, body_status);
        return;
    }
    MediaRequest & request = head.request;
    if (request.method != "GET" && request.method != "POST") {
        media_send_error(fd, 405, "invalid_request_error", "method not allowed");
        return;
    }
    const std::size_t query = request.path.find('?');
    if (query != std::string::npos) request.path.erase(query);
    const auto found = routes_.find(request.method + " " + request.path);
    if (found == routes_.end()) {
        media_send_error(fd, 404, "invalid_request_error", "endpoint not found");
        return;
    }
    found->second(fd, request);
}
} // namespace ps5lm
