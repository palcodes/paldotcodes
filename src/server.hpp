// server.hpp — minimal static HTTP server serving dist/ from RAM.
#pragma once
#include <string>

// Serve `dir` on `port`. If watch_root is non-empty, poll it for changes,
// rebuild and hot-reload the in-memory cache (dev mode). Blocks forever.
int run_serve(const std::string &dir, int port, const std::string &watch_root);
