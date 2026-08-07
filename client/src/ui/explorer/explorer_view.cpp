#include "ui/explorer/explorer_view.hpp"

namespace pipensx::ui {

int ExplorerDataSource::numberOfRows(brls::RecyclerFrame*, int) {
    return static_cast<int>(owner_->entries().size());
}

brls::RecyclerCell* ExplorerDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    auto* cell = static_cast<ExplorerCell*>(
        recycler->dequeueReusableCell("Explorer"));
    cell->setEntry(owner_->entries()[index.row]);
    return cell;
}

void ExplorerDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                        brls::IndexPath index) {
    owner_->select(static_cast<size_t>(index.row));
}

}  // namespace pipensx::ui
