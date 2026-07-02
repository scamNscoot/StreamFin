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
#include "utils/image.hpp"
#include "api/stremio.hpp"

using namespace brls::literals;

namespace {

// Minimal URL-encoding (spaces are the common case in search terms).
std::string urlEncode(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == ' ')
            out += "%20";
        else
            out += c;
    }
    return out;
}

// Same behaviour as the home grid's source: movies open the picker, series
// open the season list.
class SearchSource : public RecyclingGridDataSource {
public:
    explicit SearchSource(const std::vector<stremio::Meta>& r) : list(r) {}

    void append(const std::vector<stremio::Meta>& r) { this->list.insert(this->list.end(), r.begin(), r.end()); }

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
        cell->labelRating->setVisibility(brls::Visibility::INVISIBLE);
        cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);

        bool fav = Favourites::instance().contains(item.id);
        cell->badgeFavorite->setVisibility(fav ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
        cell->onToggleFav = [item, cell]() {
            bool nowFav = Favourites::instance().toggle(item);
            cell->badgeFavorite->setVisibility(nowFav ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
            brls::Application::notify(nowFav ? "Added to Favourites" : "Removed from Favourites");
        };

        if (!item.poster.empty()) Image::with(cell->picture, item.poster);

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
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);

    auto* header = new brls::Header();
    header->setTitle(fmt::format("Search: {}", query));
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
    brls::sync([this]() { brls::Application::giveFocus(this->recycler); });

    std::string q = urlEncode(query);
    this->fetchInto(stremio::CINEMETA + "/catalog/movie/top/search=" + q + ".json");
    this->fetchInto(stremio::CINEMETA + "/catalog/series/top/search=" + q + ".json");
}

void StremioSearch::fetchInto(const std::string& url) {
    ASYNC_RETAIN
    stremio::getJSON<stremio::MetaList>(
        [ASYNC_TOKEN](stremio::MetaList r) {
            ASYNC_RELEASE
            this->addResults(r.metas);
        },
        [ASYNC_TOKEN](const std::string& e) {
            ASYNC_RELEASE
            this->addResults({});  // let the empty-state logic run
        },
        url);
}

void StremioSearch::addResults(const std::vector<stremio::Meta>& metas) {
    auto* src = dynamic_cast<SearchSource*>(this->recycler->getDataSource());
    if (metas.empty()) {
        if (!src) this->recycler->setEmpty("No results", "icon/ico-search.svg");
        return;
    }
    if (src) {
        src->append(metas);
        this->recycler->notifyDataChanged();
    } else {
        this->recycler->setDataSource(new SearchSource(metas));
        brls::Application::giveFocus(this->recycler);
    }
}
