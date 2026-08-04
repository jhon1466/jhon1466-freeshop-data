#pragma once

// pipensx's design tokens, registered into Borealis's own theme/style
// tables so every stock widget (Box, Label, Button, Sidebar, AppletFrame,
// Cell, ProgressSpinner...) picks them up without any per-widget styling.
//
// The values are pipensx's src/ui/theme.hpp verbatim - the "pipensx/*"
// entries are its own tokens, and the "brls/*" ones override Borealis's
// defaults so the framework's built-in chrome matches too.
namespace freeshop
{
void registerTheme();
}
