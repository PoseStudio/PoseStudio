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

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QFormLayout;

namespace pose {

class ViewportWidget;

class EnvironmentPanel : public QWidget {
    Q_OBJECT

public:
    /// @param viewport The viewport this panel controls (its lighting/environment). Not owned.
    explicit EnvironmentPanel(ViewportWidget* viewport, QWidget* parent = nullptr);

private:
    void buildUi();
    void populateEnvironments(QComboBox* combo); // fills the HDRI list from <appDir>/hdri
    void pushSettings();                         // sends m_settings to the viewport
    void resetToDefaults();

    // Builds a labelled row (slider + spin box, kept in sync) into @p form and returns the spin box,
    // whose valueChanged the caller wires to the matching LightingSettings field.
    QDoubleSpinBox* addSliderRow(QFormLayout* form, const QString& label, double min, double max,
                                 double step, double value);

    ViewportWidget*  m_viewport = nullptr;
    LightingSettings m_settings;   // current values; the source of truth pushed to the viewport

    QComboBox*      m_envCombo = nullptr;
    QDoubleSpinBox* m_rotation = nullptr;
    QDoubleSpinBox* m_exposure = nullptr;
    QCheckBox*      m_tonemap = nullptr;
    QDoubleSpinBox* m_diffuse = nullptr;
    QDoubleSpinBox* m_specular = nullptr;
    QDoubleSpinBox* m_ambientFill = nullptr;
    QDoubleSpinBox* m_keyIntensity = nullptr;
};

} // namespace pose

#endif // ENVIRONMENTPANEL_H
