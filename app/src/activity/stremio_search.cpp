/*
    Stremio search screen -- implementation.
*/
#include "activity/stremio_search.hpp"
#include "activity/stremio_series.hpp"
#include "activity/stremio_detail.hpp"
#include "activity/stremio_streams.hpp"
#include "activity/stremio_favourites.hpp"
#include "activity/stremio_resume.hpp"
#include "view/recycling_grid.hpp"
#include "view/video_card.hpp"
#include "view/svg_image.hpp"
#include "view/stremio_theme.hpp"
#include "utils/image.hpp"
#include "api/stremio.hpp"

#include <algorithm>

using namespace brls::literals;

namespace {

// Percent-encode everything outside the unreserved set, so queries with
// commas, ampersands, etc. ("love, death & robots") reach Cinemeta intact.
std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// Same behaviour as the home grid's source: every result opens the detail
// screen.
class SearchSource : public RecyclingGridDataSource {
public:
    explicit SearchSource(const std::vector<stremio::Meta>& r) : list(r) {}

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
        // IMDb rating badge, straight from the Cinemeta catalog data. Hidden
        // when a poster provider is set (rating is baked into the image).
        if (item.imdbRating.empty() || !stremio::POSTER_TEMPLATE.empty()) {
            cell->labelRating->setVisibility(brls::Visibility::INVISIBLE);
        } else {
            cell->labelRating->setText("★ " + item.imdbRating);
            cell->labelRating->setVisibility(brls::Visibility::VISIBLE);
        }
        cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);

        // Heart badge is the feedback — no toast (rapid toggling stacked them).
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

}  // namespace

StremioSearch::StremioSearch(const std::string& query) {
    brls::Logger::debug("StremioSearch: {}", query);
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    // Background is the ocean gradient painted in draw().
    this->setPadding(20, 40, 20, 40);
    // Hold focus here (hidden highlight) until results arrive, so the outline
    // can't float over the covered home screen.
    this->setFocusable(true);
    this->setHideHighlight(true);

    // Plain label, styled like the home-row titles. (brls::Header draws a
    // full-width underline that looked like a stray line above the posters.)
    auto* header = new brls::Label();
    header->setText(fmt::format("Search: {}", query));
    header->setFontSize(27);
    header->setMarginBottom(14);
    header->setMarginLeft(6);
    header->setTextColor(nvgRGB(240, 242, 248));
    this->addView(header);

    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 6;
    this->recycler->estimatedRowHeight = 300;
    this->recycler->registerCell("Cell", FavCardCell::create);
    this->addView(this->recycler);
    this->recycler->showSkeleton();

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });

    // Make sure something is focused so B (back) always works.
    brls::sync([this]() { brls::Application::giveFocus(this); });

    std::string q = urlEncode(query);
    this->fetchInto(stremio::CINEMETA + "/catalog/movie/top/search=" + q + ".json", false);
    this->fetchInto(stremio::CINEMETA + "/catalog/series/top/search=" + q + ".json", true);
}

void StremioSearch::draw(
    NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) {
    stremio_theme::drawOceanBackground(vg, x, y, width, height);
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

void StremioSearch::fetchInto(const std::string& url, bool isSeries) {
    ASYNC_RETAIN
    stremio::getJSON<stremio::MetaList>(
        [ASYNC_TOKEN, isSeries](stremio::MetaList r) {
            ASYNC_RELEASE
            this->addResults(r.metas, isSeries);
        },
        [ASYNC_TOKEN, isSeries](const std::string& e) {
            ASYNC_RELEASE
            this->addResults({}, isSeries);  // let the empty-state logic run
        },
        url);
}

void StremioSearch::addResults(const std::vector<stremio::Meta>& metas, bool isSeries) {
    (isSeries ? this->series : this->movies) = metas;
    this->pending--;
    this->rebuild();
}

// Interleave series and movies (each list is already relevance-ordered by
// Cinemeta). Appending one list after the other buried every series result
// under up to ~100 movies — "Love, Death & Robots" was unfindable.
void StremioSearch::rebuild() {
    std::vector<stremio::Meta> mixed;
    mixed.reserve(this->movies.size() + this->series.size());
    size_t n = std::max(this->movies.size(), this->series.size());
    for (size_t i = 0; i < n; i++) {
        if (i < this->series.size()) mixed.push_back(this->series[i]);
        if (i < this->movies.size()) mixed.push_back(this->movies[i]);
    }

    if (mixed.empty()) {
        if (this->pending <= 0) this->recycler->setEmpty("No results", "icon/ico-search.svg");
        return;
    }
    bool firstResults = dynamic_cast<SearchSource*>(this->recycler->getDataSource()) == nullptr;
    // If focus is already inside the grid, the rebuild recycles the focused
    // cell — the outline would silently follow the reused object onto a
    // random poster. Re-give focus so it lands back on the first result.
    bool focusInside = false;
    for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
        if (f == this->recycler) {
            focusInside = true;
            break;
        }
    this->recycler->setDataSource(new SearchSource(mixed));
    if (firstResults || focusInside)
        brls::sync([this]() { brls::Application::giveFocus(this->recycler); });
}
