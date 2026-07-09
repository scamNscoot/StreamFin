/*
    Stremio browse screen

    A Stremio-style landing screen: a Favourites row (top) plus a vertical scroll
    of horizontal poster carousels filled from Cinemeta catalogs. Selecting a
    poster fetches streams from AIOStreams; X toggles favourite.
*/
#pragma once

#include <borealis.hpp>

class RecyclingGrid;
class HRecyclerFrame;

class StremioHome : public brls::Box {
public:
    StremioHome();

    // Paints the shared ocean-gradient background behind the rows.
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override;

private:
    void addRow(const std::string& title, const std::string& url);
    void addAnimeRow();  // Kitsu trending + More card (anime browse/search)
    void buildRows();    // add catalog rows per the saved row config
    void rebuildRows();  // re-apply row config after the editor closes
    void addTopBar();       // stremio logo + search button (scrolls with content)
    void addFavouritesRow();
    void refreshFavourites();
    void addContinueRow();
    void refreshContinue();
    // Move focus onto a row that can actually take it (not `avoid`); a row
    // still showing skeletons refuses focus, so try several. Returns success.
    bool parkFocus(brls::View* avoid);
    void enrichContinue();  // fetch English titles from Cinemeta, then rebuild once

    brls::ScrollingFrame* homeScroll = nullptr;  // vertical scroll (reset on rebuild)
    brls::Box* boxHome = nullptr;          // column container inside the scroll
    brls::Label* favHeader = nullptr;       // "Favourites" row title (hidden when empty)
    HRecyclerFrame* favRec = nullptr;       // favourites carousel
    brls::Label* continueHeader = nullptr;  // "Continue Watching" title (hidden when empty)
    HRecyclerFrame* continueRec = nullptr;  // continue-watching carousel
    HRecyclerFrame* firstRowRec = nullptr;  // first catalog row; safe focus parking spot
    bool continueEnriching = false;
    bool rowsSubscribed = false;            // continue/library events hooked once         // a Cinemeta title-refresh pass is in flight
};
