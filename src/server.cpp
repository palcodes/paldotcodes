// server.cpp — a small, fast static file server. No dependencies.
//
// The whole site is loaded into RAM at startup (a personal site is a few MB
// at most); every request is served from memory with a precomputed ETag.
// Conditional requests get 304s. Clean URLs: /words/foo/ -> words/foo/index.html.
#include "server.hpp"
#include "build.hpp"
#include "common.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

namespace {

struct Entry {
    std::string body;
    std::string mime;
    std::string etag;
    bool immutable = false;
};

using Cache = std::unordered_map<std::string, std::shared_ptr<const Entry>>;

std::shared_mutex g_mu;
std::shared_ptr<const Cache> g_cache;

// Each connection gets a thread; past this many, new ones are dropped so a
// flood of idle sockets can't exhaust the machine.
constexpr int kMaxConns = 256;
std::atomic<int> g_conns{0};

// A request head must arrive within this long, however slowly it trickles in.
constexpr auto kHeadDeadline = std::chrono::seconds(10);

const char *mime_for(const std::string &path) {
    static const std::unordered_map<std::string, const char *> m = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
        {".js", "application/javascript"},     {".svg", "image/svg+xml"},
        {".png", "image/png"},                 {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},               {".gif", "image/gif"},
        {".webp", "image/webp"},               {".avif", "image/avif"},
        {".ico", "image/x-icon"},              {".xml", "application/xml; charset=utf-8"},
        {".txt", "text/plain; charset=utf-8"}, {".org", "text/plain; charset=utf-8"},
        {".woff", "font/woff"},                {".woff2", "font/woff2"},
        {".ttf", "font/ttf"},                  {".pdf", "application/pdf"},
        {".mp4", "video/mp4"},                 {".json", "application/json"},
        {".webmanifest", "application/manifest+json"},
    };
    size_t dot = path.rfind('.');
    if (dot != std::string::npos) {
        auto it = m.find(lower(path.substr(dot)));
        if (it != m.end()) return it->second;
    }
    return "application/octet-stream";
}

std::shared_ptr<const Cache> load_cache(const fs::path &dir) {
    auto cache = std::make_shared<Cache>();
    if (!fs::exists(dir)) return cache;
    for (auto &e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        std::string rel = fs::relative(e.path(), dir).generic_string();
        auto ent = std::make_shared<Entry>();
        ent->body = read_file(e.path());
        ent->mime = mime_for(rel);
        char et[32];
        std::snprintf(et, sizeof et, "\"%016llx\"", (unsigned long long)fnv1a(ent->body));
        ent->etag = et;
        ent->immutable = !ends_with(rel, ".html") && !ends_with(rel, ".xml") &&
                         !ends_with(rel, ".txt") && !ends_with(rel, ".css");
        (*cache)["/" + rel] = ent;
    }
    return cache;
}

std::string url_decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int a = hex(s[i + 1]), b = hex(s[i + 2]);
            if (a >= 0 && b >= 0) { out.push_back((char)(a * 16 + b)); i += 2; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}

// normalize: strip query, decode, resolve segments, reject traversal
std::string normalize_path(std::string p) {
    size_t q = p.find_first_of("?#");
    if (q != std::string::npos) p = p.substr(0, q);
    p = url_decode(p);
    if (p.empty() || p[0] != '/') return "";
    std::vector<std::string> segs;
    std::string seg;
    bool trailing_slash = p.back() == '/';
    for (size_t i = 1; i <= p.size(); i++) {
        if (i == p.size() || p[i] == '/') {
            if (seg == "..") { if (segs.empty()) return ""; segs.pop_back(); }
            else if (!seg.empty() && seg != ".") segs.push_back(seg);
            seg.clear();
        } else seg.push_back(p[i]);
    }
    std::string out = "/";
    for (size_t i = 0; i < segs.size(); i++) {
        out += segs[i];
        if (i + 1 < segs.size()) out += "/";
    }
    if (trailing_slash && out.back() != '/') out += "/";
    return out;
}

void send_all(sock_t c, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = (int)send(c, data + sent, (int)(len - sent), 0);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

void respond(sock_t c, int code, const char *status, const Entry *ent,
             bool head, bool keep_alive, bool not_modified) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n";
    h += "X-Content-Type-Options: nosniff\r\n"
         "X-Frame-Options: DENY\r\n"
         "Referrer-Policy: strict-origin-when-cross-origin\r\n";
    if (ent) {
        h += "Content-Type: " + ent->mime + "\r\n";
        h += "ETag: " + ent->etag + "\r\n";
        h += ent->immutable ? "Cache-Control: public, max-age=86400\r\n"
                            : "Cache-Control: no-cache\r\n";
    }
    size_t body_len = (ent && !not_modified) ? ent->body.size() : 0;
    h += "Content-Length: " + std::to_string(not_modified ? 0 : body_len) + "\r\n";
    h += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    h += "\r\n";
    send_all(c, h.data(), h.size());
    if (!head && !not_modified && ent) send_all(c, ent->body.data(), ent->body.size());
}

void redirect(sock_t c, const std::string &to, bool keep_alive) {
    std::string h = "HTTP/1.1 301 Moved Permanently\r\nLocation: " + to +
                    "\r\nContent-Length: 0\r\nConnection: " +
                    (keep_alive ? "keep-alive" : "close") + "\r\n\r\n";
    send_all(c, h.data(), h.size());
}

// True if the request head declares a body: any Transfer-Encoding, or a
// Content-Length other than 0. Header names are compared after trimming, so
// padding tricks like "Content-Length :" count as declaring one too.
bool has_body(const std::string &head) {
    size_t pos = head.find("\r\n"); // skip the request line
    while (pos != std::string::npos) {
        pos += 2;
        size_t end = head.find("\r\n", pos);
        std::string line = head.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = lower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        if (name == "transfer-encoding") return true;
        if (name == "content-length" && value != "0") return true;
    }
    return false;
}

void handle_client(sock_t c) {
#ifdef _WIN32
    DWORD tv = 8000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
#else
    struct timeval tv{8, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);

    std::string buf;
    char tmp[8192];
    for (int served = 0; served < 200; served++) {
        // read one request head
        size_t head_end;
        auto deadline = std::chrono::steady_clock::now() + kHeadDeadline;
        while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
            if (buf.size() > 32768 || std::chrono::steady_clock::now() > deadline) {
                CLOSESOCK(c);
                return;
            }
            int n = (int)recv(c, tmp, sizeof tmp, 0);
            if (n <= 0) { CLOSESOCK(c); return; }
            buf.append(tmp, (size_t)n);
        }
        std::string head = buf.substr(0, head_end);
        buf.erase(0, head_end + 4);

        // request line
        size_t le = head.find("\r\n");
        std::string reqline = le == std::string::npos ? head : head.substr(0, le);
        size_t sp1 = reqline.find(' ');
        size_t sp2 = reqline.rfind(' ');
        if (sp1 == std::string::npos || sp2 == sp1) { CLOSESOCK(c); return; }
        std::string method = reqline.substr(0, sp1);
        std::string target = reqline.substr(sp1 + 1, sp2 - sp1 - 1);
        bool http10 = reqline.find("HTTP/1.0") != std::string::npos;

        std::string lhead = lower(head);
        bool keep_alive = !http10;
        if (lhead.find("connection: close") != std::string::npos) keep_alive = false;
        if (http10 && lhead.find("connection: keep-alive") != std::string::npos) keep_alive = true;

        std::string inm;
        size_t ip = lhead.find("if-none-match:");
        if (ip != std::string::npos) {
            size_t end = head.find("\r\n", ip);
            inm = trim(head.substr(ip + 14, end - ip - 14));
        }

        // Nothing here takes a request body, and bodies are never read: an
        // unread body would sit in `buf` and be parsed as the next request,
        // letting one client smuggle a response onto a proxy connection that
        // another visitor's request reuses. Refuse any request that has one.
        if (has_body(head)) {
            std::string h = "HTTP/1.1 400 Bad Request\r\n"
                            "Content-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(c, h.data(), h.size());
            CLOSESOCK(c);
            return;
        }

        bool is_head = method == "HEAD";
        if (method != "GET" && !is_head) {
            std::string h = "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\n"
                            "Content-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(c, h.data(), h.size());
            CLOSESOCK(c);
            return;
        }

        std::string path = normalize_path(target);
        std::shared_ptr<const Cache> cache;
        {
            std::shared_lock lk(g_mu);
            cache = g_cache;
        }

        const Entry *ent = nullptr;
        std::shared_ptr<const Entry> hold;
        if (!path.empty()) {
            auto lookup = [&](const std::string &k) -> bool {
                auto it = cache->find(k);
                if (it == cache->end()) return false;
                hold = it->second;
                ent = hold.get();
                return true;
            };
            if (!lookup(path)) {
                if (path.back() == '/') {
                    lookup(path + "index.html");
                } else if (cache->count(path + "/index.html")) {
                    redirect(c, path + "/", keep_alive);
                    if (!keep_alive) { CLOSESOCK(c); return; }
                    continue;
                }
            }
        }

        if (!ent) {
            auto it = cache->find("/404.html");
            const Entry *nf = it != cache->end() ? it->second.get() : nullptr;
            Entry empty{ "404 not found\n", "text/plain; charset=utf-8", "\"0\"", false };
            respond(c, 404, "Not Found", nf ? nf : &empty, is_head, keep_alive, false);
        } else if (!inm.empty() && inm == ent->etag) {
            respond(c, 304, "Not Modified", ent, is_head, keep_alive, true);
        } else {
            respond(c, 200, "OK", ent, is_head, keep_alive, false);
        }
        if (!keep_alive) { CLOSESOCK(c); return; }
    }
    CLOSESOCK(c);
}

// newest mtime under the watched trees
fs::file_time_type scan_mtime(const fs::path &root) {
    // note: default-constructed file_time_type is NOT the minimum on libstdc++
    auto latest = fs::file_time_type::min();
    auto visit = [&](const fs::path &p) {
        std::error_code ec;
        if (!fs::exists(p, ec)) return;
        if (fs::is_regular_file(p, ec)) {
            auto t = fs::last_write_time(p, ec);
            if (!ec && t > latest) latest = t;
            return;
        }
        for (auto &e : fs::recursive_directory_iterator(p, ec)) {
            if (!e.is_regular_file(ec)) continue;
            auto t = fs::last_write_time(e.path(), ec);
            if (!ec && t > latest) latest = t;
        }
    };
    visit(root / "content");
    visit(root / "templates");
    visit(root / "static");
    visit(root / "site.conf");
    return latest;
}


// Stamp contents, or "" if it doesn't exist (yet, or mid-publish).
std::string read_stamp(const std::string &path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return "";
    try { return read_file(path); } catch (...) { return ""; }
}

void swap_cache(const std::string &dir, const char *what) {
    std::shared_ptr<const Cache> fresh;
    try {
        fresh = load_cache(dir);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s: reload failed, keeping old site (%s)\n", what, e.what());
        return;
    }
    {
        std::unique_lock lk(g_mu);
        g_cache = fresh;
    }
    std::printf("%s: reloaded (%zu files)\n", what, fresh->size());
    std::fflush(stdout);
}

} // namespace

int run_serve(const ServeOpts &o) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    {
        std::unique_lock lk(g_mu);
        g_cache = load_cache(o.dir);
    }
    std::printf("serve: %zu files cached in RAM from %s\n", g_cache->size(), o.dir.c_str());

    if (!o.watch_root.empty()) {
        std::thread([o] {
            std::printf("dev: watching %s for changes\n", o.watch_root.c_str());
            std::fflush(stdout);
            auto last = scan_mtime(o.watch_root);
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                auto now = scan_mtime(o.watch_root);
                if (now != last) {
                    last = now;
                    run_build(o.watch_root, true);
                    swap_cache(o.dir, "dev");
                }
            }
        }).detach();
    }

    // `palsite publish` rewrites the stamp only after a successful build, so a
    // changed, non-empty stamp means dist/ is complete and safe to load.
    if (!o.stamp.empty()) {
        std::thread([o] {
            std::printf("serve: reloading whenever %s changes\n", o.stamp.c_str());
            std::fflush(stdout);
            std::string last = read_stamp(o.stamp);
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::string now = read_stamp(o.stamp);
                if (!now.empty() && now != last) {
                    last = now;
                    swap_cache(o.dir, "publish");
                }
            }
        }).detach();
    }

    // Loopback-only by default: in production a tunnel (Tailscale Funnel)
    // is the only thing that should reach this port. --lan opens it up.
    int one = 1;
    sock_t srv = INVALID_SOCKET;
    bool ok = false;
    if (o.lan) {
        // dual-stack socket: one listener for both IPv6 and IPv4
        srv = socket(AF_INET6, SOCK_STREAM, 0);
        if (srv != INVALID_SOCKET) {
            int zero = 0;
            setsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof zero);
            setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
            sockaddr_in6 a6{};
            a6.sin6_family = AF_INET6;
            a6.sin6_addr = in6addr_any;
            a6.sin6_port = htons((unsigned short)o.port);
            ok = bind(srv, (sockaddr *)&a6, sizeof a6) == 0;
            if (!ok) CLOSESOCK(srv);
        }
    }
    if (!ok) {
        srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv == INVALID_SOCKET) { std::fprintf(stderr, "socket() failed\n"); return 1; }
#ifdef _WIN32
        // Windows' SO_REUSEADDR lets a second process steal the port; the
        // exclusive flag is the equivalent of the POSIX default.
        setsockopt(srv, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(o.lan ? INADDR_ANY : INADDR_LOOPBACK);
        addr.sin_port = htons((unsigned short)o.port);
        if (bind(srv, (sockaddr *)&addr, sizeof addr) != 0) {
            std::fprintf(stderr, "bind() failed on port %d (already in use?)\n", o.port);
            return 1;
        }
    }
    if (listen(srv, 64) != 0) { std::fprintf(stderr, "listen() failed\n"); return 1; }
    std::printf("serve: http://%s:%d\n", o.lan ? "0.0.0.0" : "127.0.0.1", o.port);
    std::fflush(stdout);

    for (;;) {
        sock_t client = accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        if (g_conns.fetch_add(1) >= kMaxConns) {
            g_conns.fetch_sub(1);
            CLOSESOCK(client);
            continue;
        }
        std::thread([client] {
            handle_client(client);
            g_conns.fetch_sub(1);
        }).detach();
    }
}
