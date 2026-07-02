/*
    Stremio title detail screen

    Shown when a movie OR series poster is selected: poster + title,
    year/runtime/rating, genres, description, cast and director (all from
    Cinemeta's /meta endpoint). The action button is "Watch" for movies
    (fetches streams, opens the picker) and "Episodes" for series (opens the
    season list, reusing the already-fetched episodes — no refetch).
    X toggles favourite, B goes back.
*/
#pragma once

#include <borealis.hpp>
#include "api/stremio.hpp"

class StremioDetail : public brls::Box {
public:
    explicit StremioDetail(const stremio::Meta& item);
    ~StremioDetail() override;

private:
    void applyMeta(const stremio::MetaDetail& meta);
    void onAction();      // Watch (movie) / Episodes (series)
    void openStreams();

    stremio::Meta item;   // catalog entry we were opened with
    std::vector<stremio::Video> videos;  // series episodes from the meta fetch
    bool fetching = false;  // a stream request is in flight (debounce Watch)

    brls::Image* poster = nullptr;
    brls::Label* labelTitle = nullptr;
    brls::Label* labelMeta = nullptr;      // year · runtime · ★ rating
    brls::Label* labelGenres = nullptr;
    brls::Label* labelDesc = nullptr;
    brls::Label* labelCastHead = nullptr;
    brls::Label* labelCast = nullptr;
    brls::Label* labelDirector = nullptr;
    brls::Box* btnWatch = nullptr;
};
