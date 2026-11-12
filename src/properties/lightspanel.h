/**
 * @file lightspanel.h
 * @brief The "Lights" properties tab: the scene's LIGHT OBJECTS (viewport/scene/scenelight.h) —
 *        point, spot, area and sun lights — added, removed, and the selected one's numbers set.
 *
 * The tab holds no state of its own but which sections are showing: it MIRRORS the viewport's
 * list (ViewportWidget::lights / selectedLight, re-read on lightsChanged — which a drag of a
 * light's marker in the viewport, undo and an opened project announce as a slider here does)
 * and every control sets ONE field of the selected light through the viewport's edit bracket,
 * so a scrub is one undo entry and Ctrl+Z steps back through the lights as through the pose.
 *
 * Built from the shared row builders (propertyrows.h), with the rows of a kind shown for that
 * kind alone: a spot's cone, an area light's size, a sun's angular size.
 */

#ifndef LIGHTSPANEL_H
#define LIGHTSPANEL_H

#include "scenelight.h"

#include <QColor>
#include <QPointer>
#include <QWidget>

#include <array>
#include <functional>
#include <vector>

class QCheckBox;
class QColorDialog;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace pose {

class DragNumberBox;
class MenuPickerButton;
class ViewportWidget;

class LightsPanel : public QWidget {
    Q_OBJECT

public:
    explicit LightsPanel(ViewportWidget* viewport, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    /// The selected light's numbers, in the order of their rows (the scripted test's
    /// `ui set lights.field <n>`).
    enum Field {
        Intensity = 0,
        Radius,   // cm (point, spot)
        Cone,     // degrees (spot)
        Blend,    // percent (spot)
        Width,    // cm (area)
        Height,   // cm (area)
        SunAngle, // degrees (sun)
        PosX,     // cm
        PosY,
        PosZ,
        Turn, // degrees
        Tilt,
        FieldCount
    };
    struct Row {
        QLabel*        label = nullptr;
        DragNumberBox* box = nullptr;
        QToolButton*   restore = nullptr;
        QWidget*       field = nullptr;
        QFormLayout*   form = nullptr;
    };

    void buildUi();
    /// One label column for the tab's two forms (at the first show: the Materials tab's rule).
    void alignLabelColumn();
    /// Re-reads the viewport's list and selection into every control (signals blocked).
    void refresh();
    /// One row of @p form for field @p which.
    void addRow(QFormLayout* form, Field which, const QString& label, const QString& tip, double lo, double hi,
                double step, int decimals, double restoreTo);
    /// The field's value of @p light in the tab's units, and its setter.
    static double fieldValue(const SceneLight& light, Field which);
    static void   setFieldValue(SceneLight& light, Field which, double value);
    /// Whether a light of @p kind has the field at all.
    static bool fieldApplies(LightKind kind, Field which);

    /// Applies @p change to the selected light as ONE undoable edit of its own.
    void editSelected(const std::function<void(SceneLight&)>& change);
    /// … or inside the bracket a scrub has open (no bracket of its own).
    void setSelected(const std::function<void(SceneLight&)>& change);
    void pickColor();

    ViewportWidget* m_viewport = nullptr;

    MenuPickerButton* m_picker = nullptr; // which light the tab shows (= the selected light)
    QPushButton*      m_add = nullptr;
    QPushButton*      m_remove = nullptr;
    QLabel*           m_empty = nullptr;

    QWidget*          m_details = nullptr; // everything below the list: greyed with no light selected
    QLineEdit*        m_name = nullptr;
    MenuPickerButton* m_kind = nullptr;
    QCheckBox*        m_enabled = nullptr;
    QCheckBox*        m_shadow = nullptr;
    QPushButton*      m_color = nullptr;
    QPushButton*      m_aim = nullptr;
    std::array<Row, FieldCount> m_rows{};
    std::vector<QLabel*>        m_plainLabels; // the Name / Kind / Color rows' labels
    bool                        m_labelsAligned = false;

    QPointer<QColorDialog> m_colorDialog;
    QColor                 m_colorAtOpen;
    int                    m_colorLight = -1; // the light the open color dialog edits
};

} // namespace pose

#endif // LIGHTSPANEL_H
