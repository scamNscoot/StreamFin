/*
    Stremio movie detail screen -- implementation.

    Built programmatically (like the home screen) to avoid the XML-inflation
    ghosting issue. Layout: poster on the left; title, meta line, Watch button,
    description, cast and director in a column on the right.
*/
#include "activity/stremio_detail.hpp"
#include "activity/stremio_series.hpp"
#include "activity/stremio_streams.hpp"
#include "activity/stremio_favourites.hpp"
#include "activity/stremio_resume.hpp"
#include "utils/image.hpp"
#include "api/stremio.hpp"

#include <set>

using namespace brls::literals;

namespace {

// Soft palette shared with the rest of the app.
const NVGcolor COL_BG     = nvgRGB(16, 18, 24);    // cinematic slate
const NVGcolor COL_TEXT   = nvgRGB(240, 242, 248); // soft white
const NVGcolor COL_DIM    = nvgRGB(168, 176, 192); // secondary text
const NVGcolor COL_ACCENT = nvgRGB(245, 33, 42);   // StreamFin brand red (meta line)

// Join a string list: {"A","B"} -> "A, B".
std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string out;
    for (auto& s : v) {
        if (s.empty()) continue;
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

}  // namespace

StremioDetail::StremioDetail(const stremio::Meta& item) : item(item) {
    brls::Logger::debug("StremioDetail: {}", item.id);
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(COL_BG);
    this->setPadding(40, 60, 40, 60);

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setGrow(1.0f);
    this->addView(row);

    // ---- Poster (left) ----------------------------------------------------
    this->poster = new brls::Image();
    this->poster->setDimensions(340, 510);
    this->poster->setCornerRadius(10);
    this->poster->setScalingType(brls::ImageScalingType::FILL);
    // The texture is shared with the home-grid cell via the URL-keyed
    // TextureCache (refcounted). The view must never delete it itself, or the
    // grid poster goes white when this screen is popped. Image::with only sets
    // this on the download path, not on a cache hit, so set it explicitly.
    this->poster->setFreeTexture(false);
    row->addView(this->poster);
    std::string posterSrc = stremio::posterUrl(item.id, item.poster);
    if (!posterSrc.empty()) Image::with(this->poster, posterSrc);

    // ---- Info column (right) ----------------------------------------------
    auto* info = new brls::Box();
    info->setAxis(brls::Axis::COLUMN);
    info->setGrow(1.0f);
    info->setMarginLeft(44);
    row->addView(info);

    this->labelTitle = new brls::Label();
    this->labelTitle->setText(item.name);
    this->labelTitle->setFontSize(40);
    this->labelTitle->setTextColor(COL_TEXT);
    this->labelTitle->setIsWrapping(true);
    info->addView(this->labelTitle);

    this->labelMeta = new brls::Label();
    this->labelMeta->setText(item.year);
    this->labelMeta->setFontSize(22);
    this->labelMeta->setTextColor(COL_ACCENT);
    this->labelMeta->setMarginTop(10);
    info->addView(this->labelMeta);

    this->labelGenres = new brls::Label();
    this->labelGenres->setFontSize(20);
    this->labelGenres->setTextColor(COL_DIM);
    this->labelGenres->setMarginTop(6);
    this->labelGenres->setVisibility(brls::Visibility::GONE);
    info->addView(this->labelGenres);

    // Watch button: a focusable pill, same construction style as StreamCell.
    auto* btnRow = new brls::Box();
    btnRow->setAxis(brls::Axis::ROW);
    btnRow->setMarginTop(22);
    info->addView(btnRow);

    this->btnWatch = new brls::Box();
    this->btnWatch->setFocusable(true);
    this->btnWatch->setAxis(brls::Axis::ROW);
    this->btnWatch->setPadding(14, 40, 14, 40);
    this->btnWatch->setCornerRadius(8);
    this->btnWatch->setBackgroundColor(COL_ACCENT);  // brand red
    auto* btnLabel = new brls::Label();
    // ▶ is in the Switch system font; ☰ is not (renders as a crossed box).
    btnLabel->setText(item.type == "series" ? "▶  Episodes" : "▶  Watch");
    btnLabel->setFontSize(24);
    btnLabel->setTextColor(COL_TEXT);
    this->btnWatch->addView(btnLabel);
    this->btnWatch->registerClickAction([this](brls::View*) {
        this->onAction();
        return true;
    });
    this->btnWatch->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnWatch));
    btnRow->addView(this->btnWatch);

    this->labelDesc = new brls::Label();
    this->labelDesc->setText("Loading…");
    this->labelDesc->setFontSize(21);
    this->labelDesc->setTextColor(COL_DIM);
    this->labelDesc->setIsWrapping(true);
    this->labelDesc->setMarginTop(24);
    info->addView(this->labelDesc);

    this->labelCastHead = new brls::Label();
    this->labelCastHead->setText("Cast");
    this->labelCastHead->setFontSize(24);
    this->labelCastHead->setTextColor(COL_TEXT);
    this->labelCastHead->setMarginTop(26);
    this->labelCastHead->setVisibility(brls::Visibility::GONE);
    info->addView(this->labelCastHead);

    this->labelCast = new brls::Label();
    this->labelCast->setFontSize(21);
    this->labelCast->setTextColor(COL_DIM);
    this->labelCast->setIsWrapping(true);
    this->labelCast->setMarginTop(8);
    this->labelCast->setVisibility(brls::Visibility::GONE);
    info->addView(this->labelCast);

    this->labelDirector = new brls::Label();
    this->labelDirector->setFontSize(21);
    this->labelDirector->setTextColor(COL_DIM);
    this->labelDirector->setMarginTop(14);
    this->labelDirector->setVisibility(brls::Visibility::GONE);
    info->addView(this->labelDirector);

    // ---- Actions -----------------------------------------------------------
    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });

    // X toggles favourite here too, matching the poster grids.
    this->registerAction("Favourite", brls::BUTTON_X, [this](brls::View*) {
        bool nowFav = Favourites::instance().toggle(this->item);
        brls::Application::notify(nowFav ? "Added to Favourites" : "Removed from Favourites");
        return true;
    });

    // Something must hold focus so A/B work immediately.
    brls::sync([this]() { brls::Application::giveFocus(this->btnWatch); });

    // ---- Fetch full metadata from Cinemeta ---------------------------------
    ASYNC_RETAIN
    stremio::getJSON<stremio::MetaResult>(
        [ASYNC_TOKEN](stremio::MetaResult r) {
            ASYNC_RELEASE
            this->applyMeta(r.meta);
        },
        [ASYNC_TOKEN](const std::string&) {
            ASYNC_RELEASE
            this->labelDesc->setText("No details available.");
        },
        stremio::CINEMETA + "/meta/" + item.type + "/" + item.id + ".json");
}

// Release our reference on the shared poster texture (and abort any in-flight
// request) — same contract as BaseCardCell.
StremioDetail::~StremioDetail() { Image::cancel(this->poster); }

void StremioDetail::applyMeta(const stremio::MetaDetail& meta) {
    // Prefer the (English) meta name over the catalog one — for the title AND
    // for the resume/OSD label used when Watch is pressed.
    if (!meta.name.empty()) {
        this->labelTitle->setText(meta.name);
        this->item.name = meta.name;
    }

    // Keep the episodes so the Episodes button can open the season list
    // without refetching the meta, and the backdrop as episode-thumb fallback.
    this->videos = meta.videos;
    this->background = meta.background;

    // Year · runtime · ★ rating (· N seasons for series)
    std::vector<std::string> parts;
    std::string year = !meta.releaseInfo.empty() ? meta.releaseInfo : this->item.year;
    if (!year.empty()) parts.push_back(year);
    if (!meta.runtime.empty()) parts.push_back(meta.runtime);
    if (!meta.imdbRating.empty()) parts.push_back("★ " + meta.imdbRating);
    if (this->item.type == "series" && !meta.videos.empty()) {
        std::set<int> seasons;
        for (auto& v : meta.videos)
            if (v.season > 0) seasons.insert(v.season);
        if (!seasons.empty())
            parts.push_back(seasons.size() == 1 ? "1 season" : fmt::format("{} seasons", seasons.size()));
    }
    if (!parts.empty()) this->labelMeta->setText(join(parts, "   ·   "));

    if (!meta.genres.empty()) {
        this->labelGenres->setText(join(meta.genres, "  ·  "));
        this->labelGenres->setVisibility(brls::Visibility::VISIBLE);
    }

    this->labelDesc->setText(!meta.description.empty() ? meta.description : "No description available.");

    if (!meta.cast.empty()) {
        this->labelCast->setText(join(meta.cast));
        this->labelCastHead->setVisibility(brls::Visibility::VISIBLE);
        this->labelCast->setVisibility(brls::Visibility::VISIBLE);
    }

    if (!meta.director.empty()) {
        this->labelDirector->setText("Directed by " + join(meta.director));
        this->labelDirector->setVisibility(brls::Visibility::VISIBLE);
    }
}

void StremioDetail::onAction() {
    if (this->item.type == "series") {
        // Reuse the episodes we already fetched; fall back to the fetching
        // constructor if the meta hasn't arrived (or failed).
        if (!this->videos.empty())
            brls::Application::pushActivity(
                new brls::Activity(new StremioSeries(this->item.name, this->videos, this->background)),
                brls::TransitionAnimation::NONE);
        else
            brls::Application::pushActivity(
                new brls::Activity(new StremioSeries(this->item)), brls::TransitionAnimation::NONE);
        return;
    }
    this->openStreams();
}

void StremioDetail::openStreams() {
    if (this->fetching) return;  // ignore repeat presses while a request runs
    if (stremio::STREAM_ADDONS.empty()) {
        brls::Application::notify("Set your stream addon first (press − on the home screen)");
        return;
    }
    this->fetching = true;
    brls::Application::notify("Finding streams…");

    std::string name = this->item.name;
    ResumeEntry key{this->item.type, this->item.id, this->item.name, this->item.poster};

    ASYNC_RETAIN
    stremio::getStreams(this->item.type, this->item.id,
        [ASYNC_TOKEN, name, key](std::vector<stremio::Stream> streams) {
            ASYNC_RELEASE
            this->fetching = false;
            if (streams.empty()) {
                brls::Application::notify("No streams found");
                return;
            }
            brls::Application::pushActivity(new brls::Activity(new StreamPicker(name, streams, key)));
        },
        [ASYNC_TOKEN](const std::string& e) {
            ASYNC_RELEASE
            this->fetching = false;
            brls::Application::notify("Stream error: " + e);
        });
}
