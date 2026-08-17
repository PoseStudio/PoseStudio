/**
 * @file environmentpanel.h
 * @brief The "Environment" properties tab: live controls for the viewport's image-based lighting.
 *
 * A self-contained docker panel (its own module, like AssetManagerWidget and the Preferences
 * panels) that drives the viewport's lighting: the HDRI environment, exposure/tonemap, the IBL
 * diffuse/specular levels, an ambient fill, the key light, and environment rotation. Every control
 * except the HDRI selector is a live shader dial (no IBL re-bake), so tweaking is instant — the point
 * being to dial in the values that will become the defaults. It talks to the viewport through the
 * ViewportWidget facade; it owns no Vulkan state.
 */

#ifndef ENVIRONMENTPANEL_H
#define ENVIRONMENTPANEL_H

#include "lightingsettings.h"

#include <QWidget>

class QAction;
class QCheckBox;
class QFormLayout;
class QMenu;
class QPushButton;

namespace pose {

class DragNumberBox;
class ViewportWidget;

class EnvironmentPanel : public QWidget {
    Q_OBJECT

public:
    /// @param viewport The viewport this panel controls (its lighting/environment). Not owned.
    explicit EnvironmentPanel(ViewportWidget* viewport, QWidget* parent = nullptr);

private:
    void buildUi();
    void buildEnvironmentSelector(QFormLayout* form); // the HDRI button + its thumbnail menu
    void rebuildEnvironmentMenu();  // (re)fills the menu from the user library's hdri/ tree,
                                    // grouped under a heading per subfolder (= category)
    void chooseEnvironment(const QString& path, const QString& name);
    void pushSettings();            // sends m_settings to the viewport
    void resetToDefaults();

    // Backdrop mode row (Off / Environment / Dome — a button + exclusive menu, the app's picker
    // pattern) and its state plumbing. setBackdropMode is the one mutation path (undo-bracketed);
    // syncBackdropModeUi mirrors m_settings into the button text + checked action.
    void buildBackdropModeRow(QFormLayout* form);
    void setBackdropMode(int mode);
    void syncBackdropModeUi();

    // Undo bracketing: every committed edit gesture (a dial scrub or type-in, a restore button,
    // the tonemap toggle, restore-all) is wrapped begin → mutate/push → commit. begin snapshots
    // m_settings once per outermost gesture; commit registers that snapshot on the viewport's
    // undo stack if anything actually changed. Depth-counted so composite edits (restore-all
    // fires the checkbox's own wrapped handler) register as ONE entry.
    void beginSettingsEdit();
    void commitSettingsEdit();
    // Undo/redo landed a lighting state (already applied renderer-side): mirror it into
    // m_settings + the widgets, signals blocked so nothing re-pushes or re-registers.
    void applyRestoredSettings(const LightingSettings& settings);

    // Builds a labelled DragNumberBox row (the app-standard scrub field — drag to change, click to
    // type) into @p form and returns the box, whose valueChanged the caller wires to the matching
    // LightingSettings field. Each row also gets a small restore button on its right that snaps
    // just that dial back to its default (the value the row was built with).
    DragNumberBox* addValueRow(QFormLayout* form, const QString& label, double min, double max,
                               double step, double value);

    ViewportWidget*  m_viewport = nullptr;
    LightingSettings m_settings;   // current values; the source of truth pushed to the viewport

    LightingSettings m_preEditSettings; // dial state when the outermost gesture began
    int              m_editDepth = 0;   // nested-gesture guard (see beginSettingsEdit)

    // HDRI selector: a button (showing the current environment's name) that drops a menu of every
    // panorama (.hdr/.exr) in the user library's hdri/ tree, each with its image thumbnail, grouped by subfolder
    // with a category heading per folder (uncategorized root files first, headingless). The menu
    // is rebuilt on every open so files the user adds/removes/refiles while the app runs show up
    // immediately.
    QPushButton*    m_envButton = nullptr;
    QMenu*          m_envMenu = nullptr;
    QString         m_currentEnvPath;
    DragNumberBox* m_rotation = nullptr;
    DragNumberBox* m_exposure = nullptr;
    QCheckBox*     m_tonemap = nullptr;
    DragNumberBox* m_diffuse = nullptr;
    DragNumberBox* m_specular = nullptr;
    DragNumberBox* m_ambientFill = nullptr;
    DragNumberBox* m_keyIntensity = nullptr;
    DragNumberBox* m_keyAzimuth = nullptr;
    DragNumberBox* m_keyElevation = nullptr;
    DragNumberBox* m_subsurface = nullptr;
    DragNumberBox* m_rim = nullptr;

    // Backdrop rows (see buildBackdropModeRow / LightingSettings' backdrop fields).
    QPushButton*   m_backdropModeButton = nullptr;
    QAction*       m_backdropModeActions[3] = {nullptr, nullptr, nullptr}; // Off/Environment/Dome
    DragNumberBox* m_backdropBlur = nullptr;
    DragNumberBox* m_backdropBrightness = nullptr;
    DragNumberBox* m_domeRadius = nullptr;
};

} // namespace pose

#endif // ENVIRONMENTPANEL_H
