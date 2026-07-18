// main.cpp — pal.codes site tool.
//
//   site build            render content/ -> dist/
//   site serve [-p 8080]  serve dist/ (production: static, in-RAM, fast)
//   site dev   [-p 8080]  build + serve + rebuild on change
//   site new "Title"      scaffold a new article in content/words/
#include "build.hpp"
#include "server.hpp"
#include "common.hpp"
#include <cstdio>
#include <cstring>
#include <ctime>

static void usage() {
    std::puts("usage: site <build | serve | dev | new \"Title\"> [options]\n"
              "  build             render content/ into dist/\n"
              "  serve [-p PORT]   serve dist/ (default port 8080)\n"
              "  dev   [-p PORT]   build, serve, and rebuild on file changes\n"
              "  new \"Title\"       create content/words/<slug>.org\n"
              "  -C DIR            run as if started in DIR");
}

int main(int argc, char **argv) {
    std::string root = ".";
    int port = 8080;
    std::string cmd;
    std::vector<std::string> rest;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-p" || a == "--port") {
            if (i + 1 < argc) port = std::atoi(argv[++i]);
        } else if (a == "-C") {
            if (i + 1 < argc) root = argv[++i];
        } else if (cmd.empty()) cmd = a;
        else rest.push_back(a);
    }

    if (cmd == "build") return run_build(root, false);
    if (cmd == "serve") return run_serve((fs::path(root) / "dist").string(), port, "");
    if (cmd == "dev") {
        if (run_build(root, false) != 0) return 1;
        return run_serve((fs::path(root) / "dist").string(), port, root);
    }
    if (cmd == "new") {
        if (rest.empty()) { std::fprintf(stderr, "site new \"Title\"\n"); return 1; }
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
