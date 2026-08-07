#include "ui/saves/saves_view.hpp"

namespace pipensx::ui {

int SavesDataSource::numberOfRows(brls::RecyclerFrame*, int) {
    return static_cast<int>(owner_->titles().size());
}

brls::RecyclerCell* SavesDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    auto* cell = static_cast<SaveTitleCell*>(
        recycler->dequeueReusableCell("SaveTitle"));
    cell->setTitle(owner_->titles()[index.row], owner_->metadata());
    return cell;
}

void SavesDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                     brls::IndexPath index) {
    owner_->select(static_cast<size_t>(index.row));
}

}  // namespace pipensx::ui
