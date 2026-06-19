#include "pinboard.h"

#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx/addonfactory.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>

#include <clipboard_public.h>

#include <algorithm>
#include <fstream>

namespace pinboard {

// Mirror of the bits of the clipboard addon's config we follow. Option names
// must match clipboard's (see its clipboard.conf: "TriggerKey", "Number of
// entries"). Defaults mirror the clipboard addon's own defaults.
FCITX_CONFIGURATION(
    ClipboardMirrorConfig,
    fcitx::Option<fcitx::KeyList> triggerKey{
        this, "TriggerKey", "TriggerKey",
        fcitx::KeyList{fcitx::Key("Control+semicolon")}};
    fcitx::Option<int> numberOfEntries{this, "Number of entries",
                                       "Number of entries", 5};)

namespace {
constexpr uint64_t kPollIntervalUsec = 800000; // 0.8s

// Reverse of the save-side escaping: "\\" -> '\', "\n" -> newline.
std::string unescape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            char next = in[++i];
            out.push_back(next == 'n' ? '\n' : next);
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// Single-line, length-capped display string for an entry.
std::string makeDisplay(const std::string &text) {
    std::string s = text;
    std::replace(s.begin(), s.end(), '\n', ' ');
    std::replace(s.begin(), s.end(), '\t', ' ');
    constexpr size_t kMax = 60;
    if (s.size() > kMax) {
        s = s.substr(0, kMax) + "…";
    }
    return s;
}
} // namespace

// ──────────────── PinboardCandidate ────────────────

PinboardCandidate::PinboardCandidate(Pinboard *owner, uint64_t id,
                               const std::string &display)
    : fcitx::CandidateWord(fcitx::Text(display)), owner_(owner), entryId_(id) {}

void PinboardCandidate::select(fcitx::InputContext *ic) const {
    owner_->pasteEntry(entryId_, ic);
}

// ──────────────── PinboardCandidateList ────────────────

PinboardCandidateList::PinboardCandidateList(Pinboard *owner) : owner_(owner) {
    int n = 1;
    for (const Entry *e : owner_->displayOrder()) {
        std::string prefix = e->pinned ? "📌 " : "";
        words_.push_back(std::make_unique<PinboardCandidate>(
            owner_, e->id, prefix + makeDisplay(e->text)));
        labels_.emplace_back(std::to_string(n % 10) + ". ");
        ++n;
    }
}

const fcitx::Text &PinboardCandidateList::label(int idx) const {
    return labels_[idx];
}

const fcitx::CandidateWord &PinboardCandidateList::candidate(int idx) const {
    return *words_[idx];
}

int PinboardCandidateList::size() const { return static_cast<int>(words_.size()); }

int PinboardCandidateList::cursorIndex() const { return cursor_; }

fcitx::CandidateLayoutHint PinboardCandidateList::layoutHint() const {
    return fcitx::CandidateLayoutHint::Vertical;
}

void PinboardCandidateList::prevCandidate() {
    if (words_.empty()) {
        return;
    }
    cursor_ = (cursor_ - 1 + size()) % size();
}

void PinboardCandidateList::nextCandidate() {
    if (words_.empty()) {
        return;
    }
    cursor_ = (cursor_ + 1) % size();
}

bool PinboardCandidateList::hasAction(const fcitx::CandidateWord &) const {
    return true;
}

std::vector<fcitx::CandidateAction>
PinboardCandidateList::candidateActions(const fcitx::CandidateWord &candidate) const {
    const auto &c = static_cast<const PinboardCandidate &>(candidate);
    const Entry *e = owner_->findEntry(c.entryId());

    std::vector<fcitx::CandidateAction> actions;
    fcitx::CandidateAction pin;
    pin.setId(0);
    pin.setText(e && e->pinned ? _("Unpin") : _("Pin"));
    actions.push_back(std::move(pin));

    fcitx::CandidateAction del;
    del.setId(1);
    del.setText(_("Delete"));
    actions.push_back(std::move(del));
    return actions;
}

void PinboardCandidateList::triggerAction(const fcitx::CandidateWord &candidate,
                                       int id) {
    const auto &c = static_cast<const PinboardCandidate &>(candidate);
    if (id == 0) {
        owner_->togglePin(c.entryId());
    } else if (id == 1) {
        owner_->deleteEntry(c.entryId());
    }
}

// ──────────────── Pinboard ────────────────

Pinboard::Pinboard(fcitx::AddonManager *manager)
    : instance_(manager->instance()) {
    loadClipboardConfig();
    load();

    // PreInputMethod so we consume the trigger key before the built-in clipboard
    // addon's PostInputMethod handler sees it — otherwise both would pop up.
    keyHandler_ = instance_->watchEvent(
        fcitx::EventType::InputContextKeyEvent,
        fcitx::EventWatcherPhase::PreInputMethod,
        [this](fcitx::Event &event) { onKeyEvent(event); });

    startMonitor();
}

Pinboard::~Pinboard() = default;

void Pinboard::reloadConfig() { loadClipboardConfig(); }

void Pinboard::loadClipboardConfig() {
    auto *cb = clipboard();
    if (!cb) {
        return;
    }
    const fcitx::Configuration *cfg = cb->getConfig();
    if (!cfg) {
        return;
    }
    fcitx::RawConfig raw;
    cfg->save(raw);
    ClipboardMirrorConfig mirror;
    mirror.load(raw, true);
    triggerKeys_ = *mirror.triggerKey;
    int n = *mirror.numberOfEntries;
    if (n < 1) {
        n = 1;
    }
    maxHistory_ = n;
    maxPinned_ = n;
    enforceLimits();
}

// ──────────────── store ────────────────

std::string Pinboard::storePath() const {
    auto dir = fcitx::StandardPaths::global().userDirectory(
        fcitx::StandardPathsType::PkgData);
    return (dir / "pinboard-history.txt").string();
}

void Pinboard::load() {
    std::ifstream in(storePath());
    if (!in) {
        return;
    }
    // Each line: "P<tab>text" for pinned, "H<tab>text" for history.
    // Newlines in text are stored escaped as "\n".
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2 || line[1] != '\t') {
            continue;
        }
        bool pinned = line[0] == 'P';
        entries_.push_back(Entry{nextId_++, unescape(line.substr(2)), pinned});
    }
}

void Pinboard::save() {
    std::ofstream out(storePath(), std::ios::trunc);
    if (!out) {
        return;
    }
    for (const auto &e : entries_) {
        // Escape backslash first, then newline, so the record stays single-line
        // and round-trips on load.
        std::string text = fcitx::stringutils::replaceAll(e.text, "\\", "\\\\");
        text = fcitx::stringutils::replaceAll(text, "\n", "\\n");
        out << (e.pinned ? 'P' : 'H') << '\t' << text << '\n';
    }
}

// ──────────────── monitoring ────────────────

void Pinboard::startMonitor() {
    timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kPollIntervalUsec, 0,
        [this](fcitx::EventSourceTime *source, uint64_t) {
            pollClipboard();
            source->setTime(fcitx::now(CLOCK_MONOTONIC) + kPollIntervalUsec);
            source->setOneShot();
            return true;
        });
}

void Pinboard::pollClipboard() {
    auto *clip = clipboard();
    if (!clip) {
        return;
    }
    // The clipboard module tracks the selection regardless of focus, but its
    // accessor needs some input context for the seat/display. Fall back to a
    // dummy context so monitoring works even before any app is focused.
    auto *ic = instance_->lastFocusedInputContext();
    if (!ic) {
        ic = instance_->mostRecentInputContext();
    }
    if (!ic) {
        ic = instance_->inputContextManager().dummyInputContext();
    }
    if (!ic) {
        return;
    }
    std::string text = clip->call<fcitx::IClipboard::clipboard>(ic);
    // Trim trailing whitespace (CLI copies often carry a trailing newline) so
    // dedup is stable and pastes don't inject stray newlines.
    auto end = text.find_last_not_of(" \t\r\n");
    if (end != std::string::npos) {
        text.erase(end + 1);
    } else {
        text.clear();
    }
    if (text.empty() || text == lastSeen_) {
        return;
    }
    lastSeen_ = text;
    addText(text);
}

void Pinboard::addText(const std::string &text) {
    // Dedup: if it already exists, move to front of the history region (keep
    // pinned state).
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const Entry &e) { return e.text == text; });
    if (it != entries_.end()) {
        Entry existing = *it;
        entries_.erase(it);
        existing.id = nextId_++;
        entries_.push_front(std::move(existing));
    } else {
        entries_.push_front(Entry{nextId_++, text, false});
    }
    enforceLimits();
    save();
}

void Pinboard::enforceLimits() {
    // Trim history (unpinned) past maxHistory, and pinned past maxPinned (FIFO).
    int historyCount = 0;
    int pinnedCount = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->pinned) {
            if (++pinnedCount > maxPinned_) {
                it = entries_.erase(it);
                continue;
            }
        } else {
            if (++historyCount > maxHistory_) {
                it = entries_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

// ──────────────── entry access ────────────────

std::vector<const Entry *> Pinboard::displayOrder() const {
    std::vector<const Entry *> pinned;
    std::vector<const Entry *> history;
    for (const auto &e : entries_) {
        (e.pinned ? pinned : history).push_back(&e);
    }
    pinned.insert(pinned.end(), history.begin(), history.end());
    return pinned;
}

const Entry *Pinboard::findEntry(uint64_t id) const {
    for (const auto &e : entries_) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

void Pinboard::pasteEntry(uint64_t id, fcitx::InputContext *ic) {
    const Entry *e = findEntry(id);
    if (!e) {
        return;
    }
    std::string text = e->text;
    closePanel(ic);
    ic->commitString(text);
}

void Pinboard::togglePin(uint64_t id) {
    for (auto &e : entries_) {
        if (e.id == id) {
            e.pinned = !e.pinned;
            break;
        }
    }
    enforceLimits();
    save();
    if (activeIc_) {
        updatePanel(activeIc_);
    }
}

void Pinboard::deleteEntry(uint64_t id) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry &e) { return e.id == id; }),
                   entries_.end());
    save();
    if (activeIc_) {
        updatePanel(activeIc_);
    }
}

// ──────────────── panel ────────────────

void Pinboard::showPanel(fcitx::InputContext *ic) {
    activeIc_ = ic;
    activeCursor_ = 0;
    updatePanel(ic);
}

void Pinboard::updatePanel(fcitx::InputContext *ic) {
    auto list = std::make_unique<PinboardCandidateList>(this);
    if (list->size() == 0) {
        closePanel(ic);
        return;
    }
    activeCursor_ = std::clamp(activeCursor_, 0, list->size() - 1);
    list->setCursorIndex(activeCursor_);

    auto &panel = ic->inputPanel();
    panel.reset();
    panel.setCandidateList(std::move(list));
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void Pinboard::closePanel(fcitx::InputContext *ic) {
    activeIc_ = nullptr;
    ic->inputPanel().reset();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

// ──────────────── keys ────────────────

void Pinboard::onKeyEvent(fcitx::Event &event) {
    auto &keyEvent = static_cast<fcitx::KeyEvent &>(event);
    if (keyEvent.isRelease()) {
        return;
    }
    auto *ic = keyEvent.inputContext();

    if (activeIc_ == ic) {
        if (handleActiveKey(keyEvent)) {
            keyEvent.filterAndAccept();
        }
        return;
    }

    if (keyEvent.key().checkKeyList(triggerKeys_)) {
        keyEvent.filterAndAccept();
        showPanel(ic);
    }
}

bool Pinboard::handleActiveKey(fcitx::KeyEvent &keyEvent) {
    auto *ic = keyEvent.inputContext();
    const fcitx::Key key = keyEvent.key();

    // The input-method engine resets the input panel on every key before this
    // PostInputMethod handler runs, so we cannot read the live candidate list.
    // Drive everything from our own state (entries_ + activeCursor_) and rebuild
    // the panel on each change.
    auto order = displayOrder();
    int n = static_cast<int>(order.size());
    if (n == 0) {
        closePanel(ic);
        return true;
    }
    activeCursor_ = std::clamp(activeCursor_, 0, n - 1);

    // Close: trigger key again or Escape.
    if (key.checkKeyList(triggerKeys_) || key.check(FcitxKey_Escape)) {
        closePanel(ic);
        return true;
    }

    // Cursor movement.
    if (key.check(FcitxKey_Down) || key.check(FcitxKey_j)) {
        activeCursor_ = (activeCursor_ + 1) % n;
        updatePanel(ic);
        return true;
    }
    if (key.check(FcitxKey_Up) || key.check(FcitxKey_k)) {
        activeCursor_ = (activeCursor_ - 1 + n) % n;
        updatePanel(ic);
        return true;
    }

    // Number keys 1-9 select directly.
    if (key.states() == fcitx::KeyStates() && key.sym() >= FcitxKey_1 &&
        key.sym() <= FcitxKey_9) {
        int idx = key.sym() - FcitxKey_1;
        if (idx < n) {
            pasteEntry(order[idx]->id, ic);
        }
        return true;
    }

    // Enter / space select the cursor entry.
    if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter) ||
        key.check(FcitxKey_space)) {
        pasteEntry(order[activeCursor_]->id, ic);
        return true;
    }

    // Pin / unpin the cursor entry.
    if (key.check(FcitxKey_p)) {
        togglePin(order[activeCursor_]->id);
        return true;
    }

    // Delete the cursor entry.
    if (key.check(FcitxKey_d) || key.check(FcitxKey_Delete)) {
        deleteEntry(order[activeCursor_]->id);
        return true;
    }

    // Any other key dismisses the panel and is consumed.
    closePanel(ic);
    return true;
}

} // namespace pinboard

namespace {
class PinboardFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new pinboard::Pinboard(manager);
    }
};
} // namespace

FCITX_ADDON_FACTORY_V2(pinboard, PinboardFactory)
