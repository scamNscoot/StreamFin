/*
    Home row customization -- implementation.
*/
#include "activity/stremio_rows.hpp"
#include "view/recycling_grid.hpp"
#include "view/stremio_theme.hpp"
#include "utils/config.hpp"
#include "api/stremio.hpp"

#include <fstream>
#include <sys/stat.h>
#include <ctime>

using namespace brls::literals;

namespace rowcfg {

const std::vector<RowDef>& registry() {
    static std::vector<RowDef> defs = [] {
        std::vector<RowDef> d;
        auto cat = [](const char* type, const char* path) {
            return stremio::CINEMETA + "/catalog/" + type + "/" + path;
        };
        std::time_t now = std::time(nullptr);
        std::string year = std::to_string(1900 + std::localtime(&now)->tm_year);

        d.push_back({"popular-movies", "Popular Movies", cat("movie", "top.json")});
        d.push_back({"popular-series", "Popular Series", cat("series", "top.json")});
        d.push_back({"new-movies", "New Movies", cat("movie", ("year/genre=" + year + ".json").c_str())});
        d.push_back({"new-series", "New Series", cat("series", ("year/genre=" + year + ".json").c_str())});
        d.push_back({"top-movies", "Top Rated Movies", cat("movie", "imdbRating.json")});
        d.push_back({"top-series", "Top Rated Series", cat("series", "imdbRating.json")});
        d.push_back({"anime", "Anime", ""});  // special: Kitsu row with More card

        // Every Cinemeta genre, movies + series.
        static const char* genres[] = {"Action", "Adventure", "Animation", "Biography", "Comedy", "Crime",
            "Documentary", "Drama", "Family", "Fantasy", "History", "Horror", "Music", "Musical", "Mystery",
            "Romance", "Sci-Fi", "Sport", "Thriller", "War", "Western"};
        for (const char* g : genres) {
            d.push_back({std::string("movie-") + g, std::string(g) + " Movies",
                cat("movie", (std::string("top/genre=") + g + ".json").c_str())});
            d.push_back({std::string("series-") + g, std::string(g) + " Series",
                cat("series", (std::string("top/genre=") + g + ".json").c_str())});
        }
        return d;
    }();
    return defs;
}

const RowDef* find(const std::string& key) {
    for (auto& d : registry())
        if (d.key == key) return &d;
    return nullptr;
}

// The default lineup (v0.3.x order).
static std::vector<std::pair<std::string, bool>> defaults() {
    std::vector<std::pair<std::string, bool>> rows;
    for (const char* k : {"popular-movies", "popular-series", "new-movies", "new-series", "top-movies",
             "top-series", "movie-Animation", "series-Animation", "movie-Action", "series-Action",
             "movie-Sci-Fi", "series-Sci-Fi", "movie-Thriller", "movie-Horror", "anime", "movie-Comedy",
             "series-Comedy", "movie-War", "series-Drama", "movie-Fantasy", "series-Fantasy", "movie-Crime",
             "series-Crime", "movie-Mystery", "series-Mystery", "movie-Romance", "movie-Western",
             "movie-Documentary", "series-Documentary"})
        rows.push_back({k, true});
    for (auto& d : registry()) {
        bool present = false;
        for (auto& r : rows)
            if (r.first == d.key) {
                present = true;
                break;
            }
        if (!present) rows.push_back({d.key, false});
    }
    return rows;
}

static std::string cfgPath() { return AppConfig::instance().configDir() + "/rows.json"; }

std::vector<std::pair<std::string, bool>> load() {
    std::vector<std::pair<std::string, bool>> rows;
    try {
        std::ifstream in(cfgPath());
        if (in.is_open()) {
            nlohmann::json j;
            in >> j;
            for (auto& e : j)
                if (e.contains("key") && find(e["key"].get<std::string>()))
                    rows.push_back({e["key"].get<std::string>(), e.value("on", true)});
        }
    } catch (const std::exception& e) {
        brls::Logger::warning("rows load: {}", e.what());
        rows.clear();
    }
    if (rows.empty()) return defaults();
    // New registry entries (app updates) appear disabled at the end.
    for (auto& d : registry()) {
        bool present = false;
        for (auto& r : rows)
            if (r.first == d.key) {
                present = true;
                break;
            }
        if (!present) rows.push_back({d.key, false});
    }
    return rows;
}

void save(const std::vector<std::pair<std::string, bool>>& rows) {
    std::string dir = AppConfig::instance().configDir();
    ::mkdir(dir.c_str(), 0777);
    try {
        nlohmann::json j = nlohmann::json::array();
        for (auto& r : rows) j.push_back({{"key", r.first}, {"on", r.second}});
        std::ofstream out(cfgPath());
        if (out.is_open()) out << j.dump(2);
    } catch (const std::exception& e) {
        brls::Logger::warning("rows save: {}", e.what());
    }
}

}  // namespace rowcfg

namespace {

// One row in the editor list: name + On/Off state.
class RowCell : public RecyclingGridItem {
public:
    RowCell() {
        this->setFocusable(true);
        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setPadding(8, 14, 8, 14);
        this->setCornerRadius(6);

        this->name = new brls::Label();
        this->name->setFontSize(20);
        this->name->setGrow(1.0f);
        this->addView(this->name);

        this->state = new brls::Label();
        this->state->setFontSize(18);
        this->addView(this->state);

        this->registerAction("Move up", brls::BUTTON_Y, [this](brls::View*) {
            if (this->onMove) this->onMove(-1);
            return true;
        });
        this->registerAction("Move down", brls::BUTTON_X, [this](brls::View*) {
            if (this->onMove) this->onMove(+1);
            return true;
        });
    }

    brls::Label* name = nullptr;
    brls::Label* state = nullptr;
    std::function<void(int)> onMove;

    static RecyclingGridItem* create() { return new RowCell(); }
};

class RowsSource : public RecyclingGridDataSource {
public:
    RowsSource(StremioRows* owner, std::vector<std::pair<std::string, bool>>* rows,
        std::function<void(size_t, int)> move, std::function<void(size_t)> toggle)
        : rows(rows), move(move), toggle(toggle) {}

    size_t getItemCount() override { return this->rows->size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        RowCell* cell = dynamic_cast<RowCell*>(recycler->dequeueReusableCell("Cell"));
        auto& r = this->rows->at(index);
        const rowcfg::RowDef* def = rowcfg::find(r.first);
        cell->name->setText(def ? def->title : r.first);
        cell->name->setTextColor(r.second ? stremio_theme::TEXT : stremio_theme::TEXT_DIM);
        cell->state->setText(r.second ? "On" : "Off");
        cell->state->setTextColor(r.second ? stremio_theme::ACCENT_HI : stremio_theme::TEXT_DIM);
        size_t i = index;
        auto mv = this->move;
        cell->onMove = [mv, i](int dir) { mv(i, dir); };
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override { this->toggle(index); }

    void clearData() override {}

private:
    std::vector<std::pair<std::string, bool>>* rows;
    std::function<void(size_t, int)> move;
    std::function<void(size_t)> toggle;
};

}  // namespace

StremioRows::StremioRows(std::function<void()> onApply) : onApply(onApply) {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setPadding(20, 120, 20, 120);
    this->setFocusable(true);
    this->setHideHighlight(true);

    this->head = new brls::Label();
    this->head->setFontSize(26);
    this->head->setTextColor(stremio_theme::TEXT);
    this->head->setMarginBottom(14);
    this->addView(this->head);

    this->rows = rowcfg::load();

    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 3;
    this->recycler->estimatedRowHeight = 64;
    this->recycler->estimatedRowSpace = 8;
    this->recycler->registerCell("Cell", RowCell::create);
    this->addView(this->recycler);

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [this](brls::View*) {
        if (this->dirty) {
            rowcfg::save(this->rows);
            brls::Application::notify("Home rows updated");
            if (this->onApply) this->onApply();
        }
        brls::Application::popActivity();
        return true;
    });

    this->reload(0);
    brls::sync([this]() { brls::Application::giveFocus(this->recycler); });
}

void StremioRows::reload(size_t focusIndex) {
    size_t on = 0;
    for (auto& r : this->rows)
        if (r.second) on++;
    this->head->setText(fmt::format(
        "Home rows · {} of {} enabled — A toggle · Y up · X down · B save", on, this->rows.size()));
    auto move = [this](size_t i, int dir) {
        size_t j = (size_t)((int)i + dir);
        if (j >= this->rows.size()) return;
        std::swap(this->rows[i], this->rows[j]);
        this->dirty = true;
        this->reload(j);
    };
    auto toggle = [this](size_t i) {
        this->rows[i].second = !this->rows[i].second;
        this->dirty = true;
        this->reload(i);
    };
    this->recycler->setDataSource(new RowsSource(this, &this->rows, move, toggle));
    this->recycler->selectRowAt(focusIndex, false);
    brls::sync([this]() { brls::Application::giveFocus(this->recycler); });
}

void StremioRows::draw(
    NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) {
    stremio_theme::drawOceanBackground(vg, x, y, width, height);
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}
