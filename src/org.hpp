// org.hpp — org-mode -> HTML converter.
#pragma once
#include <string>
#include <map>

struct OrgDoc {
    std::map<std::string, std::string> meta; // lower-cased #+KEY: value pairs
    std::string html;                        // rendered article body
    bool has_math = false;
};

OrgDoc org_to_html(const std::string &src);
