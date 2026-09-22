// main.cpp — pal.codes site tool.
//
//   palsite build            render content/ -> dist/
//   palsite publish          build, then signal a running `serve` to reload
//   palsite serve [-p 8080]  serve dist/ (production: static, in-RAM, fast)
//   palsite dev   [-p 8080]  build + serve + rebuild on change
//   palsite new "Title"      scaffold a new article in content/words/
#include "build.hpp"
#include "server.hpp"
#include "common.hpp"
#include <cstdio>
#include <cstring>
#include <ctime>

static void usage() {
    std::puts("usage: palsite <build | publish | serve | dev | new \"Title\"> [options]\n"
              "  build             render content/ into dist/\n"
              "  publish           build, then make a running `serve` reload it\n"
              "  serve [-p PORT]   serve dist/ (default port 8080)\n"
              "  dev   [-p PORT]   build, serve, and rebuild on file changes\n"
              "  new \"Title\"       create content/words/<slug>.org\n"
              "  -C DIR            run as if started in DIR\n"
              "  --lan             listen on all interfaces, not just 127.0.0.1");
}

int main(int argc, char **argv) {
    std::string root = ".";
    int port = 8080;
    bool lan = false;
    std::string cmd;
    std::vector<std::string> rest;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-p" || a == "--port") {
            if (i + 1 < argc) port = std::atoi(argv[++i]);
        } else if (a == "--lan") {
            lan = true;
        } else if (a == "-C") {
            if (i + 1 < argc) root = argv[++i];
        } else if (cmd.empty()) cmd = a;
        else rest.push_back(a);
    }

    ServeOpts so;
    so.dir = (fs::path(root) / "dist").string();
    so.port = port;
    so.lan = lan;
    // `publish` rewrites this after a good build; `serve` reloads when it changes
    fs::path stamp = fs::path(root) / ".published";

    if (cmd == "build") return run_build(root, false);
    if (cmd == "publish") {
        if (run_build(root, false) != 0) return 1;
        write_file(stamp, std::to_string(std::time(nullptr)) + "\n");
        std::printf("published; a running `palsite serve` picks it up within a second\n");
        return 0;
    }
    if (cmd == "serve") {
        so.stamp = stamp.string();
        return run_serve(so);
    }
    if (cmd == "dev") {
        if (run_build(root, false) != 0) return 1;
        so.watch_root = root;
        return run_serve(so);
    }
    if (cmd == "new") {
        if (rest.empty()) { std::fprintf(stderr, "palsite new \"Title\"\n"); return 1; }
        std::string title = rest[0];
        std::string slug = slugify(title);
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char date[16];
        std::strftime(date, sizeof date, "%Y-%m-%d", &tm);
        fs::path p = fs::path(root) / "content" / "words" / (slug + ".org");
        if (fs::exists(p)) { std::fprintf(stderr, "%s already exists\n", p.string().c_str()); return 1; }
        write_file(p, "#+TITLE: " + title + "\n#+DATE: " + std::string(date) +
                          "\n#+SUBTITLE: \n#+DRAFT: t\n\nWrite here.\n");
        std::printf("created %s\n", p.string().c_str());
        return 0;
    }
    usage();
    return cmd.empty() ? 1 : (cmd == "help" || cmd == "--help" ? 0 : 1);
}
