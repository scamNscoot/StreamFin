/*
    Home row customization

    A registry of every available catalog row (specials + every Cinemeta genre
    for movies & series + Anime), a persisted enable/order config
    (configDir/rows.json), and the editor screen opened from the home top bar:
    A toggles a row on/off, Y moves it up, X moves it down, B saves & applies.
*/
#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>

class RecyclingGrid;

namespace rowcfg {

struct RowDef {
    std::string key;    // stable id, e.g. "movie-Action" / "popular-movies" / "anime"
    std::string title;  // header shown on home
    std::string url;    // catalog URL (empty for the special "anime" row)
};

// Every row the app knows how to show, in default order.
const std::vector<RowDef>& registry();

// Ordered {key, enabled} list: saved config merged with the registry (new
// rows appear disabled at the end; unknown keys are dropped).
std::vector<std::pair<std::string, bool>> load();
void save(const std::vector<std::pair<std::string, bool>>& rows);

const RowDef* find(const std::string& key);

}  // namespace rowcfg

// The editor screen. onApply runs after saving (home triggers its rebuild).
class StremioRows : public brls::Box {
public:
    explicit StremioRows(std::function<void()> onApply);

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override;

private:
    void reload(size_t focusIndex);

    brls::Label* head = nullptr;
    std::vector<std::pair<std::string, bool>> rows;
    bool dirty = false;
    std::function<void()> onApply;
    RecyclingGrid* recycler = nullptr;
};
