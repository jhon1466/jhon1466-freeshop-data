#include "ui/saves/save_detail_activity.hpp"

namespace pipensx::ui {

int SaveBackupDataSource::numberOfRows(brls::RecyclerFrame*, int) {
    return static_cast<int>(owner_->backups().size());
}

brls::RecyclerCell* SaveBackupDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    auto* cell = static_cast<SaveBackupCell*>(
        recycler->dequeueReusableCell("SaveBackup"));
    cell->setBackup(owner_->backups()[index.row]);
    return cell;
}

void SaveBackupDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                          brls::IndexPath index) {
    owner_->select(static_cast<size_t>(index.row));
}

}  // namespace pipensx::ui
