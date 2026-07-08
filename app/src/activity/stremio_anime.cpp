/*
    Anime browse screen -- implementation.
*/
#include "activity/stremio_anime.hpp"
#include "activity/stremio_detail.hpp"
#include "activity/stremio_favourites.hpp"
#include "view/recycling_grid.hpp"
#include "view/video_card.hpp"
#include "view/svg_image.hpp"
#include "view/stremio_theme.hpp"
#include "utils/image.hpp"

using namespace brls::literals;

namespace {

// Percent-encode for the kitsu search catalog (same as the Cinemeta search).
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

// Same cell styling as the other grids; A opens the detail screen, X toggles
// library membership.
class AnimeGridSource : public RecyclingGridDataSource {
public:
    explicit AnimeGridSource(std::vector<stremio::Meta> r) : list(std::move(r)) {}

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
        if (item.imdbRating.empty()) {
            cell->labelRating->setVisibility(brls::Visibility::INVISIBLE);
        } else {
            cell->labelRating->setText("★ " + item.imdbRating);
            cell->labelRating->setVisibility(brls::Visibility::VISIBLE);
        }
        cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);

        bool fav = Favourites::instance().contains(item.id);
        cell->badgeFavorite->setVisibility(fav ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
        cell->onToggleFav = [item, cell]() {
            bool nowFav = Favourites::instance().toggle(item);
            cell->badgeFavorite->setVisibility(nowFav ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
        };

        if (!item.poster.empty()) Image::with(cell->picture, item.poster);
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        brls::Application::pushActivity(
            new brls::Activity(new StremioDetail(this->list.at(index))), brls::TransitionAnimation::NONE);
    }

    void clearData() override { this->list.clear(); }

private:
    std::vector<stremio::Meta> list;
};

}  // namespace

StremioAnime::StremioAnime() {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setPadding(20, 40, 20, 40);
    // The root can hold focus while the grid is still loading, so the outline
    // never floats over a covered screen (it is hidden on the root itself).
    this->setFocusable(true);
    this->setHideHighlight(true);

    auto* top = new brls::Box();
    top->setAxis(brls::Axis::ROW);
    top->setAlignItems(brls::AlignItems::CENTER);
    top->setMarginBottom(14);

    this->headline = new brls::Label();
    this->headline->setFontSize(27);
    this->headline->setMarginLeft(6);
    this->headline->setTextColor(stremio_theme::TEXT);
    this->headline->setGrow(1.0f);
    this->headline->setText("Anime · Trending");
    top->addView(this->headline);

    this->btnSearch = new brls::Box();
    this->btnSearch->setFocusable(true);
    this->btnSearch->setAxis(brls::Axis::ROW);
    this->btnSearch->setPadding(8, 18, 8, 18);
    this->btnSearch->setCornerRadius(6);
    this->btnSearch->setBackgroundColor(nvgRGBA(255, 255, 255, 22));
    this->btnSearch->setHideHighlightBackground(true);
    this->lblSearch = new brls::Label();
    this->lblSearch->setText("🔍 Search anime");
    this->lblSearch->setFontSize(20);
    this->lblSearch->setTextColor(stremio_theme::TEXT);
    this->btnSearch->addView(this->lblSearch);
    top->addView(this->btnSearch);
    this->addView(top);

    auto doSearch = [this]() {
        brls::Application::getImeManager()->openForText(
            [this](const std::string& text) {
                if (text.empty()) {
                    this->headline->setText("Anime · Trending");
                    this->lblSearch->setText("🔍 Search anime");
                    this->fetch(stremio::KITSU + "/catalog/anime/kitsu-anime-trending.json");
                } else {
                    this->headline->setText("Anime · " + text);
                    this->lblSearch->setText("🔍 " + text);
                    this->fetch(stremio::KITSU + "/catalog/anime/kitsu-anime-list/search=" + urlEncode(text) + ".json");
                }
            },
            "Search anime (Kitsu)", "Leave empty to go back to trending", 64, "", 0);
    };
    this->btnSearch->registerClickAction([doSearch](brls::View*) {
        doSearch();
        return true;
    });
    this->btnSearch->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnSearch));
    this->registerAction("Search", brls::BUTTON_Y, [doSearch](brls::View*) {
        doSearch();
        return true;
    });

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

    brls::sync([this]() { brls::Application::giveFocus(this); });
    this->fetch(stremio::KITSU + "/catalog/anime/kitsu-anime-trending.json");
}

void StremioAnime::draw(
    NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) {
    stremio_theme::drawOceanBackground(vg, x, y, width, height);
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

void StremioAnime::fetch(const std::string& url) {
    ASYNC_RETAIN
    stremio::getJSON<stremio::MetaList>(
        [ASYNC_TOKEN](stremio::MetaList r) {
            ASYNC_RELEASE
            if (r.metas.empty()) {
                this->recycler->setEmpty("Nothing found");
                return;
            }
            this->recycler->setDataSource(new AnimeGridSource(r.metas));
            brls::sync([this]() { brls::Application::giveFocus(this->recycler); });
        },
        [ASYNC_TOKEN](const std::string& e) {
            ASYNC_RELEASE
            this->recycler->setEmpty("Anime addon error: " + e);
        },
        url);
}
