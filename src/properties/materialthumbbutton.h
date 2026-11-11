/**
 * @file materialthumbbutton.h
 * @brief The Materials tab's MAP THUMBNAIL: a small square button showing an image in a rounded
 *        frame, with a larger preview after the pointer rests on it.
 *
 * Shared by the tab's texture slots (materialpanel.cpp) and its overlay cards (overlaystack.cpp):
 * a click is the owner's "choose an image", a right-click its context menu (the owner connects
 * both), the hover preview asks the owner for the picture only when it is about to be shown.
 * Header-only: a QAbstractButton with no signals of its own.
 */

#ifndef MATERIALTHUMBBUTTON_H
#define MATERIALTHUMBBUTTON_H

#include <QAbstractButton>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHideEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QPixmap>
#include <QScreen>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <utility>

namespace pose {

/// The thumbnail's side in logical pixels.
inline constexpr int kThumbPx = 39;
/// … and its corner radius.
inline constexpr int kThumbRadiusPx = 6;
/// The line items' small thumbnail (beside the value its map scales) and its radius.
inline constexpr int kThumbSmallPx = 24;
inline constexpr int kThumbSmallRadiusPx = 4;
/// The hover preview: the map at this size, after this delay.
inline constexpr int kPreviewPx = 240;
inline constexpr int kPreviewDelayMs = 450;

/// THE THUMBNAIL BUTTON: the map, the rounded frame and its hover drawn in ONE pass — the image
/// clipped to the rounded rect and the 1 px border stroked over the clip's edge with the same
/// path, so the two antialiased edges can never disagree (a QSS border on the button around a
/// separately rounded icon left the background peeking through at the corners and the image
/// bleeding over the frame). Empty: the dark field inside the frame.
class MaterialThumbButton : public QAbstractButton {
public:
    MaterialThumbButton(int px, int radius, QWidget* parent = nullptr) : QAbstractButton(parent), m_px(px), m_radius(radius) {
        setFixedSize(px, px);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }
    ~MaterialThumbButton() override { delete m_popup; }
    /// The map as a plain square pixmap (kThumbPx at the device pixel ratio), or a null pixmap.
    void setImage(const QPixmap& pixmap) {
        m_image = pixmap;
        update();
    }
    /// THE HOVER PREVIEW: @p provider gives the map at kPreviewPx (asked only when the pointer has
    /// rested on the button for kPreviewDelayMs), @p caption names it under the picture.
    void setPreview(std::function<QPixmap()> provider, const QString& caption) {
        m_preview = std::move(provider);
        m_caption = caption;
    }
    QSize sizeHint() const override { return QSize(m_px, m_px); }

protected:
    void enterEvent(QEnterEvent* event) override {
        QAbstractButton::enterEvent(event);
        if (m_preview && !m_image.isNull()) {
            if (!m_timer) {
                m_timer = new QTimer(this);
                m_timer->setSingleShot(true);
                connect(m_timer, &QTimer::timeout, this, [this]() { showPreview(); });
            }
            m_timer->start(kPreviewDelayMs);
        }
    }
    void leaveEvent(QEvent* event) override {
        QAbstractButton::leaveEvent(event);
        hidePreview();
    }
    void mousePressEvent(QMouseEvent* event) override {
        hidePreview();
        QAbstractButton::mousePressEvent(event);
    }
    void hideEvent(QHideEvent* event) override {
        hidePreview();
        QAbstractButton::hideEvent(event);
    }

private:
    /// The preview window: a tool-tip-class top level (never activated, above everything) painting
    /// the map with the thumbnails' one-pass frame and the caption under it.
    class Popup : public QWidget {
    public:
        Popup() : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint) {
            setAttribute(Qt::WA_ShowWithoutActivating);
            setObjectName(QStringLiteral("MaterialTexturePreview"));
        }
        void setContent(const QPixmap& pixmap, const QString& caption) {
            m_pixmap = pixmap;
            m_caption = caption;
            const QFontMetrics metrics(font());
            m_captionHeight = caption.isEmpty() ? 0 : metrics.height() + 4;
            const QSize logical = pixmap.isNull() ? QSize(kPreviewPx, kPreviewPx) : pixmap.deviceIndependentSize().toSize();
            resize(logical.width() + 2 * kPad, logical.height() + 2 * kPad + m_captionHeight);
        }

    protected:
        void paintEvent(QPaintEvent*) override {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.fillRect(rect(), QColor(0x25, 0x26, 0x27));
            painter.setPen(QPen(QColor(0x3a, 0x3b, 0x3d), 1.0));
            painter.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
            const QSize  logical = m_pixmap.deviceIndependentSize().toSize();
            const QRectF image(kPad, kPad, logical.width(), logical.height());
            QPainterPath path;
            path.addRoundedRect(image.adjusted(0.5, 0.5, -0.5, -0.5), kThumbRadiusPx, kThumbRadiusPx);
            painter.save();
            painter.setClipPath(path);
            painter.drawPixmap(image.toRect(), m_pixmap);
            painter.restore();
            painter.setPen(QPen(QColor(0x3a, 0x3b, 0x3d), 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            if (m_captionHeight > 0) {
                painter.setPen(QColor(0xc7, 0xc8, 0xcb));
                const QFontMetrics metrics(font());
                const QRect        text(kPad, kPad + logical.height() + 2, logical.width(), m_captionHeight);
                painter.drawText(text, Qt::AlignHCenter | Qt::AlignVCenter, metrics.elidedText(m_caption, Qt::ElideMiddle, logical.width()));
            }
        }

    private:
        static constexpr int kPad = 8;
        QPixmap              m_pixmap;
        QString              m_caption;
        int                  m_captionHeight = 0;
    };

    void showPreview() {
        if (!underMouse() || !m_preview) {
            return;
        }
        const QPixmap pixmap = m_preview();
        if (pixmap.isNull()) {
            return;
        }
        if (!m_popup) {
            m_popup = new Popup;
        }
        m_popup->setContent(pixmap, m_caption);
        // Beside the button — to its left, where the panel's open space is; to its right when the
        // screen ends there — centred on it and kept on the screen.
        const QRect  button(mapToGlobal(QPoint(0, 0)), size());
        const QScreen* screen = this->screen() ? this->screen() : QGuiApplication::primaryScreen();
        const QRect  avail = screen ? screen->availableGeometry() : QRect(0, 0, 10000, 10000);
        int          x = button.left() - m_popup->width() - 6;
        if (x < avail.left()) {
            x = button.right() + 7;
        }
        int y = button.center().y() - m_popup->height() / 2;
        y = std::clamp(y, avail.top(), std::max(avail.top(), avail.bottom() - m_popup->height()));
        m_popup->move(x, y);
        m_popup->show();
    }
    void hidePreview() {
        if (m_timer) {
            m_timer->stop();
        }
        if (m_popup) {
            m_popup->hide();
        }
    }

    std::function<QPixmap()> m_preview;
    QString                  m_caption;
    QTimer*                  m_timer = nullptr;
    Popup*                   m_popup = nullptr;

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        // The frame's path runs through the middle of the 1 px border so the stroke lands on
        // whole device pixels; the image fills the path to its outer edge, under the stroke.
        const QRectF outer = QRectF(rect());
        const QRectF frame = outer.adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(frame, m_radius, m_radius);
        painter.fillPath(path, QColor(0x1f, 0x20, 0x22));
        if (!m_image.isNull()) {
            painter.save();
            painter.setClipPath(path);
            painter.setOpacity(isEnabled() ? 1.0 : 0.4);
            painter.drawPixmap(outer.toRect(), m_image);
            painter.restore();
        }
        const QColor border = !isEnabled() ? QColor(0x2e, 0x2f, 0x31)
                              : (underMouse() || isDown()) ? QColor(0x5b, 0x87, 0xcc)
                                                           : QColor(0x3a, 0x3b, 0x3d);
        painter.setPen(QPen(border, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    int     m_px;
    int     m_radius;
    QPixmap m_image;
};

} // namespace pose

#endif // MATERIALTHUMBBUTTON_H
