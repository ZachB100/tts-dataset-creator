#include "Normalizer.hpp"

#include <fstream>
#include <set>
#include <sstream>

#include "../vendor/json.hpp"

namespace ttsdc {

namespace {
// Built-in defaults. Users edit a copy in Application Support (the app writes it on first run).
// Format: {"entries": {written: spoken}, "titles": [written...]}. Titles never end a sentence.
const char* kDefaultDictionary = R"JSON({
  "version": 1,
  "titles": ["Dr.", "Mr.", "Mrs.", "Ms.", "Mx.", "Prof.", "St.", "Mt.", "Gen.", "Capt.", "Lt.",
             "Sgt.", "Col.", "Rev.", "Gov.", "Sen.", "Rep.", "Pres.", "Hon.", "Fr.", "Jr.", "Sr.", "No."],
  "entries": {
    "Dr.": "doctor", "Mr.": "mister", "Mrs.": "missus", "Ms.": "miz", "Mx.": "mix",
    "Prof.": "professor", "St.": "saint", "Mt.": "mount", "Gen.": "general", "Capt.": "captain",
    "Lt.": "lieutenant", "Sgt.": "sergeant", "Col.": "colonel", "Rev.": "reverend",
    "Gov.": "governor", "Sen.": "senator", "Rep.": "representative", "Pres.": "president",
    "Hon.": "honorable", "Fr.": "father", "No.": "number", "Nos.": "numbers", "Jr.": "junior", "Sr.": "senior",
    "e.g.": "for example", "i.e.": "that is", "etc.": "et cetera", "vs.": "versus", "vs": "versus",
    "approx.": "approximately", "cf.": "compare", "al.": "al", "esp.": "especially",
    "Inc.": "incorporated", "Ltd.": "limited", "Co.": "company", "Corp.": "corporation",
    "Dept.": "department", "Ave.": "avenue", "Blvd.": "boulevard", "Rd.": "road",
    "Jan.": "January", "Feb.": "February", "Aug.": "August", "Sept.": "September",
    "Oct.": "October", "Nov.": "November", "Dec.": "December",
    "a.m.": "a m", "p.m.": "p m", "A.M.": "a m", "P.M.": "p m",
    "U.S.": "u s", "U.S.A.": "u s a", "U.K.": "u k", "U.N.": "u n", "E.U.": "e u",
    "Ph.D.": "p h d", "PhD": "p h d", "B.C.": "b c", "A.D.": "a d",
    "NASA": "nasa", "NATO": "nato", "UNESCO": "unesco", "UNICEF": "unicef", "OPEC": "opec",
    "FIFA": "fifa", "AIDS": "aids", "NASDAQ": "nasdaq", "SWAT": "swat", "COVID": "covid",
    "NIMBY": "nimby", "FOMO": "fomo", "IKEA": "ikea", "LASER": "laser", "RADAR": "radar",
    "SCUBA": "scuba", "GIF": "gif", "JPEG": "jay peg", "PIN": "pin", "SIM": "sim", "LED": "l e d",
    "OK": "okay", "Ok": "okay", "ASAP": "a s a p", "NAFTA": "nafta", "ESPN": "e s p n",
    "UK": "u k", "US": "u s", "IT": "i t", "USA": "u s a", "EU": "e u", "UN": "u n", "BBC": "b b c", "TV": "t v",
    "CEO": "c e o", "CFO": "c f o", "CTO": "c t o", "HR": "h r", "PC": "p c", "DJ": "d j",
    "DNA": "d n a", "GPS": "g p s", "PDF": "p d f", "URL": "u r l", "USB": "u s b",
    "PR": "p r", "VIP": "v i p", "BMW": "b m w", "NHS": "n h s", "NBA": "n b a", "NFL": "n f l",
    "CV": "c v", "ID": "i d", "IQ": "i q", "PhDs": "p h ds", "mph": "miles per hour",
    "km/h": "kilometers per hour", "&": "and"
  }
})JSON";

std::string lowerFirst(std::string s) {
    if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z') s[0] = static_cast<char>(s[0] - 'A' + 'a');
    return s;
}
} // namespace

PronunciationDictionary::PronunciationDictionary() {
    std::string err;
    loadJSON(kDefaultDictionary, err);
}

std::string PronunciationDictionary::defaultJSON() { return kDefaultDictionary; }

bool PronunciationDictionary::loadJSON(const std::string& json, std::string& error) {
    try {
        auto j = nlohmann::json::parse(json);
        std::map<std::string, std::string> fresh;
        std::set<std::string> titles;
        const nlohmann::json* entries = &j;
        if (j.contains("entries")) entries = &j["entries"];
        for (auto it = entries->begin(); it != entries->end(); ++it) {
            if (!it.value().is_string()) continue; // skips "version"/"titles" in flat files
            fresh[it.key()] = it.value().get<std::string>();
        }
        if (j.contains("titles") && j["titles"].is_array())
            for (auto& t : j["titles"])
                if (t.is_string()) titles.insert(t.get<std::string>());
        entries_ = std::move(fresh);
        titles_ = std::move(titles);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool PronunciationDictionary::loadFile(const std::string& path, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return loadJSON(ss.str(), error);
}

std::string PronunciationDictionary::toJSON() const {
    nlohmann::ordered_json j;
    j["version"] = 1;
    j["titles"] = nlohmann::json::array();
    for (auto& t : titles_) j["titles"].push_back(t);
    j["entries"] = nlohmann::ordered_json::object();
    for (auto& [k, v] : entries_) j["entries"][k] = v;
    return j.dump(2);
}

bool PronunciationDictionary::saveFile(const std::string& path, std::string& error) const {
    std::ofstream f(path);
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    f << toJSON() << "\n";
    return true;
}

void PronunciationDictionary::set(const std::string& w, const std::string& s) { entries_[w] = s; }
void PronunciationDictionary::remove(const std::string& w) { entries_.erase(w); }

bool PronunciationDictionary::lookup(const std::string& written, std::string& spoken) const {
    auto it = entries_.find(written);
    if (it == entries_.end()) {
        std::string lf = lowerFirst(written);
        if (lf != written) {
            it = entries_.find(lf);
            // only lowercase keys match capitalised words ("Etc." -> "etc."), and the result is capitalised
            if (it != entries_.end()) {
                spoken = it->second;
                if (!spoken.empty() && spoken[0] >= 'a' && spoken[0] <= 'z')
                    spoken[0] = static_cast<char>(spoken[0] - 'a' + 'A');
                return true;
            }
        }
        return false;
    }
    spoken = it->second;
    return true;
}

bool PronunciationDictionary::isTitle(const std::string& w) const { return titles_.count(w) > 0; }

bool PronunciationDictionary::isAbbreviation(const std::string& w) const {
    std::string s;
    return !w.empty() && w.back() == '.' && lookup(w, s);
}

std::vector<std::string> PronunciationDictionary::keys() const {
    std::vector<std::string> k;
    for (auto& [a, b] : entries_) k.push_back(a);
    return k;
}

} // namespace ttsdc
