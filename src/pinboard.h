#pragma once

#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidateaction.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/instance.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace pinboard {

// A single clipboard entry. id is stable across the entry's lifetime so the
// candidate list can refer back to it after reordering.
struct Entry {
    uint64_t id = 0;
    std::string text;
    bool pinned = false;
};

class Pinboard;

// One clipboard entry rendered as a candidate in the native input panel.
class PinboardCandidate : public fcitx::CandidateWord {
public:
    PinboardCandidate(Pinboard *owner, uint64_t id, const std::string &display);
    void select(fcitx::InputContext *ic) const override;
    uint64_t entryId() const { return entryId_; }

private:
    Pinboard *owner_;
    uint64_t entryId_;
};

// Standalone clipboard candidate list: vertical, cursor-movable, with per-entry
// Pin/Unpin/Delete actions.
class PinboardCandidateList : public fcitx::CandidateList,
                           public fcitx::CursorMovableCandidateList,
                           public fcitx::ActionableCandidateList {
public:
    explicit PinboardCandidateList(Pinboard *owner);

    // CandidateList
    const fcitx::Text &label(int idx) const override;
    const fcitx::CandidateWord &candidate(int idx) const override;
    int size() const override;
    int cursorIndex() const override;
    fcitx::CandidateLayoutHint layoutHint() const override;

    // CursorMovableCandidateList
    void prevCandidate() override;
    void nextCandidate() override;

    // ActionableCandidateList
    bool hasAction(const fcitx::CandidateWord &candidate) const override;
    std::vector<fcitx::CandidateAction>
    candidateActions(const fcitx::CandidateWord &candidate) const override;
    void triggerAction(const fcitx::CandidateWord &candidate, int id) override;

    void setCursorIndex(int idx) { cursor_ = idx; }

private:
    Pinboard *owner_;
    std::vector<std::unique_ptr<PinboardCandidate>> words_;
    std::vector<fcitx::Text> labels_;
    int cursor_ = 0;
};

class Pinboard : public fcitx::AddonInstance {
public:
    explicit Pinboard(fcitx::AddonManager *manager);
    ~Pinboard() override;

    // Pinboard has no config of its own — it follows the built-in clipboard addon
    // (trigger key + entry limit). reloadConfig re-reads those.
    void reloadConfig() override;

    // Entries in display order: pinned first (insertion order), then the rest
    // most-recent-first.
    std::vector<const Entry *> displayOrder() const;
    const Entry *findEntry(uint64_t id) const;

    // Actions invoked from the candidate list.
    void pasteEntry(uint64_t id, fcitx::InputContext *ic);
    void togglePin(uint64_t id);
    void deleteEntry(uint64_t id);

private:
    void onKeyEvent(fcitx::Event &event);
    bool handleActiveKey(fcitx::KeyEvent &keyEvent);

    void showPanel(fcitx::InputContext *ic);
    void updatePanel(fcitx::InputContext *ic);
    void closePanel(fcitx::InputContext *ic);

    void startMonitor();
    void pollClipboard();
    void addText(const std::string &text);
    void enforceLimits();

    // Read trigger key and entry limit from the built-in clipboard addon.
    void loadClipboardConfig();

    void load();
    void save();
    std::string storePath() const;

    fcitx::Instance *instance_;

    // Resolved from the clipboard addon's config.
    fcitx::KeyList triggerKeys_{fcitx::Key("Control+semicolon")};
    int maxHistory_ = 5;
    int maxPinned_ = 5;

    std::deque<Entry> entries_;
    uint64_t nextId_ = 1;
    std::string lastSeen_;

    fcitx::InputContext *activeIc_ = nullptr;
    int activeCursor_ = 0;
    std::unique_ptr<fcitx::EventSourceTime> timer_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> keyHandler_;

    FCITX_ADDON_DEPENDENCY_LOADER(clipboard, instance_->addonManager());
};

} // namespace pinboard
