/*
    Catalogs screen -- implementation.

    Operates directly on stremio::CATALOG_ROWS (the ordered home-row
    registry). Edits are applied in memory immediately; on exit they are
    persisted and CATALOGS_CHANGED makes the home screen rebuild.
*/
#include "activity/stremio_catalogs.hpp"
#include "view/recycling_grid.hpp"
#include "api/stremio.hpp"
#include "utils/config.hpp"

#include <algorithm>

using namespace brls::literals;

namespace {

// One edit was made since the screen opened (drives save-on-exit).
bool dirty = false;

std::string sourceOf(const stremio::CatalogRowDef& def) {
    if (def.key.rfind("cinemeta:", 0) == 0) return "Cinemeta";
    // Host part of the catalog URL, so rows from different addons are
    // tellable apart without showing the whole (token-bearing) URL.
    size_t start = def.url.find("://");
    if (start == std::string::npos) return "Addon";
    start += 3;
    size_t end = def.url.find('/', start);
    return def.url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// iOS/Android-style switch: pill track with a sliding white knob. The slide
// (and the grey→red colour blend) animates over ~180ms on toggle.
class ToggleSwitch : public brls::View {
public:
    ToggleSwitch() {
        this->setFocusable(false);
        this->setDimensions(56, 30);
    }

    void setOn(bool value, bool animated) {
        this->anim.stop();
        if (animated) {
            this->anim.reset();
            this->anim.addStep(value ? 1.0f : 0.0f, 180, brls::EasingFunction::quadraticOut);
            this->anim.start();
        } else {
            this->anim.reset(value ? 1.0f : 0.0f);
        }
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override {
        float t = this->anim;
        // Track: grey when off, brand red when on.
        NVGcolor off = nvgRGB(120, 120, 128);
        NVGcolor on = nvgRGB(245, 33, 42);
        NVGcolor track = nvgRGBAf(off.r + (on.r - off.r) * t, off.g + (on.g - off.g) * t,
            off.b + (on.b - off.b) * t, this->getAlpha());
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, height / 2);
        nvgFillColor(vg, track);
        nvgFill(vg);
        // Knob: white disc sliding left<->right.
        float r = height / 2 - 3;
        float cx = x + height / 2 + t * (width - height);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, y + height / 2, r);
        nvgFillColor(vg, nvgRGBAf(1.0f, 1.0f, 1.0f, this->getAlpha()));
        nvgFill(vg);
    }

private:
    brls::Animatable anim = 0.0f;
};

// Burger drag-handle: three grey bars, the universal "this row can be moved"
// affordance. (Drawn, not a glyph — ☰ is missing from the Switch font.)
class DragHandle : public brls::View {
public:
    DragHandle() {
        this->setFocusable(false);
        this->setDimensions(22, 16);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override {
        float barH = 2.5f;
        NVGcolor c = nvgRGBAf(0.58f, 0.6f, 0.63f, this->getAlpha());
        for (int i = 0; i < 3; i++) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y + i * (height - barH) / 2, width, barH, barH / 2);
            nvgFillColor(vg, c);
            nvgFill(vg);
        }
    }
};

// Card row: [burger handle] [title + source] [toggle], vertically centred on
// a surface-coloured rounded card.
class CatalogCell : public RecyclingGridItem {
public:
    CatalogCell() {
        this->setFocusable(true);
        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setPadding(14, 24, 14, 24);
        this->setCornerRadius(10);
        this->setBackgroundColor(brls::Application::getTheme()["color/surface"]);

        this->handle = new DragHandle();
        this->handle->setMarginRight(20);
        this->addView(this->handle);

        auto* text = new brls::Box(brls::Axis::COLUMN);
        text->setGrow(1.0f);
        this->name = new brls::Label();
        this->name->setFontSize(24);
        text->addView(this->name);
        this->detail = new brls::Label();
        this->detail->setFontSize(17);
        this->detail->setMarginTop(2);
        this->detail->setTextColor(brls::Application::getTheme()["font/grey"]);
        text->addView(this->detail);
        this->addView(text);

        this->toggle = new ToggleSwitch();
        this->toggle->setMarginLeft(20);
        this->addView(this->toggle);
    }

    void apply(const stremio::CatalogRowDef& def, bool animated) {
        this->name->setText(def.title);
        this->detail->setText(sourceOf(def));
        this->toggle->setOn(def.enabled, animated);
        // Disabled rows sit dimmed so the enabled set reads at a glance.
        this->name->setAlpha(def.enabled ? 1.0f : 0.45f);
        this->handle->setAlpha(def.enabled ? 1.0f : 0.45f);
    }

    brls::Label* name = nullptr;
    brls::Label* detail = nullptr;
    ToggleSwitch* toggle = nullptr;
    DragHandle* handle = nullptr;
};

class CatalogSource : public RecyclingGridDataSource {
public:
    size_t getItemCount() override { return stremio::CATALOG_ROWS.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        CatalogCell* cell = dynamic_cast<CatalogCell*>(recycler->dequeueReusableCell("Cell"));
        cell->apply(stremio::CATALOG_ROWS.at(index), false);
        return cell;
    }

    // A = toggle on/off. The cell is updated in place — no reload, focus stays.
    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto& def = stremio::CATALOG_ROWS.at(index);
        def.enabled = !def.enabled;
        dirty = true;
        auto* grid = dynamic_cast<RecyclingGrid*>(recycler);
        if (!grid) return;
        auto* cell = dynamic_cast<CatalogCell*>(grid->getGridItemByIndex(index));
        if (cell) cell->apply(def, true);
    }

    void clearData() override {}
};

}  // namespace

StremioCatalogs::StremioCatalogs() {
    dirty = false;

    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);

    auto* header = new brls::Label();
    header->setFontSize(30);
    header->setText("Catalogs");
    header->setMarginBottom(4);
    this->addView(header);

    auto* hint = new brls::Label();
    hint->setFontSize(18);
    hint->setText("Rows appear on the home screen in this order · add catalog addons with catalog=URL lines in streamfin-addon.txt");
    hint->setMarginBottom(12);
    this->addView(hint);

    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 1;
    this->recycler->isFlowMode = true;
    this->recycler->estimatedRowSpace = 10;
    this->recycler->registerCell("Cell", []() -> RecyclingGridItem* { return new CatalogCell(); });
    this->addView(this->recycler);
    this->recycler->setDataSource(new CatalogSource());

    brls::sync([this]() { brls::Application::giveFocus(this->recycler); });

    // Move the focused row up/down. Rebuild the list with focus parked on the
    // row's new position (same select-after-reload dance the picker uses).
    auto move = [this](int delta) {
        brls::View* f = brls::Application::getCurrentFocus();
        auto* cell = dynamic_cast<RecyclingGridItem*>(f);
        if (!cell) return;
        size_t i = cell->getIndex();
        auto& rows = stremio::CATALOG_ROWS;
        if (delta < 0 && i == 0) return;
        if (delta > 0 && i + 1 >= rows.size()) return;
        std::swap(rows[i], rows[i + delta]);
        dirty = true;
        this->recycler->setDefaultCellFocus(i + delta);
        this->recycler->reloadData();
        brls::sync([this]() { brls::Application::giveFocus(this->recycler); });
    };
    this->registerAction("Move Up", brls::BUTTON_X, [move](brls::View*) {
        move(-1);
        return true;
    });
    this->registerAction("Move Down", brls::BUTTON_Y, [move](brls::View*) {
        move(+1);
        return true;
    });

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        if (dirty) {
            stremio::saveConfig(AppConfig::instance().configDir());
            stremio::CATALOGS_CHANGED.fire();
        }
        brls::Application::popActivity();
        return true;
    });
}
