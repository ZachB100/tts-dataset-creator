#include "Normalizer.hpp"

#include <array>

namespace ttsdc::numwords {

namespace {
const std::array<const char*, 20> kOnes = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
    "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
const std::array<const char*, 10> kTens = {
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
const std::array<const char*, 7> kScales = {
    "", "thousand", "million", "billion", "trillion", "quadrillion", "quintillion"};

std::string under100(int n) {
    if (n < 20) return kOnes[n];
    std::string s = kTens[n / 10];
    if (n % 10) s += std::string("-") + kOnes[n % 10];
    return s;
}

std::string under1000(int n) {
    std::string s;
    if (n >= 100) {
        s = std::string(kOnes[n / 100]) + " hundred";
        n %= 100;
        if (n) s += " ";
    }
    if (n || s.empty()) s += under100(n);
    return s;
}

// "one" -> "first", "twenty" -> "twentieth", "twelve" -> "twelfth"
std::string ordinalizeWord(const std::string& w) {
    static const std::pair<const char*, const char*> irregular[] = {
        {"one", "first"}, {"two", "second"}, {"three", "third"}, {"five", "fifth"},
        {"eight", "eighth"}, {"nine", "ninth"}, {"twelve", "twelfth"}};
    for (auto& [a, b] : irregular)
        if (w == a) return b;
    if (!w.empty() && w.back() == 'y') return w.substr(0, w.size() - 1) + "ieth";
    return w + "th";
}
} // namespace

std::string cardinal(uint64_t n) {
    if (n == 0) return "zero";
    std::vector<std::string> parts;
    int scale = 0;
    while (n > 0) {
        int chunk = static_cast<int>(n % 1000);
        if (chunk) {
            std::string p = under1000(chunk);
            if (scale) p += std::string(" ") + kScales[scale];
            parts.insert(parts.begin(), p);
        }
        n /= 1000;
        ++scale;
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? " " : "") + parts[i];
    return out;
}

std::string ordinal(uint64_t n) {
    std::string c = cardinal(n);
    // ordinalize the last word, which may be hyphenated ("twenty-one" -> "twenty-first")
    size_t cut = c.find_last_of(" -");
    std::string head = cut == std::string::npos ? "" : c.substr(0, cut + 1);
    std::string last = cut == std::string::npos ? c : c.substr(cut + 1);
    return head + ordinalizeWord(last);
}

std::string year(int n) {
    if (n < 0) return cardinal(static_cast<uint64_t>(-n));
    if (n < 1000 || n >= 10000) return cardinal(static_cast<uint64_t>(n));
    int hi = n / 100, lo = n % 100;
    if (n >= 2000 && n <= 2009) return n == 2000 ? "two thousand" : "two thousand " + under100(lo);
    if (lo == 0) return under100(hi) + " hundred";
    if (lo < 10) return under100(hi) + " oh " + under100(lo);
    return under100(hi) + " " + under100(lo);
}

std::string decade(int n) {
    // 1990 -> "nineteen nineties", 90 -> "nineties", 2000 -> "two thousands", 1900 -> "nineteen hundreds"
    auto pluralize = [](std::string w) {
        if (!w.empty() && w.back() == 'y') return w.substr(0, w.size() - 1) + "ies";
        return w + "s";
    };
    if (n < 100) return pluralize(under100(n));
    std::string y = year(n);
    size_t cut = y.find_last_of(" -");
    std::string head = cut == std::string::npos ? "" : y.substr(0, cut + 1);
    std::string last = cut == std::string::npos ? y : y.substr(cut + 1);
    return head + pluralize(last);
}

std::string digits(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c < '0' || c > '9') continue;
        if (!out.empty()) out += " ";
        out += kOnes[c - '0'];
    }
    return out;
}

std::string decimal(const std::string& s) {
    std::string intPart, frac;
    size_t dot = s.find('.');
    for (size_t i = 0; i < (dot == std::string::npos ? s.size() : dot); ++i)
        if (s[i] >= '0' && s[i] <= '9') intPart += s[i];
    if (dot != std::string::npos) frac = s.substr(dot + 1);
    std::string out;
    if (!intPart.empty()) {
        // strip leading zeros but keep a single zero
        size_t nz = intPart.find_first_not_of('0');
        intPart = nz == std::string::npos ? "0" : intPart.substr(nz);
        out = intPart.size() > 18 ? digits(intPart) : cardinal(std::stoull(intPart));
    }
    if (!frac.empty()) {
        if (!out.empty()) out += " ";
        out += "point " + digits(frac);
    }
    return out;
}

std::string pluralOrdinalDenominator(uint64_t d, bool plural) {
    std::string w;
    if (d == 2) w = plural ? "halves" : "half";
    else if (d == 4) w = plural ? "quarters" : "quarter";
    else {
        w = ordinal(d);
        if (plural) w += "s";
    }
    return w;
}

} // namespace ttsdc::numwords
