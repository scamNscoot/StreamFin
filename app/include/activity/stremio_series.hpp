/*
    Stremio series screens

    StremioSeries: fetches a series' metadata and lists its SEASONS.
    StremioSeason: lists the EPISODES of one season; selecting an episode
                   fetches its streams and opens the picker.
*/
#pragma once

#include <borealis.hpp>
#include "api/stremio.hpp"

class RecyclingGrid;

class StremioSeries : public brls::Box {
public:
    // Fetches the series meta from Cinemeta, then lists its seasons.
    explicit StremioSeries(const stremio::Meta& series);
    // Uses already-fetched episodes (e.g. from the detail screen) — no refetch.
    StremioSeries(const std::string& name, const std::vector<stremio::Video>& videos);

private:
    void init();
    void setSeasons(const std::string& name, const std::vector<stremio::Video>& videos);

    RecyclingGrid* recycler = nullptr;
};

class StremioSeason : public brls::Box {
public:
    StremioSeason(const std::string& seriesName, const std::string& title,
        const std::vector<stremio::Video>& episodes);

private:
    RecyclingGrid* recycler = nullptr;
};
