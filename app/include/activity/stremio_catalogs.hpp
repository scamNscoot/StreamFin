/*
    Catalogs screen (opened with + from home)

    Every home carousel — built-in Cinemeta rows and catalogs from the user's
    catalog addons — listed in home-screen order. A toggles a row on/off,
    X/Y move it up/down. Changes are saved on exit and the home screen
    rebuilds itself.
*/
#pragma once

#include <borealis.hpp>

class RecyclingGrid;

class StremioCatalogs : public brls::Box {
public:
    StremioCatalogs();

private:
    RecyclingGrid* recycler = nullptr;
};
