/**
 * @file materialpanel_slots.cpp
 * @brief MaterialPanel's texture slots: the thumbnail pictures and their cache, the thumbnail
 *        buttons with their menu and file dialog, and the slots' part of the mirror. See
 *        materialpanel.h.
 */

#include "materialpanel.h"

#include "constants.h"
#include "materialthumbbutton.h"
#include "modeldata.h" // DecodedImage (the thumbnails)
#include "preferencesmanager.h"
#include "viewportwidget.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace pose {

QPixmap MaterialPanel::slotThumbnail(MaterialMap map, const std::shared_ptr<const DecodedImage>& image, int channel) {
    // (Sampled at kThumbPx and drawn by the buttons at their own, smaller side.)
    return slotPixmap(map, image, channel, kThumbPx, m_thumbs);
}

QPixmap MaterialPanel::slotPreview(MaterialMap map) {
    if (!m_viewport) {
        return {};
    }
    int                                       channel = 0;
    const std::shared_ptr<const DecodedImage> image = m_viewport->materialSlotImage(map, &channel);
    return slotPixmap(map, image, channel, kPreviewPx, m_previews);
}

QPixmap MaterialPanel::slotPixmap(MaterialMap map, const std::shared_ptr<const DecodedImage>& image, int channel, int px, ThumbCache& cache) {
    if (!image || image->width == 0 || image->height == 0) {
        return {};
    }
    // A scalar map (a roughness, a mask, a height …) is shown through the channel it is read
    // from, as grey: of a packed image, the one map this slot is.
    const bool grey = materialMapIsScalar(map);
    // By the image AND the channel it is shown through: one packed image (occlusion, roughness and
    // metalness in its red, green and blue) is the same DecodedImage in three slots, and keyed by
    // the image alone each slot's lookup threw the other's picture out, at every refresh.
    const ThumbKey key(image.get(), grey ? channel : -1);
    if (auto it = cache.find(key); it != cache.end()) {
        if (!it->second.image.expired()) {
            return it->second.pixmap;
        }
        cache.erase(it); // (that image is gone: the address is another's now)
    }
    if (cache.size() >= 64) {
        cache.clear();
    }
    // Strided nearest sample into a small image (at most four times the thumbnail each way), then
    // a smooth scale to size: a 4096-square map is read at 240-square, not 16 million texels.
    const qreal dpr = devicePixelRatioF();
    const int   target = std::max(1, static_cast<int>(std::lround(px * dpr)));
    const int   sw = static_cast<int>(std::min<uint32_t>(image->width, static_cast<uint32_t>(target * 4)));
    const int   sh = static_cast<int>(std::min<uint32_t>(image->height, static_cast<uint32_t>(target * 4)));
    QImage      small(sw, sh, QImage::Format_RGBA8888);
    const int   ch = std::clamp(channel, 0, 3);
    for (int y = 0; y < sh; ++y) {
        const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(y) * image->height) / sh);
        uchar*         dst = small.scanLine(y);
        for (int x = 0; x < sw; ++x) {
            const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(x) * image->width) / sw);
            const uint8_t* px = &image->pixels[(static_cast<size_t>(sy) * image->width + sx) * 4];
            if (grey) {
                dst[0] = dst[1] = dst[2] = px[ch];
            } else {
                dst[0] = px[0];
                dst[1] = px[1];
                dst[2] = px[2];
            }
            dst[3] = 255; // (a cutout's alpha is shown as the base color's own picture)
            dst += 4;
        }
    }
    // Filled to the square (a map is a texture, not a picture to letterbox); the button rounds
    // and frames it as it paints.
    QImage  scaled = small.scaled(target, target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    QPixmap pixmap = QPixmap::fromImage(scaled);
    cache[key] = Thumb{image, pixmap};
    return pixmap;
}

MaterialThumbButton* MaterialPanel::makeThumb(MaterialMap map, int px, int radius, const QString& tip) {
    auto* thumb = new MaterialThumbButton(px, radius);
    thumb->setToolTip(tip);
    connect(thumb, &QAbstractButton::clicked, this, [this, map]() { chooseTexture(map); });
    thumb->setPreview([this, map]() { return slotPreview(map); }, QString());
    thumb->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(thumb, &QWidget::customContextMenuRequested, this, [this, map, thumb](const QPoint& pos) { showThumbMenu(map, thumb->mapToGlobal(pos)); });
    return thumb;
}

void MaterialPanel::showThumbMenu(MaterialMap map, const QPoint& globalPos) {
    if (!m_view.valid) {
        return;
    }
    const std::string& source = m_view.params.maps[at(map)].source;
    const bool         has = m_view.maps.present[at(map)];
    QMenu              menu(this);
    QAction* choose = menu.addAction(tr("Choose Image…"));
    QAction* remove = menu.addAction(tr("Remove"));
    remove->setEnabled(has);
    QAction* restore = menu.addAction(tr("Restore Imported Map"));
    restore->setEnabled(source != m_view.imported.maps[at(map)].source);
    menu.addSeparator();
    // The map's folder in the system's file browser (Qt opens a folder portably; an embedded
    // image has none).
    const QString path = QString::fromStdString(m_view.mapPaths[at(map)]);
    QAction*      openLocation = menu.addAction(tr("Open File Location"));
    openLocation->setEnabled(has && !path.isEmpty() && QFileInfo::exists(path));
    QAction* picked = menu.exec(globalPos);
    if (picked == openLocation) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        return;
    }
    if (picked == choose) {
        chooseTexture(map);
    } else if (picked == remove) {
        setTexture(map, QString());
    } else if (picked == restore) {
        setTexture(map, QString::fromLatin1(kImportedTexture));
    }
}

void MaterialPanel::chooseTexture(MaterialMap map) {
    if (!m_viewport || !m_view.valid) {
        return;
    }
    PreferencesManager& prefs = PreferencesManager::instance();
    const QString       key = QString::fromLatin1(Constants::PREF_LAST_TEXTURE_DIR);
    const QString       start = prefs.rememberedDirectory(key, QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    const QString       slot = m_slots[at(map)].label;
    const QString       path = QFileDialog::getOpenFileName(this, tr("Choose a %1 map").arg(slot), start,
                                                            tr("Images (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.tga *.webp *.gif);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    prefs.setValue(key, QFileInfo(path).absolutePath());
    setTexture(map, path);
}

void MaterialPanel::setTexture(MaterialMap map, const QString& source) {
    if (!m_viewport || !m_view.valid) {
        return;
    }
    if (!m_viewport->setMaterialTexture(map, source) && !source.isEmpty() && source != QLatin1String(kImportedTexture)) {
        QMessageBox::warning(this, tr("Materials"), tr("The image could not be read:\n%1").arg(source));
    }
    refresh(); // (a section the map brings into use opens itself: refresh)
}

void MaterialPanel::applyScrollLever() {
    // (Developer levers for the UI shot: POSESTUDIO_MATERIALS_SCROLL_END scrolls to the bottom,
    // POSESTUDIO_MATERIALS_SCROLL=<0..1> to that fraction of the way down.)
    static const bool  scrollEnd = qEnvironmentVariableIsSet("POSESTUDIO_MATERIALS_SCROLL_END");
    static const float scrollFrac = qEnvironmentVariableIsSet("POSESTUDIO_MATERIALS_SCROLL") ? qEnvironmentVariable("POSESTUDIO_MATERIALS_SCROLL").toFloat() : -1.0f;
    if ((scrollEnd || scrollFrac >= 0.0f) && m_scroll) {
        const float frac = scrollEnd ? 1.0f : scrollFrac;
        QTimer::singleShot(0, m_scroll, [scroll = m_scroll, frac]() {
            scroll->verticalScrollBar()->setValue(static_cast<int>(std::lround(frac * scroll->verticalScrollBar()->maximum())));
        });
    }
}

void MaterialPanel::refreshSlots() {
    applyScrollLever();
    const MaterialView& view = m_view;
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
        const auto         map = static_cast<MaterialMap>(i);
        const std::string& source = view.params.maps[i].source;
        const bool         has = view.valid && view.maps.present[i];

        // A slot whose thumbnails show exactly this already is left alone: a refresh runs at
        // every step of a scrub, and of the tab's thirty-five slots a scrub changes none. What
        // they show is decided by whether there is a map, where it came from, its name — and the
        // image itself (a file chosen again is decoded again under the same name).
        int                                       channel = 0;
        const std::shared_ptr<const DecodedImage> image = has && m_viewport ? m_viewport->materialSlotImage(map, &channel) : nullptr;
        SlotShown&                                shown = m_slotShown[i];
        const bool thumbsCurrent = shown.set && shown.valid == view.valid && shown.has == has && shown.source == source &&
                                   shown.name == view.mapNames[i] && shown.image.lock() == image && shown.channel == channel;
        QLabel* const name = m_slotNames[i];
        if (thumbsCurrent && name == nullptr) {
            continue;
        }
        QString text;
        if (!view.valid) {
            text.clear();
        } else if (!has) {
            text = tr("none");
        } else {
            text = QString::fromStdString(view.mapNames[i]);
            if (text.isEmpty()) {
                text = tr("embedded");
            }
        }
        // The tooltip: what is bound (and where it came from), then what the slot is for.
        const QString bound = has ? (source == kImportedTexture ? tr("%1 (imported with the file)").arg(text) : QString::fromStdString(source))
                                  : tr("%1: none").arg(m_slots[i].label);
        const QString tip = tr("%1\n%2\nClick to choose an image file; right-click to remove it, restore the imported map or open its folder.")
                                .arg(bound, m_slots[i].tip);
        if (!thumbsCurrent) {
            const QPixmap thumbnail = has ? slotThumbnail(map, image, channel) : QPixmap();
            for (MaterialThumbButton* small : m_rowThumbs[i]) {
                small->setImage(thumbnail);
                small->setCaption(text); // (under the hover preview, whose provider makeThumb set)
                small->setEnabled(view.valid);
                small->setToolTip(tip);
            }
            shown.set = true;
            shown.valid = view.valid;
            shown.has = has;
            shown.source = source;
            shown.name = view.mapNames[i];
            shown.image = image;
            shown.channel = channel;
        }
        // (A slot row's name is set every time: it is elided by its label's font as that stands.)
        if (name != nullptr) {
            const QFontMetrics metrics(name->font());
            name->setText(metrics.elidedText(text, Qt::ElideMiddle, 150));
            name->setToolTip(tip);
            name->setEnabled(view.valid);
        }
    }
}

} // namespace pose
