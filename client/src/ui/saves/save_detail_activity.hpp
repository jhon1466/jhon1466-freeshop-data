#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <borealis.hpp>

#include "app/installed_title_service.hpp"
#include "app/save_data_service.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class SaveDetailActivity;

class SaveBackupDataSource : public brls::RecyclerDataSource {
public:
    explicit SaveBackupDataSource(SaveDetailActivity* owner) : owner_(owner) {}
    int numberOfRows(brls::RecyclerFrame*, int) override;
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame*,
                                   brls::IndexPath) override;
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath) override;

private:
    SaveDetailActivity* owner_;
};

class SaveBackupCell : public brls::RecyclerCell {
public:
    SaveBackupCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setHeight(56);
        setPadding(10, 24, 10, 24);
        label_ = new brls::Label();
        label_->setSingleLine(true);
        label_->setFontSize(19);
        label_->setGrow(1);
        addView(label_);
        size_ = new brls::Label();
        size_->setSingleLine(true);
        size_->setFontSize(15);
        size_->setTextColor(theme::textTertiary());
        addView(size_);
    }

    void setBackup(const SaveBackupInfo& backup) {
        backup_ = backup;
        label_->setText(backup.label);
        size_->setText(formatBytes(backup.totalBytes));
    }

    const SaveBackupInfo& backup() const { return backup_; }

private:
    brls::Label* label_;
    brls::Label* size_;
    SaveBackupInfo backup_;
};

// One title's backups: make a new one, or restore/delete an existing one.
// Pushed from SavesView, not a top-level tab.
class SaveDetailActivity : public brls::Activity {
public:
    SaveDetailActivity(InstalledTitle title)
        : title_(std::move(title)), alive_(std::make_shared<std::atomic<bool>>(true)) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 34, 8, 34);

        statusLabel_ = new brls::Label();
        statusLabel_->setFontSize(theme::kFontSmall);
        statusLabel_->setTextColor(theme::textSecondary());
        statusLabel_->setMarginBottom(12);
        content->addView(statusLabel_);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 0, 6, 0);
        recycler_->estimatedRowHeight = 56;
        recycler_->registerCell("SaveBackup", [] { return new SaveBackupCell(); });
        recycler_->setDataSource(new SaveBackupDataSource(this));
        content->addView(recyclerHost(recycler_));

        frame_ = new brls::AppletFrame(content);
        frame_->setTitle(title_.name);

        reload();
    }

    ~SaveDetailActivity() override {
        alive_->store(false);
    }

    brls::View* createContentView() override { return frame_; }

    // registerAction needs the content view already attached - the frame
    // built in the constructor isn't part of the tree yet at that point
    // (see TorrentSelectionActivity for the same pattern).
    void onContentAvailable() override {
        registerAction(tr("pipensx/saves/backup_now"), brls::ControllerButton::BUTTON_Y,
                       [this](brls::View*) {
            selectUserProfile();
            return true;
        });
    }

    void selectUserProfile() {
        std::string error;
        std::vector<UserProfile> users;
        if (!listUserProfiles(users, error)) {
            brls::Application::notify(error);
            return;
        }
        if (users.empty()) {
            brls::Application::notify(tr("pipensx/saves/no_profiles"));
            return;
        }
        
        auto* dialog = new brls::Dialog(tr("pipensx/saves/select_profile"));
        for (const auto& user : users) {
            std::string label = user.name.empty() ? tr("pipensx/saves/unknown_profile") : user.name;
            dialog->addButton(label, [this, uid = user.uid] {
                selectedUid_ = uid;
                makeBackup();
            });
        }
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    const std::vector<SaveBackupInfo>& backups() const { return backups_; }

    void select(size_t index) {
        if (busy_ || index >= backups_.size())
            return;
        const SaveBackupInfo backup = backups_[index];
        auto* dialog = new brls::Dialog(backup.label);
        dialog->addButton(tr("pipensx/saves/restore"), [this, backup] {
            confirmRestore(backup);
        });
        dialog->addButton(tr("pipensx/explorer/delete"), [this, backup] {
            performDelete(backup);
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

private:
    void reload() {
        std::string error;
        listSaveBackups(title_.titleId, title_.name, backups_, error);
        
        std::string statusText = backups_.empty()
            ? tr("pipensx/saves/no_backups")
            : tr("pipensx/saves/backup_count", backups_.size());
        
        // Show selected profile
        if (!selectedUid_.isZero()) {
            std::vector<UserProfile> users;
            std::string error;
            listUserProfiles(users, error);
            for (const auto& user : users) {
                if (user.uid == selectedUid_) {
                    std::string userName = user.name.empty() ? tr("pipensx/saves/unknown_profile") : user.name;
                    statusText += tr("pipensx/saves/user_suffix", userName);
                    break;
                }
            }
        }
        
        statusLabel_->setText(statusText);
        recycler_->reloadData();
    }

    void setBusy(bool busy, const std::string& message) {
        busy_ = busy;
        recycler_->setFocusable(!busy);
if (busy)
            statusLabel_->setText(message);
    }

    void makeBackup() {
        if (busy_)
            return;
        setBusy(true, tr("pipensx/saves/backing_up"));
        auto alive = alive_;
        const uint64_t applicationId = title_.applicationId;
        const std::string titleId = title_.titleId;
        const std::string gameName = title_.name;
        log_msg("[saves] makeBackup: scheduling async work\n");
        brls::async([this, alive, applicationId, titleId, gameName] {
            std::string path;
            std::string error;
            const bool ok = runGuarded(
                [&](std::string& err) {
                    return backupSaveData(applicationId, titleId, gameName,
                                          path, err, selectedUid_);
                },
                error);
            log_msg("[saves] makeBackup: worker done ok=%d, scheduling sync\n",
                    ok ? 1 : 0);
            brls::sync([this, alive, ok, error] {
                log_msg("[saves] makeBackup: sync callback entered alive=%d\n",
                        alive->load() ? 1 : 0);
                if (!alive->load())
                    return;
                setBusy(false, "");
                if (!ok)
                    brls::Application::notify(
                        tr("pipensx/explorer/operation_failed", error));
                else
                    brls::Application::notify(tr("pipensx/saves/backed_up"));
                log_msg("[saves] makeBackup: calling reload()\n");
                reload();
                log_msg("[saves] makeBackup: reload() returned\n");
            });
        });
    }

    void confirmRestore(const SaveBackupInfo& backup) {
        auto* dialog =
            new brls::Dialog(tr("pipensx/saves/restore_question", backup.label));
        dialog->setCancelable(true);
        dialog->addButton(tr("pipensx/saves/restore"), [this, backup] {
            performRestore(backup);
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void performRestore(const SaveBackupInfo& backup) {
        if (busy_)
            return;
        setBusy(true, tr("pipensx/saves/restoring"));
        auto alive = alive_;
        const uint64_t applicationId = title_.applicationId;
        const std::string titleId = title_.titleId;
        const std::string gameName = title_.name;
        const std::string path = backup.path;
        brls::async([this, alive, applicationId, titleId, gameName, path] {
            std::string error;
            const bool ok = runGuarded(
                [&](std::string& err) {
                    return restoreSaveData(applicationId, titleId, gameName,
                                           path, err, selectedUid_);
                },
                error);
            brls::sync([this, alive, ok, error] {
                if (!alive->load())
                    return;
                setBusy(false, "");
                brls::Application::notify(
                    ok ? tr("pipensx/saves/restored")
                       : tr("pipensx/explorer/operation_failed", error));
                reload();
            });
        });
    }

    void performDelete(const SaveBackupInfo& backup) {
        if (busy_)
            return;
        std::string error;
        if (!deleteSaveBackup(backup.path, error))
            brls::Application::notify(
                tr("pipensx/explorer/operation_failed", error));
        reload();
    }

    InstalledTitle title_;
    std::vector<SaveBackupInfo> backups_;
    bool busy_ = false;
    brls::Label* statusLabel_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    brls::AppletFrame* frame_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_;
    AccountUserId selectedUid_;

    friend class SaveBackupDataSource;
};

}  // namespace pipensx::ui
