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
    ~StremioHome() override;

private:
    void addRow(const std::string& title, const std::string& url);
    void buildCatalogRows();    // one carousel per enabled registry row
    void rebuildCatalogRows();  // tear down + rebuild after the registry changed
    void onChildFocusGained(brls::View* directChild, brls::View* focusedView) override;
    void addFavouritesRow();
    void refreshFavourites();
    void addContinueRow();
    void refreshContinue();
    void enrichContinue();  // fetch English titles from Cinemeta, then rebuild once

    brls::Box* boxHome = nullptr;          // column container inside the scroll
    brls::Label* favHeader = nullptr;       // "Favourites" row title (hidden when empty)
    HRecyclerFrame* favRec = nullptr;       // favourites carousel
    brls::Label* continueHeader = nullptr;  // "Continue Watching" title (hidden when empty)
    HRecyclerFrame* continueRec = nullptr;  // continue-watching carousel
    HRecyclerFrame* firstRowRec = nullptr;  // first catalog row; safe focus parking spot
    std::vector<brls::View*> catalogViews;      // headers + carousels of the catalog rows
    brls::Event<>::Subscription catalogsSub;    // CATALOGS_CHANGED subscription (always set in ctor)
    bool pendingRebuild = false;            // registry changed while home was covered
    bool pendingFavRefresh = false;         // favourites changed while home was covered
    bool pendingContinueRefresh = false;    // continue-watching changed while home was covered
    bool continueEnriching = false;         // a Cinemeta title-refresh pass is in flight
};
