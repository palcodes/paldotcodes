// build.hpp — static site generation.
#pragma once
#include <string>

// Build the site from content/ + templates/ + static/ into dist/.
// Returns 0 on success. root is the project directory.
int run_build(const std::string &root, bool quiet);
