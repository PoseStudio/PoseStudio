/**
 * @file renderwindow.cpp
 * @brief The Render window. See renderwindow.h.
 */

#include "renderwindow.h"

#include "constants.h"
#include "dragnumberbox.h"
#include "menupickerbutton.h"
#include "preferencesmanager.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace pose {

namespace {

/// The one Render window of the process (see RenderWindow::open).
QPointer<RenderWindow> g_window;

/// The size presets, in the picker's order; the first follows the viewport.
struct SizePreset {
    const char* label;
    int         width;
    int         height;
    bool        separatorAfter;
};
const SizePreset kSizes[] = {
    {QT_TRANSLATE_NOOP("pose::RenderWindow", "Viewport Size"), 0, 0, true},
    {QT_TRANSLATE_NOOP("pose::RenderWindow", "1280 × 720  (HD)"), 1280, 720, false},
    {QT_TRANSLATE_NOOP("pose::RenderWindow", "1920 × 1080  (Full HD)"), 1920, 1080, false},
    {QT_TRANSLATE_NOOP("pose::RenderWindow", "2560 × 1440  (QHD)"), 2560, 1440, false},
    {QT_TRANSLATE_NOOP("pose::RenderWindow", "3840 × 2160  (4K)"), 3840, 2160, true},
    {QT_TRANSLATE_NOOP("pose::RenderWindow", "1080 × 1080  (Square)"), 1080, 1080, false},
    {QT_TRANSLATE_NOOP("pose::RenderWindow", "2048 × 2048  (Square)"), 2048, 2048, true},
    {QT_TRANSLATE_NOOP("pose::RenderWindow", "1080 × 1350  (Portrait 4:5)"), 1080, 1350, false},
    {QT_TRANSLATE_NOOP("pose::RenderWindow", "1080 × 1920  (Portrait 9:16)"), 1080, 1920, false},
    {QT_TRANSLATE_NOOP("pose::RenderWindow", "2160 × 3840  (Portrait 4K)"), 2160, 3840, false},
};
constexpr int kSizeCount = static_cast<int>(sizeof(kSizes) / sizeof(kSizes[0]));

constexpr int kDefaultSamples = 256;

QString minutesSeconds(double seconds) {
    const int total = static_cast<int>(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

QString pickerStyle(const QString& objectName, int minWidthPx) {
    // (The viewport strip's field: the menus it opens are the application's own.)
    return QStringLiteral("#%1 { background-color: #252627; color: #e8e8ea; border: 1px solid #555555;"
                          "  border-radius: 4px; padding: 5px 10px; min-width: %2px; font-size: 12px; text-align: left; }"
                          "#%1:hover { background-color: #314D7A; color: #ffffff; }"
                          "#%1:disabled { color: #777777; border-color: #3a3b3c; }"
                          "#%1::menu-indicator { subcontrol-position: right center; subcontrol-origin: padding; right: 8px; }")
        .arg(objectName)
        .arg(minWidthPx);
}

} // namespace

/// The picture, fitted into the widget (never enlarged past 1:1 … unless it is smaller than the
/// view, where a small render reads better filling it) over the window's dark field; a
/// checkerboard shows through a transparent render.
class RenderImageView : public QWidget {
public:
    explicit RenderImageView(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("RenderImageView"));
        setMinimumSize(320, 200);
    }
    void setImage(const QImage& image) {
        m_image = image;
        update();
    }
    void setHint(const QString& hint) {
        m_hint = hint;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(0x14, 0x15, 0x16));
        if (m_image.isNull()) {
            painter.setPen(QColor(0x8a, 0x8b, 0x8e));
            painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, m_hint);
            return;
        }
        const QSize  fitted = m_image.size().scaled(size() - QSize(16, 16), Qt::KeepAspectRatio);
        const QRect  target(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
        if (m_image.hasAlphaChannel()) {
            const int cell = 10;
            for (int y = target.top(); y <= target.bottom(); y += cell) {
                for (int x = target.left(); x <= target.right(); x += cell) {
                    const bool light = (((x - target.left()) / cell) + ((y - target.top()) / cell)) % 2 == 0;
                    painter.fillRect(QRect(x, y, cell, cell).intersected(target), light ? QColor(0x4a, 0x4b, 0x4d) : QColor(0x38, 0x39, 0x3b));
                }
            }
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(target, m_image);
    }

private:
    QImage  m_image;
    QString m_hint;
};

RenderWindow* RenderWindow::open(QWidget* owner) {
    if (g_window.isNull()) {
        g_window = new RenderWindow(owner);
    }
    g_window->show();
    g_window->raise();
    g_window->activateWindow();
    return g_window;
}

RenderWindow* RenderWindow::existing() {
    return g_window;
}

RenderWindow::RenderWindow(QWidget* owner)
    : QWidget(owner, Qt::Window) {
    setObjectName(QStringLiteral("RenderWindow"));
    setWindowTitle(tr("Render"));
    setAttribute(Qt::WA_DeleteOnClose, false); // (one window per app: hidden, not destroyed, on close)
    resize(1280, 820);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // --- The toolbar: Render / Stop, what the render is chosen by, Save ---
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("RenderToolbar"));
    auto* bar = new QHBoxLayout(toolbar);
    bar->setContentsMargins(10, 8, 10, 8);
    bar->setSpacing(8);

    m_render = new QPushButton(tr("Render"), toolbar);
    m_render->setObjectName(QStringLiteral("RenderStartButton"));
    m_render->setToolTip(tr("Render the scene as the viewport shows it now (F12)"));
    m_render->setProperty("testId", QStringLiteral("render.start"));
    m_stop = new QPushButton(tr("Stop"), toolbar);
    m_stop->setObjectName(QStringLiteral("RenderStopButton"));
    m_stop->setToolTip(tr("Stop the render and keep the image so far (Esc)"));
    m_stop->setProperty("testId", QStringLiteral("render.stop"));
    bar->addWidget(m_render);
    bar->addWidget(m_stop);
    bar->addSpacing(10);

    const auto caption = [&](const QString& text) {
        auto* label = new QLabel(text, toolbar);
        label->setObjectName(QStringLiteral("RenderCaption"));
        bar->addWidget(label);
    };

    caption(tr("Size"));
    m_size = new MenuPickerButton(toolbar);
    m_size->setObjectName(QStringLiteral("RenderSizeButton"));
    m_size->setStyleSheet(pickerStyle(QStringLiteral("RenderSizeButton"), 168));
    m_size->setToolTip(tr("The image's size in pixels. Viewport Size renders exactly what the viewport frames."));
    m_size->setProperty("testId", QStringLiteral("render.size"));
    QList<MenuPickerButton::Item> sizes;
    for (const SizePreset& preset : kSizes) {
        sizes.append({tr(preset.label), preset.separatorAfter, QString()});
    }
    m_size->setItems(sizes);
    m_size->setCaptionPrefix(QStringLiteral("  "));
    bar->addWidget(m_size);

    caption(tr("Samples"));
    m_samples = new DragNumberBox(toolbar);
    m_samples->setRange(1, 4096);
    m_samples->setDecimals(0);
    m_samples->setSingleStep(8);
    m_samples->setFineStep(1);
    m_samples->setScrubSpan(1024);
    m_samples->setFixedWidth(84);
    m_samples->setToolTip(tr("How many light paths are traced per pixel: more is cleaner and takes longer. "
                             "With denoising on, 64 to 256 is usually enough."));
    m_samples->setProperty("testId", QStringLiteral("render.samples"));
    bar->addWidget(m_samples);

    caption(tr("Device"));
    m_device = new MenuPickerButton(toolbar);
    m_device->setObjectName(QStringLiteral("RenderDeviceButton"));
    m_device->setStyleSheet(pickerStyle(QStringLiteral("RenderDeviceButton"), 220));
    m_device->setToolTip(tr("What renders: a graphics card is many times faster than the processor."));
    m_device->setProperty("testId", QStringLiteral("render.device"));
    m_device->setCaptionPrefix(QStringLiteral("  "));
    bar->addWidget(m_device);

    m_denoise = new QCheckBox(tr("Denoise"), toolbar);
    m_denoise->setToolTip(tr("Clean the finished image of the grain that is left at this many samples"));
    m_denoise->setProperty("testId", QStringLiteral("render.denoise"));
    bar->addWidget(m_denoise);
    m_transparent = new QCheckBox(tr("Transparent"), toolbar);
    m_transparent->setToolTip(tr("Leave the background out: the saved image is transparent where the scene is not "
                                 "(PNG and EXR keep it)"));
    m_transparent->setProperty("testId", QStringLiteral("render.transparent"));
    bar->addWidget(m_transparent);

    bar->addStretch(1);
    m_save = new QPushButton(tr("Save Image…"), toolbar);
    m_save->setObjectName(QStringLiteral("RenderSaveButton"));
    m_save->setToolTip(tr("Save the image: PNG or JPEG as shown, or EXR with the renderer's full linear range (Ctrl+S)"));
    m_save->setProperty("testId", QStringLiteral("render.save"));
    bar->addWidget(m_save);
    outer->addWidget(toolbar);

    // --- The picture ---
    m_view = new RenderImageView(this);
    outer->addWidget(m_view, 1);

    // --- The status line ---
    auto* statusBar = new QWidget(this);
    statusBar->setObjectName(QStringLiteral("RenderStatusBar"));
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(12, 6, 12, 6);
    statusLayout->setSpacing(12);
    m_statusText = new QLabel(statusBar);
    m_statusText->setObjectName(QStringLiteral("RenderStatusText"));
    m_engineText = new QLabel(statusBar);
    m_engineText->setObjectName(QStringLiteral("RenderEngineText"));
    m_progress = new QProgressBar(statusBar);
    m_progress->setObjectName(QStringLiteral("RenderProgress"));
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progress->setFixedWidth(220);
    m_progress->setFixedHeight(8);
    statusLayout->addWidget(m_statusText, 1);
    statusLayout->addWidget(m_engineText);
    statusLayout->addWidget(m_progress);
    outer->addWidget(statusBar);

    buildDevices();
    loadSettings();

    connect(m_render, &QPushButton::clicked, this, &RenderWindow::startRender);
    connect(m_stop, &QPushButton::clicked, this, &RenderWindow::stopRender);
    connect(m_save, &QPushButton::clicked, this, &RenderWindow::saveImage);
    connect(m_size, &MenuPickerButton::currentChanged, this, [this](int) { storeSettings(); });
    connect(m_device, &MenuPickerButton::currentChanged, this, [this](int) { storeSettings(); });
    connect(m_samples, &DragNumberBox::editingFinished, this, [this] { storeSettings(); });
    connect(m_denoise, &QCheckBox::toggled, this, [this](bool) { storeSettings(); });
    connect(m_transparent, &QCheckBox::toggled, this, [this](bool) { storeSettings(); });

    connect(&m_session, &RenderSession::imageChanged, this, [this] { m_view->setImage(m_session.image()); });
    connect(&m_session, &RenderSession::statusChanged, this, &RenderWindow::showStatus);
    connect(&m_session, &RenderSession::ended, this, [this] {
        syncControls();
        showStatus();
    });

    m_view->setHint(tr("Press Render (F12) to render the scene as the viewport shows it."));
    syncControls();
    showStatus();
}

void RenderWindow::buildDevices() {
    m_devices = RenderSession::devices();
    QList<MenuPickerButton::Item> items;
    for (const RenderDevice& device : m_devices) {
        items.append({device.label(), device.type == renderapi::DeviceType::Cpu && m_devices.size() > 1, QString()});
    }
    if (items.isEmpty()) {
        items.append({tr("No render engine"), false, QString()});
    }
    m_device->setItems(items);
    const QString engine = RenderSession::engineName();
    m_engineText->setText(engine);
}

void RenderWindow::loadSettings() {
    const PreferencesManager& prefs = PreferencesManager::instance();
    bool                      ok = false;
    const int size = prefs.getValue(Constants::PREF_RENDER_SIZE).toString().toInt(&ok);
    m_size->setCurrentIndex(ok ? qBound(0, size, kSizeCount - 1) : 0);
    const int samples = prefs.getValue(Constants::PREF_RENDER_SAMPLES).toString().toInt(&ok);
    m_samples->setValue(ok ? qBound(1, samples, 4096) : kDefaultSamples);
    m_denoise->setChecked(prefs.getValue(Constants::PREF_RENDER_DENOISE, QStringLiteral("1")).toString() != QStringLiteral("0"));
    m_transparent->setChecked(prefs.getValue(Constants::PREF_RENDER_TRANSPARENT).toString() == QStringLiteral("1"));

    // The device chosen last, if it is still there; else the best one there is.
    QString wanted = prefs.getValue(Constants::PREF_RENDER_DEVICE).toString();
    int     index = -1;
    for (int pass = 0; pass < 2 && index < 0; ++pass) {
        for (int i = 0; i < m_devices.size(); ++i) {
            if (m_devices[i].id == wanted) {
                index = i;
                break;
            }
        }
        wanted = RenderSession::defaultDeviceId();
    }
    m_device->setCurrentIndex(qMax(index, 0));
}

void RenderWindow::storeSettings() {
    PreferencesManager& prefs = PreferencesManager::instance();
    prefs.setValue(Constants::PREF_RENDER_SIZE, QString::number(qMax(m_size->currentIndex(), 0)));
    prefs.setValue(Constants::PREF_RENDER_SAMPLES, QString::number(static_cast<int>(m_samples->value())));
    prefs.setValue(Constants::PREF_RENDER_DENOISE, m_denoise->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
    prefs.setValue(Constants::PREF_RENDER_TRANSPARENT, m_transparent->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
    const int device = m_device->currentIndex();
    if (device >= 0 && device < m_devices.size()) {
        prefs.setValue(Constants::PREF_RENDER_DEVICE, m_devices[device].id);
    }
}

QSize RenderWindow::renderSize(const RenderSceneHolder::View& view) const {
    const SizePreset& preset = kSizes[qBound(0, m_size->currentIndex(), kSizeCount - 1)];
    if (preset.width > 0) {
        return QSize(preset.width, preset.height);
    }
    // The viewport's own pixels (even numbers: every image format and encoder is happy with them).
    return QSize(qMax(16, view.width & ~1), qMax(16, view.height & ~1));
}

void RenderWindow::startRender() {
    QString error;
    if (RenderSession::engine(&error) == nullptr) {
        QMessageBox::information(this, tr("Render"),
                                 tr("This copy of PoseStudio has no render engine.\n\n%1").arg(error));
        return;
    }
    const std::shared_ptr<RenderSceneHolder> scene = m_source ? m_source() : nullptr;
    if (!scene) {
        QMessageBox::information(this, tr("Render"), tr("The viewport is not ready to be rendered yet."));
        return;
    }
    if (scene->scene().meshCount == 0) {
        QMessageBox::information(this, tr("Render"),
                                 scene->skippedSplatScenes() > 0
                                     ? tr("There is nothing here the render engine can draw: a Gaussian splat scene is shown by the "
                                          "viewport alone. Import a figure or a model to render it.")
                                     : tr("The scene is empty. Import a figure or a model to render it."));
        return;
    }
    m_skippedSplats = scene->skippedSplatScenes();

    RenderOptions     options;
    const QSize       size = renderSize(scene->view());
    const int         device = m_device->currentIndex();
    options.width = size.width();
    options.height = size.height();
    options.samples = static_cast<int>(m_samples->value());
    options.deviceId = (device >= 0 && device < m_devices.size() && m_devices[device].type != renderapi::DeviceType::Cpu)
                           ? m_devices[device].id
                           : QString();
    options.denoise = m_denoise->isChecked();
    options.transparent = m_transparent->isChecked();
    storeSettings();

    m_view->setImage(QImage());
    m_view->setHint(tr("Rendering…"));
    if (!m_session.start(scene, options, &error)) {
        m_view->setHint(tr("The render could not start."));
        QMessageBox::warning(this, tr("Render"), tr("The render could not start.\n\n%1").arg(error));
    }
    syncControls();
    showStatus();
}

void RenderWindow::stopRender() {
    m_session.stop();
}

void RenderWindow::saveImage() {
    if (!m_session.hasImage()) {
        return;
    }
    PreferencesManager& prefs = PreferencesManager::instance();
    const QString start = prefs.rememberedDirectory(Constants::PREF_LAST_RENDER_DIR,
                                                    QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    QString       filter;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Image"), QDir(start).filePath(tr("render.png")),
        tr("PNG image (*.png);;JPEG image (*.jpg *.jpeg);;OpenEXR image, linear (*.exr)"), &filter);
    if (path.isEmpty()) {
        return;
    }
    prefs.setValue(Constants::PREF_LAST_RENDER_DIR, QFileInfo(path).absolutePath());
    QString error;
    if (!m_session.save(path, &error)) {
        QMessageBox::warning(this, tr("Save Image"), tr("The image could not be saved.\n\n%1").arg(error));
    }
}

void RenderWindow::syncControls() {
    const bool running = m_session.running();
    const bool engine = !m_devices.isEmpty();
    m_render->setEnabled(engine);
    m_render->setText(running ? tr("Render Again") : tr("Render"));
    m_stop->setEnabled(running);
    m_save->setEnabled(m_session.hasImage());
    m_device->setEnabled(engine);
}

void RenderWindow::showStatus() {
    const renderapi::Status& status = m_session.status();
    QString                  text;
    switch (status.state) {
    case renderapi::State::Idle:
        text = m_devices.isEmpty() ? tr("No render engine is installed.") : tr("Ready");
        break;
    case renderapi::State::Starting:
    case renderapi::State::Rendering: {
        text = QString::fromUtf8(status.text);
        if (status.seconds > 0.5) {
            text += QStringLiteral("   ·   %1").arg(minutesSeconds(status.seconds));
        }
        break;
    }
    case renderapi::State::Finished:
        text = tr("Finished in %1   ·   %2 × %3   ·   %4 samples")
                   .arg(minutesSeconds(status.seconds))
                   .arg(m_session.options().width)
                   .arg(m_session.options().height)
                   .arg(m_session.options().samples);
        break;
    case renderapi::State::Cancelled:
        text = tr("Stopped at sample %1 of %2").arg(status.sample).arg(status.samples);
        break;
    case renderapi::State::Failed:
        text = tr("The render failed: %1").arg(QString::fromUtf8(status.error));
        break;
    }
    if (m_skippedSplats > 0 && status.state != renderapi::State::Idle) {
        text += QStringLiteral("   ·   ") + tr("Gaussian splat scenes are not rendered");
    }
    m_statusText->setText(text);
    m_progress->setValue(static_cast<int>(status.progress * 1000.0f + 0.5f));
    m_progress->setVisible(status.state == renderapi::State::Starting || status.state == renderapi::State::Rendering);
    m_save->setEnabled(m_session.hasImage());
    m_stop->setEnabled(m_session.running());
    m_render->setText(m_session.running() ? tr("Render Again") : tr("Render"));
}

void RenderWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (m_session.running()) {
            stopRender();
        } else {
            close();
        }
        return;
    }
    if (event->matches(QKeySequence::Save)) {
        saveImage();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RenderWindow::closeEvent(QCloseEvent* event) {
    // Closing the window is not stopping the render: it goes on, and the window shows it again
    // when it is reopened. (Quitting the application ends it — the session is this window's.)
    event->accept();
}

} // namespace pose
