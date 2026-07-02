/*
    Stremio resume / continue-watching

    Tracks playback position keyed by content id (movie ttID, episode ttID:S:E)
    so any stream of the same title resumes where you left off. Persists to
    configDir/resume.json. Powers the "Continue Watching" row.
*/
#pragma once

#include <borealis.hpp>
#include "api/stremio.hpp"

struct ResumeEntry {
    std::string streamType;  // "movie" | "series"
    std::string streamId;    // movie: ttID ; episode: ttID:season:episode
    std::string name;        // display label
    std::string poster;      // poster / episode thumbnail
    double position = 0;     // seconds watched
    double duration = 0;     // total seconds
    long updated = 0;        // unix timestamp (ordering)
    bool enriched = false;   // name/poster already refreshed from Cinemeta (English)
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ResumeEntry, streamType, streamId, name, poster, position, duration, updated, enriched);

class ResumeTracker {
public:
    static ResumeTracker& instance();

    void init();                            // load + hook MPV events (once)
    void setCurrent(const ResumeEntry& e);  // call right before playing a title
    std::vector<ResumeEntry> continueWatching() const;  // in-progress, most-recent first
    void remove(const std::string& streamId);           // clear one item
    // Refresh a stored entry's display name/poster (e.g. with an English title
    // fetched from Cinemeta). Persists silently; does NOT fire changed().
    void updateMeta(const std::string& streamId, const std::string& name, const std::string& poster);
    brls::VoidEvent* changed() { return &changedEvent; }

private:
    void load();
    void save();
    void onProgress(double pos, double dur, bool fire);
    void upsert(const ResumeEntry& e);
    void removeId(const std::string& id);

    bool inited = false;
    bool hasCurrent = false;
    ResumeEntry current;
    double pendingSeek = 0;
    long lastTick = 0;
    std::vector<ResumeEntry> items;
    brls::VoidEvent changedEvent;
};
