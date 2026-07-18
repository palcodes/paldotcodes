// org.cpp — a line-oriented org-mode parser producing semantic HTML.
//
// Supported org syntax (the subset Emacs writes by hand):
//   #+TITLE/#+DATE/#+SUBTITLE/...     document metadata
//   * ** *** headings                 -> h2..h5 with anchor ids
//   *bold* /italic/ _underline_ +strike+ ~code~ =verbatim=
//   [[url][desc]] links, bare http(s) auto-links, [[file:img.png]] images
//   #+CAPTION / #+ATTR_HTML           figure captions and attributes
//   - lists, 1. lists, - term :: desc lists, [ ]/[X] checkboxes, nesting
//   #+BEGIN_SRC / EXAMPLE / QUOTE / EXPORT html / any other -> <div class>
//   | tables |  with |---| header separators
//   \( \) \[ \] $$ $$ math            -> MathML at build time
//   [fn:1] + [fn:1] def               -> numbered margin sidenotes (citations)
//   [fn::text]                        -> unnumbered margin notes
#include "org.hpp"
#include "mathml.hpp"
#include "common.hpp"
#include <set>

namespace {

struct Ctx {
    std::map<std::string, std::string> fndefs;  // label -> raw def text
    std::map<std::string, int> fnnum;           // label -> assigned number
    int next_fn = 1;
    int uniq = 0;                               // unique ids for toggle checkboxes
    bool has_math = false;
};

std::string fmt_inline(const std::string &s, Ctx &ctx, bool allow_fn = true);

bool is_space_c(char c) { return c == ' ' || c == '\t'; }

bool emph_pre_ok(const std::string &s, size_t i) {
    if (i == 0) return true;
    char c = s[i - 1];
    return is_space_c(c) || c == '(' || c == '{' || c == '\'' || c == '"' || c == '-' ||
           c == '[' || c == '>' || c == '\n';
}
bool emph_post_ok(const std::string &s, size_t j) {
    if (j >= s.size()) return true;
    char c = s[j];
    return is_space_c(c) || c == '.' || c == ',' || c == ';' || c == ':' || c == '!' ||
           c == '?' || c == ')' || c == '}' || c == '\'' || c == '"' || c == '-' ||
           c == ']' || c == '<' || c == '\n' || c == '[';
}

// find the closing emphasis marker for s[i]==marker; returns npos if invalid
size_t find_emph_close(const std::string &s, size_t i, char marker) {
    if (i + 1 >= s.size() || is_space_c(s[i + 1])) return std::string::npos;
    for (size_t k = i + 2; k < s.size() && k < i + 400; k++) {
        if (s[k] == marker && !is_space_c(s[k - 1]) && emph_post_ok(s, k + 1)) return k;
        if (s[k] == '\n') return std::string::npos;
    }
    return std::string::npos;
}

bool is_image_path(const std::string &p) {
    std::string l = lower(p);
    return ends_with(l, ".png") || ends_with(l, ".jpg") || ends_with(l, ".jpeg") ||
           ends_with(l, ".gif") || ends_with(l, ".svg") || ends_with(l, ".webp") ||
           ends_with(l, ".avif");
}

std::string link_target(std::string t) {
    if (starts_with(t, "file:")) t = t.substr(5);
    return t;
}

// render a sidenote (numbered) or margin note (unnumbered, label empty)
std::string render_note(Ctx &ctx, const std::string &def_raw, int num) {
    std::string id = "sn" + std::to_string(++ctx.uniq);
    std::string def_html = fmt_inline(def_raw, ctx, /*allow_fn=*/false);
    std::string out;
    if (num > 0) {
        std::string n = std::to_string(num);
        out += "<label for=\"" + id + "\" class=\"sn-toggle\"><sup class=\"sn-ref\">" + n + "</sup></label>";
        out += "<input type=\"checkbox\" id=\"" + id + "\" class=\"sn-check\"/>";
        out += "<span class=\"sidenote\"><sup class=\"sn-num\">" + n + "</sup> " + def_html + "</span>";
    } else {
        out += "<label for=\"" + id + "\" class=\"sn-toggle mn-mark\">&#8853;</label>";
        out += "<input type=\"checkbox\" id=\"" + id + "\" class=\"sn-check\"/>";
        out += "<span class=\"sidenote marginnote\">" + def_html + "</span>";
    }
    return out;
}

// The inline formatter: a single left-to-right scan. Earlier constructs
// (math, code, links, footnotes) win over emphasis markers.
std::string fmt_inline(const std::string &s, Ctx &ctx, bool allow_fn) {
    std::string out;
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        // --- math ---------------------------------------------------------
        if (c == '\\' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '[')) {
            bool disp = s[i + 1] == '[';
            std::string close = disp ? "\\]" : "\\)";
            size_t e = s.find(close, i + 2);
            if (e != std::string::npos) {
                ctx.has_math = true;
                out += latex_to_mathml(s.substr(i + 2, e - i - 2), disp);
                i = e + 2;
                continue;
            }
        }
        if (c == '$' && i + 1 < n && s[i + 1] == '$') {
            size_t e = s.find("$$", i + 2);
            if (e != std::string::npos) {
                ctx.has_math = true;
                out += latex_to_mathml(s.substr(i + 2, e - i - 2), true);
                i = e + 2;
                continue;
            }
        }
        // --- line break \\ at end of line ---------------------------------
        if (c == '\\' && i + 1 < n && s[i + 1] == '\\' &&
            (i + 2 >= n || s[i + 2] == '\n')) {
            out += "<br/>";
            i += 2;
            continue;
        }
        // --- footnotes ----------------------------------------------------
        if (c == '[' && allow_fn && s.compare(i, 4, "[fn:") == 0) {
            int depth = 1;
            size_t k = i + 4;
            while (k < n && depth > 0) {
                if (s[k] == '[') depth++;
                else if (s[k] == ']') depth--;
                k++;
            }
            if (depth == 0) {
                std::string body = s.substr(i + 4, k - i - 5); // label / label:def / :def
                size_t colon = body.find(':');
                std::string label = colon == std::string::npos ? body : body.substr(0, colon);
                std::string inline_def = colon == std::string::npos ? "" : body.substr(colon + 1);
                if (label.empty()) {
                    out += render_note(ctx, inline_def, 0); // anonymous margin note
                } else {
                    if (!inline_def.empty()) ctx.fndefs[label] = inline_def;
                    int num;
                    auto it = ctx.fnnum.find(label);
                    if (it != ctx.fnnum.end()) num = it->second;
                    else num = ctx.fnnum[label] = ctx.next_fn++;
                    out += render_note(ctx, ctx.fndefs.count(label) ? ctx.fndefs[label] : "", num);
                }
                i = k;
                continue;
            }
        }
        // --- [[links]] ----------------------------------------------------
        if (c == '[' && i + 1 < n && s[i + 1] == '[') {
            size_t te = s.find(']', i + 2);
            if (te != std::string::npos) {
                std::string target = link_target(s.substr(i + 2, te - i - 2));
                std::string desc;
                size_t after = te + 1;
                if (after < n && s[after] == '[') {
                    size_t de = s.find("]]", after + 1);
                    if (de != std::string::npos) {
                        desc = s.substr(after + 1, de - after - 1);
                        after = de + 2;
                    }
                } else if (after < n && s[after] == ']') {
                    after++;
                }
                if (desc.empty() && is_image_path(target)) {
                    out += "<img src=\"" + html_escape(target) + "\" alt=\"\" loading=\"lazy\"/>";
                } else {
                    std::string href = target;
                    if (!href.empty() && href[0] == '*') href = "#" + slugify(href.substr(1));
                    out += "<a href=\"" + html_escape(href) + "\">";
                    out += desc.empty() ? html_escape(target) : fmt_inline(desc, ctx, allow_fn);
                    out += "</a>";
                }
                i = after;
                continue;
            }
        }
        // --- bare URLs ----------------------------------------------------
        if (c == 'h' && (s.compare(i, 8, "https://") == 0 || s.compare(i, 7, "http://") == 0)) {
            size_t k = i;
            while (k < n && !is_space_c(s[k]) && s[k] != '\n' && s[k] != '<' && s[k] != '>' &&
                   s[k] != '"') k++;
            while (k > i && (s[k - 1] == '.' || s[k - 1] == ',' || s[k - 1] == ')' ||
                             s[k - 1] == ';' || s[k - 1] == ']')) k--;
            std::string url = s.substr(i, k - i);
            out += "<a href=\"" + html_escape(url) + "\">" + html_escape(url) + "</a>";
            i = k;
            continue;
        }
        // --- code / verbatim (no nesting inside) --------------------------
        if ((c == '~' || c == '=') && emph_pre_ok(s, i)) {
            size_t k = find_emph_close(s, i, c);
            if (k != std::string::npos) {
                out += "<code>" + html_escape(s.substr(i + 1, k - i - 1)) + "</code>";
                i = k + 1;
                continue;
            }
        }
        // --- emphasis -----------------------------------------------------
        if ((c == '*' || c == '/' || c == '_' || c == '+') && emph_pre_ok(s, i)) {
            size_t k = find_emph_close(s, i, c);
            if (k != std::string::npos) {
                std::string inner = fmt_inline(s.substr(i + 1, k - i - 1), ctx, allow_fn);
                if (c == '*') out += "<strong>" + inner + "</strong>";
                else if (c == '/') out += "<em>" + inner + "</em>";
                else if (c == '_') out += "<span class=\"ul\">" + inner + "</span>";
                else out += "<del>" + inner + "</del>";
                i = k + 1;
                continue;
            }
        }
        // --- smart typography: em dash, ellipsis, curly quotes ------------
        // (never reached for code/verbatim/math/URLs — those already
        // continue'd above — so this only touches actual prose.)
        if (c == '-' && i + 1 < n && s[i + 1] == '-' && (i + 2 >= n || s[i + 2] != '-')) {
            out += cp_utf8(0x2014); // —
            i += 2;
            continue;
        }
        if (c == '.' && s.compare(i, 3, "...") == 0) {
            out += cp_utf8(0x2026); // …
            i += 3;
            continue;
        }
        if (c == '"') {
            out += cp_utf8(emph_pre_ok(s, i) ? 0x201C : 0x201D); // “ ”
            i++;
            continue;
        }
        if (c == '\'') {
            out += cp_utf8(emph_pre_ok(s, i) ? 0x2018 : 0x2019); // ‘ ’
            i++;
            continue;
        }
        // --- plain character ----------------------------------------------
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out.push_back(c);
        }
        i++;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Block-level parsing
// ---------------------------------------------------------------------------

struct ListFrame {
    int indent;
    std::string tag;   // ul / ol / dl
    bool item_open = false;
};

struct Blocks {
    std::string out;
    Ctx &ctx;
    std::vector<ListFrame> lists;
    std::string para;
    std::string caption;              // pending #+CAPTION
    std::map<std::string, std::string> attrs; // pending #+ATTR_HTML

    explicit Blocks(Ctx &c) : ctx(c) {}

    void flush_para() {
        std::string p = trim(para);
        para.clear();
        if (p.empty()) return;
        std::string html = fmt_inline(p, ctx);
        if (!lists.empty() && lists.back().item_open) {
            out += "<p>" + html + "</p>";
        } else {
            close_lists(-1);
            out += "<p>" + html + "</p>\n";
        }
    }

    void close_item(ListFrame &f) {
        if (!f.item_open) return;
        out += f.tag == "dl" ? "</dd>" : "</li>";
        f.item_open = false;
    }

    void close_lists(int to_indent) {
        while (!lists.empty() && lists.back().indent > to_indent) {
            close_item(lists.back());
            out += "</" + lists.back().tag + ">\n";
            lists.pop_back();
        }
    }

    std::string take_attr(const std::string &k) {
        auto it = attrs.find(k);
        if (it == attrs.end()) return "";
        std::string v = it->second;
        attrs.erase(it);
        return v;
    }

    void emit_figure(const std::string &src) {
        flush_para();
        close_lists(-1);
        std::string cls = take_attr("class");
        std::string alt = take_attr("alt");
        std::string width = take_attr("width");
        out += "<figure" + (cls.empty() ? "" : " class=\"" + html_escape(cls) + "\"") + ">";
        out += "<img src=\"" + html_escape(src) + "\" alt=\"" + html_escape(alt) +
               "\" loading=\"lazy\"" + (width.empty() ? "" : " width=\"" + html_escape(width) + "\"") + "/>";
        if (!caption.empty())
            out += "<figcaption>" + fmt_inline(caption, ctx) + "</figcaption>";
        out += "</figure>\n";
        caption.clear();
        attrs.clear();
    }
};

// list item test: returns marker length, or 0. Sets ordered / dt (desc term).
int list_marker(const std::string &line, int &indent, bool &ordered, std::string &content) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') i++;
    indent = (int)i;
    if (i >= line.size()) return 0;
    if ((line[i] == '-' || line[i] == '+') && i + 1 < line.size() && line[i + 1] == ' ') {
        ordered = false;
        content = line.substr(i + 2);
        return 2;
    }
    size_t d = i;
    while (d < line.size() && std::isdigit((unsigned char)line[d])) d++;
    if (d > i && d < line.size() && (line[d] == '.' || line[d] == ')') &&
        d + 1 < line.size() && line[d + 1] == ' ') {
        ordered = true;
        content = line.substr(d + 2);
        return (int)(d + 2 - i);
    }
    return 0;
}

} // namespace

OrgDoc org_to_html(const std::string &src) {
    OrgDoc doc;
    Ctx ctx;
    auto lines = split_lines(src);
    std::set<size_t> skip; // lines consumed as footnote definitions

    // ---- pass 1: metadata and footnote definitions -----------------------
    for (size_t li = 0; li < lines.size(); li++) {
        const std::string &line = lines[li];
        if (istarts_with(line, "#+") && !istarts_with(line, "#+begin") && !istarts_with(line, "#+end")) {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = lower(trim(line.substr(2, colon - 2)));
                if (key != "caption" && key != "attr_html" && key != "html")
                    doc.meta[key] = trim(line.substr(colon + 1));
            }
        }
        if (starts_with(line, "[fn:")) {
            size_t close = line.find(']');
            if (close != std::string::npos && close > 4 && line.find(':', 4) >= close) {
                std::string label = line.substr(4, close - 4);
                std::string def = trim(line.substr(close + 1));
                skip.insert(li);
                size_t k = li + 1;
                while (k < lines.size() && !trim(lines[k]).empty() &&
                       !starts_with(lines[k], "[fn:") && !starts_with(lines[k], "*") &&
                       !istarts_with(lines[k], "#+")) {
                    def += " " + trim(lines[k]);
                    skip.insert(k);
                    k++;
                }
                ctx.fndefs[label] = def;
            }
        }
    }

    // ---- pass 2: block conversion ---------------------------------------
    Blocks b(ctx);
    size_t i = 0;
    const size_t N = lines.size();
    while (i < N) {
        if (skip.count(i)) { i++; continue; }
        const std::string &line = lines[i];
        std::string t = trim(line);

        // blank line: end paragraph; a list survives one blank line if the
        // next non-blank line is another item at a valid indent
        if (t.empty()) {
            b.flush_para();
            if (!b.lists.empty()) {
                size_t k = i + 1;
                while (k < N && trim(lines[k]).empty()) k++;
                bool cont = false;
                if (k < N) {
                    int ind; bool ord; std::string cont_c;
                    if (list_marker(lines[k], ind, ord, cont_c) && ind >= b.lists.front().indent)
                        cont = true;
                }
                if (!cont) b.close_lists(-1);
            }
            i++;
            continue;
        }

        // org comments and property drawers
        if (starts_with(t, "# ") || t == "#") { i++; continue; }
        if (t == ":PROPERTIES:") {
            while (i < N && trim(lines[i]) != ":END:") i++;
            i++;
            continue;
        }

        // #+ keywords
        if (istarts_with(t, "#+")) {
            std::string lt = lower(t);
            if (istarts_with(t, "#+caption:")) { b.caption = trim(t.substr(10)); i++; continue; }
            if (istarts_with(t, "#+attr_html:")) {
                // parse ":key value :key value"
                std::string rest = trim(t.substr(12));
                size_t p = 0;
                while (p < rest.size()) {
                    if (rest[p] == ':') {
                        size_t ke = rest.find(' ', p);
                        if (ke == std::string::npos) break;
                        std::string key = rest.substr(p + 1, ke - p - 1);
                        size_t ve = rest.find(" :", ke + 1);
                        std::string val = ve == std::string::npos ? rest.substr(ke + 1)
                                                                  : rest.substr(ke + 1, ve - ke - 1);
                        b.attrs[lower(key)] = trim(val);
                        p = ve == std::string::npos ? rest.size() : ve + 1;
                    } else p++;
                }
                i++;
                continue;
            }
            if (istarts_with(t, "#+html:")) {
                b.flush_para(); b.close_lists(-1);
                b.out += trim(t.substr(7)) + "\n";
                i++;
                continue;
            }
            if (istarts_with(t, "#+begin_src") || istarts_with(t, "#+begin_example")) {
                b.flush_para(); b.close_lists(-1);
                std::string lang;
                if (istarts_with(t, "#+begin_src")) {
                    std::string rest = trim(t.substr(11));
                    size_t sp = rest.find(' ');
                    lang = sp == std::string::npos ? rest : rest.substr(0, sp);
                }
                std::string code;
                i++;
                while (i < N && !istarts_with(trim(lines[i]), "#+end_")) {
                    std::string cl = lines[i];
                    if (starts_with(cl, "  ")) cl = cl.substr(2); // org indents src content
                    code += html_escape(cl) + "\n";
                    i++;
                }
                i++;
                b.out += "<pre" + std::string(lang.empty() ? "" : " data-lang=\"" + html_escape(lang) + "\"") +
                         "><code" + (lang.empty() ? "" : " class=\"language-" + html_escape(lang) + "\"") +
                         ">" + code + "</code></pre>\n";
                continue;
            }
            if (istarts_with(t, "#+begin_quote")) {
                b.flush_para(); b.close_lists(-1);
                std::string q;
                i++;
                while (i < N && !istarts_with(trim(lines[i]), "#+end_quote")) {
                    q += lines[i] + "\n";
                    i++;
                }
                i++;
                // paragraphs inside the quote
                std::string inner;
                std::string cur;
                for (auto &ql : split_lines(q)) {
                    std::string qt = trim(ql);
                    if (qt.empty()) {
                        if (!cur.empty()) { inner += "<p>" + fmt_inline(trim(cur), ctx) + "</p>"; cur.clear(); }
                    } else if (starts_with(qt, "-- ") || starts_with(qt, cp_utf8(0x2014))) {
                        if (!cur.empty()) { inner += "<p>" + fmt_inline(trim(cur), ctx) + "</p>"; cur.clear(); }
                        std::string attr = starts_with(qt, "-- ") ? qt.substr(3) : qt;
                        inner += "<footer>" + fmt_inline(attr, ctx) + "</footer>";
                    } else cur += ql + "\n";
                }
                if (!cur.empty()) inner += "<p>" + fmt_inline(trim(cur), ctx) + "</p>";
                b.out += "<blockquote>" + inner + "</blockquote>\n";
                continue;
            }
            if (istarts_with(t, "#+begin_export")) {
                bool html = lower(t).find("html") != std::string::npos;
                b.flush_para(); b.close_lists(-1);
                i++;
                while (i < N && !istarts_with(trim(lines[i]), "#+end_export")) {
                    if (html) b.out += lines[i] + "\n";
                    i++;
                }
                i++;
                continue;
            }
            if (istarts_with(t, "#+begin_")) { // generic special block -> classed div
                std::string name = lower(trim(t.substr(8)));
                size_t sp = name.find(' ');
                if (sp != std::string::npos) name = name.substr(0, sp);
                b.flush_para(); b.close_lists(-1);
                std::string body;
                i++;
                while (i < N && !istarts_with(trim(lines[i]), "#+end_")) {
                    body += lines[i] + "\n";
                    i++;
                }
                i++;
                OrgDoc sub = org_to_html(body); // recurse for full block support
                for (auto &fd : sub.meta) (void)fd;
                if (sub.has_math) ctx.has_math = true;
                b.out += "<div class=\"block-" + html_escape(name) + "\">" + sub.html + "</div>\n";
                continue;
            }
            // other #+keyword lines are metadata, already captured
            i++;
            continue;
        }

        // headings
        if (line[0] == '*') {
            size_t stars = 0;
            while (stars < line.size() && line[stars] == '*') stars++;
            if (stars < line.size() && line[stars] == ' ') {
                b.flush_para(); b.close_lists(-1);
                std::string text = trim(line.substr(stars + 1));
                int h = std::min<int>((int)stars + 1, 5);
                std::string id = slugify(text);
                b.out += "<h" + std::to_string(h) + " id=\"" + id + "\">" +
                         fmt_inline(text, ctx) + "</h" + std::to_string(h) + ">\n";
                i++;
                continue;
            }
        }

        // horizontal rule
        if (t.size() >= 5 && t.find_first_not_of('-') == std::string::npos) {
            b.flush_para(); b.close_lists(-1);
            b.out += "<hr/>\n";
            i++;
            continue;
        }

        // tables
        if (t[0] == '|') {
            b.flush_para(); b.close_lists(-1);
            std::vector<std::vector<std::string>> rows;
            int header_end = -1;
            while (i < N) {
                std::string rt = trim(lines[i]);
                if (rt.empty() || rt[0] != '|') break;
                if (rt.size() > 1 && (rt[1] == '-' || rt[1] == '+')) {
                    header_end = (int)rows.size();
                    i++;
                    continue;
                }
                std::vector<std::string> cells;
                std::string cell;
                for (size_t p = 1; p < rt.size(); p++) {
                    if (rt[p] == '|') { cells.push_back(trim(cell)); cell.clear(); }
                    else cell.push_back(rt[p]);
                }
                if (!trim(cell).empty()) cells.push_back(trim(cell));
                rows.push_back(cells);
                i++;
            }
            b.out += "<table>";
            for (size_t r = 0; r < rows.size(); r++) {
                bool head = header_end > 0 && (int)r < header_end;
                b.out += "<tr>";
                for (auto &cell2 : rows[r])
                    b.out += head ? "<th>" + fmt_inline(cell2, ctx) + "</th>"
                                  : "<td>" + fmt_inline(cell2, ctx) + "</td>";
                b.out += "</tr>";
            }
            b.out += "</table>\n";
            if (!b.caption.empty()) {
                b.out += "<p class=\"table-caption\">" + fmt_inline(b.caption, ctx) + "</p>\n";
                b.caption.clear();
            }
            continue;
        }

        // standalone image line -> figure
        if (starts_with(t, "[[") && ends_with(t, "]]")) {
            std::string target = link_target(t.substr(2, t.size() - 4));
            if (is_image_path(target) && target.find("][") == std::string::npos) {
                b.emit_figure(target);
                i++;
                continue;
            }
        }

        // list items
        {
            int indent; bool ordered; std::string content;
            if (list_marker(line, indent, ordered, content)) {
                b.flush_para();
                // description list?  "term :: description"
                std::string term, rest = content;
                size_t sep = content.find(" :: ");
                bool is_desc = sep != std::string::npos;
                if (is_desc) { term = content.substr(0, sep); rest = content.substr(sep + 4); }
                std::string tag = is_desc ? "dl" : (ordered ? "ol" : "ul");

                if (!b.lists.empty() && indent > b.lists.back().indent) {
                    b.out += "<" + tag + ">";
                    b.lists.push_back({indent, tag, false});
                } else {
                    b.close_lists(indent);
                    if (b.lists.empty() || b.lists.back().indent < indent ||
                        b.lists.back().tag != tag) {
                        if (!b.lists.empty() && b.lists.back().indent == indent) {
                            b.close_item(b.lists.back());
                            b.out += "</" + b.lists.back().tag + ">\n";
                            b.lists.pop_back();
                        }
                        b.out += "<" + tag + ">";
                        b.lists.push_back({indent, tag, false});
                    } else {
                        b.close_item(b.lists.back());
                    }
                }
                // checkbox?
                std::string check;
                if (starts_with(rest, "[ ] ")) { check = "<input type=\"checkbox\" disabled/> "; rest = rest.substr(4); }
                else if (starts_with(rest, "[X] ") || starts_with(rest, "[x] ")) {
                    check = "<input type=\"checkbox\" checked disabled/> ";
                    rest = rest.substr(4);
                }
                if (is_desc) {
                    b.out += "<dt>" + fmt_inline(term, ctx) + "</dt><dd>" + check + fmt_inline(rest, ctx);
                } else {
                    b.out += "<li>" + check + fmt_inline(rest, ctx);
                }
                b.lists.back().item_open = true;
                i++;
                // continuation lines (indented beyond the marker, not new items)
                while (i < N) {
                    if (skip.count(i)) { i++; continue; }
                    const std::string &cl = lines[i];
                    std::string ct = trim(cl);
                    if (ct.empty()) break;
                    int ci; bool co; std::string cc;
                    if (list_marker(cl, ci, co, cc)) break;
                    if (cl[0] == '*' || istarts_with(ct, "#+") || ct[0] == '|') break;
                    int lead = 0;
                    while (lead < (int)cl.size() && cl[lead] == ' ') lead++;
                    if (lead <= indent) break;
                    b.out += " " + fmt_inline(ct, ctx);
                    i++;
                }
                continue;
            }
        }

        // plain paragraph text
        b.para += line + "\n";
        i++;
    }
    b.flush_para();
    b.close_lists(-1);

    doc.html = b.out;
    doc.has_math = ctx.has_math;
    return doc;
}
