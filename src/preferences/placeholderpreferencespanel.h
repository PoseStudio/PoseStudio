// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file placeholderpreferencespanel.h
 * @brief A Preferences page that exists only to hold its tab's place until the real page is built.
 *
 * Several tabs (Interface, Input, Navigation, System) are planned but have no settings yet. One
 * parameterized class registers all of them instead of a subclass file per empty page — their
 * whole behaviour is a title and a muted "will appear here" line. See preferencespanel.h for how
 * to promote one of these into a real page.
 */

#ifndef PLACEHOLDERPREFERENCESPANEL_H
#define PLACEHOLDERPREFERENCESPANEL_H

#include "preferencespanel.h"

/// A titled page whose content is a single placeholder message.
class PlaceholderPreferencesPanel : public PreferencesPanel {
    Q_OBJECT

public:
    /// @param title   The page heading (matches the tab label).
    /// @param message The muted placeholder line shown under it.
    PlaceholderPreferencesPanel(const QString& title, const QString& message,
                                QWidget* parent = nullptr);
};

#endif // PLACEHOLDERPREFERENCESPANEL_H
