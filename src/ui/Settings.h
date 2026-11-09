#pragma once

#include <QString>

namespace framescope::ui {

// FrameScope is portable: settings live in framescope.ini next to the exe.
// Only if that folder isn't writable (e.g. under Program Files) they go to
// the user's config folder instead.
QString settingsFile();

// One-time move of settings from the registry (beta.1-beta.5) into the ini
// file; the old registry key is deleted afterwards.
void migrateLegacySettings();

} // namespace framescope::ui
