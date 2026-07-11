/*
    Stremio addon-protocol client for Switchfin

    This is the data layer that replaces the Jellyfin API. Everything in the
    Stremio addon protocol is a plain HTTP GET that returns JSON, so this mirrors
    Switchfin's jellyfin::getJSON helper -- minus the auth header, and taking a
    full URL (because we talk to two hosts: Cinemeta for metadata, the user's
    stream addon for streams).
*/
#pragma once

#include <nlohmann/json.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>
#include "api/http.hpp"

#include <thread>
#include <chrono>
#include <fstream>
#include <sys/stat.h>
#include <regex>
#include <vector>
#include <set>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace stremio {

using OnError = std::function<void(const std::string&)>;

// Cinemeta sometimes sends null (or wrong-typed) fields; read tolerantly so a
// single null doesn't blow up the whole parse.
inline std::string jstr(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}
inline int jint(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return 0;
    return (int)it->get<double>();
}
// Array-of-strings field (cast, director, genres). Non-string entries skipped.
inline std::vector<std::string> jstrlist(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (auto& e : *it)
        if (e.is_string()) out.push_back(e.get<std::string>());
    return out;
}

// ---------------------------------------------------------------------------
// Addon bases
// ---------------------------------------------------------------------------

// Cinemeta: public catalog + metadata addon (browse / search / details).
inline const std::string CINEMETA = "https://v3-cinemeta.strem.io";

// The user's stream addons (AIOStreams, Torrentio, or anything implementing
// the Stremio "stream" resource). Each base is everything BEFORE
// "/manifest.json". Stream requests look like:
//   {addon}/stream/movie/{imdbId}.json
//   {addon}/stream/series/{imdbId}:{season}:{episode}.json
// Multiple addons are supported (one bare URL line each in
// streamfin-addon.txt); on play, ALL of them are queried and the results
// merged in this order. Set on first launch via the on-screen keyboard (single
// addon) or the text file, persisted in the config directory. Empty until the
// user provides one (catalog browsing still works).
inline std::vector<std::string> STREAM_ADDONS;

// Optional poster provider: a URL template with an {imdbId} placeholder,
// e.g. RPDB's rating-embedded posters. When set, all poster images load from
// it (and the text ★ badge is hidden — the rating is baked into the image).
// Configured via streamfin-addon.txt lines "rpdb=KEY" or "poster=TEMPLATE".
inline std::string POSTER_TEMPLATE;

// Optional subtitles addon (SubSource, OpenSubtitles, or anything
// implementing the Stremio "subtitles" resource). Base URL, like the stream
// addon. Configured via a "subtitles=URL" line in streamfin-addon.txt.
// Requests look like: {SUBTITLES_ADDON}/subtitles/{type}/{id}.json
inline std::string SUBTITLES_ADDON;

inline std::string trimJunk(std::string s) {
    auto junk = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\''; };
    while (!s.empty() && junk(s.front())) s.erase(s.begin());
    while (!s.empty() && junk(s.back())) s.pop_back();
    return s;
}

// Normalize a pasted addon URL: trim whitespace/quotes, drop a trailing
// "/manifest.json" and trailing slashes. Returns empty if it isn't http(s).
inline std::string normalizeAddonUrl(std::string s) {
    s = trimJunk(s);
    const std::string suffix = "/manifest.json";
    if (s.size() > suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
        s.resize(s.size() - suffix.size());
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.rfind("http://", 0) != 0 && s.rfind("https://", 0) != 0) return std::string();
    return s;
}

// A poster template must be http(s) and contain the {imdbId} placeholder.
inline std::string normalizePosterTemplate(std::string s) {
    s = trimJunk(s);
    if (s.rfind("http://", 0) != 0 && s.rfind("https://", 0) != 0) return std::string();
    if (s.find("{imdbId}") == std::string::npos) return std::string();
    return s;
}

// RatingPosterDB shorthand: "rpdb=KEY" expands to their rated-poster URL.
// ?fallback=true keeps unrated titles showing a normal poster.
inline std::string rpdbTemplate(const std::string& key) {
    return "https://api.ratingposterdb.com/" + key + "/imdb/poster-default/{imdbId}.jpg?fallback=true";
}

// Poster for a title: the provider template when configured (IMDb ids only),
// otherwise whatever poster the catalog gave us. Strips ":season:episode" so
// series ids resolve to the series poster.
inline std::string posterUrl(const std::string& id, const std::string& fallback) {
    if (!POSTER_TEMPLATE.empty() && id.rfind("tt", 0) == 0) {
        std::string tt = id.substr(0, id.find(':'));
        std::string url = POSTER_TEMPLATE;
        url.replace(url.find("{imdbId}"), 8, tt);
        return url;
    }
    return fallback;
}

inline std::string addonConfigPath(const std::string& configDir) { return configDir + "/stremio_addon.json"; }

inline void saveConfig(const std::string& configDir) {
    ::mkdir(configDir.c_str(), 0777);
    try {
        std::ofstream out(addonConfigPath(configDir));
        if (out.is_open())
            // "url" (first addon) is kept alongside "urls" so a config written
            // by this version still works if an older build reads it.
            out << nlohmann::json{{"url", STREAM_ADDONS.empty() ? "" : STREAM_ADDONS.front()},
                       {"urls", STREAM_ADDONS}, {"poster", POSTER_TEMPLATE}, {"subtitles", SUBTITLES_ADDON}}
                       .dump(2);
    } catch (const std::exception& e) {
        brls::Logger::warning("saveConfig: {}", e.what());
    }
}

inline void loadAddon(const std::string& configDir) {
    try {
        std::ifstream in(addonConfigPath(configDir));
        if (!in.is_open()) return;
        nlohmann::json j;
        in >> j;
        // New format: "urls" array. Old format (pre-multi-addon): "url" string.
        STREAM_ADDONS.clear();
        for (auto& u : jstrlist(j, "urls")) {
            std::string n = normalizeAddonUrl(u);
            if (!n.empty()) STREAM_ADDONS.push_back(n);
        }
        if (STREAM_ADDONS.empty()) {
            std::string n = normalizeAddonUrl(jstr(j, "url"));
            if (!n.empty()) STREAM_ADDONS.push_back(n);
        }
        POSTER_TEMPLATE = normalizePosterTemplate(jstr(j, "poster"));
        SUBTITLES_ADDON = normalizeAddonUrl(jstr(j, "subtitles"));
    } catch (const std::exception& e) {
        brls::Logger::warning("loadAddon: {}", e.what());
    }
}

inline void saveAddon(const std::string& configDir, const std::string& url) {
    STREAM_ADDONS.clear();
    std::string n = normalizeAddonUrl(url);
    if (!n.empty()) STREAM_ADDONS.push_back(n);
    saveConfig(configDir);
}

// Easier setup than typing on the on-screen keyboard: the user drops a
// plain-text file on the SD card. Line format (any order, all optional):
//   https://your-stream-addon...      a stream addon base URL — repeat the
//                                     line to use several addons at once
//   rpdb=YOUR_KEY                     rated posters via RatingPosterDB
//   poster=https://...{imdbId}...     any custom poster provider template
// Checked at every launch; when the file exists it is the source of truth
// and is persisted, so editing it is also how you change settings later.
inline void importAddonFromFile(const std::string& configDir) {
    const std::string candidates[] = {
        "sdmc:/switch/streamfin-addon.txt",   // next to where the .nro lives
        configDir + "/addon.txt",             // alongside the app's other config
    };
    for (auto& path : candidates) {
        std::ifstream in(path);
        if (!in.is_open()) continue;

        std::vector<std::string> urls;
        std::string poster, subs, line;
        while (std::getline(in, line)) {
            line = trimJunk(line);
            if (line.rfind("poster=", 0) == 0) {
                poster = normalizePosterTemplate(line.substr(7));
            } else if (line.rfind("rpdb=", 0) == 0) {
                std::string key = trimJunk(line.substr(5));
                if (!key.empty()) poster = rpdbTemplate(key);
            } else if (line.rfind("subtitles=", 0) == 0) {
                subs = normalizeAddonUrl(line.substr(10));
            } else if (!line.empty() && line[0] != '#') {
                std::string u = normalizeAddonUrl(line);
                // Every bare URL line is an addon; skip exact repeats.
                if (!u.empty() && std::find(urls.begin(), urls.end(), u) == urls.end())
                    urls.push_back(u);
            }
        }

        // The file is the source of truth: apply all values (an absent
        // rpdb/poster/subtitles line turns that feature off again). Missing
        // addon lines never wipe a working addon list.
        std::vector<std::string> effective = urls.empty() ? STREAM_ADDONS : urls;
        if (effective != STREAM_ADDONS || poster != POSTER_TEMPLATE || subs != SUBTITLES_ADDON) {
            STREAM_ADDONS = effective;
            POSTER_TEMPLATE = poster;
            SUBTITLES_ADDON = subs;
            saveConfig(configDir);
            brls::Logger::info("settings imported from {}", path);
        }
        return;  // first file found wins
    }
}

// ---------------------------------------------------------------------------
// Generic async JSON GET (runs off the UI thread, calls back on it)
// ---------------------------------------------------------------------------
template <typename Result>
inline void getJSON(std::function<void(Result)> then, OnError error, const std::string& url) {
    brls::async([then, error, url]() {
        std::string lastErr = "request failed";
        // Retry a few times to ride out cold-start DNS / transient failures.
        for (int attempt = 0; attempt < 3; ++attempt) {
            try {
                auto resp = HTTP::get(url, HTTP::Timeout{20000});
                if (resp.empty()) {
                    lastErr = "empty response";
                } else {
                    auto j = nlohmann::json::parse(resp).get<Result>();
                    brls::sync(std::bind(std::move(then), std::move(j)));
                    return;
                }
            } catch (const std::exception& ex) {
                lastErr = ex.what();
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (error) brls::sync(std::bind(error, lastErr));
    });
}

// ---------------------------------------------------------------------------
// Models (unknown JSON fields are ignored automatically by nlohmann)
// ---------------------------------------------------------------------------

// A catalog entry / lightweight meta summary.
struct Meta {
    std::string id;    // e.g. "tt1375666"
    std::string type;  // "movie" | "series"
    std::string name;
    std::string poster;
    std::string year;        // release year, e.g. "2010" (series may be "2010-2019")
    std::string imdbRating;  // e.g. "7.5"; Cinemeta sends it in every catalog item
};
inline void from_json(const nlohmann::json& j, Meta& m) {
    m.id = jstr(j, "id");
    m.type = jstr(j, "type");
    m.name = jstr(j, "name");
    m.poster = jstr(j, "poster");
    m.year = jstr(j, "year");
    m.imdbRating = jstr(j, "imdbRating");
}
inline void to_json(nlohmann::json& j, const Meta& m) {
    j = nlohmann::json{{"id", m.id}, {"type", m.type}, {"name", m.name}, {"poster", m.poster}, {"year", m.year},
        {"imdbRating", m.imdbRating}};
}

// Response of /catalog/{type}/{id}.json
struct MetaList {
    std::vector<Meta> metas;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MetaList, metas);

// One subtitle offered by a subtitles addon (/subtitles/{type}/{id}.json).
struct Subtitle {
    std::string url;   // direct link to the .srt/.vtt file
    std::string lang;  // ISO code or language name, addon-dependent
};
inline void from_json(const nlohmann::json& j, Subtitle& s) {
    s.url = jstr(j, "url");
    s.lang = jstr(j, "lang");
}
struct SubtitleList {
    std::vector<Subtitle> subtitles;
};
inline void from_json(const nlohmann::json& j, SubtitleList& l) {
    l.subtitles.clear();
    auto it = j.find("subtitles");
    if (it == j.end() || !it->is_array()) return;
    for (auto& e : *it) {
        Subtitle s = e.get<Subtitle>();
        if (!s.url.empty()) l.subtitles.push_back(s);
    }
}

// A single playable result from a stream addon (AIOStreams).
struct Stream {
    std::string name;         // source/quality label
    std::string title;        // detailed label (older addons)
    std::string description;  // detailed label (newer addons, incl. AIOStreams)
    std::string url;          // <-- the playable (debrid) URL we hand to MPV
};
inline void from_json(const nlohmann::json& j, Stream& s) {
    s.name = jstr(j, "name");
    s.title = jstr(j, "title");
    s.description = jstr(j, "description");
    s.url = jstr(j, "url");
}

// Response of /stream/{type}/{id}.json
struct StreamList {
    std::vector<Stream> streams;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StreamList, streams);

// Fetch /stream/{type}/{id}.json from EVERY configured addon in parallel and
// merge the results in addon order (first addon in the file wins ties).
// Duplicate stream URLs are dropped — AIOStreams commonly wraps Torrentio, so
// the same debrid link can come back from both. Per-addon failures are
// tolerated: the merged list is delivered as long as at least one addon
// answered; error() fires only when every addon failed. All getJSON callbacks
// arrive on the UI thread, so the shared state needs no locking.
inline void getStreams(const std::string& type, const std::string& id,
    std::function<void(std::vector<Stream>)> then, OnError error) {
    struct Agg {
        std::vector<std::vector<Stream>> results;
        size_t pending;
        bool anyOk = false;
        std::string lastErr;
    };
    if (STREAM_ADDONS.empty()) {  // call sites check first; belt-and-braces
        if (error) error("no stream addon configured");
        return;
    }
    auto agg = std::make_shared<Agg>();
    agg->results.resize(STREAM_ADDONS.size());
    agg->pending = STREAM_ADDONS.size();

    auto finish = [agg, then, error]() {
        if (--agg->pending > 0) return;
        std::vector<Stream> merged;
        std::set<std::string> seen;
        for (auto& r : agg->results)
            for (auto& s : r)
                if (s.url.empty() || seen.insert(s.url).second) merged.push_back(s);
        if (!agg->anyOk) {
            if (error) error(agg->lastErr);
            return;
        }
        if (then) then(std::move(merged));
    };

    for (size_t i = 0; i < STREAM_ADDONS.size(); ++i) {
        std::string url = STREAM_ADDONS[i] + "/stream/" + type + "/" + id + ".json";
        getJSON<StreamList>(
            [agg, i, finish](StreamList r) {
                agg->results[i] = std::move(r.streams);
                agg->anyOk = true;
                finish();
            },
            [agg, finish](const std::string& e) {
                agg->lastErr = e;
                finish();
            },
            url);
    }
}

// ---------------------------------------------------------------------------
// Stream display parsing (for a clean, readable picker)
//
// AIOStreams packs everything (source, quality, codec, audio, size, duration,
// and a long country/language list) into one description blob. parseStream()
// pulls out the useful bits so the UI can show a tidy title + a few tags
// instead of one giant run-on line. Everything is best-effort: if a field
// isn't found it's simply omitted, and parsing never throws.
// ---------------------------------------------------------------------------
struct StreamInfo {
    std::string title;                // e.g. "⚡ Knaben · 1080p"
    std::vector<std::string> meta;    // source, video codec, size, duration
    std::vector<std::string> audio;   // audio codecs + channel layout
    std::vector<std::string> langs;   // AUDIO languages (e.g. {"PL","GB"})
    std::vector<std::string> badges;  // quality flags: 4K / HDR / HDR10+ / DV
    std::string source;               // tracker/indexer, e.g. "The Pirate Bay"
    std::string raw;                  // fallback (full description) if nothing parsed
};

// Drop every non-ASCII byte (strips emoji/icons the addon embeds in labels).
inline std::string stripNonAscii(const std::string& s) {
    std::string out;
    for (unsigned char c : s)
        if (c < 0x80) out += (char)c;
    return out;
}

inline std::string reFirst(const std::string& s, const char* pat) {
    try {
        std::regex re(pat, std::regex::icase);
        std::smatch m;
        if (std::regex_search(s, m, re)) return (m.size() > 1 ? m[1].str() : m[0].str());
    } catch (...) {
    }
    return std::string();
}

inline std::string reStrip(const std::string& s, const char* pat) {
    try {
        return std::regex_replace(s, std::regex(pat, std::regex::icase), " ");
    } catch (...) {
        return s;
    }
}

inline std::string trimCollapse(std::string s) {
    s = reStrip(s, "\\s+");
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}

// Pull the AUDIO language list out of the description. Your AIOStreams renders
// audio languages right after a globe icon (🌐) and subtitle languages after a
// memo icon (📝); we read the codes between the two. Tolerant of a few icon
// variants and returns empty (rather than guessing) if no marker is found.
inline std::vector<std::string> extractAudioLangs(const std::string& desc) {
    std::vector<std::string> out;
    static const char* audioMarks[] = {
        "\xF0\x9F\x8C\x90", "\xF0\x9F\x8C\x8D", "\xF0\x9F\x8C\x8E",
        "\xF0\x9F\x8C\x8F", "\xF0\x9F\x97\xA3", "\xF0\x9F\x94\x8A"};  // 🌐 🌍 🌎 🌏 🗣 🔊
    size_t start = std::string::npos, markLen = 0;
    for (auto m : audioMarks) {
        size_t p = desc.find(m);
        if (p != std::string::npos && (start == std::string::npos || p < start)) {
            start = p;
            markLen = std::strlen(m);
        }
    }
    if (start == std::string::npos) return out;
    size_t from = start + markLen;
    // Language codes are plain ASCII; the NEXT non-ASCII byte begins the next
    // section (subtitles, source, etc.). Stopping there keeps the source name
    // (e.g. "The Pirate Bay") from leaking in as bogus codes.
    size_t end = desc.size();
    for (size_t i = from; i < desc.size(); ++i) {
        if ((unsigned char)desc[i] >= 0x80) {
            end = i;
            break;
        }
    }
    std::string seg = desc.substr(from, end - from);
    try {
        std::regex re("[A-Za-z]{2,3}");
        for (auto it = std::sregex_iterator(seg.begin(), seg.end(), re); it != std::sregex_iterator(); ++it) {
            std::string t = it->str();
            for (auto& c : t) c = (char)std::toupper((unsigned char)c);
            if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
            if (out.size() >= 6) break;
        }
    } catch (...) {
    }
    return out;
}

// Tracker/indexer name, rendered right after a magnifier icon (🔍) by your
// AIOStreams (e.g. "🔍 The Pirate Bay"). Reads the ASCII run up to the next icon.
inline std::string extractSource(const std::string& desc) {
    size_t p = desc.find("\xF0\x9F\x94\x8D");  // 🔍
    if (p == std::string::npos) return std::string();
    std::string out;
    for (size_t i = p + 4; i < desc.size(); ++i) {
        unsigned char c = desc[i];
        if (c >= 0x80) break;  // next icon -> stop
        out += (char)c;
    }
    return trimCollapse(out);
}

inline StreamInfo parseStream(const Stream& s) {
    StreamInfo info;
    const std::string desc = !s.description.empty() ? s.description : s.title;
    info.raw = !desc.empty() ? desc : s.name;
    // Search both the short name and the description for tags.
    std::string blob = s.name + "  " + desc;

    std::string res = reFirst(blob, "(2160p|1440p|1080p|720p|480p|4k)");

    // Instant/cached availability: a ⚡ in the source name (e.g. "[PM⚡]").
    bool instant = s.name.find("\xE2\x9A\xA1") != std::string::npos;  // ⚡

    // Provider/indexer label = stream name minus bracketed prefixes, the
    // resolution token, and any embedded emoji/icons (the "coffee-cup" glyph),
    // e.g. "🤖[PM⚡] Knaben 1080p" -> "Knaben".
    std::string provider = s.name;
    provider = reStrip(provider, "\\[[^\\]]*\\]");
    provider = reStrip(provider, "(2160p|1440p|1080p|720p|480p|4k)");
    provider = stripNonAscii(provider);
    provider = trimCollapse(provider);

    std::string body;
    if (!provider.empty() && !res.empty())
        body = provider + " · " + res;
    else if (!provider.empty())
        body = provider;
    else if (!res.empty())
        body = res;
    else
        body = "Stream";
    // Lead with a ⚡ when the title is instantly available (cached on debrid).
    info.title = instant ? ("\xE2\x9A\xA1 " + body) : body;

    auto addTo = [](std::vector<std::string>& v, const std::string& t) {
        if (!t.empty() && std::find(v.begin(), v.end(), t) == v.end()) v.push_back(t);
    };

    // Line 2 — source · video codec · size · duration.
    addTo(info.meta, reFirst(blob, "(WEB-?DL|WEB-?Rip|BluRay|BDRip|BRRip|HDRip|HDTV|REMUX|DVDRip|CAM)"));
    addTo(info.meta, reFirst(blob, "(HEVC|AVC|x265|x264|H\\.?265|H\\.?264|AV1)"));
    addTo(info.meta, reFirst(blob, "([0-9]+(?:\\.[0-9]+)?\\s?(?:GB|MB))"));
    std::string dur = reFirst(blob, "([0-9]+\\s?h[:\\s]?[0-9]+\\s?m(?:[:\\s]?[0-9]+\\s?s)?)");
    if (!dur.empty()) {
        dur = std::regex_replace(dur, std::regex("[:\\s]?[0-9]+\\s?s\\s*$"), "");
        dur = trimCollapse(reStrip(dur, "[:\\s]"));
        addTo(info.meta, dur);
    }

    // Line 3 — audio codecs (most specific first to avoid double-counting) + channels.
    if (!reFirst(blob, "(atmos)").empty()) info.audio.push_back("Atmos");
    if (!reFirst(blob, "(true-?hd)").empty()) info.audio.push_back("TrueHD");
    if (!reFirst(blob, "(dts-?hd(?:\\s?ma)?)").empty())
        info.audio.push_back("DTS-HD");
    else if (!reFirst(blob, "(\\bdts\\b)").empty())
        info.audio.push_back("DTS");
    if (!reFirst(blob, "(dd\\+|ddp|e-?ac-?3|eac3)").empty())
        info.audio.push_back("DD+");
    else if (!reFirst(blob, "(\\bac-?3\\b|\\bdd\\b|dolby digital)").empty())
        info.audio.push_back("DD");
    if (!reFirst(blob, "(\\baac\\b)").empty()) info.audio.push_back("AAC");
    if (!reFirst(blob, "(\\bflac\\b)").empty()) info.audio.push_back("FLAC");
    if (!reFirst(blob, "(\\bopus\\b)").empty()) info.audio.push_back("Opus");
    std::string chans = reFirst(blob, "(7\\.1|5\\.1|2\\.0)");
    if (!chans.empty()) info.audio.push_back(chans);

    // Line 4 — audio languages.
    info.langs = extractAudioLangs(desc);

    // Footer — tracker/indexer source.
    info.source = extractSource(desc);

    // Quality badges (pills).
    if (!res.empty()) {
        std::string r = res;
        for (auto& c : r) c = (char)std::tolower((unsigned char)c);
        if (r == "2160p" || r == "4k") addTo(info.badges, "4K");
    }
    if (!reFirst(blob, "(hdr10\\+|hdr10plus)").empty())
        addTo(info.badges, "HDR10+");
    else if (!reFirst(blob, "(\\bhdr\\b|hdr10)").empty())
        addTo(info.badges, "HDR");
    if (!reFirst(blob, "(dolby ?vision|\\bdovi\\b|\\bdv\\b)").empty()) addTo(info.badges, "DV");

    return info;
}

// An episode within a series meta. Cinemeta uses "name" for the episode title
// and an id of the form "tt1234567:season:episode".
struct Video {
    std::string id;
    std::string name;
    std::string thumbnail;
    std::string overview;
    std::string released;  // ISO date, e.g. "2010-12-06T05:00:00.000Z"
    int season = 0;
    int episode = 0;
};
inline void from_json(const nlohmann::json& j, Video& v) {
    v.id = jstr(j, "id");
    v.name = jstr(j, "name");
    v.thumbnail = jstr(j, "thumbnail");
    v.overview = jstr(j, "overview");
    v.released = jstr(j, "released");
    v.season = jint(j, "season");
    v.episode = jint(j, "episode");
}

// Full metadata for a movie or series (from /meta/{type}/{id}.json).
struct MetaDetail {
    std::string id, type, name, poster, background, description;
    std::string releaseInfo;  // "2010" (series: "2010-2019")
    std::string runtime;      // "148 min"
    std::string imdbRating;   // "8.8"
    std::vector<std::string> genres, cast, director;
    std::vector<Video> videos;  // populated for series
};
inline void from_json(const nlohmann::json& j, MetaDetail& m) {
    m.id = jstr(j, "id");
    m.type = jstr(j, "type");
    m.name = jstr(j, "name");
    m.poster = jstr(j, "poster");
    m.background = jstr(j, "background");
    m.description = jstr(j, "description");
    m.releaseInfo = jstr(j, "releaseInfo");
    if (m.releaseInfo.empty()) m.releaseInfo = jstr(j, "year");
    m.runtime = jstr(j, "runtime");
    m.imdbRating = jstr(j, "imdbRating");
    // Cinemeta has used both "genres" and "genre" over time; accept either.
    m.genres = jstrlist(j, "genres");
    if (m.genres.empty()) m.genres = jstrlist(j, "genre");
    m.cast = jstrlist(j, "cast");
    m.director = jstrlist(j, "director");
    m.videos.clear();
    auto it = j.find("videos");
    if (it != j.end() && it->is_array()) m.videos = it->get<std::vector<Video>>();
}

// Response of /meta/{type}/{id}.json
struct MetaResult {
    MetaDetail meta;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MetaResult, meta);

}  // namespace stremio
