// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file generalpreferencespanel.h
 * @brief "General" page of the Preferences dialog.
 */

#ifndef GENERALPREFERENCESPANEL_H
#define GENERALPREFERENCESPANEL_H

#include "preferencespanel.h"

/// General application settings, each a toggle: the anonymous install ping (installping.h, with
/// its disclosure and the install ID), the update check at startup (updatecheck.h), the What's
/// New window after an update (whatsnewdialog.h), and whether camera moves count as unsaved
/// changes of the project.
class GeneralPreferencesPanel : public PreferencesPanel {
    Q_OBJECT

public:
    explicit GeneralPreferencesPanel(QWidget* parent = nullptr);
};

#endif // GENERALPREFERENCESPANEL_H
