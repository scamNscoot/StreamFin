/*
    Stremio favourites

    A small persistent list of favourited movies/series (saved to
    configDir/favourites.json), plus FavCardCell — a poster card whose X button
    toggles favourite status.
*/
#pragma once

#include <borealis.hpp>
#include "view/video_card.hpp"
#include "api/stremio.hpp"

// Poster card with X = toggle favourite. The closure is (re)set in cellForRow.
class FavCardCell : public VideoCardCell {
public:
    FavCardCell() {
        this->registerAction("Favourite", brls::BUTTON_X, [this](brls::View*) {
            if (this->onToggleFav) this->onToggleFav();
            return true;
        });
    }

    std::function<void()> onToggleFav;

    static RecyclingGridItem* create() { return new FavCardCell(); }
};

// Persistent favourites (movies + series). Survives restarts.
class Favourites {
public:
    static Favourites& instance();

    void load();                                  // read from disk once
    bool contains(const std::string& id) const;   // is this id favourited?
    bool toggle(const stremio::Meta& m);          // add/remove; returns true if now favourited
    const std::vector<stremio::Meta>& all() const { return items; }
    brls::VoidEvent* changed() { return &changedEvent; }  // fires on any add/remove

private:
    void save();
    bool loaded = false;
    std::vector<stremio::Meta> items;
    brls::VoidEvent changedEvent;
};
