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
#include "activity/stremio_catalogs.hpp"
#include "view/recycling_grid.hpp"
#include "view/h_recycling.hpp"
#include "view/video_card.hpp"
#include "view/svg_image.hpp"
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
        "Type your addon URL — or easier: put it in a text file at switch/streamfin-addon.txt on the SD card and relaunch "
        "(one URL per line; multiple lines = multiple addons, keyboard sets one)",
        1024, initial, 0);
}

// Consistent, polished styling for every row title (Continue Watching,
// Favourites, and all catalog rows) — one place to tweak the look.
void styleRowHeader(brls::Label* h, const std::string& text) {
    h->setText(text);
    h->setFontSize(27);
    h->setMarginTop(28);
    h->setMarginBottom(12);
    h->setMarginLeft(6);
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
        if (stremio::STREAM_ADDONS.empty()) {
            brls::Application::notify("Set your stream addon first (press −)");
            return;
        }
        brls::Application::notify("Finding streams…");
        stremio::getStreams(e.streamType, e.streamId,
            [e](std::vector<stremio::Stream> streams) {
                if (streams.empty()) {
                    brls::Application::notify("No streams found");
                    return;
                }
                brls::Application::pushActivity(new brls::Activity(new StreamPicker(e.name, streams, e)));
            },
            [](const std::string& err) { brls::Application::notify("Stream error: " + err); });
    }

    void clearData() override { this->list.clear(); }

private:
    std::vector<ResumeEntry> list;
};

}  // namespace

StremioHome::~StremioHome() { stremio::CATALOGS_CHANGED.unsubscribe(this->catalogsSub); }

StremioHome::StremioHome() {
    brls::Logger::debug("StremioHome: create");
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(nvgRGB(16, 18, 24));  // deep cinematic slate

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingIndicatorVisible(false);

    this->boxHome = new brls::Box();
    this->boxHome->setAxis(brls::Axis::COLUMN);
    this->boxHome->setPadding(16, 50, 32, 50);  // more breathing room at the edges
    scroll->setContentView(this->boxHome);

    this->addView(scroll);

    // Continue Watching (very top) + Favourites rows — both hidden until non-empty.
    Favourites::instance().load();
    ResumeTracker::instance().init();
    this->addContinueRow();
    this->addFavouritesRow();

    // Catalog carousels come from the persisted row registry: the built-in
    // Cinemeta rows plus every catalog offered by the user's catalog addons
    // (catalog=URL lines in streamfin-addon.txt), in the user's order.
    this->buildCatalogRows();

    // Manifest refreshes and Catalogs-screen edits rebuild the carousels.
    this->catalogsSub = stremio::CATALOGS_CHANGED.subscribe([this]() { this->rebuildCatalogRows(); });

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
        promptForAddon(stremio::STREAM_ADDONS.empty() ? "" : stremio::STREAM_ADDONS.front());
        return true;
    });

    // + opens the Catalogs screen: toggle home rows on/off and reorder them.
    this->registerAction("Catalogs", brls::BUTTON_START, [](brls::View*) {
        brls::Application::pushActivity(new brls::Activity(new StremioCatalogs()));
        return true;
    });
    if (stremio::STREAM_ADDONS.empty())
        brls::sync([]() { promptForAddon(""); });
}

void StremioHome::addRow(const std::string& title, const std::string& url) {
    auto* header = new brls::Label();
    styleRowHeader(header, title);
    this->boxHome->addView(header);

    auto* rec = new HRecyclerFrame();
    rec->setHeight(300);
    rec->estimatedRowWidth = 175;
    rec->registerCell("Cell", FavCardCell::create);
    this->boxHome->addView(rec);
    this->catalogViews.push_back(header);
    this->catalogViews.push_back(rec);
    if (!this->firstRowRec) this->firstRowRec = rec;  // safe parking spot for focus
    rec->showSkeleton(8);

    ASYNC_RETAIN
    stremio::getJSON<stremio::MetaList>(
        [ASYNC_TOKEN, rec](stremio::MetaList r) {
            ASYNC_RELEASE
            // The registry may have rebuilt the carousels while this fetch was
            // in flight (manifest refresh on a cold start) — the recycler this
            // callback captured could be destroyed. Only touch it if it's
            // still one of ours.
            auto& live = this->catalogViews;
            if (std::find(live.begin(), live.end(), (brls::View*)rec) == live.end()) return;
            if (r.metas.empty()) return;
            bool focusHere = false;
            for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
                if (f == rec) {
                    focusHere = true;
                    break;
                }
            rec->setDataSource(new StremioSource(r.metas));
            // Focus sat on this row's skeleton (parked there during a rebuild):
            // re-land it on the real first cell now that the data is in.
            if (focusHere) brls::sync([this, rec]() {
                auto& v = this->catalogViews;
                if (std::find(v.begin(), v.end(), (brls::View*)rec) != v.end())
                    brls::Application::giveFocus(rec);
            });
        },
        [ASYNC_TOKEN, rec](const std::string&) { ASYNC_RELEASE },
        url);
}

void StremioHome::buildCatalogRows() {
    for (auto& def : stremio::CATALOG_ROWS)
        if (def.enabled) this->addRow(def.title, def.url);
}

void StremioHome::rebuildCatalogRows() {
    // While another screen is on top (Catalogs, detail, player), borealis'
    // focus stack holds a pointer to the home view that had focus — and
    // popActivity gives focus back to that exact pointer. Destroying rows now
    // would leave it dangling (ghost highlight, then a crash on back).
    // Defer; onChildFocusGained fires the rebuild once focus returns to home.
    auto stack = brls::Application::getActivitiesStack();
    if (!stack.empty() && this->getParentActivity() != stack.back()) {
        this->pendingRebuild = true;
        return;
    }
    this->pendingRebuild = false;

    // Same focus-parking discipline as refreshFavourites/refreshContinue:
    // never leave focus on a view that's about to be destroyed.
    bool focusHere = false;
    for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
        if (std::find(this->catalogViews.begin(), this->catalogViews.end(), f) != this->catalogViews.end()) {
            focusHere = true;
            break;
        }

    // Build the new rows first (appended after the old ones), so there is
    // always a live row to park focus on before the old cells are destroyed.
    std::vector<brls::View*> old = std::move(this->catalogViews);
    this->catalogViews.clear();
    this->firstRowRec = nullptr;
    this->buildCatalogRows();

    if (focusHere) {
        if (this->firstRowRec)
            brls::Application::giveFocus(this->firstRowRec);
        else
            brls::Application::giveFocus(this);  // all rows toggled off
    }

    for (auto* v : old) this->boxHome->removeView(v);
    // Force a clean relayout — stale yoga incremental layout after toggling
    // row visibility is this screen's oldest bug (see refreshFavourites).
    this->boxHome->invalidate();
}

void StremioHome::onChildFocusGained(brls::View* directChild, brls::View* focusedView) {
    brls::Box::onChildFocusGained(directChild, focusedView);
    // Focus coming back into home (e.g. the Catalogs screen was just popped)
    // with a deferred registry change: rebuild now. Next frame, so the
    // focus restore fully settles first.
    if (this->pendingRebuild) brls::sync([this]() { this->rebuildCatalogRows(); });
}

void StremioHome::addFavouritesRow() {
    this->favHeader = new brls::Label();
    styleRowHeader(this->favHeader, "Favourites");
    this->boxHome->addView(this->favHeader);

    this->favRec = new HRecyclerFrame();
    this->favRec->setHeight(300);
    this->favRec->estimatedRowWidth = 175;
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
    if (focusHere && this->firstRowRec) brls::Application::giveFocus(this->firstRowRec);

    auto& favs = Favourites::instance().all();
    if (favs.empty()) {
        this->favHeader->setVisibility(brls::Visibility::GONE);
        this->favRec->setVisibility(brls::Visibility::GONE);
        // Row is gone; focus stays parked.
    } else {
        this->favHeader->setVisibility(brls::Visibility::VISIBLE);
        this->favRec->setVisibility(brls::Visibility::VISIBLE);
        this->favRec->setDataSource(new StremioSource(favs));
        if (focusHere) brls::sync([this]() { brls::Application::giveFocus(this->favRec); });
    }
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

    // Park focus on a stable row BEFORE we tear down the old cells, so the focus
    // highlight is never left pointing at a cell that's about to be destroyed.
    if (focusHere && this->firstRowRec) brls::Application::giveFocus(this->firstRowRec);

    auto cw = ResumeTracker::instance().continueWatching();
    if (cw.empty()) {
        this->continueHeader->setVisibility(brls::Visibility::GONE);
        this->continueRec->setVisibility(brls::Visibility::GONE);
        // Row is gone now; leave focus on the parked row.
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
        if (focusHere && this->firstRowRec) brls::Application::giveFocus(this->firstRowRec);
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
