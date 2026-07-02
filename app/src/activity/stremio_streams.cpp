/*
    Stremio stream picker -- implementation.

    A compact, scrollable, selectable list of streams. Uses the file-list row
    layout (small static icon + text, no thumbnails to load) so it scrolls fast.
*/
#include "activity/stremio_streams.hpp"
#include "activity/stremio_resume.hpp"
#include "view/recycling_grid.hpp"
#include "view/svg_image.hpp"
#include "view/mpv_core.hpp"
#include "tab/remote_view.hpp"
#include "api/stremio.hpp"

#include <algorithm>

using namespace brls::literals;

namespace {

// Clean text row built in code: name on top, full description below.
class StreamCell : public RecyclingGridItem {
public:
    StreamCell() {
        this->setFocusable(true);
        this->setAxis(brls::Axis::COLUMN);
        this->setPadding(12, 20, 12, 20);
        this->setCornerRadius(6);

        this->name = new brls::Label();
        this->name->setFontSize(24);
        this->addView(this->name);

        this->detail = new brls::Label();
        this->detail->setFontSize(18);
        this->addView(this->detail);
    }

    brls::Label* name = nullptr;
    brls::Label* detail = nullptr;
};

class StreamSource : public RecyclingGridDataSource {
public:
    StreamSource(const ResumeEntry& key, const std::vector<stremio::Stream>& s)
        : key(key), list(std::move(s)) {}

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        StreamCell* cell = dynamic_cast<StreamCell*>(recycler->dequeueReusableCell("Cell"));
        auto& s = this->list.at(index);

        std::string name = s.name;
        std::replace(name.begin(), name.end(), '\n', ' ');
        std::string desc = !s.description.empty() ? s.description : s.title;
        std::replace(desc.begin(), desc.end(), '\n', ' ');

        cell->name->setText(name.empty() ? "Stream" : name);
        cell->detail->setText(desc);
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto s = this->list.at(index);
        if (s.url.empty()) {
            brls::Application::notify("This stream has no URL");
            return;
        }
        // Remember what we're playing so the tracker can resume/record by id.
        ResumeTracker::instance().setCurrent(this->key);
        // Prefer English audio tracks for this (and subsequent) playback.
        MPVCore::instance().command("set", "alang", "eng,en");
        // Show the media title (movie name / "Series · S1E5 · Episode") on the
        // player OSD, not the raw stream/source name.
        std::string title = this->key.name.empty() ? s.name : this->key.name;
        RemoteView::play(s.url, title);
    }

    void clearData() override { this->list.clear(); }

private:
    ResumeEntry key;
    std::vector<stremio::Stream> list;
};

}  // namespace

StreamPicker::StreamPicker(
    const std::string& title, const std::vector<stremio::Stream>& streams, const ResumeEntry& resumeKey) {
    brls::Logger::debug("StreamPicker: {} streams", streams.size());
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);

    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 1;
    this->recycler->isFlowMode = true;
    this->recycler->estimatedRowSpace = 10;
    this->recycler->registerCell("Cell", []() -> RecyclingGridItem* { return new StreamCell(); });
    this->addView(this->recycler);

    this->recycler->setDataSource(new StreamSource(resumeKey, streams));
    // Defer focus until after the activity is on screen.
    brls::sync([this]() { brls::Application::giveFocus(this->recycler); });

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });
}
