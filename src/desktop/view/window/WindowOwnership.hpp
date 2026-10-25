#pragma once

#include "../../DesktopTypes.hpp"

namespace Desktop::View {
    // Native transient ownership, independent of process names and layout slots.
    // Returns null for a missing or malformed owner chain.
    PHLWINDOW ownerRoot(PHLWINDOW window);
}
