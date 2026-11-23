// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file jointmodebadge.cpp
 * @brief Implementation of JointModeBadge. See jointmodebadge.h.
 */

#include "jointmodebadge.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QString>

#include <algorithm>
#include <cmath>

namespace pose {

namespace {
constexpr qreal kGlyphLeft = 9.0;  // the arrow glyph's inset from the badge's left edge
constexpr qreal kTextGap = 8.0;    // between the glyph and the name
constexpr qreal kTextRight = 10.0; // the name's inset from the right edge

QFont badgeFont(const QFont& base) {
    QFont f = base;
    f.setBold(true);
    return f;
}
} // namespace

QString JointModeBadge::modeName(int kind) {
    switch (kind) {
    case 0:  return tr("Bend");
    case 1:  return tr("Side-Side");
    case 2:  return tr("Twist");
    default: return QString();
    }
}

JointModeBadge::JointModeBadge(QWidget* parent, int height) : QWidget(parent) {
    // Wide enough for the longest of the three names: the badge does not change size between
    // modes (the strip is re-anchored only when it appears and disappears).
    const QFontMetrics fm(badgeFont(font()));
    int                text = 0;
    for (int k = 0; k < 3; ++k) {
        text = std::max(text, fm.horizontalAdvance(modeName(k)));
    }
    const int glyph = static_cast<int>(std::ceil(height * 0.52));
    setFixedSize(static_cast<int>(kGlyphLeft + glyph + kTextGap + text + kTextRight), height);
    setToolTip(tr("Move the mouse to turn the selected joint. Click to keep the result; "
                  "Esc or a right-click puts the joint back."));
}

void JointModeBadge::setMode(int kind) {
    m_kind = kind;
    update();
}

void JointModeBadge::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QColor(0x55, 0x55, 0x55));
    p.setBrush(QColor(0x25, 0x26, 0x27));
    p.drawRoundedRect(r, 4.0, 4.0);
    const QString name = modeName(m_kind);
    if (name.isEmpty()) {
        return;
    }
    const QColor c(0x5b, 0x87, 0xcc); // the accent (Constants::COLOR_ACCENT)

    // Rotation glyph: a 270° arc (counter-clockwise from 45°) with an arrowhead on its end.
    const qreal  d = r.height() * 0.52;
    const QRectF arc(r.left() + kGlyphLeft, r.center().y() - d * 0.5, d, d);
    QPen         pen(c, 2.0);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(arc, 45 * 16, 270 * 16);
    const qreal   endDeg = 45.0 + 270.0; // Qt angles: 0° = 3 o'clock, counter-clockwise
    const qreal   endRad = endDeg * 3.14159265 / 180.0;
    const QPointF tip(arc.center().x() + std::cos(endRad) * d * 0.5,
                      arc.center().y() - std::sin(endRad) * d * 0.5);
    const QPointF tangent(-std::sin(endRad), -std::cos(endRad)); // direction of travel (screen)
    const QPointF normal(-tangent.y(), tangent.x());
    const QPointF base = tip - tangent * 5.0;
    QPainterPath  head;
    head.moveTo(tip + tangent * 1.5);
    head.lineTo(base + normal * 3.5);
    head.lineTo(base - normal * 3.5);
    head.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPath(head);

    // The dial's name.
    p.setFont(badgeFont(font()));
    p.setPen(c);
    p.drawText(QRectF(arc.right() + kTextGap, r.top(), r.right() - arc.right() - kTextGap, r.height()),
               Qt::AlignVCenter | Qt::AlignLeft, name);
}

} // namespace pose
