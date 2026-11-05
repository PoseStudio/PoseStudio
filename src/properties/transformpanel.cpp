/**
 * @file transformpanel.cpp
 * @brief Implementation of TransformPanel. See transformpanel.h.
 */

#include "transformpanel.h"

#include "dragnumberbox.h"
#include "jointtransform.h"
#include "propertyrows.h"
#include "viewportwidget.h"

#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace pose {

TransformPanel::TransformPanel(ViewportWidget* viewport, QWidget* parent)
    : QWidget(parent), m_viewport(viewport) {
    setObjectName(QStringLiteral("TransformPanel"));
    buildUi();
    if (m_viewport) {
        // The selection or the selected joint's pose changed — by a drag, the B / S / T keys, undo, a pose
        // load, one of these dials: the rows follow.
        connect(m_viewport, &ViewportWidget::jointTransformChanged, this, &TransformPanel::refresh);
    }
    refresh();
}

void TransformPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("TransformContent"));
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(12, 12, 12, 12);
    col->setSpacing(12);

    m_jointName = new QLabel;
    m_jointName->setObjectName(QStringLiteral("TransformJointName"));
    col->addWidget(m_jointName);

    // --- Figure: where the active figure stands and which way she faces (2026-11-05; the user:
    // "we have to be able to easily position the character (up down left right) and twist the
    // entire character") — the rig root's pose translation and heading, a pose edit like a dial's.
    QFormLayout* figure = propertyrows::addGroup(col, tr("Figure"));
    const std::array<QString, 7> figureLabels = {tr("Left-Right"), tr("Up-Down"), tr("Back-Forward"), tr("Turn"), tr("Tip"), tr("Tilt"), tr("Scale")};
    const std::array<QString, 7> figureTips = {
        tr("Moves the whole figure, as posed, to her left (positive) or right, in centimetres."),
        tr("Moves the whole figure up (positive) or down, in centimetres. The Ground button puts her back on the floor."),
        tr("Moves the whole figure forward (positive, the way an unturned figure faces) or back, in centimetres."),
        tr("Turns the whole figure about the vertical, in degrees: 0 faces the way she was imported, positive turns her toward her left."),
        tr("Tips the whole figure forward (positive, face down) or back, in degrees, about her own left-right axis."),
        tr("Tilts the whole figure toward her left (positive) or right, in degrees, about her own front-back axis."),
        tr("A static model's size (an OBJ, a Gaussian splat scene), in percent of the size it was imported at. A figure keeps her own."),
    };
    for (int i = 0; i < 7; ++i) {
        Row& row = m_figureRows[static_cast<std::size_t>(i)];
        row.label = new QLabel(figureLabels[static_cast<std::size_t>(i)]);
        row.label->setToolTip(figureTips[static_cast<std::size_t>(i)]);
        row.box = new DragNumberBox;
        if (i < 3) {
            row.box->setRange(-500.0, 500.0);
            row.box->setSingleStep(1.0);
            row.box->setDecimals(1);
            row.box->setOrigin(0.0);
        } else if (i < 6) {
            row.box->setRange(-180.0, 180.0);
            row.box->setSingleStep(1.0);
            row.box->setDecimals(0);
            row.box->setOrigin(0.0);
        } else {
            row.box->setRange(1.0, 2000.0);
            row.box->setSingleStep(5.0);
            row.box->setDecimals(0);
            row.box->setOrigin(100.0); // (as imported)
        }
        row.box->setToolTip(figureTips[static_cast<std::size_t>(i)]);
        connect(row.box, &DragNumberBox::editingStarted, this, [this]() {
            if (m_viewport) {
                m_viewport->beginJointTransformEdit();
            }
        });
        connect(row.box, &DragNumberBox::editingFinished, this, [this]() {
            if (m_viewport) {
                m_viewport->endJointTransformEdit();
            }
        });
        connect(row.box, &DragNumberBox::valueChanged, this, [this, i](double value) {
            if (m_viewport) {
                if (i < 3) {
                    m_viewport->setFigurePosition(i, value);
                } else if (i < 6) {
                    m_viewport->setFigureRotation(i - 3, value);
                } else {
                    m_viewport->setFigureScale(value);
                }
                refresh();
            }
        });
        row.restore = propertyrows::makeRestoreButton();
        row.restore->setToolTip(i < 3 ? tr("Back to 0: where she was imported")
                                : i < 6 ? tr("Back to 0: as she was imported")
                                        : tr("Back to 100: the size it was imported at"));
        connect(row.restore, &QToolButton::clicked, this, [this, i]() {
            if (m_viewport && m_viewport->beginJointTransformEdit()) {
                if (i < 3) {
                    m_viewport->setFigurePosition(i, 0.0);
                } else if (i < 6) {
                    m_viewport->setFigureRotation(i - 3, 0.0);
                } else {
                    m_viewport->setFigureScale(100.0);
                }
                m_viewport->endJointTransformEdit();
                refresh();
            }
        });
        figure->addRow(row.label, propertyrows::rowWithRestore(row.box, row.restore));
    }

    // --- Rotation: the three dials, in JointDialKind order ---
    QFormLayout* rotation = propertyrows::addGroup(col, tr("Rotation"));
    const std::array<QString, 3> labels = {tr("Bend"), tr("Side-Side"), tr("Twist")};
    const std::array<QString, 3> tips = {
        tr("Bends the joint in its natural direction. 0 is the rest pose, 100 is fully bent; "
           "negative values bend it the other way, as far as the joint allows.\n"
           "In the viewport: press B and move the mouse."),
        tr("Bends the joint in its secondary direction. 0 is the rest pose, 100 is the joint's limit; "
           "negative values bend it the other way, as far as the joint allows.\n"
           "In the viewport: press S and move the mouse."),
        tr("Turns the joint about its own length. 0 is the rest pose, 100 is the joint's limit; "
           "negative values turn it the other way, as far as the joint allows.\n"
           "In the viewport: press T and move the mouse."),
    };
    for (int i = 0; i < 3; ++i) {
        Row& row = m_rows[static_cast<std::size_t>(i)];
        row.label = new QLabel(labels[static_cast<std::size_t>(i)]);
        row.label->setToolTip(tips[static_cast<std::size_t>(i)]);

        row.box = new DragNumberBox;
        row.box->setRange(-100.0, 100.0);
        row.box->setOrigin(0.0); // the rest pose: the bar fills from it, either way
        row.box->setSingleStep(1.0);
        row.box->setDecimals(0);
        row.box->setToolTip(tips[static_cast<std::size_t>(i)]);
        // One undo entry per completed gesture (a scrub or a type-in), not one per value it
        // passes through: the box brackets the user's changes with these two signals.
        connect(row.box, &DragNumberBox::editingStarted, this, [this]() {
            if (m_viewport) {
                m_viewport->beginJointTransformEdit();
            }
        });
        connect(row.box, &DragNumberBox::editingFinished, this, [this]() {
            if (m_viewport) {
                m_viewport->endJointTransformEdit();
            }
        });
        // Live: every value the box lands on poses the joint. The pose may rest short of it (a
        // joint limit, the body in the way), so the row is read back at once.
        connect(row.box, &DragNumberBox::valueChanged, this, [this, i](double value) {
            if (m_viewport) {
                m_viewport->setJointTransformDial(i, value);
                refresh();
            }
        });

        // Per-row restore: just this dial back to 0, the rest pose.
        row.restore = propertyrows::makeRestoreButton();
        row.restore->setToolTip(tr("Back to the rest pose"));
        connect(row.restore, &QToolButton::clicked, this, [this, i]() {
            if (m_viewport && m_viewport->beginJointTransformEdit()) {
                m_viewport->setJointTransformDial(i, 0.0);
                m_viewport->endJointTransformEdit(); // registers no entry if the dial was already at 0
                refresh();
            }
        });

        rotation->addRow(row.label, propertyrows::rowWithRestore(row.box, row.restore));
    }

    m_hint = new QLabel(tr("Click a joint on a figure to select it. Its rotation can then be "
                           "set here."));
    m_hint->setObjectName(QStringLiteral("TransformHint"));
    m_hint->setWordWrap(true);
    col->addWidget(m_hint);

    // --- Reset ---
    m_reset = new QPushButton(tr("Reset All"));
    m_reset->setObjectName(QStringLiteral("PropertyResetButton"));
    m_reset->setToolTip(tr("Returns all three dials to 0: the selected joint's rest pose"));
    col->addWidget(m_reset, 0, Qt::AlignLeft);
    connect(m_reset, &QPushButton::clicked, this, &TransformPanel::resetRotation);

    col->addStretch(1);
    scroll->setWidget(content);
}

void TransformPanel::refresh() {
    const JointTransform state = m_viewport ? m_viewport->jointTransform() : JointTransform{};
    {
        const JointTransform::Figure& fig = state.figure;
        const std::array<double, 7>   values = {fig.x, fig.y, fig.z, fig.turnDeg, fig.tipDeg, fig.tiltDeg, fig.scalePct};
        for (std::size_t i = 0; i < m_figureRows.size(); ++i) {
            Row&       row = m_figureRows[i];
            const bool on = fig.valid && (i < 6 || fig.isStatic); // (Scale is a static model's row)
            {
                const QSignalBlocker blocker(row.box);
                row.box->setValue(fig.valid ? values[i] : (i == 6 ? 100.0 : 0.0));
            }
            row.box->setEnabled(on);
            row.label->setEnabled(on);
            row.restore->setEnabled(on);
        }
    }
    m_jointName->setText(state.valid ? QString::fromStdString(state.name) : tr("No joint selected"));
    m_hint->setVisible(!state.valid);
    m_reset->setEnabled(state.valid);
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        Row&                        row = m_rows[i];
        const JointTransform::Dial& dial = state.dials[i];
        const bool                  on = state.valid && dial.enabled;
        {
            // Signals blocked: mirroring the pose into the row must not pose the figure back.
            const QSignalBlocker blocker(row.box);
            if (on) {
                row.box->setRange(dial.minValue, dial.maxValue);
                row.box->setValue(dial.value);
            } else {
                row.box->setRange(-100.0, 100.0);
                row.box->setValue(0.0);
            }
        }
        // (After the value: disabling a box mid-gesture ends the gesture, which closes its edit.)
        row.box->setEnabled(on);
        row.label->setEnabled(on);
        row.restore->setEnabled(on);
    }
}

void TransformPanel::resetRotation() {
    if (!m_viewport || !m_viewport->beginJointTransformEdit()) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        m_viewport->setJointTransformDial(i, 0.0);
    }
    m_viewport->endJointTransformEdit();
    refresh();
}

} // namespace pose
