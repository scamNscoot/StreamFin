/*
    Stremio series + season screens -- implementation.
*/
#include "activity/stremio_series.hpp"
#include "activity/stremio_streams.hpp"
#include "activity/stremio_resume.hpp"
#include "view/recycling_grid.hpp"
#include "view/video_card.hpp"
#include "view/svg_image.hpp"
#include "utils/image.hpp"
#include "api/stremio.hpp"

#include <map>

using namespace brls::literals;

namespace {

// ---- Season list cell (compact icon + name + episode count) --------------
class SeasonCell : public RecyclingGridItem {
public:
    SeasonCell() { this->inflateFromXMLRes("xml/view/dir_entry.xml"); }

    BRLS_BIND(SVGImage, icon, "file/icon");
    BRLS_BIND(brls::Label, name, "file/name");
    BRLS_BIND(brls::Label, detail, "file/misc");
};

struct SeasonEntry {
    std::string label;
    std::vector<stremio::Video> episodes;
};

class SeasonSource : public RecyclingGridDataSource {
public:
    SeasonSource(std::string seriesName, std::string background, std::vector<SeasonEntry> s)
        : seriesName(std::move(seriesName)), background(std::move(background)), list(std::move(s)) {}

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        SeasonCell* cell = dynamic_cast<SeasonCell*>(recycler->dequeueReusableCell("Cell"));
        auto& s = this->list.at(index);
        cell->icon->setImageFromSVGRes("icon/ico-list.svg");
        cell->name->setText(s.label);
        cell->detail->setText(
            s.episodes.size() == 1 ? "1 episode" : fmt::format("{} episodes", s.episodes.size()));
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto& s = this->list.at(index);
        brls::Application::pushActivity(
            new brls::Activity(new StremioSeason(this->seriesName, s.label, s.episodes, this->background)));
    }

    void clearData() override { this->list.clear(); }

private:
    std::string seriesName;
    std::string background;
    std::vector<SeasonEntry> list;
};

// ---- Episode list cell (wide card with thumbnail) ------------------------
class EpisodeCell : public BaseCardCell {
public:
    EpisodeCell() { this->inflateFromXMLRes("xml/view/episode_card.xml"); }

    BRLS_BIND(brls::Label, labelName, "episode/card/name");
    BRLS_BIND(brls::Label, labelOverview, "episode/card/overview");
};

class EpisodeSource : public RecyclingGridDataSource {
public:
    EpisodeSource(std::string seriesName, std::string background, const std::vector<stremio::Video>& v)
        : seriesName(std::move(seriesName)), background(std::move(background)), list(std::move(v)) {}

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        EpisodeCell* cell = dynamic_cast<EpisodeCell*>(recycler->dequeueReusableCell("Cell"));
        auto& v = this->list.at(index);

        cell->labelName->setText(fmt::format("E{} - {}", v.episode, v.name));
        // Prefix the air date (YYYY-MM-DD) to the overview when available.
        std::string date = v.released.size() >= 10 ? v.released.substr(0, 10) : "";
        std::string ov = v.overview;
        if (!date.empty()) ov = ov.empty() ? date : (date + "  ·  " + ov);
        cell->labelOverview->setText(ov);
        cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);

        // Newly-aired episodes often have no still yet (metahub 404s); fall
        // back to the series backdrop instead of the grey placeholder.
        if (!v.thumbnail.empty())
            Image::with(cell->picture, v.thumbnail, this->background);
        else if (!this->background.empty())
            Image::with(cell->picture, this->background);

        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto v = this->list.at(index);
        if (stremio::STREAM_ADDONS.empty()) {
            brls::Application::notify("Set your stream addon first (press − on the home screen)");
            return;
        }
        brls::Application::notify("Finding streams…");

        // Player OSD / resume label: include the series name so it reads
        // "Series · S1E5 · Episode name" instead of just the episode.
        std::string label = this->seriesName.empty()
            ? fmt::format("S{}E{} · {}", v.season, v.episode, v.name)
            : fmt::format("{} · S{}E{} · {}", this->seriesName, v.season, v.episode, v.name);
        ResumeEntry key{"series", v.id, label, v.thumbnail};
        stremio::getStreams("series", v.id,
            [label, key](std::vector<stremio::Stream> streams) {
                if (streams.empty()) {
                    brls::Application::notify("No streams found");
                    return;
                }
                brls::Application::pushActivity(new brls::Activity(new StreamPicker(label, streams, key)));
            },
            [](const std::string& e) { brls::Application::notify("Stream error: " + e); });
    }

    void clearData() override { this->list.clear(); }

private:
    std::string seriesName;
    std::string background;
    std::vector<stremio::Video> list;
};

}  // namespace

// ---- StremioSeries: lists seasons ----------------------------------------
void StremioSeries::init() {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);

    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 1;
    this->recycler->estimatedRowHeight = 60;
    this->recycler->estimatedRowSpace = 10;
    this->recycler->registerCell("Cell", []() -> RecyclingGridItem* { return new SeasonCell(); });
    this->addView(this->recycler);
    brls::sync([this]() { brls::Application::giveFocus(this->recycler); });

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });
}

void StremioSeries::setSeasons(
    const std::string& name, const std::vector<stremio::Video>& videos, const std::string& background) {
    if (videos.empty()) {
        this->recycler->setError("No episodes found");
        return;
    }
    // Group episodes by season (std::map keeps seasons in order).
    std::map<int, std::vector<stremio::Video>> bySeason;
    for (auto& v : videos) bySeason[v.season].push_back(v);

    std::vector<SeasonEntry> seasons;
    for (auto& kv : bySeason) {
        if (kv.first == 0) continue;  // skip specials (season 0)
        seasons.push_back({fmt::format("Season {}", kv.first), kv.second});
    }
    if (seasons.empty()) {
        this->recycler->setError("No episodes found");
        return;
    }
    this->recycler->setDataSource(new SeasonSource(name, background, std::move(seasons)));
    brls::Application::giveFocus(this->recycler);
}

StremioSeries::StremioSeries(const stremio::Meta& series) {
    brls::Logger::debug("StremioSeries: create {}", series.id);
    this->init();
    this->recycler->showSkeleton();

    std::string seriesName = series.name;
    ASYNC_RETAIN
    stremio::getJSON<stremio::MetaResult>(
        [ASYNC_TOKEN, seriesName](stremio::MetaResult r) {
            ASYNC_RELEASE
            // Prefer the (English) name from the meta response; fall back to the
            // catalog name we were opened with.
            this->setSeasons(r.meta.name.empty() ? seriesName : r.meta.name, r.meta.videos, r.meta.background);
        },
        [ASYNC_TOKEN](const std::string& e) {
            ASYNC_RELEASE
            this->recycler->setError(e);
        },
        stremio::CINEMETA + "/meta/series/" + series.id + ".json");
}

StremioSeries::StremioSeries(
    const std::string& name, const std::vector<stremio::Video>& videos, const std::string& background) {
    brls::Logger::debug("StremioSeries: create from {} prefetched episodes", videos.size());
    this->init();
    this->setSeasons(name, videos, background);
}

// ---- StremioSeason: lists episodes of one season -------------------------
StremioSeason::StremioSeason(const std::string& seriesName, const std::string& title,
    const std::vector<stremio::Video>& episodes, const std::string& background) {
    brls::Logger::debug("StremioSeason: {} ({} eps)", title, episodes.size());
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);

    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 1;
    this->recycler->isFlowMode = true;
    this->recycler->estimatedRowSpace = 10;
    this->recycler->registerCell("Cell", []() -> RecyclingGridItem* { return new EpisodeCell(); });
    this->addView(this->recycler);

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });

    this->recycler->setDataSource(new EpisodeSource(seriesName, background, episodes));
    brls::sync([this]() { brls::Application::giveFocus(this->recycler); });
}
