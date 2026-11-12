/**
 * @file lightspanel.cpp
 * @brief Implements the "Lights" properties tab (see lightspanel.h).
 */

#include "lightspanel.h"

#include "dragnumberbox.h"
#include "menupickerbutton.h"
#include "propertyrows.h"
#include "viewportwidget.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pose {

namespace {

/// Linear → display (sRGB) and back, per channel: the swatch shows what the screen shows.
float linearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
float srgbToLinear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
QColor displayColor(const glm::vec3& linear) {
    return QColor::fromRgbF(linearToSrgb(linear.x), linearToSrgb(linear.y), linearToSrgb(linear.z));
}
glm::vec3 linearColor(const QColor& srgb) {
    return glm::vec3(srgbToLinear(static_cast<float>(srgb.redF())), srgbToLinear(static_cast<float>(srgb.greenF())),
                     srgbToLinear(static_cast<float>(srgb.blueF())));
}

/// The swatch's look: its background IS the color, the caption in a contrasting ink (the
/// Materials tab's swatch).
void paintSwatch(QPushButton* swatch, const QColor& c, bool enabled) {
    const bool dark = c.lightnessF() < 0.5;
    swatch->setText(enabled ? c.name(QColor::HexRgb).toUpper() : QString());
    swatch->setStyleSheet(QStringLiteral("QPushButton#MaterialColorButton { background-color: %1; color: %2; border: 1px solid #4a4b4d; "
                                         "border-radius: 4px; min-height: 20px; padding: 2px 8px; }"
                                         "QPushButton#MaterialColorButton:hover { border-color: #5b87cc; }"
                                         "QPushButton#MaterialColorButton:disabled { background-color: #2a2b2d; color: #6a6b6e; }")
                              .arg(c.name(QColor::HexRgb), dark ? QStringLiteral("#e0e0e0") : QStringLiteral("#1b1c1e")));
}

QString kindLabel(LightKind kind) {
    switch (kind) {
    case LightKind::Spot: return LightsPanel::tr("Spot Light");
    case LightKind::Area: return LightsPanel::tr("Area Light");
    case LightKind::Sun: return LightsPanel::tr("Sun");
    default: return LightsPanel::tr("Point Light");
    }
}

} // namespace

LightsPanel::LightsPanel(ViewportWidget* viewport, QWidget* parent) : QWidget(parent), m_viewport(viewport) {
    setObjectName(QStringLiteral("LightsPanel"));
    buildUi();
    if (m_viewport) {
        connect(m_viewport, &ViewportWidget::lightsChanged, this, &LightsPanel::refresh);
    }
    refresh();
}

double LightsPanel::fieldValue(const SceneLight& light, Field which) {
    switch (which) {
    case Intensity: return light.intensity;
    case Radius: return light.radius * 100.0;
    case Cone: return light.coneDeg;
    case Blend: return light.blend * 100.0;
    case Width: return light.width * 100.0;
    case Height: return light.height * 100.0;
    case SunAngle: return light.sunAngleDeg;
    case PosX: return light.position.x * 100.0;
    case PosY: return light.position.y * 100.0;
    case PosZ: return light.position.z * 100.0;
    case Turn: return light.turnDeg;
    case Tilt: return light.tiltDeg;
    default: return 0.0;
    }
}

void LightsPanel::setFieldValue(SceneLight& light, Field which, double value) {
    const float v = static_cast<float>(value);
    switch (which) {
    case Intensity: light.intensity = v; break;
    case Radius: light.radius = v / 100.0f; break;
    case Cone: light.coneDeg = v; break;
    case Blend: light.blend = v / 100.0f; break;
    case Width: light.width = v / 100.0f; break;
    case Height: light.height = v / 100.0f; break;
    case SunAngle: light.sunAngleDeg = v; break;
    case PosX: light.position.x = v / 100.0f; break;
    case PosY: light.position.y = v / 100.0f; break;
    case PosZ: light.position.z = v / 100.0f; break;
    case Turn: light.turnDeg = v; break;
    case Tilt: light.tiltDeg = v; break;
    default: break;
    }
}

bool LightsPanel::fieldApplies(LightKind kind, Field which) {
    switch (which) {
    case Radius: return kind == LightKind::Point || kind == LightKind::Spot;
    case Cone:
    case Blend: return kind == LightKind::Spot;
    case Width:
    case Height: return kind == LightKind::Area;
    case SunAngle: return kind == LightKind::Sun;
    default: return true;
    }
}

void LightsPanel::addRow(QFormLayout* form, Field which, const QString& label, const QString& tip, double lo, double hi,
                         double step, int decimals, double restoreTo) {
    Row& row = m_rows[static_cast<std::size_t>(which)];
    row.form = form;
    row.label = new QLabel(label);
    row.label->setToolTip(tip);
    row.box = new DragNumberBox;
    row.box->setRange(lo, hi);
    row.box->setSingleStep(step);
    row.box->setDecimals(decimals);
    if (which == PosX || which == PosY || which == PosZ) {
        row.box->setOrigin(0.0);
        row.box->setScrubSpan(400.0); // a sweep of the box is four metres: a light is placed, not flung
    } else if (which == Turn || which == Tilt) {
        row.box->setOrigin(0.0);
    }
    row.box->setToolTip(tip);
    row.box->setProperty("testId", QStringLiteral("lights.field")); // (the scripted test's `ui`: the Field's number)
    // One undo entry per completed gesture (a scrub or a type-in), not one per value it passes
    // through: the box brackets the user's changes with these two signals.
    connect(row.box, &DragNumberBox::editingStarted, this, [this]() {
        if (m_viewport) {
            m_viewport->beginLightEdit();
        }
    });
    connect(row.box, &DragNumberBox::editingFinished, this, [this]() {
        if (m_viewport) {
            m_viewport->endLightEdit();
        }
    });
    connect(row.box, &DragNumberBox::valueChanged, this,
            [this, which](double value) { setSelected([which, value](SceneLight& light) { setFieldValue(light, which, value); }); });
    row.restore = propertyrows::makeRestoreButton();
    row.restore->setToolTip(tr("Back to %1").arg(restoreTo));
    connect(row.restore, &QToolButton::clicked, this, [this, which, restoreTo]() {
        editSelected([which, restoreTo](SceneLight& light) { setFieldValue(light, which, restoreTo); });
    });
    row.field = propertyrows::rowWithRestore(row.box, row.restore);
    form->addRow(row.label, row.field);
}

void LightsPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("LightsContent"));
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(12, 12, 12, 12);
    col->setSpacing(12);

    // --- The list: which light, add, delete ---
    QFormLayout* list = propertyrows::addGroup(col, tr("Lights"));
    m_picker = new MenuPickerButton;
    m_picker->setObjectName(QStringLiteral("EnvironmentHdriButton"));
    // (The caption's left inset is a transparent spacer icon: Qt's style engine ignores QSS
    // padding-left on a left-aligned button label — the HDRI selector's rule.)
    QPixmap spacer(10, 10);
    spacer.fill(Qt::transparent);
    m_picker->setIcon(QIcon(spacer));
    m_picker->setIconSize(QSize(10, 10));
    m_picker->setCursor(Qt::PointingHandCursor);
    m_picker->setToolTip(tr("The light shown below. Clicking a light's marker in the viewport selects it."));
    m_picker->setProperty("testId", QStringLiteral("lights.picker"));
    connect(m_picker, &MenuPickerButton::currentChanged, this, [this](int index) {
        if (m_viewport) {
            m_viewport->selectLight(index);
        }
    });
    list->addRow(m_picker);

    auto* buttons = new QWidget;
    auto* buttonRow = new QHBoxLayout(buttons);
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);
    m_add = new QPushButton(tr("Add Light"));
    m_add->setObjectName(QStringLiteral("PropertyResetButton"));
    m_add->setToolTip(tr("Adds a light two metres from the figure, above it and aimed at it. Each new light stands a step "
                         "further round than the last."));
    auto* addMenu = new QMenu(m_add);
    for (const LightKind kind : {LightKind::Point, LightKind::Spot, LightKind::Area, LightKind::Sun}) {
        QAction* action = addMenu->addAction(kindLabel(kind));
        connect(action, &QAction::triggered, this, [this, kind]() {
            if (m_viewport) {
                m_viewport->addLight(kind);
            }
        });
    }
    m_add->setMenu(addMenu);
    m_remove = new QPushButton(tr("Delete Light"));
    m_remove->setObjectName(QStringLiteral("PropertyResetButton"));
    m_remove->setToolTip(tr("Removes the light shown below (the Delete key does the same while it is selected in the viewport)"));
    m_remove->setProperty("testId", QStringLiteral("lights.remove"));
    connect(m_remove, &QPushButton::clicked, this, [this]() {
        if (m_viewport && m_viewport->selectedLight() >= 0) {
            m_viewport->removeLight(m_viewport->selectedLight());
        }
    });
    buttonRow->addWidget(m_add);
    buttonRow->addWidget(m_remove);
    buttonRow->addStretch(1);
    list->addRow(buttons);

    m_empty = new QLabel(tr("The scene has no light objects yet: it is lit by its environment and the key light "
                            "(the Environment tab). Add Light places a lamp, a spotlight, a softbox or a sun of "
                            "your own."));
    m_empty->setObjectName(QStringLiteral("TransformHint"));
    m_empty->setWordWrap(true);
    list->addRow(m_empty);

    // --- The selected light ---
    m_details = new QWidget;
    auto* details = new QVBoxLayout(m_details);
    details->setContentsMargins(0, 0, 0, 0);
    details->setSpacing(12);

    QFormLayout* light = propertyrows::addGroup(details, tr("Light"));
    m_name = new QLineEdit;
    m_name->setObjectName(QStringLiteral("LightNameEdit"));
    m_name->setToolTip(tr("The light's name"));
    m_name->setProperty("inlineEdit", true); // (Esc while typing leaves the field, not the selection)
    connect(m_name, &QLineEdit::editingFinished, this, [this]() {
        const QString text = m_name->text().trimmed();
        if (!text.isEmpty()) {
            editSelected([text](SceneLight& l) { l.name = text.toStdString(); });
        } else {
            refresh();
        }
    });
    light->addRow(tr("Name"), m_name);
    m_plainLabels.push_back(qobject_cast<QLabel*>(light->labelForField(m_name)));

    m_kind = new MenuPickerButton;
    m_kind->setObjectName(QStringLiteral("EnvironmentHdriButton"));
    m_kind->setIcon(QIcon(spacer));
    m_kind->setIconSize(QSize(10, 10));
    m_kind->setCursor(Qt::PointingHandCursor);
    m_kind->setToolTip(tr("What the light is: a Point light shines every way from a point; a Spot light is a point light "
                          "held to a cone; an Area light is a rectangle that shines from one face, a softbox; a Sun "
                          "shines the same way and as brightly everywhere."));
    m_kind->setProperty("testId", QStringLiteral("lights.kind"));
    QList<MenuPickerButton::Item> kinds;
    for (const LightKind kind : {LightKind::Point, LightKind::Spot, LightKind::Area, LightKind::Sun}) {
        kinds.push_back({kindLabel(kind), false, QString()});
    }
    m_kind->setItems(kinds);
    connect(m_kind, &MenuPickerButton::currentChanged, this, [this](int index) {
        editSelected([index](SceneLight& l) {
            l.kind = static_cast<LightKind>(std::clamp(index, 0, kLightKindCount - 1));
            // (A sun's intensity is its brightness everywhere; the others' a metre away — a light
            // two metres off turned into a sun at the same number would be four times as bright.)
        });
    });
    light->addRow(tr("Kind"), m_kind);
    m_plainLabels.push_back(qobject_cast<QLabel*>(light->labelForField(m_kind)));

    m_enabled = new QCheckBox(tr("Switched on"));
    m_enabled->setToolTip(tr("Off, the light stays where it is and lights nothing"));
    m_enabled->setProperty("testId", QStringLiteral("lights.enabled"));
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool on) { editSelected([on](SceneLight& l) { l.enabled = on; }); });
    light->addRow(m_enabled);

    m_color = new QPushButton;
    m_color->setObjectName(QStringLiteral("MaterialColorButton"));
    m_color->setCursor(Qt::PointingHandCursor);
    m_color->setToolTip(tr("The light's color"));
    connect(m_color, &QPushButton::clicked, this, &LightsPanel::pickColor);
    auto* colorRestore = propertyrows::makeRestoreButton();
    colorRestore->setToolTip(tr("Back to white"));
    connect(colorRestore, &QToolButton::clicked, this, [this]() { editSelected([](SceneLight& l) { l.color = glm::vec3(1.0f); }); });
    QWidget* colorField = propertyrows::rowWithRestore(m_color, colorRestore);
    light->addRow(tr("Color"), colorField);
    m_plainLabels.push_back(qobject_cast<QLabel*>(light->labelForField(colorField)));

    addRow(light, Intensity, tr("Intensity"),
           tr("How brightly the light lights a surface facing it, one metre away — in the units of the Environment "
              "tab's Key Intensity. Twice as far away it lights a quarter as much. A Sun is this bright everywhere."),
           0.0, 100.0, 0.5, 1, 4.0);
    addRow(light, Radius, tr("Radius (cm)"),
           tr("The size of the source. A larger source casts softer shadows in a render; it is no brighter."), 0.0, 200.0,
           1.0, 0, 5.0);
    addRow(light, Cone, tr("Cone (°)"), tr("How wide the spotlight's cone is, edge to edge"), 1.0, 179.0, 1.0, 0, 50.0);
    addRow(light, Blend, tr("Edge (%)"),
           tr("How much of the cone its edge fades over: 0 is a hard-edged circle of light, 100 fades from the middle out"),
           0.0, 100.0, 1.0, 0, 20.0);
    addRow(light, Width, tr("Width (cm)"),
           tr("The area light's width. A larger light casts softer shadows in a render; the subject is no brighter."), 1.0,
           1000.0, 5.0, 0, 80.0);
    addRow(light, Height, tr("Height (cm)"), tr("The area light's height"), 1.0, 1000.0, 5.0, 0, 80.0);
    addRow(light, SunAngle, tr("Size (°)"),
           tr("How large the sun is in the sky: a larger one casts softer shadows in a render"), 0.0, 45.0, 0.5, 1, 2.0);

    m_shadow = new QCheckBox(tr("Casts shadows in a render"));
    m_shadow->setToolTip(tr("Whether the light casts shadows when the scene is rendered, and in the Rendered viewport mode. "
                            "The PBR Shaded mode draws shadows for the key light alone."));
    connect(m_shadow, &QCheckBox::toggled, this, [this](bool on) { editSelected([on](SceneLight& l) { l.castShadow = on; }); });
    light->addRow(m_shadow);

    QFormLayout* place = propertyrows::addGroup(details, tr("Placement"));
    addRow(place, PosX, tr("Left-Right (cm)"), tr("Where the light stands: to the figure's left (positive) or right"),
           -5000.0, 5000.0, 1.0, 0, 0.0);
    addRow(place, PosY, tr("Up-Down (cm)"), tr("How high the light stands over the floor"), 0.0, 5000.0, 1.0, 0, 180.0);
    addRow(place, PosZ, tr("Back-Forward (cm)"),
           tr("Where the light stands: in front of the figure (positive, the way an unturned figure faces) or behind"),
           -5000.0, 5000.0, 1.0, 0, 150.0);
    addRow(place, Turn, tr("Turn (°)"),
           tr("Where the light shines, about the vertical: 0 shines toward the back of the scene — at a figure from her "
              "front — and positive values swing the beam toward her right, counter-clockwise seen from above."),
           -180.0, 180.0, 1.0, 0, 0.0);
    addRow(place, Tilt, tr("Tilt (°)"), tr("Where the light shines, below the horizon: 0 is level, 90 straight down, negative up"),
           -90.0, 90.0, 1.0, 0, 30.0);
    m_aim = new QPushButton(tr("Aim at Subject"));
    m_aim->setObjectName(QStringLiteral("PropertyResetButton"));
    m_aim->setToolTip(tr("Turns the light to shine at the figure from where it stands (after moving it, say)"));
    m_aim->setProperty("testId", QStringLiteral("lights.aim"));
    connect(m_aim, &QPushButton::clicked, this, [this]() {
        if (m_viewport && m_viewport->selectedLight() >= 0) {
            m_viewport->aimLightAtSubject(m_viewport->selectedLight());
        }
    });
    place->addRow(m_aim);

    auto* hint = new QLabel(tr("In the viewport, drag a light's marker to move it (the mouse wheel moves it nearer or "
                               "farther), right-click it for its menu. The PBR Shaded mode lights the scene with up to "
                               "eight lights and draws shadows for the key light alone; the Rendered mode and a render "
                               "trace every light, shadows and bounced light included."));
    hint->setObjectName(QStringLiteral("TransformHint"));
    hint->setWordWrap(true);
    details->addWidget(hint);

    col->addWidget(m_details);
    col->addStretch(1);
    scroll->setWidget(content);
}

void LightsPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!m_labelsAligned) {
        m_labelsAligned = true;
        alignLabelColumn();
    }
}

void LightsPanel::alignLabelColumn() {
    // The Light and the Placement groups are two forms, and left to themselves their label columns
    // differ by the longest label each holds: every label takes the widest one's width, so the
    // fields start at one x down the tab. At the first show — at construction the labels still
    // wear the application's default font, not the stylesheet's.
    std::vector<QLabel*> labels;
    for (const Row& row : m_rows) {
        if (row.label != nullptr) {
            labels.push_back(row.label);
        }
    }
    for (QLabel* label : m_plainLabels) {
        if (label != nullptr) {
            labels.push_back(label);
        }
    }
    int widest = 0;
    for (QLabel* label : labels) {
        label->ensurePolished();
        widest = std::max(widest, label->fontMetrics().horizontalAdvance(label->text()) + 2);
    }
    for (QLabel* label : labels) {
        label->setMinimumWidth(widest);
    }
}

void LightsPanel::refresh() {
    const std::vector<SceneLight> lights = m_viewport ? m_viewport->lights() : std::vector<SceneLight>();
    const int selected = m_viewport ? m_viewport->selectedLight() : -1;
    const bool has = selected >= 0 && static_cast<std::size_t>(selected) < lights.size();

    // The picker: every light by name, the selected one current.
    {
        const QSignalBlocker blocker(m_picker);
        QList<MenuPickerButton::Item> items;
        for (const SceneLight& light : lights) {
            items.push_back({QString::fromStdString(light.name), false, QString()});
        }
        m_picker->setItems(items);
        if (has) {
            m_picker->setCurrentIndex(selected);
        } else {
            m_picker->setDetachedCaption(lights.empty() ? tr("No lights") : tr("Select a light"));
        }
        m_picker->setEnabled(!lights.empty());
    }
    m_remove->setEnabled(has);
    m_empty->setVisible(lights.empty());
    m_details->setEnabled(has);
    m_details->setVisible(!lights.empty());

    const SceneLight light = has ? lights[static_cast<std::size_t>(selected)] : SceneLight();
    {
        const QSignalBlocker b1(m_name), b2(m_kind), b3(m_enabled), b4(m_shadow);
        if (!m_name->hasFocus()) {
            m_name->setText(has ? QString::fromStdString(light.name) : QString());
        }
        m_kind->setCurrentIndex(static_cast<int>(light.kind));
        m_enabled->setChecked(light.enabled);
        m_shadow->setChecked(light.castShadow);
    }
    paintSwatch(m_color, displayColor(light.color), has);
    for (int f = 0; f < FieldCount; ++f) {
        Row& row = m_rows[static_cast<std::size_t>(f)];
        if (row.box == nullptr) {
            continue;
        }
        const bool applies = fieldApplies(light.kind, static_cast<Field>(f));
        row.form->setRowVisible(row.field, applies);
        const QSignalBlocker blocker(row.box);
        row.box->setValue(fieldValue(light, static_cast<Field>(f)));
    }
    // (A point light shines every way: its aim is only its marker's.)
    const bool aims = light.kind != LightKind::Point;
    m_rows[Turn].form->setRowVisible(m_rows[Turn].field, aims);
    m_rows[Tilt].form->setRowVisible(m_rows[Tilt].field, aims);
    m_aim->setVisible(aims);

    if (m_colorDialog && m_colorLight != selected) {
        m_colorDialog->close(); // the color being picked was another light's
    }
}

void LightsPanel::setSelected(const std::function<void(SceneLight&)>& change) {
    if (!m_viewport) {
        return;
    }
    const std::vector<SceneLight> lights = m_viewport->lights();
    const int                     selected = m_viewport->selectedLight();
    if (selected < 0 || static_cast<std::size_t>(selected) >= lights.size()) {
        return;
    }
    SceneLight light = lights[static_cast<std::size_t>(selected)];
    change(light);
    m_viewport->setLight(selected, light);
}

void LightsPanel::editSelected(const std::function<void(SceneLight&)>& change) {
    if (!m_viewport || !m_viewport->beginLightEdit()) {
        return;
    }
    setSelected(change);
    m_viewport->endLightEdit();
    refresh();
}

void LightsPanel::pickColor() {
    if (!m_viewport || m_viewport->selectedLight() < 0) {
        return;
    }
    if (m_colorDialog) {
        m_colorDialog->raise();
        m_colorDialog->activateWindow();
        return;
    }
    if (!m_viewport->beginLightEdit()) {
        return;
    }
    const std::vector<SceneLight> lights = m_viewport->lights();
    m_colorLight = m_viewport->selectedLight();
    m_colorAtOpen = displayColor(lights[static_cast<std::size_t>(m_colorLight)].color);
    // The app's own themed dialog (the native one ignores the dark theme), non-modal so the
    // color can be judged on the scene while it changes.
    m_colorDialog = new QColorDialog(m_colorAtOpen, this);
    m_colorDialog->setOption(QColorDialog::DontUseNativeDialog, true);
    m_colorDialog->setWindowTitle(tr("Light Color"));
    m_colorDialog->setAttribute(Qt::WA_DeleteOnClose);
    const auto apply = [this](const QColor& c) {
        if (!m_viewport || !c.isValid() || m_viewport->selectedLight() != m_colorLight) {
            return;
        }
        setSelected([c](SceneLight& l) { l.color = linearColor(c); });
    };
    connect(m_colorDialog, &QColorDialog::currentColorChanged, this, apply);
    connect(m_colorDialog, &QColorDialog::finished, this, [this, apply](int result) {
        if (result != QDialog::Accepted) {
            apply(m_colorAtOpen); // Cancel: the color as it was when the dialog opened
        }
        if (m_viewport) {
            m_viewport->endLightEdit(); // one undo entry for the whole pick
        }
        m_colorDialog = nullptr;
        m_colorLight = -1;
        refresh();
    });
    m_colorDialog->open();
}

} // namespace pose
