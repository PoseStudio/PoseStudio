// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
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
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace pose {

namespace {

/// The Figure group's position rows, in centimetres. The RANGE is only a bound on what can be
/// typed — a figure walked or carried far from where it was imported, a splat scene placed a
/// street away, must be shown where it stands: a box that clamped what it shows (at five metres,
/// once) read 500 for a figure at 620, and the next scrub, which sets an absolute position from
/// the value shown, moved her 1.2 m on its first pixel. A full sweep of the box is ten metres
/// whatever the range (the Lights tab's position rows are built the same way).
constexpr double kFigurePositionRangeCm = 100000.0;
constexpr double kFigurePositionScrubCm = 1000.0;

} // namespace

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
    const propertyrows::ScrollColumn tab = propertyrows::makeScrollColumn(this, QStringLiteral("TransformContent"), 12);
    QVBoxLayout*                     col = tab.column;

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
        // Centimetres to a tenth, whole degrees, whole percent of the imported size.
        const QString&              label = figureLabels[static_cast<std::size_t>(i)];
        const QString&              tip = figureTips[static_cast<std::size_t>(i)];
        const propertyrows::ScrubRow built = i < 3   ? propertyrows::makeScrubRow(label, tip, -kFigurePositionRangeCm, kFigurePositionRangeCm, 1.0, 1)
                                             : i < 6 ? propertyrows::makeScrubRow(label, tip, -180.0, 180.0, 1.0, 0)
                                                     : propertyrows::makeScrubRow(label, tip, 1.0, 2000.0, 5.0, 0);
        Row& row = m_figureRows[static_cast<std::size_t>(i)];
        row.label = built.label;
        row.box = built.box;
        row.restore = built.restore;
        row.box->setOrigin(i < 6 ? 0.0 : 100.0); // (as imported)
        if (i < 3) {
            row.box->setScrubSpan(kFigurePositionScrubCm);
        }
        row.box->setProperty("testId", QStringLiteral("transform.figure")); // (the scripted test's `ui`: row i)
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
        figure->addRow(row.label, built.field);
    }

    // --- Rotation: the three dials, in JointDialKind order ---
    QFormLayout* rotation = propertyrows::addGroup(col, tr("Rotation"));
    const std::array<QString, 3> labels = {tr("Bend"), tr("Side-Side"), tr("Twist")};
    const std::array<QString, 3> tips = {
        tr("Bends the joint in its natural direction. 0 is the rest pose, 100 is fully bent; "
           "negative values bend it the other way, as far as the joint allows.\n"
           "In the viewport: press B and move the mouse.\n"
           "With several joints selected (Shift+click), all of them turn by the same amount."),
        tr("Bends the joint in its secondary direction. 0 is the rest pose, 100 is the joint's limit; "
           "negative values bend it the other way, as far as the joint allows.\n"
           "In the viewport: press S and move the mouse.\n"
           "With several joints selected (Shift+click), all of them turn by the same amount."),
        tr("Turns the joint about its own length. 0 is the rest pose, 100 is the joint's limit; "
           "negative values turn it the other way, as far as the joint allows.\n"
           "In the viewport: press T and move the mouse.\n"
           "With several joints selected (Shift+click), all of them turn by the same amount."),
    };
    for (int i = 0; i < 3; ++i) {
        const propertyrows::ScrubRow built =
            propertyrows::makeScrubRow(labels[static_cast<std::size_t>(i)], tips[static_cast<std::size_t>(i)], -100.0, 100.0, 1.0, 0);
        Row& row = m_rows[static_cast<std::size_t>(i)];
        row.label = built.label;
        row.box = built.box;
        row.restore = built.restore;
        row.box->setOrigin(0.0); // the rest pose: the bar fills from it, either way
        row.box->setProperty("testId", QStringLiteral("transform.dial")); // (the scripted test's `ui`: 0 Bend, 1 Side-Side, 2 Twist)
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
        row.restore->setToolTip(tr("Back to the rest pose (every selected joint's, with several selected)"));
        connect(row.restore, &QToolButton::clicked, this, [this, i]() {
            if (m_viewport && m_viewport->beginJointTransformEdit()) {
                m_viewport->setJointTransformDial(i, 0.0, true); // (every selected joint's, with a group)
                m_viewport->endJointTransformEdit(); // registers no entry if the dial was already at 0
                refresh();
            }
        });

        rotation->addRow(row.label, built.field);
    }

    m_hint = new QLabel(tr("Click a joint on a figure to select it. Its rotation can then be "
                           "set here."));
    m_hint->setObjectName(QStringLiteral("TransformHint"));
    m_hint->setWordWrap(true);
    col->addWidget(m_hint);

    // --- Reset ---
    m_reset = new QPushButton(tr("Reset All"));
    m_reset->setObjectName(QStringLiteral("PropertyResetButton"));
    m_reset->setToolTip(tr("Returns all three dials to 0: the selected joint's rest pose (every selected joint's, with several selected)"));
    col->addWidget(m_reset, 0, Qt::AlignLeft);
    connect(m_reset, &QPushButton::clicked, this, &TransformPanel::resetRotation);

    col->addStretch(1);
    tab.finish();
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
    // With a Shift+click selection the dials turn every joint of it: the name says so.
    const QString name = QString::fromStdString(state.name);
    m_jointName->setText(!state.valid            ? tr("No joint selected")
                         : state.groupCount == 1 ? tr("%1 + 1 more joint").arg(name)
                         : state.groupCount > 1 ? tr("%1 + %2 more joints").arg(name).arg(state.groupCount)
                                                : name);
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
        m_viewport->setJointTransformDial(i, 0.0, true); // (every selected joint's, with a group)
    }
    m_viewport->endJointTransformEdit();
    refresh();
}

} // namespace pose
