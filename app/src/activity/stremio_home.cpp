/*
    Stremio browse screen -- implementation.

    Built programmatically: a ScrollingFrame whose single content view is a
    column Box. Each row = a Header + an HRecyclerFrame (horizontal poster
    carousel) fed by StremioSource.
*/
#include "activity/stremio_home.hpp"
#include "activity/stremio_series.hpp"
#include "activity/stremio_detail.hpp"
#include "activity/stremio_streams.hpp"
#include "activity/stremio_search.hpp"
#include "activity/stremio_favourites.hpp"
#include "activity/stremio_resume.hpp"
#include "activity/stremio_library.hpp"
#include "view/mpv_core.hpp"
#include "view/recycling_grid.hpp"
#include "view/h_recycling.hpp"
#include "view/video_card.hpp"
#include "view/svg_image.hpp"
#include "view/stremio_theme.hpp"
#include "utils/image.hpp"
#include "utils/config.hpp"
#include "api/stremio.hpp"

#include <ctime>
#include <memory>

using namespace brls::literals;

namespace {

// First-run / on-demand prompt for the stream addon URL (on-screen keyboard).
void promptForAddon(const std::string& initial) {
    brls::Application::getImeManager()->openForText(
        [](const std::string& text) {
            std::string url = stremio::normalizeAddonUrl(text);
            if (url.empty()) {
                brls::Application::notify("No addon set — press − to set it later");
                return;
            }
            stremio::saveAddon(AppConfig::instance().configDir(), url);
            brls::Application::notify("Stream addon saved");
        },
        "Stream addon URL",
        "Type your addon URL — or easier: put it in a text file at switch/streamfin-addon.txt on the SD card and relaunch",
        1024, initial, 0);
}

// Consistent, polished styling for every row title (Continue Watching,
// Favourites, and all catalog rows) — one place to tweak the look.
void styleRowHeader(brls::Label* h, const std::string& text) {
    h->setText(text);
    h->setFontSize(27);
    h->setMarginTop(28);
    h->setMarginBottom(12);
    h->setMarginLeft(56);  // rows are edge-to-edge; keep titles at the old inset
    h->setTextColor(nvgRGB(240, 242, 248));  // soft white for clear hierarchy
}

// Data source: turns a list of Cinemeta metas into poster cards. Selecting a
// movie opens the stream picker; selecting a series opens its episode list.
class StremioSource : public RecyclingGridDataSource {
public:
    explicit StremioSource(const std::vector<stremio::Meta>& r) : list(std::move(r)) {}

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        FavCardCell* cell = dynamic_cast<FavCardCell*>(recycler->dequeueReusableCell("Cell"));
        auto item = this->list.at(index);

        cell->labelTitle->setText(item.name);
        if (item.year.empty()) {
            cell->labelExt->setVisibility(brls::Visibility::GONE);
        } else {
            cell->labelExt->setText(item.year);
            cell->labelExt->setVisibility(brls::Visibility::VISIBLE);
        }
        // IMDb rating badge (bottom-right on the poster), straight from the
        // Cinemeta catalog data — no extra request. Hidden when a poster
        // provider (RPDB etc.) is set: those bake the rating into the image.
        if (item.imdbRating.empty() || !stremio::POSTER_TEMPLATE.empty()) {
            cell->labelRating->setVisibility(brls::Visibility::INVISIBLE);
        } else {
            cell->labelRating->setText("★ " + item.imdbRating);
            cell->labelRating->setVisibility(brls::Visibility::VISIBLE);
        }
        cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);

        // Heart badge reflects favourite state; X toggles it. The badge itself
        // is the feedback — no toast (rapid toggling stacked them on screen).
        bool fav = Favourites::instance().contains(item.id);
        cell->badgeFavorite->setVisibility(fav ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
        cell->onToggleFav = [item, cell]() {
            bool nowFav = Favourites::instance().toggle(item);
            cell->badgeFavorite->setVisibility(nowFav ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
        };

        std::string poster = stremio::posterUrl(item.id, item.poster);
        if (!poster.empty()) Image::with(cell->picture, poster);

        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto item = this->list.at(index);
        // Both movies and series open the detail screen (info + cast +
        // Watch/Episodes button).
        brls::Application::pushActivity(new brls::Activity(new StremioDetail(item)), brls::TransitionAnimation::NONE);
    }

    void clearData() override { this->list.clear(); }

private:
    std::vector<stremio::Meta> list;
};

// Home "Library" row: the newest few saved titles plus a final "See all" card
// that opens the full library grid.
class LibraryRowSource : public RecyclingGridDataSource {
public:
    explicit LibraryRowSource(std::vector<stremio::Meta> r) : list(std::move(r)) {}

    size_t getItemCount() override { return this->list.size() + 1; }  // + See-all card

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        FavCardCell* cell = dynamic_cast<FavCardCell*>(recycler->dequeueReusableCell("Cell"));
        cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);
        cell->labelRating->setVisibility(brls::Visibility::INVISIBLE);

        if (index == this->list.size()) {  // the See-all card
            cell->labelTitle->setText("Library");
            cell->labelExt->setText(this->list.empty() ? "Nothing saved yet" : "See all");
            cell->labelExt->setVisibility(brls::Visibility::VISIBLE);
            cell->badgeFavorite->setVisibility(brls::Visibility::INVISIBLE);
            cell->onToggleFav = nullptr;
            cell->picture->setImageFromRes("img/library-card.png");
            return cell;
        }

        auto item = this->list.at(index);
        cell->labelTitle->setText(item.name);
        if (item.year.empty()) {
            cell->labelExt->setVisibility(brls::Visibility::GONE);
        } else {
            cell->labelExt->setText(item.year);
            cell->labelExt->setVisibility(brls::Visibility::VISIBLE);
        }
        cell->badgeFavorite->setVisibility(brls::Visibility::VISIBLE);
        cell->onToggleFav = [item]() { Favourites::instance().toggle(item); };

        std::string poster = stremio::posterUrl(item.id, item.poster);
        if (!poster.empty()) Image::with(cell->picture, poster);
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        if (index == this->list.size()) {
            brls::Application::pushActivity(
                new brls::Activity(new StremioLibrary()), brls::TransitionAnimation::NONE);
            return;
        }
        brls::Application::pushActivity(
            new brls::Activity(new StremioDetail(this->list.at(index))), brls::TransitionAnimation::NONE);
    }

    void clearData() override { this->list.clear(); }

private:
    std::vector<stremio::Meta> list;
};

// Continue Watching: poster cards with a progress bar; selecting one fetches
// streams and resumes where you left off.
class ContinueSource : public RecyclingGridDataSource {
public:
    explicit ContinueSource(const std::vector<ResumeEntry>& r) : list(r) {}

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        FavCardCell* cell = dynamic_cast<FavCardCell*>(recycler->dequeueReusableCell("Cell"));
        auto& e = this->list.at(index);

        cell->labelTitle->setText(e.name);
        cell->labelExt->setVisibility(brls::Visibility::GONE);
        cell->labelRating->setVisibility(brls::Visibility::INVISIBLE);
        cell->badgeFavorite->setVisibility(brls::Visibility::INVISIBLE);
        cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        // X clears this item from Continue Watching. Deferred to the next frame
        // so the button press finishes before the row tears down and rebuilds.
        cell->onToggleFav = [id = e.streamId]() {
            brls::sync([id]() {
                ResumeTracker::instance().remove(id);
                brls::Application::notify("Removed from Continue Watching");
            });
        };

        if (e.duration > 0) {
            cell->rectProgress->setWidthPercentage((float)(e.position / e.duration * 100.0));
            cell->rectProgress->getParent()->setVisibility(brls::Visibility::VISIBLE);
        } else {
            cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);
        }

        // posterUrl strips ":season:episode", so with a poster provider set,
        // series resolve to the SERIES poster instead of an episode thumb.
        std::string poster = stremio::posterUrl(e.streamId, e.poster);
        if (!poster.empty()) Image::with(cell->picture, poster);

        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto e = this->list.at(index);
        if (stremio::STREAM_ADDON.empty()) {
            brls::Application::notify("Set your stream addon first (press −)");
            return;
        }
        brls::Application::notify("Finding streams…");
        std::string url = stremio::STREAM_ADDON + "/stream/" + e.streamType + "/" + e.streamId + ".json";
        stremio::getJSON<stremio::StreamList>(
            [e](stremio::StreamList r) {
                if (r.streams.empty()) {
                    brls::Application::notify("No streams found");
                    return;
                }
                brls::Application::pushActivity(new brls::Activity(new StreamPicker(e.name, r.streams, e)));
            },
            [](const std::string& err) { brls::Application::notify("Stream error: " + err); }, url);
    }

    void clearData() override { this->list.clear(); }

private:
    std::vector<ResumeEntry> list;
};

}  // namespace

StremioHome::StremioHome() {
    brls::Logger::debug("StremioHome: create");
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    // Background is the ocean gradient painted in draw().

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingIndicatorVisible(false);
    // Centered scrolling: every Down/Up press hops one row (which is then
    // scrolled into view) — natural scrolling instead pins focus to the
    // current row until the next one is fully on screen, which made moving
    // between rows below the fold need press-and-hold.
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    this->boxHome = new brls::Box();
    this->boxHome->setAxis(brls::Axis::COLUMN);
    // No side padding: each row runs edge-to-edge and carries its own 50px
    // inner padding instead, so a scrolled row shows a sliver of the previous
    // poster on the left (like the next-poster sliver on the right) instead
    // of clipping to nothing at the old padding boundary.
    this->boxHome->setPadding(16, 0, 32, 0);
    scroll->setContentView(this->boxHome);

    this->addView(scroll);

    // When stremio playback stops (B in the player or the file ended), close
    // the stream picker / detail screens too and land back on the home screen
    // with the Continue Watching row focused. Must be subscribed BEFORE
    // ResumeTracker::init(), whose own MPV_STOP handler clears isPlaying().
    Favourites::instance().load();
    MPVCore::instance().getEvent()->subscribe([this](MpvEventEnum e) {
        if (e != MpvEventEnum::MPV_STOP && e != MpvEventEnum::END_OF_FILE) return;
        if (!ResumeTracker::instance().isPlaying()) return;
        brls::sync([this]() { this->popToHome(); });
    });
    ResumeTracker::instance().init();
    this->addContinueRow();
    this->addFavouritesRow();

    // Cinemeta catalogs: top = Popular, year = New (newest releases),
    // imdbRating = Top Rated. The /year catalog requires a year value.
    this->addRow("Popular Movies", stremio::CINEMETA + "/catalog/movie/top.json");
    this->addRow("Popular Series", stremio::CINEMETA + "/catalog/series/top.json");
    // Current year computed at runtime, so "New" auto-rolls each year (no manual edit).
    std::time_t now = std::time(nullptr);
    std::string year = std::to_string(1900 + std::localtime(&now)->tm_year);
    this->addRow("New Movies", stremio::CINEMETA + "/catalog/movie/year/genre=" + year + ".json");
    this->addRow("New Series", stremio::CINEMETA + "/catalog/series/year/genre=" + year + ".json");
    this->addRow("Top Rated Movies", stremio::CINEMETA + "/catalog/movie/imdbRating.json");
    this->addRow("Top Rated Series", stremio::CINEMETA + "/catalog/series/imdbRating.json");
    // Surprise Me: a random genre catalog each launch.
    {
        static const char* genres[] = {"Action", "Adventure", "Animation", "Comedy", "Crime", "Drama", "Family",
            "Fantasy", "History", "Horror", "Mystery", "Romance", "Sci-Fi", "Thriller", "War", "Western"};
        std::srand((unsigned)now);
        bool series = std::rand() & 1;
        std::string genre = genres[std::rand() % 16];
        this->addRow("Surprise Me", stremio::CINEMETA + std::string("/catalog/") + (series ? "series" : "movie") +
                                        "/top/genre=" + genre + ".json");
    }
    this->addRow("Animation Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Animation.json");
    this->addRow("Animation Series", stremio::CINEMETA + "/catalog/series/top/genre=Animation.json");
    this->addRow("Action Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Action.json");
    this->addRow("Action Series", stremio::CINEMETA + "/catalog/series/top/genre=Action.json");
    this->addRow("Sci-Fi Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Sci-Fi.json");
    this->addRow("Sci-Fi Series", stremio::CINEMETA + "/catalog/series/top/genre=Sci-Fi.json");
    this->addRow("Thriller Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Thriller.json");
    this->addRow("Horror Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Horror.json");
    // Anime via the public Kitsu addon (Cinemeta has no separate anime genre).
    this->addRow("Anime Series", "https://anime-kitsu.strem.fun/catalog/series/kitsu-anime-trending.json");
    this->addRow("Comedy Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Comedy.json");
    this->addRow("Comedy Series", stremio::CINEMETA + "/catalog/series/top/genre=Comedy.json");
    this->addRow("War Movies", stremio::CINEMETA + "/catalog/movie/top/genre=War.json");
    this->addRow("Drama Series", stremio::CINEMETA + "/catalog/series/top/genre=Drama.json");
    this->addRow("Fantasy Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Fantasy.json");
    this->addRow("Fantasy Series", stremio::CINEMETA + "/catalog/series/top/genre=Fantasy.json");
    this->addRow("Crime Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Crime.json");
    this->addRow("Crime Series", stremio::CINEMETA + "/catalog/series/top/genre=Crime.json");
    this->addRow("Mystery Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Mystery.json");
    this->addRow("Mystery Series", stremio::CINEMETA + "/catalog/series/top/genre=Mystery.json");
    this->addRow("Romance Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Romance.json");
    this->addRow("Western Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Western.json");
    this->addRow("Documentary Movies", stremio::CINEMETA + "/catalog/movie/top/genre=Documentary.json");
    this->addRow("Documentary Series", stremio::CINEMETA + "/catalog/series/top/genre=Documentary.json");

    // Y opens the on-screen keyboard to search Cinemeta.
    this->registerAction("Search", brls::BUTTON_Y, [](brls::View*) {
        brls::Application::getImeManager()->openForText(
            [](const std::string& text) {
                if (!text.empty())
                    brls::Application::pushActivity(new brls::Activity(new StremioSearch(text)));
            },
            "Search movies & series", "", 64, "", 0);
        return true;
    });

    // − sets/changes the stream addon URL; also prompts automatically on
    // first launch (deferred a frame so the window is up).
    this->registerAction("Stream Addon", brls::BUTTON_BACK, [](brls::View*) {
        promptForAddon(stremio::STREAM_ADDON);
        return true;
    });
    if (stremio::STREAM_ADDON.empty())
        brls::sync([]() { promptForAddon(""); });
}

void StremioHome::draw(
    NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) {
    stremio_theme::drawOceanBackground(vg, x, y, width, height);
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

bool StremioHome::parkFocus(brls::View* avoid) {
    HRecyclerFrame* candidates[] = {this->favRec, this->firstRowRec, this->continueRec};
    for (auto* row : candidates) {
        if (!row || row == avoid || row->getVisibility() != brls::Visibility::VISIBLE) continue;
        brls::View* target = row->getDefaultFocus();
        if (!target) continue;  // e.g. still showing skeletons
        brls::Application::giveFocus(target);
        return true;
    }
    return false;
}

// Pop one activity per frame until home is on top, then focus Continue Watching.
void StremioHome::popToHome() {
    if (brls::Application::popActivity(brls::TransitionAnimation::NONE)) {
        brls::sync([this]() { this->popToHome(); });
    } else if (this->continueRec && this->continueRec->getVisibility() == brls::Visibility::VISIBLE) {
        brls::sync([this]() { brls::Application::giveFocus(this->continueRec); });
    }
}

void StremioHome::addRow(const std::string& title, const std::string& url) {
    auto* header = new brls::Label();
    styleRowHeader(header, title);
    this->boxHome->addView(header);

    auto* rec = new HRecyclerFrame();
    rec->setHeight(300);
    rec->estimatedRowWidth = 175;
    rec->setPadding(0, 50, 0, 50);
    rec->registerCell("Cell", FavCardCell::create);
    this->boxHome->addView(rec);
    if (!this->firstRowRec) this->firstRowRec = rec;  // safe parking spot for focus
    rec->showSkeleton(8);

    ASYNC_RETAIN
    stremio::getJSON<stremio::MetaList>(
        [ASYNC_TOKEN, rec](stremio::MetaList r) {
            ASYNC_RELEASE
            if (!r.metas.empty()) rec->setDataSource(new StremioSource(r.metas));
        },
        [ASYNC_TOKEN, rec](const std::string&) { ASYNC_RELEASE },
        url);
}

void StremioHome::addFavouritesRow() {
    this->favHeader = new brls::Label();
    styleRowHeader(this->favHeader, "Library");
    this->boxHome->addView(this->favHeader);

    this->favRec = new HRecyclerFrame();
    this->favRec->setHeight(300);
    this->favRec->estimatedRowWidth = 175;
    this->favRec->setPadding(0, 50, 0, 50);
    this->favRec->registerCell("Cell", FavCardCell::create);
    this->boxHome->addView(this->favRec);

    // Deferred one frame so the X press finishes before the row (and possibly
    // the pressed cell itself) is torn down and rebuilt.
    Favourites::instance().changed()->subscribe([this]() {
        brls::sync([this]() { this->refreshFavourites(); });
    });
    this->refreshFavourites();
}

void StremioHome::refreshFavourites() {
    // Same focus-parking dance as refreshContinue(): if focus is inside this
    // row, park it on a stable row BEFORE the cells are destroyed, or the
    // focus highlight is left stranded at stale coordinates.
    bool focusHere = false;
    for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
        if (f == this->favRec) {
            focusHere = true;
            break;
        }
    if (focusHere) this->parkFocus(this->favRec);

    // The Library row is ALWAYS visible: up to 4 newest saved titles plus the
    // "See all" card (which is the whole row when the library is empty). Not
    // toggling the row's visibility also removes the stranded-outline glitch
    // the appear/disappear cycle used to cause.
    auto& favs = Favourites::instance().all();
    std::vector<stremio::Meta> newest(favs.rbegin(), favs.rend());
    if (newest.size() > 4) newest.resize(4);
    this->favRec->setDataSource(new LibraryRowSource(std::move(newest)));
    if (focusHere) brls::sync([this]() { brls::Application::giveFocus(this->favRec); });
    // Force a clean relayout (see refreshContinue for why).
    this->boxHome->invalidate();
}

void StremioHome::addContinueRow() {
    this->continueHeader = new brls::Label();
    styleRowHeader(this->continueHeader, "Continue Watching");
    this->boxHome->addView(this->continueHeader);

    this->continueRec = new HRecyclerFrame();
    this->continueRec->setHeight(300);
    this->continueRec->estimatedRowWidth = 175;
    this->continueRec->setPadding(0, 50, 0, 50);
    this->continueRec->registerCell("Cell", FavCardCell::create);
    this->boxHome->addView(this->continueRec);

    ResumeTracker::instance().changed()->subscribe([this]() { this->refreshContinue(); });
    this->refreshContinue();
}

void StremioHome::refreshContinue() {
    // Was focus inside this row? (true when the user just cleared an item;
    // false when the row is refreshing because playback stopped elsewhere.)
    bool focusHere = false;
    for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
        if (f == this->continueRec) {
            focusHere = true;
            break;
        }

    // Park focus on a row that can actually take it BEFORE we tear down the
    // old cells. If parking failed (everything else still loading), fall back
    // to the home box so focus can never stay on a cell about to be destroyed
    // (that left a floating outline through which the removed item was still
    // openable).
    if (focusHere && !this->parkFocus(this->continueRec)) brls::Application::giveFocus(this);

    auto cw = ResumeTracker::instance().continueWatching();
    if (cw.empty()) {
        this->continueHeader->setVisibility(brls::Visibility::GONE);
        this->continueRec->setVisibility(brls::Visibility::GONE);
        // Purge the old cells and data source too: a hidden row keeping them
        // alive is what made the removed poster still selectable.
        this->continueRec->setDataSource(new ContinueSource(std::vector<ResumeEntry>{}));
    } else {
        this->continueHeader->setVisibility(brls::Visibility::VISIBLE);
        this->continueRec->setVisibility(brls::Visibility::VISIBLE);
        this->continueRec->setDataSource(new ContinueSource(cw));
        if (focusHere) brls::sync([this]() { brls::Application::giveFocus(this->continueRec); });
        this->enrichContinue();  // resolve any non-English / stale titles, then rebuild once
    }
    // Force a clean relayout of the whole column: a row toggling between
    // hidden and visible mid-frame left everything below it with a doubled
    // left padding (the "band on the left side" bug).
    this->boxHome->invalidate();
}

// Resolve English titles (and series "Name · S1E5") from Cinemeta for any
// Continue-Watching entry not yet enriched, persist them, and rebuild the row
// once when done. Only ever touches the (retained) activity and its row — never
// a recycled cell pointer — so it's safe against scrolling/teardown.
void StremioHome::enrichContinue() {
    if (this->continueEnriching) return;  // a pass is already running

    // Movies only: this fixes localized/original-language movie titles (e.g. a
    // Polish title) by pulling the English name from Cinemeta. Series already
    // get a clean English "Series · S1E5 · Episode" label built on the series
    // screen, and we don't want to shorten that here.
    auto cw = ResumeTracker::instance().continueWatching();
    std::vector<ResumeEntry> todo;
    for (auto& e : cw)
        if (!e.enriched && e.streamType == "movie") todo.push_back(e);
    if (todo.empty()) return;

    this->continueEnriching = true;
    auto pending = std::make_shared<int>((int)todo.size());
    auto changed = std::make_shared<bool>(false);

    auto finalize = [this, changed]() {
        this->continueEnriching = false;
        if (!*changed) return;
        auto fresh = ResumeTracker::instance().continueWatching();
        if (fresh.empty()) return;
        bool focusHere = false;
        for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
            if (f == this->continueRec) {
                focusHere = true;
                break;
            }
        if (focusHere) this->parkFocus(this->continueRec);
        this->continueRec->setDataSource(new ContinueSource(fresh));
        if (focusHere) brls::sync([this]() { brls::Application::giveFocus(this->continueRec); });
    };

    for (auto& e : todo) {
        const std::string id = e.streamId;  // movie id == ttID (no ":season:episode")
        const std::string oldName = e.name;

        std::string url = stremio::CINEMETA + "/meta/movie/" + id + ".json";
        ASYNC_RETAIN
        stremio::getJSON<stremio::MetaResult>(
            [ASYNC_TOKEN, id, oldName, pending, changed, finalize](stremio::MetaResult r) {
                ASYNC_RELEASE
                if (!r.meta.name.empty()) {
                    ResumeTracker::instance().updateMeta(id, r.meta.name, "");
                    if (r.meta.name != oldName) *changed = true;
                }
                if (--(*pending) == 0) finalize();
            },
            [ASYNC_TOKEN, pending, finalize](const std::string&) {
                ASYNC_RELEASE
                if (--(*pending) == 0) finalize();
            },
            url);
    }
}
