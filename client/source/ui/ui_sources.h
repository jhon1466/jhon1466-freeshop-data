#pragma once
#include "../catalog/sources.h"
#include <stdbool.h>

// Shows the source-management screen: A toggles the highlighted source's
// enabled state, Y adds a new source (prompts for URL then name via the
// system keyboard), X removes the highlighted source (refused when it's the
// last remaining one - there must always be at least one), B goes back.
// Returns true if `list` was modified, in which case the caller should
// persist it (sources_save) and refetch the merged catalog.
bool ui_show_sources(SourceList *list);
