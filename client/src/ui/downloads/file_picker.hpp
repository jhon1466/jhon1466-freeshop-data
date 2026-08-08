#pragma once

#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/download_manager.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/debrid_ui.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

struct FileEntry {
    std::string name;
    std::string path;
    bool directory = false;
};

class FilePickerActivity;

class FileDataSource : public brls::RecyclerDataSource {
public:
    explicit FileDataSource(FilePickerActivity* owner) : owner_(owner) {}
    int numberOfRows(brls::RecyclerFrame*, int) override;
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame*,
                                    brls::IndexPath) override;
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath) override;

private:
    FilePickerActivity* owner_;
};

class FileCell : public brls::RecyclerCell {
public:
    FileCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setHeight(64);
        setPadding(12, 24, 12, 24);
        label_ = new brls::Label();
        label_->setSingleLine(true);
        label_->setFontSize(21);
        label_->setGrow(1);
        label_->setTextColor(theme::textPrimary());
        addView(label_);
    }
    void setEntry(const FileEntry& entry) {
        label_->setText(entry.directory
                            ? tr("pipensx/picker/folder", entry.name)
                                        : entry.name);
    }
private:
    brls::Label* label_;
};

class FilePickerActivity : public brls::Activity {
public:
    FilePickerActivity(DownloadManager* manager, AppSettings* settings)
        : manager_(manager), settings_(settings), currentPath_("sdmc:/") {
        recycler_ = new brls::RecyclerFrame();
        // Every other recyclerHost() screen (Explorer, torrent selection, ...)
        // sets grow=1 on the recycler itself so it fills its host box instead
        // of collapsing to its minimum content height — missing here, which
        // left room for roughly one row before the frame ran out of space.
        recycler_->setGrow(1);
        recycler_->setPadding(8, 32, 8, 32);
        recycler_->estimatedRowHeight = 64;
        recycler_->registerCell("File", [] { return new FileCell(); });
        recycler_->setDataSource(new FileDataSource(this));
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setGrow(1);
        content->addView(recyclerHost(recycler_));
        frame_ = new brls::AppletFrame(content);
        loadDirectory(currentPath_);
    }

    brls::View* createContentView() override {
        return frame_;
    }

    const std::vector<FileEntry>& entries() const { return entries_; }

    void select(size_t index) {
        if (index >= entries_.size())
            return;
        const FileEntry entry = entries_[index];
        if (entry.directory) {
            brls::sync([this, path = entry.path] {
                loadDirectory(path);
            });
            return;
        }

        pipensx::TorrentPreview preview;
        std::string error;
        if (!DownloadManager::previewTorrent(entry.path, preview, error)) {
            brls::Application::notify(error);
            return;
        }
        std::string text = tr("pipensx/picker/preview", preview.name,
                              formatBytes(preview.totalBytes),
                              preview.fileCount, preview.trackerCount);
        if (preview.packageCount)
            text += tr("pipensx/picker/preview_packages",
                       preview.packageCount);
        auto* dialog = new brls::Dialog(text);
        auto add = [this, path = entry.path, preview](TransferMode mode) {
            std::string id;
            std::string error;
            bool imported = false;
            if (debridModeActive(settings_)) {
                if (!ensureDebridLinked(settings_, manager_))
                    return;
                DebridImport import;
                import.infoHash = preview.infoHash;
                import.name = preview.name;
                import.totalBytes = preview.totalBytes;
                import.provider = settings_->get().debridProvider;
                import.torrentPath = path;
                import.mode = mode;
                import.packageCount = mode == TransferMode::StreamInstall
                    ? preview.packageCount : 0;
                imported = manager_->importDebrid(import, id, error);
            } else {
                imported = manager_->importTorrent(path, mode, id, error);
            }
            if (imported) {
                brls::Application::notify(tr("pipensx/picker/added"));
                brls::Application::popActivity();
            } else {
                brls::Application::notify(error);
            }
        };
        if (preview.packageCount) {
            dialog->addButton(tr("pipensx/picker/stream_install"),
                [add] { add(TransferMode::StreamInstall); });
            dialog->addButton(tr("pipensx/picker/download_only"),
                [add] { add(TransferMode::DownloadOnly); });
        } else {
            dialog->addButton(tr("pipensx/picker/add_to_queue"),
                [add] { add(TransferMode::DownloadOnly); });
        }
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

private:
    static bool hasTorrentExtension(const std::string& name) {
        if (name.size() < 8)
            return false;
        std::string extension = name.substr(name.size() - 8);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return extension == ".torrent";
    }

    static std::string parentPath(const std::string& path) {
        if (path == "sdmc:/" || path == "/")
            return path;
        std::string trimmed = path;
        while (trimmed.size() > 1 && trimmed.back() == '/')
            trimmed.pop_back();
        size_t slash = trimmed.find_last_of('/');
        if (slash == std::string::npos)
            return "sdmc:/";
        return trimmed.substr(0, slash + 1);
    }

    void loadDirectory(const std::string& path) {
        std::vector<FileEntry> directories;
        std::vector<FileEntry> files;
        DIR* dir = opendir(path.c_str());
        if (!dir) {
            log_msg("[picker] opendir failed '%s': %s\n", path.c_str(),
                    strerror(errno));
            brls::Application::notify(tr("pipensx/picker/unable_to_open"));
            return;
        }
        int rawEntries = 0;
        int statFailures = 0;
        while (dirent* item = readdir(dir)) {
            if (std::strcmp(item->d_name, ".") == 0 ||
                std::strcmp(item->d_name, "..") == 0)
                continue;
            ++rawEntries;
            std::string child = path;
            if (!child.empty() && child.back() != '/')
                child += '/';
            child += item->d_name;
            struct stat st {};
            if (stat(child.c_str(), &st) != 0) {
                ++statFailures;
                log_msg("[picker] stat failed '%s': %s\n", child.c_str(),
                        strerror(errno));
                continue;
            }
            if (S_ISDIR(st.st_mode))
                directories.push_back({item->d_name, child, true});
            else if (hasTorrentExtension(item->d_name))
                files.push_back({item->d_name, child, false});
        }
        closedir(dir);
        log_msg("[picker] loaded '%s': raw=%d dirs=%zu torrents=%zu "
                "stat_failed=%d\n", path.c_str(), rawEntries,
                directories.size(), files.size(), statFailures);
        auto byName = [](const FileEntry& a, const FileEntry& b) {
            return a.name < b.name;
        };
        std::sort(directories.begin(), directories.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        entries_.clear();
        if (path != "sdmc:/" && path != "/")
            entries_.push_back({"..", parentPath(path), true});
        entries_.insert(entries_.end(), directories.begin(), directories.end());
        entries_.insert(entries_.end(), files.begin(), files.end());
        currentPath_ = path;
        frame_->setTitle(tr("pipensx/picker/frame_title", currentPath_));
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

    DownloadManager* manager_;
    AppSettings* settings_;
    std::string currentPath_;
    std::vector<FileEntry> entries_;
    brls::RecyclerFrame* recycler_;
    brls::AppletFrame* frame_;

    friend class FileDataSource;
};

}  // namespace pipensx::ui
