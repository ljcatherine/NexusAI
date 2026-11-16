// Opt-in, bearer-protected control API for the NexusAI native app.
// No shell, file tool, model download or path-based load endpoint is exposed.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexus_control.hpp"
#include "nexus_peer.hpp"
extern "C" {
#include "hash/sha256/sha256.h"
}
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

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
bool startup_failure(const char * stage, int error = 0) {
    // Fixed stage names and numeric errno only: never log the credential,
    // digest, caller-controlled path or HTTP headers.
    std::printf("ps5lm-app: Nexus control unavailable stage=%s errno=%d\n", stage, error);
    return false;
}
std::string json_string(const std::string & input) {
    std::string result = "\"";
    for (const unsigned char c : input) {
        switch (c) {
            case '\"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20) {
                    char encoded[7]; std::snprintf(encoded, sizeof(encoded), "\\u%04x", c);
                    result += encoded;
                } else { result.push_back((char)c); }
        }
    }
    return result + "\"";
}
std::string state_name(ServerState state) {
    switch (state) {
        case ServerState::loading: return "loading";
        case ServerState::ready: return "ready";
        case ServerState::generating: return "generating";
        case ServerState::failed: return "failed";
        case ServerState::no_model: return "no_model";
    }
    return "unknown";
}
std::string trim(std::string text) {
    const auto a = text.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = text.find_last_not_of(" \t\r\n");
    return text.substr(a, b - a + 1);
}
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
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
bool valid_bearer(const std::string & supplied) {
    return supplied.size() >= 32 && supplied.size() <= 128 &&
        std::all_of(supplied.begin(), supplied.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
}
bool token_matches(const std::array<unsigned char, 32> & expected, const std::string & supplied) {
    if (!valid_bearer(supplied)) return false;
    std::array<unsigned char, 32> actual{};
    sha256_hash(actual.data(), reinterpret_cast<const unsigned char *>(supplied.data()), supplied.size());
    unsigned int mismatch = 0;
    for (size_t i = 0; i < actual.size(); ++i) mismatch |= actual[i] ^ expected[i];
    return mismatch == 0;
}
std::string json_number(double value) {
    // Format decimal JSON without locale-sensitive streams. The target's
    // console locale can report an empty decimal separator.
    if (!std::isfinite(value) || value < 0 || value > 1.0e15) return "null";
    const uint64_t milli = static_cast<uint64_t>(value * 1000.0 + 0.5);
    std::string fraction = std::to_string(milli % 1000);
    while (fraction.size() < 3) fraction.insert(fraction.begin(), '0');
    return std::to_string(milli / 1000) + "." + fraction;
}
// Log files can hold cut-off multi-byte text; invalid bytes would break the JSON.
// shortcut: checks sequence shape only (overlong forms pass), tighten if a strict client rejects a reply
std::string valid_utf8(const std::string & in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = (unsigned char)in[i];
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n != 0 && i + n <= in.size();
        for (size_t k = 1; ok && k < n; ++k) ok = ((unsigned char)in[i + k] & 0xC0) == 0x80;
        if (ok) { out.append(in, i, n); i += n; } else { out += '?'; ++i; }
    }
    return out;
}
// One page of a log as whole lines, plus the byte cursor to ask from next time.
// after < 0, or past the end (the log was rewritten), means the newest lines.
// shortcut: cursor polling, not a held-open stream, since one thread serves every request; give streams their own thread first
std::string log_page(const std::string & file, const std::string & source, long after) {
    constexpr long chunk = 65536;
    std::string text;
    long from = 0;
    bool tail = false;
    if (FILE * f = std::fopen(file.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::max(0L, std::ftell(f));
        tail = after < 0 || after > size;
        from = tail ? std::max(0L, size - chunk) : after;
        std::fseek(f, from, SEEK_SET);
        text.resize((size_t)std::min(chunk, size - from));
        text.resize(std::fread(text.data(), 1, text.size(), f));
        std::fclose(f);
    }
    size_t start = tail && from > 0 ? text.find('\n') + 1 : 0; // the first line is cut
    size_t stop = text.rfind('\n');
    // No newline: wait for the line to finish, unless it alone fills a page.
    stop = stop != std::string::npos ? stop + 1 : text.size() == (size_t)chunk ? text.size() : start;
    stop = std::max(stop, start);
    std::string body = "{\"source\":" + json_string(source) + ",\"next\":" + std::to_string(from + (long)stop) + ",\"lines\":[";
    bool first = true;
    while (start < stop) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos || end > stop) end = stop;
        size_t last = end;
        if (last > start && text[last - 1] == '\r') --last;
        if (!first) body += ',';
        first = false;
        body += json_string(valid_utf8(text.substr(start, last - start)));
        start = end + 1;
    }
    return body + "]}";
}
void response(int fd, int code, const std::string & body) {
    const char * reason = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 400 ? "Bad Request" :
         code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" :
         code == 409 ? "Conflict" : code == 413 ? "Payload Too Large" : "Service Unavailable";
    const std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n"
        "Content-Type: application/json\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    const std::string all = head + body;
    size_t sent = 0;
    while (sent < all.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t count = send(fd, all.data() + sent, all.size()-sent, MSG_NOSIGNAL);
#else
        const ssize_t count = send(fd, all.data() + sent, all.size()-sent, 0);
#endif
        if (count <= 0) break;
        sent += (size_t)count;
    }
}
} // namespace

std::string NexusControl::model_id(const std::string & path) {
    // Stable ID across app restarts; only used to look up exact enumerated paths.
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char c : path) { hash ^= c; hash *= 1099511628211ULL; }
    char id[17]; std::snprintf(id, sizeof(id), "%016llx", (unsigned long long)hash);
    return id;
}
NexusControl::~NexusControl() { stop(); }

bool NexusControl::start(const std::string & token_file, bool allow_lan, unsigned short port) {
    if (!stopped_ || thread_.joinable()) return startup_failure("already_started");
    struct stat token_stat{};
    const int token_fd = open(token_file.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (token_fd < 0) return startup_failure("credential_open", errno);
    const int stat_result = fstat(token_fd, &token_stat);
    const int stat_error = stat_result == 0 ? 0 : errno;
    const uid_t process_euid = geteuid();
    bool pinned_sandbox_metadata = false;
#if defined(NEXUS_CONTROL_REQUIRE_PROVISIONING_PIN)
    // The title's mounted filesystem conceals owner/link metadata. This is
    // accepted only with an independently embedded approved digest, checked
    // below before any listener exists. It is not proof of filesystem ownership.
    pinned_sandbox_metadata = process_euid == 1 && token_stat.st_uid == 0 &&
        token_stat.st_nlink == 0 && (token_stat.st_mode & 07777) == 0600;
#endif
    const bool safe_file = stat_result == 0 && S_ISREG(token_stat.st_mode) &&
        (token_stat.st_uid == process_euid || pinned_sandbox_metadata) &&
        (token_stat.st_mode & (S_IRWXG | S_IRWXO)) == 0 && (token_stat.st_mode & S_IRUSR) != 0;
    char token_bytes[128];
    size_t token_length = 0;
    bool token_too_large = false;
    int read_error = 0;
    while (safe_file) {
        const ssize_t count = read(token_fd, token_bytes + token_length, sizeof(token_bytes) - token_length);
        if (count == 0) break;
        if (count < 0) { read_error = errno; token_too_large = true; break; }
        token_length += static_cast<size_t>(count);
        if (token_length == sizeof(token_bytes)) {
            char extra;
            token_too_large = read(token_fd, &extra, 1) != 0;
            break;
        }
    }
    close(token_fd);
    if (!safe_file) {
        if (stat_result != 0) return startup_failure("credential_stat", stat_error);
        if (!S_ISREG(token_stat.st_mode)) return startup_failure("credential_type");
        if (token_stat.st_uid != process_euid) {
            struct stat path_stat{};
            const int path_result = lstat(token_file.c_str(), &path_stat);
            const int path_error = path_result == 0 ? 0 : errno;
            std::printf("ps5lm-app: Nexus control path_identity result=%d errno=%d dev=%llu ino=%llu uid=%llu mode=%u size=%lld\n",
                        path_result, path_error,
                        static_cast<unsigned long long>(path_stat.st_dev),
                        static_cast<unsigned long long>(path_stat.st_ino),
                        static_cast<unsigned long long>(path_stat.st_uid),
                        static_cast<unsigned>(path_stat.st_mode),
                        static_cast<long long>(path_stat.st_size));
            // Numeric OS user IDs are diagnostic metadata, not credentials.
            std::printf("ps5lm-app: Nexus control ownership file_uid=%llu process_euid=%llu\n",
                        static_cast<unsigned long long>(token_stat.st_uid),
                        static_cast<unsigned long long>(process_euid));
            std::printf("ps5lm-app: Nexus control identity dev=%llu ino=%llu mode=%u gid=%llu size=%lld stat_bytes=%zu\n",
                        static_cast<unsigned long long>(token_stat.st_dev),
                        static_cast<unsigned long long>(token_stat.st_ino),
                        static_cast<unsigned>(token_stat.st_mode),
                        static_cast<unsigned long long>(token_stat.st_gid),
                        static_cast<long long>(token_stat.st_size), sizeof(token_stat));
            // Status structure prefix only, never credential file contents.
            // Compare these metadata words with the trusted payload context
            // to distinguish layout/view differences before changing ownership.
            const auto * metadata = reinterpret_cast<const unsigned char *>(&token_stat);
            for (size_t offset = 0; offset < 32; offset += 4) {
                uint32_t word = 0;
                std::memcpy(&word, metadata + offset, sizeof(word));
                std::printf("ps5lm-app: Nexus control stat_word offset=%zu value=%u\n", offset, word);
            }
            return startup_failure("credential_owner");
        }
        return startup_failure("credential_permissions");
    }
    if (read_error) return startup_failure("credential_read", read_error);
    if (token_too_large || token_length == 0) return startup_failure("credential_size");
    std::string encoded_hash(token_bytes, token_length);
#if defined(NEXUS_CONTROL_REQUIRE_PROVISIONING_PIN)
    // Every pin-configured startup is bound, including owner-valid files.
    // Require the exact provisioned record and EOF, rather than trimming data.
    if ((token_stat.st_mode & 07777) != 0600 || encoded_hash.size() != 72 ||
        encoded_hash.back() != '\n') return startup_failure("credential_record");
    encoded_hash.pop_back();
    for (size_t i = 7; i < encoded_hash.size(); ++i)
        if (!((encoded_hash[i] >= '0' && encoded_hash[i] <= '9') ||
              (encoded_hash[i] >= 'a' && encoded_hash[i] <= 'f')))
            return startup_failure("credential_format");
#else
    encoded_hash = trim(encoded_hash);
#endif
    std::array<unsigned char, 32> expected_hash{};
    if (!parse_token_hash(encoded_hash, expected_hash)) return startup_failure("credential_format");
#if defined(NEXUS_CONTROL_REQUIRE_PROVISIONING_PIN)
    std::array<unsigned char, 32> provisioned_hash{};
    if (!parse_token_hash(provisioning_record, provisioned_hash)) return startup_failure("credential_pin");
    unsigned int difference = 0;
    for (size_t i = 0; i < expected_hash.size(); ++i)
        difference |= expected_hash[i] ^ provisioned_hash[i];
    if (difference != 0) return startup_failure("credential_pin");
#endif
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return startup_failure("socket", errno);
    const int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(allow_lan ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        const int error = errno;
        close(fd); return startup_failure("bind", error);
    }
    if (listen(fd, 4) != 0) {
        const int error = errno;
        close(fd); return startup_failure("listen", error);
    }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        const int error = errno;
        close(fd); return startup_failure("socket_address", error);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token_hash_ = expected_hash;
        port_ = ntohs(addr.sin_port);
        listener_ = fd;
        stopped_ = false;
        switching_ = false;
        restart_failed_ = false;
        pending_.reset();
        allow_lan_ = allow_lan;
    }
    thread_ = std::thread(&NexusControl::serve, this);
    return true;
}
void NexusControl::stop() {
    stopped_ = true;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    if (listener_ >= 0) close(listener_);
    listener_ = -1;
    token_hash_.fill(0);
}
void NexusControl::set_log_files(std::string app, std::string llama) {
    std::lock_guard<std::mutex> lock(mutex_);
    app_log_ = std::move(app);
    llama_log_ = std::move(llama);
}
unsigned short NexusControl::port() const { std::lock_guard<std::mutex> lock(mutex_); return port_; }
void NexusControl::publish(const Live & live, const std::vector<ModelRow> & models) {
    std::lock_guard<std::mutex> lock(mutex_);
    live_ = live;
    models_ = models;
}
std::optional<std::string> NexusControl::take_request() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto answer = pending_;
    pending_.reset();
    return answer;
}
void NexusControl::mark_restart_failed() {
    std::lock_guard<std::mutex> lock(mutex_);
    switching_ = false;
    restart_failed_ = true;
    pending_.reset();
}
void NexusControl::set_engine(const std::string & engine) {
    std::lock_guard<std::mutex> lock(mutex_);
    engine_ = engine == "media" ? "media" : "llm";
}
void NexusControl::serve() {
    while (!stopped_) {
        fd_set set; FD_ZERO(&set);
        int fd;
        { std::lock_guard<std::mutex> lock(mutex_); fd = listener_; }
        if (fd < 0) break;
        FD_SET(fd, &set);
        timeval timeout{}; timeout.tv_usec = 250000;
        if (select(fd+1, &set, nullptr, nullptr, &timeout) <= 0 || stopped_) continue;
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        const int client = accept(fd, reinterpret_cast<sockaddr *>(&peer), &peer_length);
        if (client < 0) continue;
        bool allow_lan;
        { std::lock_guard<std::mutex> lock(mutex_); allow_lan = allow_lan_; }
        // Refused peers are closed before any byte is read, as the media listener does.
        if (!peer_allowed_ipv4(ntohl(peer.sin_addr.s_addr), allow_lan)) { close(client); continue; }
        timeval io{}; io.tv_sec = 2;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof(io));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof(io));
        handle(client);
        close(client);
    }
}
void NexusControl::handle(int fd) {
    std::string request;
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() <= 8192) {
        const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) return;
        request.append(buffer, (size_t)got);
    }
    const size_t end = request.find("\r\n\r\n");
    if (end == std::string::npos || request.size() > 8192) {
        response(fd, 413, "{\"error\":\"headers too large\"}"); return;
    }
    if (request.size() != end + 4) {
        response(fd, 400, "{\"error\":\"unexpected request body\"}"); return;
    }
    std::istringstream lines(request.substr(0, end));
    std::string first; std::getline(lines, first);
    first = trim(first);
    std::istringstream start(first);
    std::string method, path, version, extra;
    if (!(start >> method >> path >> version) || start >> extra || version != "HTTP/1.1" ||
        (method != "GET" && method != "POST")) {
        response(fd, 400, "{\"error\":\"invalid request\"}"); return;
    }
    std::string supplied;
    bool auth_seen = false, has_body = false;
    for (std::string line; std::getline(lines, line);) {
        line = trim(line);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) { response(fd, 400, "{\"error\":\"invalid header\"}"); return; }
        std::string key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        const std::string value = trim(line.substr(colon + 1));
        if (key == "authorization") {
            if (auth_seen || value.rfind("Bearer ", 0) != 0) {
                response(fd, 401, "{\"error\":\"unauthorized\"}"); return;
            }
            auth_seen = true; supplied = value.substr(7);
        }
        if (key == "content-length") {
            if (has_body || value != "0") { response(fd, 400, "{\"error\":\"body not supported\"}"); return; }
            has_body = true;
        }
        if (key == "transfer-encoding") { response(fd, 400, "{\"error\":\"transfer encoding not supported\"}"); return; }
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto reply = [&](int code, const std::string & payload) {
            lock.unlock(); // never block the 60 FPS main thread on a slow HTTP client
            response(fd, code, payload);
        };
        if (!auth_seen || !token_matches(token_hash_, supplied)) {
            reply(401, "{\"error\":\"unauthorized\"}"); return;
        }
        if (path == "/v1/nexus/status" && method == "GET") {
            const bool ready = live_.state == ServerState::ready || live_.state == ServerState::generating;
            reply(200, "{\"state\":" + json_string(restart_failed_ ? "restart_failed" : (switching_ ? "switching" : state_name(live_.state))) +
                ",\"model_id\":" + json_string(live_.model_file.empty() ? "" : model_id(live_.model_file)) +
                ",\"model\":" + json_string(live_.model_label) +
                ",\"context_size\":" + std::to_string(live_.ctx) +
                ",\"context_used\":" + std::to_string(live_.ctx_used) +
                ",\"generation_tps\":" + json_number(live_.gen_tps) +
                ",\"prompt_tps\":" + json_number(live_.prompt_tps) +
                ",\"memory_pool_gib\":" + json_number(live_.pool_gib) +
                ",\"memory_free_gib\":" + json_number(live_.free_gib) +
                ",\"model_gib\":" + json_number(live_.model_gib) +
                ",\"kv_cache_gib\":" + json_number(live_.kv_gib) +
                ",\"load_progress\":" + (live_.load_progress < 0 ? "null" : json_number(live_.load_progress)) +
                ",\"engine\":" + json_string(engine_) +
                ",\"ready\":" + (ready && !switching_ && !restart_failed_ ? "true" : "false") + "}"); return;
        }
        if (method == "GET" && path.rfind("/v1/nexus/logs/", 0) == 0) {
            // The caller picks a source name and a cursor, never a path.
            const size_t prefix = std::string("/v1/nexus/logs/").size();
            const size_t mark = path.find('?');
            const std::string source = path.substr(prefix, mark == std::string::npos ? mark : mark - prefix);
            long after = -1;
            if (mark != std::string::npos) {
                const std::string query = path.substr(mark + 1);
                const std::string digits = query.rfind("after=", 0) == 0 ? query.substr(6) : "";
                if (digits.empty() || digits.size() > 18 ||
                    !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) {
                    reply(400, "{\"error\":\"invalid cursor\"}"); return;
                }
                after = std::stol(digits);
            }
            const std::string file = source == "app" ? app_log_ : source == "llama" ? llama_log_ : "";
            if (file.empty()) { reply(404, "{\"error\":\"unknown endpoint\"}"); return; }
            lock.unlock(); // the file read stays off the mutex the main thread publishes under
            response(fd, 200, log_page(file, source, after)); return;
        }
        if (path == "/v1/nexus/models" && method == "GET") {
            std::string body = "{\"models\":[";
            for (size_t i = 0; i < models_.size(); ++i) {
                const auto & model = models_[i];
                if (i) body += ',';
                body += "{\"id\":" + json_string(model_id(model.file)) +
                    ",\"label\":" + json_string(model.label) +
                    ",\"drive\":" + json_string(model.drive) +
                    ",\"architecture\":" + (model.arch.empty() || model.arch.size() > 64 ||
                        !std::all_of(model.arch.begin(), model.arch.end(), [](unsigned char c) {
                            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
                        }) ? "null" : json_string(model.arch)) +
                    ",\"file_size_bytes\":" + (model.file_bytes > 0 && model.file_bytes <= 9007199254740991ULL ? std::to_string(model.file_bytes) : "null") +
                    ",\"quantization_type\":" + (model.file_type.has_value() && *model.file_type != 1024 ? std::to_string(*model.file_type) : "null") +
                    ",\"fits\":" + (model.fits ? "true" : "false") +
                    ",\"current\":" + (model.current ? "true" : "false") + "}";
            }
            reply(200, body + "]}"); return;
        }
        if (method == "POST" && (path == "/v1/nexus/models/unload" ||
             (path.rfind("/v1/nexus/models/", 0) == 0 && path.size() == std::string("/v1/nexus/models/").size() + 16 + 5 &&
              path.substr(path.size()-5) == "/load"))) {
            if (switching_ || pending_) { reply(409, "{\"error\":\"model switch already pending\"}"); return; }
            // Never deliberately interrupt an inference the native app has
            // reported as active. Cross-process scheduling is also required:
            // this sampled state alone cannot close the race with new requests.
            if (live_.state == ServerState::generating) {
                reply(409, "{\"error\":\"inference is active\"}"); return;
            }
            const bool unload = path == "/v1/nexus/models/unload";
            std::string chosen = "none";
            if (!unload) {
                const std::string id = path.substr(std::string("/v1/nexus/models/").size(), 16);
                size_t matches = 0;
                for (const auto & model : models_) {
                    if (model.fits && model_id(model.file) == id) { chosen = model.file; ++matches; }
                }
                if (matches != 1) { reply(404, "{\"error\":\"unknown or incompatible model\"}"); return; }
                if (model_id(live_.model_file) == id) {
                    reply(409, "{\"error\":\"model already selected\"}"); return;
                }
            }
            pending_ = chosen;
            switching_ = true;
            restart_failed_ = false;
            reply(202, "{\"status\":\"switching\",\"restart_required\":true}"); return;
        }
        if (method == "POST" && (path == "/v1/nexus/engine/media" || path == "/v1/nexus/engine/llm")) {
            // Same refusal rules as the model load above.
            if (switching_ || pending_) { reply(409, "{\"error\":\"model switch already pending\"}"); return; }
            if (live_.state == ServerState::generating) {
                reply(409, "{\"error\":\"inference is active\"}"); return;
            }
            pending_ = path == "/v1/nexus/engine/media" ? "engine:media" : "engine:llm";
            switching_ = true;
            restart_failed_ = false;
            reply(202, "{\"status\":\"switching\",\"restart_required\":true}"); return;
        }
        reply(method == "GET" || method == "POST" ? 404 : 405, "{\"error\":\"unknown endpoint\"}");
    }
}
} // namespace ps5lm
