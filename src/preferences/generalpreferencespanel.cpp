/**
 * @file generalpreferencespanel.cpp
 * @brief Implements the "General" preferences page.
 */

#include "generalpreferencespanel.h"
#include "installping.h"
#include "updatecheck.h"
#include "whatsnewdialog.h"
#include "constants.h"
#include "preferencesmanager.h"

#include <QCheckBox>
#include <QVBoxLayout>

GeneralPreferencesPanel::GeneralPreferencesPanel(QWidget* parent)
    : PreferencesPanel(tr("General"), parent) {

    // --- Anonymous install ping ---
    // The toggle is the user-facing half of InstallPing; the copy below is the disclosure of
    // exactly what the ping carries, so keep it in step with InstallPing::buildPayload().
    auto* pingToggle = new QCheckBox(tr("Send an anonymous install ping"), this);
    pingToggle->setChecked(InstallPing::isEnabled());
    contentLayout()->addWidget(pingToggle);

    addDescription(
        tr("Each time it starts, PoseStudio lets posestudio.io know that this installation exists, so we can "
           "see how many people use it and which versions are out there. The ping carries a random "
           "install ID, the app version, your operating system and CPU type, whether the app was "
           "installed or is running portable, the Qt library version, and the time of the ping — "
           "nothing else: no names, no files, no usage data, and the ID isn't linked to you in any way."));
    // Shown, never minted here: opening Preferences must not create and persist an identifier
    // (in a keyless build, or with the toggle off, one may legitimately never exist).
    const QString id = InstallPing::existingInstallId();
    addDescription(id.isEmpty()
                       ? tr("Install ID: not created yet (assigned when the first ping is sent)")
                       : tr("Install ID: %1").arg(id));

    if (!InstallPing::isBuiltIn()) {
        // Local builds carry no signing key (it comes from the release pipeline's secret), so
        // the setting is honoured but moot — say so rather than leave a developer guessing.
        addDescription(tr("This build has no ping key, so nothing is sent regardless of this setting."));
    }

    connect(pingToggle, &QCheckBox::toggled, this, [](bool enabled) {
        InstallPing::setEnabled(enabled);
    });

    // --- Update check ---
    auto* updateToggle = new QCheckBox(tr("Check for updates at startup"), this);
    updateToggle->setChecked(UpdateCheck::isEnabled());
    contentLayout()->addWidget(updateToggle);
    addDescription(
        tr("A few seconds after it starts, PoseStudio asks GitHub for the latest release and, when a newer "
           "one exists, shows a message with a link to its download page. Nothing but the request itself "
           "is sent. Help → Check for Updates… does the same on demand."));
    connect(updateToggle, &QCheckBox::toggled, this, [](bool enabled) {
        UpdateCheck::setEnabled(enabled);
    });

    // --- What's New after an update ---
    auto* whatsNewToggle = new QCheckBox(tr("Show What's New after an update"), this);
    whatsNewToggle->setChecked(WhatsNewDialog::isEnabled());
    contentLayout()->addWidget(whatsNewToggle);
    addDescription(
        tr("The first time a new version of PoseStudio starts, a window with that version's release notes "
           "opens once the splash screen is dismissed. Help → What's New… opens the same window whenever you "
           "like."));
    connect(whatsNewToggle, &QCheckBox::toggled, this, [](bool enabled) {
        WhatsNewDialog::setEnabled(enabled);
    });

    // --- Camera changes as unsaved ---
    // Off by default: orbiting/panning to look at a figure is not an edit worth being prompted
    // about. The framing is still written into the .pss document on Save either way.
    auto* cameraToggle = new QCheckBox(tr("Count camera changes as unsaved"), this);
    cameraToggle->setChecked(PreferencesManager::instance()
                                   .getValue(Constants::PREF_CAMERA_CHANGES_MARK_DIRTY, false).toBool());
    contentLayout()->addWidget(cameraToggle);
    addDescription(
        tr("When on, moving the camera (orbit, pan, zoom, the view hotkeys) marks the scene as having "
           "unsaved changes — the close prompt and File → New will ask before discarding it. Off by "
           "default: looking around is not an edit."));
    connect(cameraToggle, &QCheckBox::toggled, this, [](bool enabled) {
        PreferencesManager::instance().setValue(Constants::PREF_CAMERA_CHANGES_MARK_DIRTY,
                                               enabled ? "1" : "0");
    });
}

