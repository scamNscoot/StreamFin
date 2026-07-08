/*
    Anime browse screen

    Full-screen grid fed by the Anime Kitsu addon: trending anime by default,
    with a search control on top that queries Kitsu's own search catalog (so
    anime stays out of the regular Cinemeta search). A opens the title,
    X adds/removes it from the Library.
*/
#pragma once

#include <borealis.hpp>
#include "api/stremio.hpp"

class RecyclingGrid;

class StremioAnime : public brls::Box {
public:
    StremioAnime();

    // Paints the shared ocean-gradient background.
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override;

private:
    void fetch(const std::string& url);  // load a kitsu catalog into the grid

    brls::Label* headline = nullptr;
    brls::Label* lblSearch = nullptr;
    brls::Box* btnSearch = nullptr;
    RecyclingGrid* recycler = nullptr;
};
