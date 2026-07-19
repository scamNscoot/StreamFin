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

// Index of the row currently picked up with X (-1 = none). While a row is
// held, up/down carries it through the list instead of moving the highlight.
int heldIndex = -1;

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

    // Carry styling: red-tinted card while the row is picked up — the locked
    // focus highlight plus this tint reads as "you are holding this".
    void setHeld(bool held) {
        NVGcolor c = brls::Application::getTheme()["color/surface"];
        if (held) c = nvgLerpRGBA(c, nvgRGB(245, 33, 42), 0.22f);
        this->setBackgroundColor(c);
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
        cell->setHeld((int)index == heldIndex);
        return cell;
    }

    // A = toggle on/off. The cell is updated in place — no reload, focus stays.
    void onItemSelected(brls::Box* recycler, size_t index) override {
        if (heldIndex >= 0) return;  // A is inert while carrying a row
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
    heldIndex = -1;

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
    hint->setText("Rows appear on the home screen in this order · X picks a row up, carry it with up/down, Y places it · add catalog addons with catalog=URL lines in streamfin-addon.txt");
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

    // Grab-and-carry reorder: X picks the focused row up; while held, up/down
    // (d-pad or left stick — both arrive here as focus navigation) carries it
    // through the list; Y or X places it. The interceptor swaps the DATA of
    // adjacent rows in place and hands focus to the neighbouring cell, so the
    // tinted card + highlight track the carried row with the normal focus
    // animation — no reload, no focus parking in the common case.
    this->recycler->cellNavigationInterceptor = [this](brls::FocusDirection dir,
                                                    brls::View* current) -> brls::View* {
        if (heldIndex < 0) return nullptr;  // not carrying: stock navigation
        auto* cell = dynamic_cast<CatalogCell*>(current);
        if (!cell) return nullptr;
        if (dir != brls::FocusDirection::UP && dir != brls::FocusDirection::DOWN)
            return current;  // no sideways escape while carrying
        size_t i = cell->getIndex();
        auto& rows = stremio::CATALOG_ROWS;
        int t = (int)i + (dir == brls::FocusDirection::DOWN ? 1 : -1);
        if (t < 0 || t >= (int)rows.size()) {
            current->shakeHighlight(dir);  // top/bottom of the list: stay put
            return current;
        }
        std::swap(rows[i], rows[t]);
        dirty = true;
        heldIndex = t;
        auto* from = dynamic_cast<CatalogCell*>(this->recycler->getGridItemByIndex(i));
        auto* to = dynamic_cast<CatalogCell*>(this->recycler->getGridItemByIndex(t));
        if (from) {
            from->apply(rows[i], false);
            from->setHeld(false);
        }
        if (to) {
            to->apply(rows[t], false);
            to->setHeld(true);
            return to;
        }
        // Neighbour cell not instantiated (recycled off-screen): rebuild with
        // focus parked on the row's new position (the old select-after-reload
        // dance), held styling reapplied by cellForRow.
        this->recycler->setDefaultCellFocus(t);
        this->recycler->reloadData();
        brls::sync([this]() { brls::Application::giveFocus(this->recycler); });
        return current;
    };

    auto pickUpOrPlace = []() {
        auto* cell = dynamic_cast<CatalogCell*>(brls::Application::getCurrentFocus());
        if (!cell) return;
        if (heldIndex < 0) {
            heldIndex = (int)cell->getIndex();
            cell->setHeld(true);
        } else {
            cell->setHeld(false);
            heldIndex = -1;
        }
    };
    this->registerAction("Pick Up / Place", brls::BUTTON_X, [pickUpOrPlace](brls::View*) {
        pickUpOrPlace();
        return true;
    });
    this->registerAction("Place", brls::BUTTON_Y, [pickUpOrPlace](brls::View*) {
        if (heldIndex < 0) return false;  // Y only acts while carrying
        pickUpOrPlace();
        return true;
    });

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        heldIndex = -1;  // B always places first — the order so far is already applied
        if (dirty) {
            stremio::saveConfig(AppConfig::instance().configDir());
            stremio::CATALOGS_CHANGED.fire();
        }
        brls::Application::popActivity();
        return true;
    });
}
