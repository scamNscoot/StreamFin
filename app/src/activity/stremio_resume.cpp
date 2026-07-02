/*
    Stremio resume / continue-watching -- implementation.
*/
#include "activity/stremio_resume.hpp"
#include "view/mpv_core.hpp"
#include "utils/config.hpp"

#include <fstream>
#include <sys/stat.h>
#include <algorithm>
#include <ctime>

ResumeTracker& ResumeTracker::instance() {
    static ResumeTracker t;
    return t;
}

void ResumeTracker::init() {
    if (this->inited) return;
    this->inited = true;
    this->load();

    MPVCore::instance().getEvent()->subscribe([this](MpvEventEnum e) {
        auto& mpv = MPVCore::instance();
        switch (e) {
        case MpvEventEnum::MPV_LOADED:
            // Resume: jump to the saved spot once the file is loaded.
            if (this->hasCurrent && this->pendingSeek > 10) mpv.seek(this->pendingSeek, "absolute");
            this->pendingSeek = 0;
            break;
        case MpvEventEnum::UPDATE_PROGRESS: {
            if (!this->hasCurrent) break;
            long now = (long)time(nullptr);
            if (now - this->lastTick >= 20) {  // save at most every 20s
                this->lastTick = now;
                this->onProgress(mpv.playback_time, mpv.duration, false);
            }
            break;
        }
        case MpvEventEnum::MPV_STOP:
        case MpvEventEnum::END_OF_FILE:
            if (this->hasCurrent) this->onProgress(mpv.playback_time, mpv.duration, true);
            this->hasCurrent = false;
            break;
        default:
            break;
        }
    });
}

void ResumeTracker::setCurrent(const ResumeEntry& e) {
    this->current = e;
    this->hasCurrent = true;
    this->lastTick = 0;
    this->pendingSeek = 0;
    for (auto& it : this->items)
        if (it.streamId == e.streamId) {
            this->pendingSeek = it.position;
            break;
        }
}

void ResumeTracker::onProgress(double pos, double dur, bool fire) {
    if (pos < 60) return;  // ignore < 1 minute watched
    if (dur > 0 && pos / dur >= 0.92) {
        this->removeId(this->current.streamId);  // finished -> drop
    } else {
        ResumeEntry e = this->current;
        e.position = pos;
        e.duration = dur;
        e.updated = (long)time(nullptr);
        this->upsert(e);
    }
    this->save();
    if (fire) this->changedEvent.fire();
}

void ResumeTracker::upsert(const ResumeEntry& e) {
    for (auto& it : this->items)
        if (it.streamId == e.streamId) {
            it = e;
            return;
        }
    this->items.push_back(e);
}

void ResumeTracker::removeId(const std::string& id) {
    this->items.erase(std::remove_if(this->items.begin(), this->items.end(),
                          [&](const ResumeEntry& x) { return x.streamId == id; }),
        this->items.end());
}

void ResumeTracker::remove(const std::string& streamId) {
    this->removeId(streamId);
    if (this->current.streamId == streamId) this->hasCurrent = false;
    this->save();
    this->changedEvent.fire();
}

void ResumeTracker::updateMeta(
    const std::string& streamId, const std::string& name, const std::string& poster) {
    bool dirty = false;
    for (auto& it : this->items) {
        if (it.streamId == streamId) {
            if (!name.empty() && it.name != name) {
                it.name = name;
                dirty = true;
            }
            if (!poster.empty() && it.poster != poster) {
                it.poster = poster;
                dirty = true;
            }
            if (!it.enriched) {
                it.enriched = true;  // mark resolved so we don't refetch every load
                dirty = true;
            }
            break;
        }
    }
    // Persist so the fix sticks across restarts. No changedEvent here: the
    // caller decides whether/when to rebuild the row.
    if (dirty) this->save();
}

std::vector<ResumeEntry> ResumeTracker::continueWatching() const {
    std::vector<ResumeEntry> r = this->items;
    std::sort(r.begin(), r.end(), [](const ResumeEntry& a, const ResumeEntry& b) { return a.updated > b.updated; });
    return r;
}

void ResumeTracker::load() {
    try {
        std::ifstream in(AppConfig::instance().configDir() + "/resume.json");
        if (in.is_open()) {
            nlohmann::json j;
            in >> j;
            this->items = j.get<std::vector<ResumeEntry>>();
        }
    } catch (const std::exception& e) {
        brls::Logger::warning("Resume load: {}", e.what());
        this->items.clear();
    }
}

void ResumeTracker::save() {
    std::string dir = AppConfig::instance().configDir();
    ::mkdir(dir.c_str(), 0777);
    try {
        std::ofstream out(dir + "/resume.json");
        if (out.is_open()) {
            nlohmann::json j = this->items;
            out << j.dump(2);
        }
    } catch (const std::exception& e) {
        brls::Logger::warning("Resume save: {}", e.what());
    }
}
