// mathml.cpp — a recursive-descent LaTeX -> MathML Core converter.
//
// Covers the working set for technical / proof-based writing: fractions, roots,
// sub/superscripts, big operators with limits, greek and common symbols,
// \mathbb/\mathcal/\mathbf letter mapping, \text, accents, \left...\right,
// and the matrix/cases/aligned environments. Unknown commands degrade to
// upright identifiers instead of failing, so an article never breaks.
//
// Math renders natively in every modern browser via MathML — no JS, no fonts.
#include "mathml.hpp"
#include "common.hpp"
#include <map>
#include <cctype>

namespace {

struct Node {
    std::string xml;
    bool bigop = false;      // \sum, \lim, ... — limits go under/over in display mode
    bool sideop = false;     // \int — limits stay at the side
};

struct Parser {
    const std::string &s;
    size_t i = 0;
    bool display;
    Parser(const std::string &src, bool disp) : s(src), display(disp) {}

    bool eof() const { return i >= s.size(); }
    char peek() const { return eof() ? '\0' : s[i]; }
    void skip_ws() { while (!eof() && std::isspace((unsigned char)s[i])) i++; }

    // ---- symbol tables -------------------------------------------------
    static const std::map<std::string, uint32_t> &greek() {
        static const std::map<std::string, uint32_t> m = {
            {"alpha",0x3B1},{"beta",0x3B2},{"gamma",0x3B3},{"delta",0x3B4},
            {"epsilon",0x3F5},{"varepsilon",0x3B5},{"zeta",0x3B6},{"eta",0x3B7},
            {"theta",0x3B8},{"vartheta",0x3D1},{"iota",0x3B9},{"kappa",0x3BA},
            {"lambda",0x3BB},{"mu",0x3BC},{"nu",0x3BD},{"xi",0x3BE},{"pi",0x3C0},
            {"varpi",0x3D6},{"rho",0x3C1},{"sigma",0x3C3},{"varsigma",0x3C2},
            {"tau",0x3C4},{"upsilon",0x3C5},{"phi",0x3D5},{"varphi",0x3C6},
            {"chi",0x3C7},{"psi",0x3C8},{"omega",0x3C9},
            {"Gamma",0x393},{"Delta",0x394},{"Theta",0x398},{"Lambda",0x39B},
            {"Xi",0x39E},{"Pi",0x3A0},{"Sigma",0x3A3},{"Upsilon",0x3A5},
            {"Phi",0x3A6},{"Psi",0x3A8},{"Omega",0x3A9},
            {"infty",0x221E},{"partial",0x2202},{"nabla",0x2207},{"hbar",0x210F},
            {"ell",0x2113},{"aleph",0x2135},{"Re",0x211C},{"Im",0x2111},
            {"emptyset",0x2205},{"varnothing",0x2205},
        };
        return m;
    }
    static const std::map<std::string, uint32_t> &ops() {
        static const std::map<std::string, uint32_t> m = {
            {"pm",0xB1},{"mp",0x2213},{"cdot",0x22C5},{"times",0xD7},{"div",0xF7},
            {"ast",0x2217},{"star",0x22C6},{"circ",0x2218},{"bullet",0x2219},
            {"leq",0x2264},{"le",0x2264},{"geq",0x2265},{"ge",0x2265},
            {"neq",0x2260},{"ne",0x2260},{"approx",0x2248},{"equiv",0x2261},
            {"sim",0x223C},{"simeq",0x2243},{"cong",0x2245},{"propto",0x221D},
            {"ll",0x226A},{"gg",0x226B},{"prec",0x227A},{"succ",0x227B},
            {"subset",0x2282},{"supset",0x2283},{"subseteq",0x2286},{"supseteq",0x2287},
            {"in",0x2208},{"notin",0x2209},{"ni",0x220B},
            {"cup",0x222A},{"cap",0x2229},{"setminus",0x2216},
            {"forall",0x2200},{"exists",0x2203},{"nexists",0x2204},
            {"neg",0xAC},{"lnot",0xAC},{"land",0x2227},{"wedge",0x2227},
            {"lor",0x2228},{"vee",0x2228},
            {"implies",0x27F9},{"iff",0x27FA},{"to",0x2192},
            {"rightarrow",0x2192},{"leftarrow",0x2190},{"leftrightarrow",0x2194},
            {"Rightarrow",0x21D2},{"Leftarrow",0x21D0},{"Leftrightarrow",0x21D4},
            {"longrightarrow",0x27F6},{"longmapsto",0x27FC},{"mapsto",0x21A6},
            {"uparrow",0x2191},{"downarrow",0x2193},
            {"angle",0x2220},{"perp",0x22A5},{"parallel",0x2225},{"mid",0x2223},
            {"nmid",0x2224},{"therefore",0x2234},{"because",0x2235},
            {"oplus",0x2295},{"ominus",0x2296},{"otimes",0x2297},{"odot",0x2299},
            {"langle",0x27E8},{"rangle",0x27E9},
            {"lfloor",0x230A},{"rfloor",0x230B},{"lceil",0x2308},{"rceil",0x2309},
            {"vdash",0x22A2},{"models",0x22A8},{"top",0x22A4},{"bot",0x22A5},
            {"ldots",0x2026},{"cdots",0x22EF},{"vdots",0x22EE},{"ddots",0x22F1},
            {"dots",0x2026},{"dotsc",0x2026},{"dotsb",0x22EF},
            {"prime",0x2032},{"dagger",0x2020},{"colon",0x3A},
        };
        return m;
    }
    // named function-style operators rendered upright
    static bool is_func(const std::string &n) {
        static const char *fns[] = {"sin","cos","tan","cot","sec","csc","arcsin","arccos",
            "arctan","sinh","cosh","tanh","coth","log","ln","lg","exp","deg","det","dim",
            "ker","arg","gcd","hom","mod","bmod","Pr"};
        for (auto f : fns) if (n == f) return true;
        return false;
    }
    // big operators: limits under/over in display mode
    static uint32_t bigop_cp(const std::string &n) {
        static const std::map<std::string, uint32_t> m = {
            {"sum",0x2211},{"prod",0x220F},{"coprod",0x2210},
            {"bigcup",0x22C3},{"bigcap",0x22C2},{"bigoplus",0x2A01},
            {"bigotimes",0x2A02},{"bigvee",0x22C1},{"bigwedge",0x22C0},
        };
        auto it = m.find(n);
        return it == m.end() ? 0 : it->second;
    }
    static uint32_t intop_cp(const std::string &n) {
        static const std::map<std::string, uint32_t> m = {
            {"int",0x222B},{"iint",0x222C},{"iiint",0x222D},{"oint",0x222E},
        };
        auto it = m.find(n);
        return it == m.end() ? 0 : it->second;
    }
    // limit-style word operators (limits go underneath)
    static bool is_limword(const std::string &n) {
        return n == "lim" || n == "limsup" || n == "liminf" || n == "max" ||
               n == "min" || n == "sup" || n == "inf" || n == "argmax" || n == "argmin";
    }

    // map a letter to a math-alphabet codepoint (bb, cal, frak, bf)
    static std::string map_alpha(const std::string &variant, char c) {
        auto in = [&](char a, char b) { return c >= a && c <= b; };
        if (variant == "bb") {
            static const std::map<char, uint32_t> ex = {
                {'C',0x2102},{'H',0x210D},{'N',0x2115},{'P',0x2119},
                {'Q',0x211A},{'R',0x211D},{'Z',0x2124}};
            auto it = ex.find(c);
            if (it != ex.end()) return cp_utf8(it->second);
            if (in('A','Z')) return cp_utf8(0x1D538 + (c - 'A'));
            if (in('a','z')) return cp_utf8(0x1D552 + (c - 'a'));
            if (in('0','9')) return cp_utf8(0x1D7D8 + (c - '0'));
        } else if (variant == "cal") {
            static const std::map<char, uint32_t> ex = {
                {'B',0x212C},{'E',0x2130},{'F',0x2131},{'H',0x210B},{'I',0x2110},
                {'L',0x2112},{'M',0x2133},{'R',0x211B},{'e',0x212F},{'g',0x210A},{'o',0x2134}};
            auto it = ex.find(c);
            if (it != ex.end()) return cp_utf8(it->second);
            if (in('A','Z')) return cp_utf8(0x1D49C + (c - 'A'));
            if (in('a','z')) return cp_utf8(0x1D4B6 + (c - 'a'));
        } else if (variant == "frak") {
            static const std::map<char, uint32_t> ex = {
                {'C',0x212D},{'H',0x210C},{'I',0x2111},{'R',0x211C},{'Z',0x2128}};
            auto it = ex.find(c);
            if (it != ex.end()) return cp_utf8(it->second);
            if (in('A','Z')) return cp_utf8(0x1D504 + (c - 'A'));
            if (in('a','z')) return cp_utf8(0x1D51E + (c - 'a'));
        } else if (variant == "bf") {
            if (in('A','Z')) return cp_utf8(0x1D400 + (c - 'A'));
            if (in('a','z')) return cp_utf8(0x1D41A + (c - 'a'));
            if (in('0','9')) return cp_utf8(0x1D7CE + (c - '0'));
        }
        return std::string(1, c);
    }

    static std::string esc(const std::string &t) { return html_escape(t); }
    static std::string mo(const std::string &t) { return "<mo>" + esc(t) + "</mo>"; }
    static std::string mi(const std::string &t) { return "<mi>" + esc(t) + "</mi>"; }
    static std::string mrow(const std::string &t) { return "<mrow>" + t + "</mrow>"; }

    // read a \command name after the backslash was consumed
    std::string read_cmd() {
        std::string name;
        if (!eof() && std::isalpha((unsigned char)s[i]))
            while (!eof() && std::isalpha((unsigned char)s[i])) name.push_back(s[i++]);
        else if (!eof()) name.push_back(s[i++]);
        return name;
    }

    // raw text until matching close brace (for \text{...})
    std::string read_braced_raw() {
        skip_ws();
        if (peek() != '{') return "";
        i++;
        std::string out;
        int depth = 1;
        while (!eof()) {
            char c = s[i++];
            if (c == '{') depth++;
            else if (c == '}') { if (--depth == 0) break; }
            if (depth > 0) out.push_back(c);
        }
        return out;
    }

    // a group {..} or a single atom
    Node parse_arg() {
        skip_ws();
        if (peek() == '{') {
            i++;
            std::string body = parse_seq("}");
            if (peek() == '}') i++;
            Node n; n.xml = mrow(body);
            return n;
        }
        return parse_atom();
    }

    Node symbol_or_unknown(const std::string &name) {
        Node n;
        {
            auto &g = greek();
            auto it = g.find(name);
            if (it != g.end()) { n.xml = "<mi>" + cp_utf8(it->second) + "</mi>"; return n; }
        }
        {
            auto &o = ops();
            auto it = o.find(name);
            if (it != o.end()) { n.xml = "<mo>" + cp_utf8(it->second) + "</mo>"; return n; }
        }
        if (uint32_t cp = bigop_cp(name)) {
            n.xml = "<mo movablelimits=\"true\">" + cp_utf8(cp) + "</mo>";
            n.bigop = true;
            return n;
        }
        if (uint32_t cp = intop_cp(name)) {
            n.xml = "<mo>" + cp_utf8(cp) + "</mo>";
            n.sideop = true;
            return n;
        }
        if (is_limword(name)) {
            n.xml = "<mo movablelimits=\"true\" form=\"prefix\">" + esc(name) + "</mo>";
            n.bigop = true;
            return n;
        }
        if (is_func(name)) { n.xml = mi(name); return n; }
        // graceful degradation: unknown command shows as an upright identifier
        n.xml = mi(name);
        return n;
    }

    Node parse_command() {
        std::string name = read_cmd();
        Node n;
        if (name == "frac" || name == "dfrac" || name == "tfrac") {
            Node a = parse_arg(), b = parse_arg();
            n.xml = "<mfrac>" + a.xml + b.xml + "</mfrac>";
        } else if (name == "binom") {
            Node a = parse_arg(), b = parse_arg();
            n.xml = mrow("<mo>(</mo><mfrac linethickness=\"0\">" + a.xml + b.xml + "</mfrac><mo>)</mo>");
        } else if (name == "sqrt") {
            skip_ws();
            if (peek() == '[') {
                i++;
                std::string idx = parse_seq("]");
                if (peek() == ']') i++;
                Node a = parse_arg();
                n.xml = "<mroot>" + a.xml + mrow(idx) + "</mroot>";
            } else {
                Node a = parse_arg();
                n.xml = "<msqrt>" + a.xml + "</msqrt>";
            }
        } else if (name == "text" || name == "textrm" || name == "textit" ||
                   name == "textbf" || name == "mbox") {
            n.xml = "<mtext>" + esc(read_braced_raw()) + "</mtext>";
        } else if (name == "mathrm" || name == "operatorname") {
            n.xml = "<mi mathvariant=\"normal\">" + esc(read_braced_raw()) + "</mi>";
        } else if (name == "mathbb" || name == "mathcal" || name == "mathfrak" || name == "mathbf") {
            std::string variant = name == "mathbb" ? "bb" : name == "mathcal" ? "cal"
                                : name == "mathfrak" ? "frak" : "bf";
            std::string raw = read_braced_raw(), mapped;
            if (raw.empty() && !eof()) raw = std::string(1, s[i++]); // \mathbb R form
            for (char c : raw) mapped += map_alpha(variant, c);
            n.xml = "<mi>" + esc(mapped) + "</mi>";
        } else if (name == "hat" || name == "bar" || name == "vec" || name == "tilde" ||
                   name == "dot" || name == "ddot" || name == "overline" || name == "widehat" ||
                   name == "widetilde") {
            static const std::map<std::string, uint32_t> acc = {
                {"hat",0x2C6},{"widehat",0x2C6},{"bar",0xAF},{"overline",0xAF},
                {"vec",0x2192},{"tilde",0x2DC},{"widetilde",0x2DC},{"dot",0x2D9},{"ddot",0xA8}};
            Node a = parse_arg();
            n.xml = "<mover accent=\"true\">" + a.xml + "<mo>" + cp_utf8(acc.at(name)) + "</mo></mover>";
        } else if (name == "underline") {
            Node a = parse_arg();
            n.xml = "<munder>" + a.xml + "<mo>" + cp_utf8(0x332) + "</mo></munder>";
        } else if (name == "overbrace" || name == "underbrace") {
            Node a = parse_arg();
            bool over = name == "overbrace";
            n.xml = std::string(over ? "<mover" : "<munder") + " accent=\"true\">" + a.xml +
                    "<mo stretchy=\"true\">" + cp_utf8(over ? 0x23DE : 0x23DF) + "</mo>" +
                    (over ? "</mover>" : "</munder>");
            n.bigop = true; // following ^/_ label goes over/under
        } else if (name == "left") {
            skip_ws();
            std::string open = read_delim();
            std::string body = parse_seq("\\right");
            std::string close = read_delim();
            std::string x = "<mrow>";
            if (open != ".") x += "<mo stretchy=\"true\">" + esc(open) + "</mo>";
            x += body;
            if (close != ".") x += "<mo stretchy=\"true\">" + esc(close) + "</mo>";
            x += "</mrow>";
            n.xml = x;
        } else if (name == "begin") {
            n = parse_env(read_braced_raw());
        } else if (name == "quad") { n.xml = "<mspace width=\"1em\"/>"; }
        else if (name == "qquad") { n.xml = "<mspace width=\"2em\"/>"; }
        else if (name == ",") { n.xml = "<mspace width=\"0.167em\"/>"; }
        else if (name == ";") { n.xml = "<mspace width=\"0.278em\"/>"; }
        else if (name == ":") { n.xml = "<mspace width=\"0.222em\"/>"; }
        else if (name == "!") { n.xml = ""; }
        else if (name == "{") { n.xml = mo("{"); }
        else if (name == "}") { n.xml = mo("}"); }
        else if (name == "|") { n.xml = mo(cp_utf8(0x2016)); }
        else if (name == "\\") { n.xml = "<mspace linebreak=\"newline\"/>"; }
        else n = symbol_or_unknown(name);
        return n;
    }

    // delimiter after \left or \right
    std::string read_delim() {
        skip_ws();
        if (eof()) return ".";
        char c = s[i];
        if (c == '\\') {
            i++;
            std::string name = read_cmd();
            if (name == "{") return "{";
            if (name == "}") return "}";
            if (name == "|") return cp_utf8(0x2016);
            auto &o = ops();
            auto it = o.find(name);
            if (it != o.end()) return cp_utf8(it->second);
            return ".";
        }
        i++;
        if (c == '.') return ".";
        return std::string(1, c);
    }

    // environments: matrices, cases, aligned
    Node parse_env(const std::string &env) {
        std::vector<std::vector<std::string>> rows(1);
        rows[0].emplace_back();
        if (env == "array") { // swallow the column spec {rcl}
            read_braced_raw();
        }
        for (;;) {
            skip_ws();
            if (eof()) break;
            if (peek() == '&') { i++; rows.back().emplace_back(); continue; }
            if (peek() == '\\' && i + 1 < s.size() && s[i + 1] == '\\') {
                i += 2;
                rows.emplace_back();
                rows.back().emplace_back();
                continue;
            }
            if (peek() == '\\') {
                size_t save = i;
                i++;
                std::string name = read_cmd();
                if (name == "end") { read_braced_raw(); break; }
                i = save;
            }
            Node a = parse_atom_with_scripts();
            rows.back().back() += a.xml;
        }
        std::string align = (env == "cases" || istarts_with(env, "align") || env == "split")
                            ? "left" : "center";
        std::string table = "<mtable columnalign=\"" + align + "\" rowspacing=\"0.5ex\">";
        for (auto &r : rows) {
            if (r.size() == 1 && r[0].empty()) continue;
            table += "<mtr>";
            for (auto &c : r) table += "<mtd>" + c + "</mtd>";
            table += "</mtr>";
        }
        table += "</mtable>";
        Node n;
        std::string open, close;
        if (env == "pmatrix") { open = "("; close = ")"; }
        else if (env == "bmatrix") { open = "["; close = "]"; }
        else if (env == "Bmatrix") { open = "{"; close = "}"; }
        else if (env == "vmatrix") { open = "|"; close = "|"; }
        else if (env == "Vmatrix") { open = cp_utf8(0x2016); close = open; }
        else if (env == "cases") { open = "{"; }
        if (!open.empty() || !close.empty()) {
            std::string x = "<mrow>";
            if (!open.empty()) x += "<mo stretchy=\"true\">" + esc(open) + "</mo>";
            x += table;
            if (!close.empty()) x += "<mo stretchy=\"true\">" + esc(close) + "</mo>";
            x += "</mrow>";
            n.xml = x;
        } else n.xml = table;
        return n;
    }

    Node parse_atom() {
        skip_ws();
        Node n;
        if (eof()) return n;
        char c = peek();
        if (c == '{') {
            i++;
            n.xml = mrow(parse_seq("}"));
            if (peek() == '}') i++;
            return n;
        }
        if (c == '\\') { i++; return parse_command(); }
        if (std::isalpha((unsigned char)c)) { i++; n.xml = mi(std::string(1, c)); return n; }
        if (std::isdigit((unsigned char)c)) {
            std::string num;
            while (!eof() && (std::isdigit((unsigned char)peek()) || peek() == '.')) num.push_back(s[i++]);
            n.xml = "<mn>" + num + "</mn>";
            return n;
        }
        i++;
        if (c == '-') { n.xml = "<mo>" + cp_utf8(0x2212) + "</mo>"; return n; }
        if (c == '\'') {
            int k = 1;
            while (peek() == '\'') { i++; k++; }
            std::string primes;
            for (int j = 0; j < k; j++) primes += cp_utf8(0x2032);
            n.xml = "<mo>" + primes + "</mo>";
            return n;
        }
        n.xml = mo(std::string(1, c));
        return n;
    }

    // atom plus any ^ / _ scripts attached to it
    Node parse_atom_with_scripts() {
        Node base = parse_atom();
        std::string sub, sup;
        for (;;) {
            skip_ws();
            if (peek() == '_') { i++; sub = parse_arg().xml; }
            else if (peek() == '^') { i++; sup = parse_arg().xml; }
            else break;
        }
        if (sub.empty() && sup.empty()) return base;
        Node n;
        bool underover = base.bigop && display;
        if (!sub.empty() && !sup.empty())
            n.xml = std::string(underover ? "<munderover>" : "<msubsup>") + base.xml + sub + sup +
                    (underover ? "</munderover>" : "</msubsup>");
        else if (!sub.empty())
            n.xml = std::string(underover ? "<munder>" : "<msub>") + base.xml + sub +
                    (underover ? "</munder>" : "</msub>");
        else
            n.xml = std::string(underover ? "<mover>" : "<msup>") + base.xml + sup +
                    (underover ? "</mover>" : "</msup>");
        return n;
    }

    // sequence until eof, unmatched '}', a ']', or the literal stop "\right"
    std::string parse_seq(const std::string &stop) {
        std::string out;
        for (;;) {
            skip_ws();
            if (eof()) break;
            if (stop == "}" && peek() == '}') break;
            if (stop == "]" && peek() == ']') break;
            if (stop == "\\right" && peek() == '\\') {
                size_t save = i;
                i++;
                std::string name = read_cmd();
                if (name == "right") break;
                i = save;
            }
            out += parse_atom_with_scripts().xml;
        }
        return out;
    }
};

} // namespace

std::string latex_to_mathml(const std::string &tex, bool display) {
    Parser p(tex, display);
    std::string body = p.parse_seq("");
    std::string out = "<math xmlns=\"http://www.w3.org/1998/Math/MathML\" display=\"";
    out += display ? "block" : "inline";
    out += "\"><semantics><mrow>" + body + "</mrow>";
    out += "<annotation encoding=\"application/x-tex\">" + html_escape(tex) + "</annotation>";
    out += "</semantics></math>";
    return out;
}
