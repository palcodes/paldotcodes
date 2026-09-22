// server.hpp — minimal static HTTP server serving dist/ from RAM.
#pragma once
#include <string>

struct ServeOpts {
    std::string dir;         // directory to serve (dist/)
    int port = 8080;
    bool lan = false;        // bind all interfaces instead of loopback only
    std::string watch_root;  // dev: poll sources, rebuild + hot-reload on change
    std::string stamp;       // serve: hot-reload dir whenever this file changes
};

// Serve opts.dir on opts.port. Blocks forever.
int run_serve(const ServeOpts &opts);
