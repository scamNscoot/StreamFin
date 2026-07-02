/*
    Stremio search screen

    Searches Cinemeta for movies and series matching a query and shows the
    results in the poster grid. Selecting a result behaves like the home grid
    (movie -> stream picker, series -> season list).
*/
#pragma once

#include <borealis.hpp>
#include "api/stremio.hpp"

class RecyclingGrid;

class StremioSearch : public brls::Box {
public:
    explicit StremioSearch(const std::string& query);

    // Appends a batch of results to the grid (called from each search request).
    void addResults(const std::vector<stremio::Meta>& metas);

private:
    void fetchInto(const std::string& url);

    RecyclingGrid* recycler = nullptr;
};
