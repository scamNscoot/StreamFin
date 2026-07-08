/*
    Stremio stream picker -- implementation.

    A compact, scrollable, selectable list of streams. Uses the file-list row
    layout (small static icon + text, no thumbnails to load) so it scrolls fast.
*/
#include "activity/stremio_streams.hpp"
#include "activity/stremio_resume.hpp"
#include "view/recycling_grid.hpp"
#include "view/svg_image.hpp"
#include "view/mpv_core.hpp"
#include "tab/remote_view.hpp"
#include "api/stremio.hpp"

#include <algorithm>

using namespace brls::literals;

namespace {

// Fetches subtitles from the configured subtitles addon (SubSource etc.) for
// the title being played and attaches them to MPV once the file is loaded.
// They then appear in the player's + menu Subtitle picker alongside any
// embedded tracks.
class SubLoader {
public:
    static SubLoader& instance() {
        static SubLoader inst;
        return inst;
    }

    // Call right before starting playback of {type, id}.
    void prepare(const std::string& type, const std::string& id) {
        this->init();
        this->pending.clear();
        this->fileLoaded = false;
        if (stremio::SUBTITLES_ADDON.empty()) return;

        int ticket = ++this->requestTicket;
        std::string url = stremio::SUBTITLES_ADDON + "/subtitles/" + type + "/" + id + ".json";
        stremio::getJSON<stremio::SubtitleList>(
            [this, ticket](stremio::SubtitleList r) {
                if (ticket != this->requestTicket) return;  // a newer title started
                // Keep the first subtitle per language, capped, so the picker
                // stays usable (addons can return dozens per release).
                std::vector<std::string> seen;
                for (auto& s : r.subtitles) {
                    if (this->pending.size() >= 12) break;
                    if (std::find(seen.begin(), seen.end(), s.lang) != seen.end()) continue;
                    seen.push_back(s.lang);
                    this->pending.push_back(s);
                }
                if (this->fileLoaded) this->attach();
            },
            [](const std::string& e) { brls::Logger::warning("subtitles addon: {}", e); }, url);
    }

private:
    void init() {
        if (this->inited) return;
        this->inited = true;
        MPVCore::instance().getEvent()->subscribe([this](MpvEventEnum e) {
            switch (e) {
            case MpvEventEnum::MPV_LOADED:
                this->fileLoaded = true;
                this->attach();
                break;
            case MpvEventEnum::MPV_STOP:
            case MpvEventEnum::END_OF_FILE:
                this->fileLoaded = false;
                this->pending.clear();
                break;
            default:
                break;
            }
        });
    }

    void attach() {
        auto& mpv = MPVCore::instance();
        for (auto& s : this->pending) {
            std::string title = s.lang.empty() ? "External" : s.lang;
            // "auto" = add without selecting; the user picks via the + menu.
            mpv.command("sub-add", s.url.c_str(), "auto", title.c_str(), s.lang.c_str());
        }
        this->pending.clear();
    }

    bool inited = false;
    bool fileLoaded = false;
    int requestTicket = 0;
    std::vector<stremio::Subtitle> pending;
};

// Clean text row built in code: name on top, full description below.
class StreamCell : public RecyclingGridItem {
public:
    StreamCell() {
        this->setFocusable(true);
        this->setAxis(brls::Axis::COLUMN);
        this->setPadding(12, 20, 12, 20);
        this->setCornerRadius(6);

        this->name = new brls::Label();
        this->name->setFontSize(24);
        this->addView(this->name);

        this->detail = new brls::Label();
        this->detail->setFontSize(18);
        this->addView(this->detail);
    }

    brls::Label* name = nullptr;
    brls::Label* detail = nullptr;
};

class StreamSource : public RecyclingGridDataSource {
public:
    StreamSource(const ResumeEntry& key, const std::vector<stremio::Stream>& s)
        : key(key), list(std::move(s)) {}

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        StreamCell* cell = dynamic_cast<StreamCell*>(recycler->dequeueReusableCell("Cell"));
        auto& s = this->list.at(index);

        std::string name = s.name;
        std::replace(name.begin(), name.end(), '\n', ' ');
        std::string desc = !s.description.empty() ? s.description : s.title;
        std::replace(desc.begin(), desc.end(), '\n', ' ');

        cell->name->setText(name.empty() ? "Stream" : name);
        cell->detail->setText(desc);
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto s = this->list.at(index);
        if (s.url.empty()) {
            brls::Application::notify("This stream has no URL");
            return;
        }
        // Remember what we're playing so the tracker can resume/record by id.
        ResumeTracker::instance().setCurrent(this->key);
        // Ask the subtitles addon (if configured) for subs for this title.
        SubLoader::instance().prepare(this->key.streamType, this->key.streamId);
        // Prefer English audio tracks for this (and subsequent) playback.
        MPVCore::instance().command("set", "alang", "eng,en");
        // Show the media title (movie name / "Series · S1E5 · Episode") on the
        // player OSD, not the raw stream/source name.
        std::string title = this->key.name.empty() ? s.name : this->key.name;
        RemoteView::play(s.url, title);
    }

    void clearData() override { this->list.clear(); }

private:
    ResumeEntry key;
    std::vector<stremio::Stream> list;
};

}  // namespace

StreamPicker::StreamPicker(
    const std::string& title, const std::vector<stremio::Stream>& streams, const ResumeEntry& resumeKey) {
    brls::Logger::debug("StreamPicker: {} streams", streams.size());
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);
    // Hold focus here (hidden highlight) while the list loads / when empty,
    // so the outline can't float over the covered detail screen.
    this->setFocusable(true);
    this->setHideHighlight(true);

    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 1;
    this->recycler->isFlowMode = true;
    this->recycler->estimatedRowSpace = 10;
    this->recycler->registerCell("Cell", []() -> RecyclingGridItem* { return new StreamCell(); });
    this->addView(this->recycler);

    // Addons (e.g. AIOStreams) may return non-playable info entries alongside
    // real streams — "Removal Reasons", "no results", etc. — with no URL.
    // Keep only playable streams so the user can't select a dead entry.
    std::vector<stremio::Stream> playable;
    for (auto& s : streams)
        if (!s.url.empty()) playable.push_back(s);

    if (playable.empty()) {
        // Everything the addon offered was filtered out (often by the addon's
        // own quality/resolution filters). Point the user at the real cause.
        this->recycler->setEmpty("No playable streams — check your addon's quality/resolution filters");
    } else {
        this->recycler->setDataSource(new StreamSource(resumeKey, playable));
    }
    // Defer focus until after the activity is on screen.
    brls::sync([this]() { brls::Application::giveFocus(this); });
    brls::sync([this]() { brls::Application::giveFocus(this->recycler); });

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });
}
