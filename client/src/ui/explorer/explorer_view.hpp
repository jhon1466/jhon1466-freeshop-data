#pragma once

#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <borealis.hpp>

#include "app/file_explorer_service.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class ExplorerView;

class ExplorerDataSource : public brls::RecyclerDataSource {
public:
    explicit ExplorerDataSource(ExplorerView* owner) : owner_(owner) {}
    int numberOfRows(brls::RecyclerFrame*, int) override;
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame*,
                                   brls::IndexPath) override;
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath) override;

private:
    ExplorerView* owner_;
};

class ExplorerCell : public brls::RecyclerCell {
public:
    ExplorerCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setHeight(60);
        setPadding(10, 24, 10, 24);
        name_ = new brls::Label();
        name_->setSingleLine(true);
        name_->setFontSize(20);
        name_->setGrow(1);
        addView(name_);
        size_ = new brls::Label();
        size_->setSingleLine(true);
        size_->setFontSize(15);
        size_->setTextColor(theme::textTertiary());
        addView(size_);
    }

    void setEntry(const ExplorerEntry& entry) {
        entry_ = entry;
        name_->setText(entry.directory
                           ? tr("pipensx/explorer/folder", entry.name)
                                       : entry.name);
        size_->setText(entry.directory ? "" : formatBytes(entry.size));
    }

    const ExplorerEntry& entry() const { return entry_; }

private:
    brls::Label* name_;
    brls::Label* size_;
    ExplorerEntry entry_;
};

// A file manager for the SD card: browse, copy, move, delete, rename, and
// create folders. Starts at sdmc:/ - the whole card, not just this app's own
// data directory, since that is the point of an explorer.
class ExplorerView : public brls::Box {
public:
    ExplorerView()
        : brls::Box(brls::Axis::COLUMN), currentPath_("sdmc:/"),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        setPadding(0, 34, 0, 34);

        auto* header = new brls::Box(brls::Axis::COLUMN);
        header->setPadding(24, 0, 8, 0);
        pathLabel_ = new brls::Label();
        pathLabel_->setSingleLine(true);
        pathLabel_->setFontSize(theme::kFontBody);
        pathLabel_->setTextColor(theme::textPrimary());
        header->addView(pathLabel_);
        clipboardLabel_ = new brls::Label();
        clipboardLabel_->setSingleLine(true);
        clipboardLabel_->setFontSize(theme::kFontCaption);
        clipboardLabel_->setTextColor(theme::accent());
        clipboardLabel_->setMarginTop(4);
        clipboardLabel_->setVisibility(brls::Visibility::GONE);
        header->addView(clipboardLabel_);
        addView(header);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 0, 6, 0);
        recycler_->estimatedRowHeight = 60;
        recycler_->registerCell("Explorer", [] { return new ExplorerCell(); });
        recycler_->setDataSource(new ExplorerDataSource(this));
        recyclerHost_ = recyclerHost(recycler_);
        addView(recyclerHost_);

        registerAction(tr("pipensx/explorer/options"), brls::BUTTON_X,
                       [this](brls::View*) {
            openFocusedOptions();
            return true;
        });
        registerAction(tr("pipensx/explorer/new_folder"), brls::BUTTON_Y,
                       [this](brls::View*) {
            promptNewFolder();
            return true;
        });
        registerAction(tr("pipensx/common/refresh"), brls::BUTTON_RB,
                       [this](brls::View*) {
            reload();
            return true;
        });

        reload();
    }

    ~ExplorerView() override {
        alive_->store(false);
    }

    const std::vector<ExplorerEntry>& entries() const { return entries_; }

    void select(size_t index) {
        if (busy_ || index >= entries_.size())
            return;
        const ExplorerEntry entry = entries_[index];
        if (entry.directory) {
            navigateTo(entry.path);
            return;
        }
        openOptions(entry);
    }

private:
    void navigateTo(const std::string& path) {
        currentPath_ = path;
        reload();
    }

    void reload() {
        std::string error;
        std::vector<ExplorerEntry> listed;
        if (!listDirectory(currentPath_, listed, error)) {
            brls::Application::notify(
                tr("pipensx/explorer/list_failed", error));
            return;
        }
        entries_.clear();
        if (currentPath_ != "sdmc:/") {
            ExplorerEntry up;
            up.name = "..";
            up.path = explorerParentPath(currentPath_);
            up.directory = true;
            entries_.push_back(up);
        }
        entries_.insert(entries_.end(), listed.begin(), listed.end());
        pathLabel_->setText(currentPath_);
        reloadRecycler();
    }

    void reloadRecycler() {
        brls::View* focused = brls::Application::getCurrentFocus();
        bool ownsFocus = focused && recycler_->getParentActivity() &&
                        focused->getParentActivity() ==
                            recycler_->getParentActivity();
        if (ownsFocus) {
            recycler_->setFocusable(true);
            brls::Application::giveFocus(recycler_);
        }
        recycler_->setDefaultCellFocus(brls::IndexPath(0, 0));
        recycler_->reloadData();
        if (ownsFocus) {
            recycler_->setFocusable(false);
            brls::Application::giveFocus(recycler_);
        }
    }

    void openFocusedOptions() {
        if (busy_)
            return;
        auto* cell =
            dynamic_cast<ExplorerCell*>(brls::Application::getCurrentFocus());
        if (!cell || cell->entry().name == "..")
            return;
        openOptions(cell->entry());
    }

    void openOptions(const ExplorerEntry& entry) {
        auto* dialog = new brls::Dialog(entry.name);
        dialog->addButton(tr("pipensx/explorer/copy"), [this, entry] {
            clipboard_ = {entry.path, entry.name, entry.directory,
                         ClipboardMode::Copy};
            updateClipboardLabel();
        });
        dialog->addButton(tr("pipensx/explorer/move"), [this, entry] {
            clipboard_ = {entry.path, entry.name, entry.directory,
                         ClipboardMode::Move};
            updateClipboardLabel();
        });
        dialog->addButton(tr("pipensx/explorer/rename"), [this, entry] {
            promptRename(entry);
        });
        dialog->addButton(tr("pipensx/explorer/delete"), [this, entry] {
            confirmDelete(entry);
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void updateClipboardLabel() {
        if (pasteAction_) {
            unregisterAction(*pasteAction_);
            pasteAction_.reset();
        }
        if (!clipboard_) {
            clipboardLabel_->setVisibility(brls::Visibility::GONE);
            return;
        }
        const bool moving = clipboard_->mode == ClipboardMode::Move;
        clipboardLabel_->setText(tr(moving ? "pipensx/explorer/clipboard_move"
                                            : "pipensx/explorer/clipboard_copy",
                                    clipboard_->name));
        clipboardLabel_->setVisibility(brls::Visibility::VISIBLE);
        pasteAction_ = registerAction(tr("pipensx/explorer/paste"),
                                      brls::BUTTON_LB, [this](brls::View*) {
            paste();
            return true;
        });
    }

    void paste() {
        if (busy_ || !clipboard_)
            return;
        if (explorerParentPath(clipboard_->path) == currentPath_) {
            brls::Application::notify(tr("pipensx/explorer/paste_same_place"));
            return;
        }
        // Pasting a folder into its own subtree would have copyEntry/
        // moveEntry recurse into the copy it is still writing. currentPath_
        // always ends in '/', so this only matches a real path-component
        // boundary, not e.g. "sdmc:/Foo" matching "sdmc:/FooBar/".
        if (clipboard_->directory &&
            currentPath_.rfind(clipboard_->path + "/", 0) == 0) {
            brls::Application::notify(tr("pipensx/explorer/paste_into_self"));
            return;
        }
        const ClipboardEntry job = *clipboard_;
        const std::string destination = currentPath_;
        clipboard_.reset();
        updateClipboardLabel();
        setBusy(true, tr(job.mode == ClipboardMode::Move
                             ? "pipensx/explorer/moving"
                                         : "pipensx/explorer/copying",
                        job.name));
        auto alive = alive_;
        std::thread([this, alive, job, destination] {
            std::string error;
            const bool ok = job.mode == ClipboardMode::Move
                ? moveEntry(job.path, destination, error)
                : copyEntry(job.path, destination, error);
            brls::sync([this, alive, ok, error] {
                if (!alive->load())
                    return;
                setBusy(false, "");
                if (!ok)
                    brls::Application::notify(
                        tr("pipensx/explorer/operation_failed", error));
                reload();
            });
        }).detach();
    }

    void confirmDelete(const ExplorerEntry& entry) {
        auto* dialog =
            new brls::Dialog(tr("pipensx/explorer/delete_question", entry.name));
        dialog->setCancelable(true);
        dialog->addButton(tr("pipensx/explorer/delete"), [this, entry] {
            performDelete(entry);
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void performDelete(const ExplorerEntry& entry) {
        if (busy_)
            return;
        setBusy(true, tr("pipensx/explorer/deleting", entry.name));
        auto alive = alive_;
        const std::string path = entry.path;
        std::thread([this, alive, path] {
            std::string error;
            const bool ok = deleteEntry(path, error);
            brls::sync([this, alive, ok, error] {
                if (!alive->load())
                    return;
                setBusy(false, "");
                if (!ok)
                    brls::Application::notify(
                        tr("pipensx/explorer/operation_failed", error));
                reload();
            });
        }).detach();
    }

    void promptRename(const ExplorerEntry& entry) {
        if (busy_)
            return;
        brls::Application::getImeManager()->openForText(
            [this, entry](std::string text) {
                if (text.empty() || text == entry.name)
                    return;
                std::string error;
                if (!renameEntry(entry.path, text, error))
                    brls::Application::notify(
                        tr("pipensx/explorer/operation_failed", error));
                reload();
            },
            tr("pipensx/explorer/rename"), "", 128, entry.name,
            brls::KEYBOARD_DISABLE_FORWSLASH | brls::KEYBOARD_DISABLE_BACKSLASH);
    }

    void promptNewFolder() {
        if (busy_)
            return;
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                if (text.empty())
                    return;
                std::string error;
                if (!createDirectory(currentPath_, text, error))
                    brls::Application::notify(
                        tr("pipensx/explorer/operation_failed", error));
                reload();
            },
            tr("pipensx/explorer/new_folder"), "", 128, "",
            brls::KEYBOARD_DISABLE_FORWSLASH | brls::KEYBOARD_DISABLE_BACKSLASH);
    }

    void setBusy(bool busy, const std::string& message) {
        busy_ = busy;
        recycler_->setFocusable(!busy);
        if (busy)
            pathLabel_->setText(message);
        else
            pathLabel_->setText(currentPath_);
    }

    enum class ClipboardMode { Copy, Move };
    struct ClipboardEntry {
        std::string path;
        std::string name;
        bool directory;
        ClipboardMode mode;
    };

    std::string currentPath_;
    std::vector<ExplorerEntry> entries_;
    std::optional<ClipboardEntry> clipboard_;
    std::optional<brls::ActionIdentifier> pasteAction_;
    bool busy_ = false;
    brls::Label* pathLabel_ = nullptr;
    brls::Label* clipboardLabel_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    brls::Box* recyclerHost_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_;

    friend class ExplorerDataSource;
};

}  // namespace pipensx::ui
