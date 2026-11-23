// English normalization. Two passes:
//  1. regex pre-pass for multi-token patterns (phone numbers, times, dates, fractions, ranges);
//  2. a token pass (whitespace-delimited) for abbreviations, acronyms, money, %, ordinals, years, ...
#include "Normalizer.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <regex>
#include <set>

#include "Flags.hpp"

namespace ttsdc {

namespace {

using numwords::cardinal;

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool isLower(char c) { return c >= 'a' && c <= 'z'; }
bool isAlpha(char c) { return isUpper(c) || isLower(c); }

std::string toLower(std::string s) {
    for (auto& c : s)
        if (isUpper(c)) c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool hasDigit(const std::string& s) { return std::any_of(s.begin(), s.end(), isDigit); }

void addFlag(std::vector<std::string>& flags, const char* f) {
    if (std::find(flags.begin(), flags.end(), f) == flags.end()) flags.emplace_back(f);
}

/// regex replace with a callback.
std::string replaceAll(const std::string& s, const std::regex& re,
                       const std::function<std::string(const std::smatch&, const std::string& rest)>& fn) {
    std::string out;
    auto begin = s.cbegin();
    std::smatch m;
    while (std::regex_search(begin, s.cend(), m, re)) {
        out.append(begin, m[0].first);
        std::string rest(m[0].second, s.cend());
        out += fn(m, rest);
        begin = m[0].second;
        if (m[0].length() == 0) {
            if (begin == s.cend()) break;
            out += *begin++;
        }
    }
    out.append(begin, s.cend());
    return out;
}

/// True if only whitespace/closing quotes remain, i.e. a period consumed by an abbreviation was the sentence end.
bool atTextEnd(const std::string& rest) {
    for (unsigned char c : rest)
        if (!(c == ' ' || c == '"' || c == '\'' || c == ')' || c >= 0x80)) return false;
    return true;
}

const std::vector<std::string> kMonths = {"January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December"};

int monthIndex(const std::string& m) {
    std::string l = toLower(m);
    if (!l.empty() && l.back() == '.') l.pop_back();
    for (int i = 0; i < 12; ++i) {
        std::string full = toLower(kMonths[i]);
        if (l == full || (l.size() >= 3 && full.compare(0, l.size(), l) == 0 && (l.size() == 3 || l == "sept")))
            return i;
    }
    return -1;
}

const char* kMonthRe =
    "(January|February|March|April|May|June|July|August|September|October|November|December|"
    "Jan|Feb|Mar|Apr|Jun|Jul|Aug|Sept|Sep|Oct|Nov|Dec)\\.?";

std::string twoDigitYear(int y) {
    if (y < 10) return "oh " + cardinal(y);
    return cardinal(y);
}

std::string clockTime(int h, int m) {
    std::string s = cardinal(h);
    if (m == 0) return s;
    if (m < 10) return s + " oh " + cardinal(m);
    return s + " " + cardinal(m);
}

// ---------------------------------------------------------------- pre-pass

std::string prePass(const std::string& in, std::vector<std::string>& flags) {
    std::string s = in;

    // phone numbers: (555) 123-4567, 555-123-4567, 555.123.4567, 123-4567
    static const std::regex phone(R"((\(\d{3}\)\s?|\b\d{3}[-.])\d{3}[-.]\d{4}\b|\b\d{3}-\d{4}\b)");
    s = replaceAll(s, phone, [](const std::smatch& m, const std::string&) {
        std::string out, group;
        auto flush = [&]() {
            if (group.empty()) return;
            if (!out.empty()) out += ", ";
            out += numwords::digits(group);
            group.clear();
        };
        for (char c : m.str()) {
            if (isDigit(c)) group += c;
            else flush();
        }
        flush();
        return out;
    });

    // times with am/pm: 10am, 10 a.m., 3:30 PM
    static const std::regex ampm(R"(\b(\d{1,2})(?::(\d{2}))?\s?([aApP])\.?\s?[mM]\b(\.?))");
    s = replaceAll(s, ampm, [](const std::smatch& m, const std::string& rest) {
        int h = std::stoi(m[1].str());
        int mi = m[2].matched ? std::stoi(m[2].str()) : 0;
        std::string out = clockTime(h, mi) + (toLower(m[3].str()) == "a" ? " a m" : " p m");
        if (m[4].length() && atTextEnd(rest)) out += ".";
        return out;
    });

    // plain times 3:30, 3:00, 1:30:15
    static const std::regex clock(R"(\b(\d{1,2}):(\d{2})(?::(\d{2}))?\b)");
    s = replaceAll(s, clock, [](const std::smatch& m, const std::string&) {
        int h = std::stoi(m[1].str()), mi = std::stoi(m[2].str());
        if (m[3].matched) {
            int sec = std::stoi(m[3].str());
            return cardinal(h) + (h == 1 ? " hour " : " hours ") + cardinal(mi) + (mi == 1 ? " minute " : " minutes ") +
                   "and " + cardinal(sec) + (sec == 1 ? " second" : " seconds");
        }
        if (mi == 0) return cardinal(h) + " o'clock";
        return clockTime(h, mi);
    });

    // ISO dates 2024-03-05
    static const std::regex iso(R"(\b(1\d{3}|20\d{2})-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])\b)");
    s = replaceAll(s, iso, [](const std::smatch& m, const std::string&) {
        return kMonths[std::stoi(m[2].str()) - 1] + " " + numwords::ordinal(std::stoi(m[3].str())) + ", " +
               numwords::year(std::stoi(m[1].str()));
    });

    // numeric dates 12/25/2020, 5/3/99 (US order assumed; flagged when ambiguous)
    static const std::regex slashDate(R"(\b(\d{1,2})/(\d{1,2})/(\d{4}|\d{2})\b)");
    s = replaceAll(s, slashDate, [&flags](const std::smatch& m, const std::string&) {
        int a = std::stoi(m[1].str()), b = std::stoi(m[2].str());
        int y = std::stoi(m[3].str());
        int month = a, day = b;
        if (a > 12 && b <= 12) std::swap(month, day);
        else if (a <= 12 && b <= 12 && a != b) addFlag(flags, flag::ambiguousNumber);
        if (month < 1 || month > 12 || day < 1 || day > 31) {
            addFlag(flags, flag::ambiguousNumber);
            return cardinal(a) + " " + cardinal(b) + " " + cardinal(y);
        }
        std::string ys = m[3].length() == 4 ? numwords::year(y) : twoDigitYear(y);
        return kMonths[month - 1] + " " + numwords::ordinal(day) + ", " + ys;
    });

    // Month day[, year]: "March 5", "Mar. 5th, 1999"
    static const std::regex monthDay(std::string("\\b") + kMonthRe + R"(\s+(\d{1,2})(st|nd|rd|th)?\b)");
    s = replaceAll(s, monthDay, [](const std::smatch& m, const std::string&) {
        int mi = monthIndex(m[1].str());
        int d = std::stoi(m[2].str());
        if (mi < 0 || d < 1 || d > 31) return m.str();
        return kMonths[mi] + " " + numwords::ordinal(d);
    });

    // day Month: "5 March", "5th of March" -> "the fifth of March"
    static const std::regex dayMonth(std::string(R"(\b(\d{1,2})(st|nd|rd|th)?\s+(of\s+)?)") + kMonthRe + "(?![A-Za-z])");
    s = replaceAll(s, dayMonth, [&s](const std::smatch& m, const std::string& rest) {
        int d = std::stoi(m[1].str());
        int mi = monthIndex(m[4].str());
        if (mi < 0 || d < 1 || d > 31) return m.str();
        // avoid "the the" when the source already said "the 5th of March"
        size_t pos = s.size() - rest.size() - m.length();
        std::string prefix = toLower(s.substr(pos >= 4 ? pos - 4 : 0, pos >= 4 ? 4 : pos));
        std::string the = prefix == "the " ? "" : "the ";
        return the + numwords::ordinal(d) + " of " + kMonths[mi];
    });

    // mixed fractions "1 1/2"
    static const std::regex mixed(R"(\b(\d+)\s+(\d)/(\d{1,2})\b)");
    s = replaceAll(s, mixed, [&flags](const std::smatch& m, const std::string&) {
        uint64_t n = std::stoull(m[2].str()), d = std::stoull(m[3].str());
        if (d == 0) return m.str();
        addFlag(flags, flag::ambiguousNumber);
        return cardinal(std::stoull(m[1].str())) + " and " + cardinal(n) + " " +
               numwords::pluralOrdinalDenominator(d, n != 1);
    });

    // fractions "1/2", "3/4" (could be a date or a ratio -> flagged)
    static const std::regex frac(R"((^|[^\d/])(\d{1,3})/(\d{1,3})(?![\d/]))");
    s = replaceAll(s, frac, [&flags](const std::smatch& m, const std::string&) {
        uint64_t n = std::stoull(m[2].str()), d = std::stoull(m[3].str());
        if (d == 0) return m.str();
        addFlag(flags, flag::ambiguousNumber);
        if (d == 1) return m[1].str() + cardinal(n) + " over one";
        return m[1].str() + cardinal(n) + " " + numwords::pluralOrdinalDenominator(d, n != 1);
    });

    // numeric ranges "1990-1995", "9-5", "10–20%"  (hyphen, en dash, em dash)
    static const std::regex range(R"(\b(\d+(?:\.\d+)?)\s?(?:-|\xE2\x80\x93|\xE2\x80\x94)\s?(\$?)(\d+(?:\.\d+)?)\b)");
    s = replaceAll(s, range, [](const std::smatch& m, const std::string&) {
        std::string a = m[1].str(), b = m[3].str();
        auto isYear = [](const std::string& x) {
            return x.size() == 4 && x.find('.') == std::string::npos && std::stoi(x) >= 1000 && std::stoi(x) < 2100;
        };
        if (isYear(a) && (isYear(b) || b.size() == 2))
            return numwords::year(std::stoi(a)) + " to " +
                   (isYear(b) ? numwords::year(std::stoi(b)) : numwords::year(std::stoi(a.substr(0, 2) + b)));
        return a + " to " + m[2].str() + b;
    });

    return s;
}

// ---------------------------------------------------------------- token pass

struct Currency {
    const char* symbol;
    const char* one;
    const char* many;
    const char* subOne;
    const char* subMany;
};
const Currency kCurrencies[] = {
    {"US$", "dollar", "dollars", "cent", "cents"},
    {"$", "dollar", "dollars", "cent", "cents"},
    {"\xC2\xA3", "pound", "pounds", "penny", "pence"},
    {"\xE2\x82\xAC", "euro", "euros", "cent", "cents"},
    {"\xC2\xA5", "yen", "yen", "sen", "sen"},
};

struct Unit {
    const char* one;
    const char* many;
};
const std::map<std::string, Unit> kUnits = {
    {"km", {"kilometer", "kilometers"}},   {"kg", {"kilogram", "kilograms"}},
    {"cm", {"centimeter", "centimeters"}}, {"mm", {"millimeter", "millimeters"}},
    {"mi", {"mile", "miles"}},             {"ft", {"foot", "feet"}},
    {"lb", {"pound", "pounds"}},           {"lbs", {"pound", "pounds"}},
    {"oz", {"ounce", "ounces"}},           {"mg", {"milligram", "milligrams"}},
    {"ml", {"milliliter", "milliliters"}}, {"mph", {"mile per hour", "miles per hour"}},
    {"kph", {"kilometer per hour", "kilometers per hour"}}, {"km/h", {"kilometer per hour", "kilometers per hour"}},
    {"GB", {"gigabyte", "gigabytes"}},     {"MB", {"megabyte", "megabytes"}},
    {"KB", {"kilobyte", "kilobytes"}},     {"TB", {"terabyte", "terabytes"}},
    {"GHz", {"gigahertz", "gigahertz"}},   {"MHz", {"megahertz", "megahertz"}},
    {"kHz", {"kilohertz", "kilohertz"}},   {"Hz", {"hertz", "hertz"}},
    {"kW", {"kilowatt", "kilowatts"}},     {"kWh", {"kilowatt hour", "kilowatt hours"}},
    {"ms", {"millisecond", "milliseconds"}}, {"hrs", {"hour", "hours"}}, {"hr", {"hour", "hours"}},
    {"min", {"minute", "minutes"}},        {"sec", {"second", "seconds"}},
    {"yr", {"year", "years"}},             {"yrs", {"year", "years"}},
    {"fps", {"frame per second", "frames per second"}},
    {"x", {"times", "times"}},
    {"\xC2\xB0" "C", {"degree celsius", "degrees celsius"}}, {"\xC2\xB0" "F", {"degree fahrenheit", "degrees fahrenheit"}},
    {"\xC2\xB0", {"degree", "degrees"}},
};

const std::map<std::string, std::string> kMagnitudes = {
    {"k", "thousand"}, {"K", "thousand"}, {"m", "million"}, {"M", "million"}, {"mn", "million"},
    {"bn", "billion"}, {"b", "billion"},  {"B", "billion"}, {"tn", "trillion"}, {"T", "trillion"}};

const std::set<std::string> kMagnitudeWords = {"hundred", "thousand", "million", "billion", "trillion"};

const std::set<std::string> kYearCues = {"in", "since", "by", "from", "until", "till", "til", "of", "during",
                                         "circa", "year", "early", "late", "mid", "before", "after", "summer",
                                         "winter", "spring", "autumn", "fall", "christmas", "back"};
const std::set<std::string> kCountCues = {"about", "over", "under", "nearly", "almost", "than", "approximately",
                                          "some", "roughly", "only", "just", "exactly", "total", "costs", "cost"};
const std::set<std::string> kEraWords = {"AD", "BC", "BCE", "CE", "A.D.", "B.C."};

struct Token {
    std::string prefix, core, suffix;
};

bool isOpenChar(const std::string& s, size_t i, size_t& len) {
    unsigned char c = s[i];
    if (c == '"' || c == '\'' || c == '(' || c == '[' || c == '{') { len = 1; return true; }
    if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 &&
        ((unsigned char)s[i + 2] == 0x9C || (unsigned char)s[i + 2] == 0x98)) { len = 3; return true; }
    if (c == 0xC2 && i + 1 < s.size() && ((unsigned char)s[i + 1] == 0xBF || (unsigned char)s[i + 1] == 0xA1)) { len = 2; return true; }
    return false;
}

bool isCloseCharAt(const std::string& s, size_t end, size_t& len) {
    // checks the char ending at position end (exclusive)
    if (end == 0) return false;
    unsigned char c = s[end - 1];
    if (std::string(".,;:!?\"')]}").find(static_cast<char>(c)) != std::string::npos) { len = 1; return true; }
    if (end >= 3 && (unsigned char)s[end - 3] == 0xE2 && (unsigned char)s[end - 2] == 0x80) {
        unsigned char d = s[end - 1];
        if (d == 0x9D || d == 0x99 || d == 0xA6) { len = 3; return true; } // ” ’ …
    }
    return false;
}

Token splitToken(const std::string& t) {
    Token tok;
    size_t b = 0, e = t.size(), len = 0;
    while (b < e && isOpenChar(t, b, len)) b += len;
    while (e > b && isCloseCharAt(t, e, len)) e -= len;
    tok.prefix = t.substr(0, b);
    tok.core = t.substr(b, e - b);
    tok.suffix = t.substr(e);
    // a leading apostrophe before digits is a decade marker ('90s): keep it with the core
    if (!tok.prefix.empty() && tok.prefix.back() == '\'' && !tok.core.empty() && isDigit(tok.core[0])) {
        tok.prefix.pop_back();
        tok.core = "'" + tok.core;
    }
    // possessive/plural apostrophe at the end ("students'") stays in the suffix, fine either way
    return tok;
}

std::string spellLetters(const std::string& letters) {
    std::string out;
    for (char c : letters) {
        if (!isAlpha(c)) continue;
        if (!out.empty()) out += " ";
        out += static_cast<char>(isUpper(c) ? c - 'A' + 'a' : c);
    }
    return out;
}

bool containsStandaloneA(const std::string& spoken) {
    std::string padded = " " + spoken + " ";
    return padded.find(" a ") != std::string::npos;
}

class TokenPass {
public:
    TokenPass(const PronunciationDictionary& d, std::vector<std::string>& f) : dict(d), flags(f) {}

    std::string run(const std::string& text) {
        // split on whitespace
        std::vector<std::string> raw;
        std::string cur;
        for (char c : text) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!cur.empty()) raw.push_back(cur), cur.clear();
            } else cur += c;
        }
        if (!cur.empty()) raw.push_back(cur);
        toks.clear();
        for (auto& r : raw) toks.push_back(splitToken(r));

        std::vector<std::string> out;
        for (i = 0; i < toks.size(); ++i) {
            Token& t = toks[i];
            const bool capitalized = !t.core.empty() && isUpper(t.core[0]);
            std::string spoken = processToken(t);
            // keep sentence casing: "Dr. Smith ..." -> "Doctor Smith ...", "NASA said" -> "Nasa said"
            if (capitalized && sentenceStart() && !spoken.empty() && isLower(spoken[0]))
                spoken[0] = static_cast<char>(spoken[0] - 'a' + 'A');
            std::string piece = t.prefix + spoken + t.suffix;
            if (!piece.empty()) out.push_back(piece);
        }
        std::string joined;
        for (auto& p : out) joined += (joined.empty() ? "" : " ") + p;
        return joined;
    }

private:
    const PronunciationDictionary& dict;
    std::vector<std::string>& flags;
    std::vector<Token> toks;
    size_t i = 0;

    bool sentenceStart() const {
        if (i == 0) return true;
        const std::string& s = toks[i - 1].suffix;
        return s.find_first_of(".!?") != std::string::npos;
    }
    bool isLast() const { return i + 1 == toks.size(); }
    const Token* next() const { return i + 1 < toks.size() ? &toks[i + 1] : nullptr; }
    std::string prevCoreLower() const { return i > 0 ? toLower(toks[i - 1].core) : ""; }

    // Try a dictionary hit on core (+ one trailing period from the suffix).
    bool dictionaryHit(Token& t, std::string& spoken) {
        if (!t.suffix.empty() && t.suffix[0] == '.' && dict.lookup(t.core + ".", spoken)) {
            std::string rest = t.suffix.substr(1);
            // "No. 5" -> "number five"; "No." elsewhere is the word "no"
            if (t.core == "No" || t.core == "no" || t.core == "Nos") {
                const Token* n = next();
                if (!(n && !n->core.empty() && (isDigit(n->core[0]) || n->core[0] == '#'))) return false;
                spoken = t.core == "Nos" ? "numbers" : "number";
            }
            // the period doubled as the sentence end (abbreviation closes the text): keep it
            bool keepPeriod = isLast() && rest.find_first_of(".!?") == std::string::npos;
            t.suffix = (keepPeriod ? "." : "") + rest;
            return true;
        }
        if (dict.lookup(t.core, spoken)) return true;
        return false;
    }

    std::string processToken(Token& t) {
        if (t.core.empty()) {
            if (t.suffix.find('|') != std::string::npos) addFlag(flags, flag::symbol);
            return "";
        }
        std::string spoken;
        if (dictionaryHit(t, spoken)) {
            if (containsStandaloneA(spoken)) addFlag(flags, flag::letterA);
            return spoken;
        }
        if (hasDigit(t.core) || t.core[0] == '#') return numberToken(t);
        return wordToken(t.core);
    }

    std::string acronym(const std::string& base) {
        std::string spoken;
        if (dict.lookup(base, spoken)) return spoken;
        spoken = spellLetters(base);
        addFlag(flags, flag::spelledAcronym);
        if (containsStandaloneA(spoken)) addFlag(flags, flag::letterA);
        return spoken;
    }

    std::string wordToken(const std::string& core) {
        // plain symbols
        static const std::map<std::string, std::string> symbols = {
            {"&", "and"}, {"+", "plus"}, {"=", "equals"}, {"@", "at"}, {"%", "percent"}};
        auto sym = symbols.find(core);
        if (sym != symbols.end()) return sym->second;

        // ALL-CAPS acronyms with optional plural/possessive: FBI, CEOs, NASA's
        static const std::regex caps(R"(^([A-Z]{2,})('s|\xE2\x80\x99s|s)?$)");
        std::smatch m;
        if (std::regex_match(core, m, caps)) {
            std::string tail = m[2].matched ? (m[2].str() == "s" ? "s" : "'s") : "";
            if (!tail.empty()) {
                std::string spoken;
                if (dict.lookup(core, spoken)) return spoken; // whole-word entry wins (e.g. "AIDS")
            }
            std::string base = acronym(m[1].str());
            return base + tail;
        }
        // standalone capital "A" mid-sentence is almost always the letter
        if (core == "A" && !sentenceStart()) {
            addFlag(flags, flag::letterA);
            return core;
        }
        // hyphenated / slashed compounds: process parts (e.g. "and/or", "U.S.-based")
        if (core.find('/') != std::string::npos) {
            addFlag(flags, flag::symbol);
            std::string out;
            size_t start = 0;
            while (start <= core.size()) {
                size_t p = core.find('/', start);
                std::string part = core.substr(start, p == std::string::npos ? std::string::npos : p - start);
                if (!part.empty()) out += (out.empty() ? "" : " ") + wordToken(part);
                if (p == std::string::npos) break;
                start = p + 1;
            }
            return out;
        }
        if (core.find('-') != std::string::npos && core.size() > 1) {
            std::string out;
            bool changed = false;
            size_t start = 0;
            while (true) {
                size_t p = core.find('-', start);
                std::string part = core.substr(start, p == std::string::npos ? std::string::npos : p - start);
                std::string sp = part.empty() ? "" : wordToken(part);
                if (sp != part) changed = true;
                out += sp;
                if (p == std::string::npos) break;
                out += "-";
                start = p + 1;
            }
            if (changed) return out;
            return core;
        }
        // strip stray symbols that TTS front ends choke on
        std::string cleaned;
        for (char c : core) {
            if (std::string("*_~^<>|\\`#").find(c) != std::string::npos) {
                addFlag(flags, flag::symbol);
                continue;
            }
            cleaned += c;
        }
        return cleaned;
    }

    // -------- numbers

    std::string numberToken(Token& t) {
        std::string core = t.core;
        std::string pre;

        if (core[0] == '#') { // "#1" -> "number one"
            core = core.substr(1);
            if (core.empty() || !isDigit(core[0])) return wordToken(t.core);
            pre = "number ";
        }
        bool negative = false;
        if (core[0] == '-' && core.size() > 1) { negative = true; core = core.substr(1); }
        else if (core.compare(0, 3, "\xE2\x88\x92") == 0) { negative = true; core = core.substr(3); } // U+2212

        const Currency* cur = nullptr;
        for (auto& c : kCurrencies) {
            size_t n = strlen(c.symbol);
            if (core.compare(0, n, c.symbol) == 0) { cur = &c; core = core.substr(n); break; }
        }
        // decade marker 'NNs
        bool apostropheDecade = false;
        if (!core.empty() && core[0] == '\'') { apostropheDecade = true; core = core.substr(1); }

        static const std::regex num(R"(^(\d{1,3}(?:,\d{3})+(?:\.\d+)?|\d+(?:\.\d+)?|\.\d+)(.*)$)");
        std::smatch m;
        if (!std::regex_match(core, m, num)) return pre + mixedToken(t.core);
        std::string body = m[1].str(), tail = m[2].str();
        bool hasComma = body.find(',') != std::string::npos;
        bool isDecimal = body.find('.') != std::string::npos;
        std::string plain;
        for (char c : body) if (c != ',') plain += c;

        std::string sign = negative ? "minus " : "";
        auto intValue = [&]() -> uint64_t { return plain.size() > 18 ? 0 : std::stoull(plain); };

        // currency amounts
        if (cur) {
            std::string magnitude;
            auto mg = kMagnitudes.find(tail);
            if (mg != kMagnitudes.end()) magnitude = mg->second, tail.clear();
            else if (tail.empty() && next() && kMagnitudeWords.count(toLower(next()->core))) {
                magnitude = toLower(next()->core);
                consumeNext(t);
            }
            if (!tail.empty()) return pre + sign + mixedToken(t.core);
            if (!magnitude.empty()) {
                std::string amount = isDecimal ? numwords::decimal(plain) : cardinal(intValue());
                return pre + sign + amount + " " + magnitude + " " + cur->many;
            }
            if (isDecimal) {
                std::string whole = plain.substr(0, plain.find('.'));
                std::string fracS = plain.substr(plain.find('.') + 1);
                if (fracS.size() == 2) {
                    uint64_t w = whole.empty() ? 0 : std::stoull(whole), c = std::stoull(fracS);
                    std::string out;
                    if (w) out = cardinal(w) + " " + (w == 1 ? cur->one : cur->many);
                    if (c) out += (out.empty() ? "" : " and ") + cardinal(c) + " " + (c == 1 ? cur->subOne : cur->subMany);
                    if (out.empty()) out = std::string("zero ") + cur->many;
                    return pre + sign + out;
                }
                return pre + sign + numwords::decimal(plain) + " " + cur->many;
            }
            uint64_t v = intValue();
            return pre + sign + cardinal(v) + " " + (v == 1 ? cur->one : cur->many);
        }

        // percent
        if (tail == "%") return pre + sign + (isDecimal ? numwords::decimal(plain) : cardinal(intValue())) + " percent";

        // ordinals 1st 2nd 23rd 4th
        std::string lt = toLower(tail);
        if (!isDecimal && (lt == "st" || lt == "nd" || lt == "rd" || lt == "th")) return pre + numwords::ordinal(intValue());

        // decades 1990s, '90s, 80s
        if (!isDecimal && !hasComma && (lt == "s" || lt == "'s")) {
            uint64_t v = intValue();
            if ((plain.size() == 2 && v % 10 == 0) || (plain.size() == 4 && v % 10 == 0 && v >= 1000 && v < 2100))
                return pre + numwords::decade(static_cast<int>(v));
            return pre + cardinal(v) + "s";
        }
        (void)apostropheDecade;

        // magnitude suffix without currency: 5k
        if (tail == "k" || tail == "K") return pre + sign + (isDecimal ? numwords::decimal(plain) : cardinal(intValue())) + " thousand";

        // units attached (5km) or as next token (5 km)
        auto unitWord = [&](const std::string& u) -> std::string {
            auto it = kUnits.find(u);
            if (it == kUnits.end()) return "";
            bool one = !isDecimal && intValue() == 1;
            return one ? it->second.one : it->second.many;
        };
        if (!tail.empty()) {
            std::string uw = unitWord(tail);
            if (!uw.empty()) return pre + sign + (isDecimal ? numwords::decimal(plain) : cardinal(intValue())) + " " + uw;
            return pre + mixedToken(t.core);
        }
        if (next() && next()->prefix.empty()) {
            std::string uw = unitWord(next()->core);
            if (!uw.empty() && next()->core != "x") {
                std::string amount = isDecimal ? numwords::decimal(plain) : cardinal(intValue());
                consumeNext(t);
                return pre + sign + amount + " " + uw;
            }
        }

        if (isDecimal) return pre + sign + numwords::decimal(plain);
        if (hasComma) return pre + sign + cardinal(intValue());

        // plain integers
        if (plain.size() > 1 && plain[0] == '0') return pre + numwords::digits(plain);
        if (plain.size() > 12) {
            addFlag(flags, flag::ambiguousNumber);
            return pre + numwords::digits(plain);
        }
        uint64_t v = intValue();
        if (!negative && pre.empty() && plain.size() == 4 && v >= 1000 && v < 2100) return yearOrCount(static_cast<int>(v));
        return pre + sign + cardinal(v);
    }

    std::string yearOrCount(int v) {
        std::string prev = prevCoreLower();
        const Token* n = next();
        bool yearCue = kYearCues.count(prev) || monthIndex(prev) >= 0 || (n && kEraWords.count(n->core));
        // "March fifth, 1999": the pre-pass already wrote the month, so a preceding ordinal + comma is a year cue
        if (i > 0 && toks[i - 1].suffix == "," && (prev.size() > 2 && (prev.substr(prev.size() - 2) == "th" ||
                                                                       prev.substr(prev.size() - 2) == "st" ||
                                                                       prev.substr(prev.size() - 2) == "nd" ||
                                                                       prev.substr(prev.size() - 2) == "rd")))
            yearCue = true;
        static const std::set<std::string> notPlural = {"this", "does", "plus", "thus", "yes", "less", "was", "has", "is",
                                                         "its", "his", "hers", "ours", "yours", "theirs", "always",
                                                         "perhaps", "towards", "afterwards", "whereas", "unless"};
        bool plural = n && n->prefix.empty() && toks[i].suffix.empty() && n->core.size() >= 4 && isLower(n->core[0]) &&
                      n->core.back() == 's' && n->core[n->core.size() - 2] != 's' && !notPlural.count(n->core);
        bool countCue = kCountCues.count(prev) || plural;
        if (yearCue && !countCue) return numwords::year(v);
        if (countCue && !yearCue) return cardinal(v);
        if (v >= 1100) {
            addFlag(flags, flag::ambiguousNumber);
            return numwords::year(v);
        }
        return cardinal(v);
    }

    // Merge the next token into this one (e.g. "$5 million", "5 km"); the emptied token outputs nothing.
    void consumeNext(Token& t) {
        Token& n = toks[i + 1];
        t.suffix = n.suffix;
        n.prefix.clear();
        n.core.clear();
        n.suffix.clear();
    }

    // Letters and digits glued together (MP3, 3D, B2B, COVID-19): split into runs.
    std::string mixedToken(const std::string& s) {
        std::vector<std::string> parts;
        std::string run;
        int kind = -1; // 0 letters, 1 digits
        auto flush = [&]() {
            if (run.empty()) return;
            if (kind == 1) parts.push_back(run.size() > 1 && run[0] == '0' ? numwords::digits(run) : cardinal(std::stoull(run.size() > 18 ? run.substr(0, 18) : run)));
            else parts.push_back(run.size() >= 2 && std::all_of(run.begin(), run.end(), isUpper) ? acronym(run) : wordToken(run));
            run.clear();
        };
        for (size_t k = 0; k < s.size(); ++k) {
            unsigned char c = s[k];
            int kk = isDigit(c) ? 1 : (isAlpha(c) || c >= 0x80 || c == '\'') ? 0 : -1;
            if (kk != kind) flush();
            kind = kk;
            if (kk == -1) {
                if (c == '.' && k + 1 < s.size() && isDigit(s[k + 1]) && !parts.empty()) parts.push_back("point");
                else if (c == '+' ) parts.push_back("plus");
                else if (c == '&') parts.push_back("and");
                else if (c == '%') parts.push_back("percent");
                else if (c == '$') { /* dropped */ }
                continue;
            }
            run += static_cast<char>(c);
        }
        flush();
        std::string out;
        for (auto& p : parts) out += (out.empty() ? "" : " ") + p;
        return out;
    }
};

std::string cleanupSpaces(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        if ((c == ',' || c == '.' || c == '!' || c == '?' || c == ';' || c == ':') && !out.empty() && out.back() == ' ')
            out.pop_back();
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    // ",," from phone numbers followed by commas, etc.
    std::string dedup;
    for (char c : out) {
        if (c == ',' && !dedup.empty() && dedup.back() == ',') continue;
        dedup += c;
    }
    return dedup;
}

} // namespace

NormalizeResult Normalizer::normalize(const std::string& text, const NormalizeOptions& opts) const {
    NormalizeResult r;
    std::string s = prePass(text, r.flags);
    TokenPass pass(dict_, r.flags);
    s = pass.run(s);
    s = cleanupSpaces(s);
    if (s.find('|') != std::string::npos) {
        std::replace(s.begin(), s.end(), '|', ' ');
        addFlag(r.flags, flag::symbol);
        s = cleanupSpaces(s);
    }
    if (hasDigit(s)) addFlag(r.flags, flag::digitsLeft);
    if (opts.lowercaseAll) s = toLower(s);
    r.text = s;
    return r;
}

NormalizeResult normalizeText(const std::string& text, bool lowercaseAll, const PronunciationDictionary& dict) {
    Normalizer n(dict);
    NormalizeOptions o;
    o.lowercaseAll = lowercaseAll;
    return n.normalize(text, o);
}

} // namespace ttsdc
