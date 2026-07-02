/*
    Stremio favourites -- implementation.
*/
#include "activity/stremio_favourites.hpp"
#include "utils/config.hpp"

#include <fstream>
#include <sys/stat.h>

Favourites& Favourites::instance() {
    static Favourites inst;
    return inst;
}

void Favourites::load() {
    if (this->loaded) return;
    this->loaded = true;
    try {
        std::ifstream in(AppConfig::instance().configDir() + "/favourites.json");
        if (in.is_open()) {
            nlohmann::json j;
            in >> j;
            this->items = j.get<std::vector<stremio::Meta>>();
        }
    } catch (const std::exception& e) {
        brls::Logger::warning("Favourites load: {}", e.what());
        this->items.clear();
    }
}

void Favourites::save() {
    std::string dir = AppConfig::instance().configDir();
    ::mkdir(dir.c_str(), 0777);
    try {
        std::ofstream out(dir + "/favourites.json");
        if (out.is_open()) {
            nlohmann::json j = this->items;
            out << j.dump(2);
        }
    } catch (const std::exception& e) {
        brls::Logger::warning("Favourites save: {}", e.what());
    }
}

bool Favourites::contains(const std::string& id) const {
    for (auto& m : this->items)
        if (m.id == id) return true;
    return false;
}

bool Favourites::toggle(const stremio::Meta& m) {
    for (auto it = this->items.begin(); it != this->items.end(); ++it) {
        if (it->id == m.id) {
            this->items.erase(it);
            this->save();
            this->changedEvent.fire();
            return false;
        }
    }
    this->items.push_back(m);
    this->save();
    this->changedEvent.fire();
    return true;
}
