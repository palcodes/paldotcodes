// build.cpp — reads content/*.org, renders through templates/, writes dist/.
//
// Templates are plain HTML with {{placeholder}} substitution and
// {{include:file.html}} partials — edit them freely, no recompile needed.
#include "build.hpp"
#include "org.hpp"
#include "common.hpp"
#include <chrono>
#include <cstdio>
#include <ctime>

namespace {

struct SiteConf {
    std::map<std::string, std::string> kv;
    std::vector<std::pair<std::string, std::string>> redirects;
    std::string get(const std::string &k, const std::string &def = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
};

SiteConf load_conf(const fs::path &root) {
    SiteConf c;
    for (auto &line : split_lines(read_file(root / "site.conf"))) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (starts_with(t, "redirect ")) {
            std::string rest = trim(t.substr(9));
            size_t sp = rest.find(' ');
            if (sp != std::string::npos)
                c.redirects.push_back({trim(rest.substr(0, sp)), trim(rest.substr(sp + 1))});
            continue;
        }
        size_t eq = t.find('=');
        if (eq != std::string::npos)
            c.kv[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }
    return c;
}

// {{include:name.html}} expansion, recursive
std::string load_template(const fs::path &tdir, const std::string &name, int depth = 0) {
    std::string t = read_file(tdir / name);
    if (depth > 8) return t;
    std::string out;
    size_t i = 0;
    while (i < t.size()) {
        size_t open = t.find("{{include:", i);
        if (open == std::string::npos) { out += t.substr(i); break; }
        out += t.substr(i, open - i);
        size_t close = t.find("}}", open);
        if (close == std::string::npos) { out += t.substr(open); break; }
        std::string inc = trim(t.substr(open + 10, close - open - 10));
        out += load_template(tdir, inc, depth + 1);
        i = close + 2;
    }
    return out;
}

std::string render(std::string tpl, const std::map<std::string, std::string> &vars) {
    for (auto &kv : vars) {
        std::string key = "{{" + kv.first + "}}";
        size_t pos = 0;
        while ((pos = tpl.find(key, pos)) != std::string::npos) {
            tpl.replace(pos, key.size(), kv.second);
            pos += kv.second.size();
        }
    }
    return tpl;
}

struct Article {
    std::string slug, title, subtitle, date, html, description;
    bool has_math = false;
};

// "2024-02-10" -> "February 10, 2024"
std::string pretty_date(const std::string &iso) {
    static const char *months[] = {"January", "February", "March", "April", "May", "June",
                                   "July", "August", "September", "October", "November", "December"};
    if (iso.size() < 10) return iso;
    int m = std::atoi(iso.substr(5, 2).c_str());
    int d = std::atoi(iso.substr(8, 2).c_str());
    if (m < 1 || m > 12) return iso;
    return std::string(months[m - 1]) + " " + std::to_string(d) + ", " + iso.substr(0, 4);
}

std::string strip_tags(const std::string &html, size_t maxlen) {
    std::string out;
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) {
            out.push_back(c == '\n' ? ' ' : c);
            if (out.size() >= maxlen) break;
        }
    }
    return trim(out);
}

void copy_tree(const fs::path &from, const fs::path &to) {
    if (!fs::exists(from)) return;
    for (auto &e : fs::recursive_directory_iterator(from)) {
        if (!e.is_regular_file()) continue;
        fs::path rel = fs::relative(e.path(), from);
        fs::path dest = to / rel;
        fs::create_directories(dest.parent_path());
        fs::copy_file(e.path(), dest, fs::copy_options::overwrite_existing);
    }
}

} // namespace

int run_build(const std::string &root_s, bool quiet) {
    auto t0 = std::chrono::steady_clock::now();
    fs::path root = root_s;
    fs::path dist = root / "dist";
    fs::path tdir = root / "templates";
    SiteConf conf = load_conf(root);

    // Wipe dist/ first: without this, articles that go back to draft, get
    // renamed, or get deleted leave their old rendered page reachable.
    std::error_code ec;
    fs::remove_all(dist, ec);

    std::map<std::string, std::string> site_vars = {
        {"site_title", conf.get("title", "aayush pal")},
        {"site_url", conf.get("url", "")},
        {"site_author", conf.get("author", "")},
        {"site_description", conf.get("description", "")},
        {"year", [] {
             std::time_t t = std::time(nullptr);
             std::tm tm{};
#ifdef _WIN32
             localtime_s(&tm, &t);
#else
             localtime_r(&t, &tm);
#endif
             return std::to_string(tm.tm_year + 1900);
         }()},
    };

    // ---- articles: content/words/*.org -----------------------------------
    std::vector<Article> articles;
    fs::path words = root / "content" / "words";
    if (fs::exists(words)) {
        for (auto &e : fs::directory_iterator(words)) {
            if (!e.is_regular_file() || e.path().extension() != ".org") continue;
            OrgDoc doc = org_to_html(read_file(e.path()));
            if (lower(doc.meta["draft"]) == "t" || lower(doc.meta["draft"]) == "true") continue;
            Article a;
            a.slug = e.path().stem().string();
            a.title = doc.meta.count("title") ? doc.meta["title"] : a.slug;
            a.subtitle = doc.meta["subtitle"];
            a.date = doc.meta["date"];
            // strip org's <2024-02-10 Sat> brackets if present
            if (!a.date.empty() && a.date.front() == '<') {
                a.date = a.date.substr(1, a.date.find_first_of(" >") - 1);
            }
            a.html = doc.html;
            a.has_math = doc.has_math;
            a.description = doc.meta.count("description") ? doc.meta["description"]
                                                          : strip_tags(doc.html, 160);
            articles.push_back(std::move(a));
        }
    }
    std::sort(articles.begin(), articles.end(),
              [](const Article &x, const Article &y) { return x.date > y.date; });

    std::string article_tpl = load_template(tdir, "article.html");
    for (auto &a : articles) {
        auto vars = site_vars;
        vars["title"] = html_escape(a.title);
        vars["subtitle"] = a.subtitle.empty() ? "" : "<p class=\"subtitle\">" + html_escape(a.subtitle) + "</p>";
        vars["date"] = a.date;
        vars["date_pretty"] = pretty_date(a.date);
        vars["description"] = html_escape(a.description);
        vars["content"] = a.html;
        vars["url"] = conf.get("url") + "/words/" + a.slug + "/";
        write_file(dist / "words" / a.slug / "index.html", render(article_tpl, vars));
    }

    // ---- words listing ---------------------------------------------------
    {
        std::string items;
        std::string cur_year;
        for (auto &a : articles) {
            std::string y = a.date.size() >= 4 ? a.date.substr(0, 4) : "";
            if (y != cur_year) {
                if (!cur_year.empty()) items += "</ul>\n";
                items += "<h2 class=\"year\">" + y + "</h2>\n<ul class=\"word-list\">\n";
                cur_year = y;
            }
            items += "<li><a href=\"/words/" + a.slug + "/\"><span class=\"wl-date\">" +
                     a.date + "</span><span class=\"wl-title\">" + html_escape(a.title) + "</span>";
            if (!a.subtitle.empty())
                items += "<span class=\"wl-sub\">" + html_escape(a.subtitle) + "</span>";
            items += "</a></li>\n";
        }
        if (!cur_year.empty()) items += "</ul>\n";
        auto vars = site_vars;
        vars["items"] = items;
        write_file(dist / "words" / "index.html", render(load_template(tdir, "list.html"), vars));
    }

    // ---- standalone pages: content/pages/*.org ---------------------------
    fs::path pages = root / "content" / "pages";
    std::string page_tpl = load_template(tdir, "page.html");
    if (fs::exists(pages)) {
        for (auto &e : fs::directory_iterator(pages)) {
            if (!e.is_regular_file() || e.path().extension() != ".org") continue;
            OrgDoc doc = org_to_html(read_file(e.path()));
            std::string name = e.path().stem().string();
            auto vars = site_vars;
            vars["title"] = html_escape(doc.meta.count("title") ? doc.meta["title"] : name);
            vars["description"] = html_escape(doc.meta.count("description") ? doc.meta["description"]
                                                                            : strip_tags(doc.html, 160));
            vars["content"] = doc.html;
            write_file(dist / name / "index.html", render(page_tpl, vars));
        }
    }

    // ---- landing page ----------------------------------------------------
    {
        std::string recent;
        int shown = 0;
        for (auto &a : articles) {
            if (shown++ >= 5) break;
            recent += "<a class=\"recent-item\" href=\"/words/" + a.slug + "/\">" +
                      "<span class=\"recent-date\">" + a.date + "</span> " +
                      html_escape(a.title) + "</a>\n";
        }
        auto vars = site_vars;
        vars["recent"] = recent;
        write_file(dist / "index.html", render(load_template(tdir, "index.html"), vars));
    }

    // ---- 404, feed, robots, redirects ------------------------------------
    write_file(dist / "404.html", render(load_template(tdir, "404.html"), site_vars));
    write_file(dist / "robots.txt", "User-agent: *\nAllow: /\n");

    {
        std::string url = conf.get("url");
        std::string feed = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                           "<feed xmlns=\"http://www.w3.org/2005/Atom\">\n"
                           "<title>" + html_escape(conf.get("title")) + "</title>\n"
                           "<link href=\"" + url + "/\"/>\n"
                           "<link rel=\"self\" href=\"" + url + "/feed.xml\"/>\n"
                           "<id>" + url + "/</id>\n";
        if (!articles.empty()) feed += "<updated>" + articles.front().date + "T00:00:00Z</updated>\n";
        for (auto &a : articles) {
            feed += "<entry><title>" + html_escape(a.title) + "</title>"
                    "<link href=\"" + url + "/words/" + a.slug + "/\"/>"
                    "<id>" + url + "/words/" + a.slug + "/</id>"
                    "<updated>" + a.date + "T00:00:00Z</updated>"
                    "<summary>" + html_escape(a.description) + "</summary>"
                    "<content type=\"html\">" + html_escape(a.html) + "</content>"
                    "</entry>\n";
        }
        feed += "</feed>\n";
        write_file(dist / "feed.xml", feed);
    }

    for (auto &r : conf.redirects) {
        std::string to = r.second;
        std::string stub = "<!doctype html><meta charset=\"utf-8\">"
                           "<meta http-equiv=\"refresh\" content=\"0; url=" + to + "\">"
                           "<link rel=\"canonical\" href=\"" + to + "\">"
                           "<a href=\"" + to + "\">moved here</a>\n";
        std::string from = r.first;
        if (!from.empty() && from[0] == '/') from = from.substr(1);
        write_file(dist / fs::path(from), stub);
    }

    // ---- static assets ---------------------------------------------------
    copy_tree(root / "static", dist);

    if (!quiet) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        std::printf("build: %zu articles -> dist/ in %lldms\n", articles.size(), (long long)ms);
    }
    return 0;
}
