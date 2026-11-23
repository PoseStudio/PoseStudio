// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file placeholderpreferencespanel.cpp
 * @brief Implements PlaceholderPreferencesPanel. See placeholderpreferencespanel.h.
 */

#include "placeholderpreferencespanel.h"

PlaceholderPreferencesPanel::PlaceholderPreferencesPanel(const QString& title,
                                                         const QString& message, QWidget* parent)
    : PreferencesPanel(title, parent) {
    addPlaceholder(message);
}
