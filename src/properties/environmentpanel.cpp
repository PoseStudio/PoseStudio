/**
 * @file environmentpanel.cpp
 * @brief Implementation of EnvironmentPanel. See environmentpanel.h.
 */

#include "environmentpanel.h"

#include "dragnumberbox.h"
#include "librarypaths.h"
#include "viewportwidget.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>

namespace pose {

namespace {
// Builds a titled QGroupBox with a QFormLayout inside, appended to @p parent; returns the form.
QFormLayout* addGroup(QVBoxLayout* parent, const QString& title) {
    auto* box = new QGroupBox(title);
    box->setObjectName(QStringLiteral("EnvironmentGroup"));
    auto* form = new QFormLayout(box);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    parent->addWidget(box);
    return form;
}

// Extensions an .hdr's menu thumbnail may use: a same-basename image file next to the panorama
// (the bundled set ships .webp previews; users can pair any of these). Same pairing rule as the
// Asset Manager's thumbnail matching, first hit wins.
constexpr const char* kIconExtensions[] = {"webp", "png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff"};

// Thumbnail size (2:1 like the equirect panoramas) and how many rows the drop-down shows before
// scrolling. The list inside the menu is a real QListWidget, so beyond kHdriVisibleRows the rest
// of the collection is reached with an ordinary scroll bar.
constexpr int kHdriIconW = 178;
constexpr int kHdriIconH = 89;
constexpr int kHdriVisibleRows = 6;

// The menu thumbnail for @p baseName: the first same-basename image file found in @p dir, decoded
// at thumbnail size (the codec does the reduction — same trick as the Asset Manager grid). An .hdr
// with no paired image simply gets no icon.
QIcon hdriIcon(const QDir& dir, const QString& baseName) {
    for (const char* ext : kIconExtensions) {
        const QString candidate = dir.filePath(baseName + QLatin1Char('.') + QLatin1String(ext));
        if (!QFileInfo::exists(candidate)) {
            continue;
        }
        QImageReader reader(candidate);
        reader.setAutoTransform(true);
        const QSize orig = reader.size();
        if (orig.isValid()) {
            reader.setScaledSize(orig.scaled(kHdriIconW, kHdriIconH, Qt::KeepAspectRatio));
        }
        const QImage img = reader.read();
        if (!img.isNull()) {
            return QIcon(QPixmap::fromImage(img));
        }
        // Undecodable (e.g. .webp on a Qt install without the optional Image Formats add-on):
        // fall through and try the next extension rather than giving up on this panorama.
    }
    return QIcon();
}
} // namespace

EnvironmentPanel::EnvironmentPanel(ViewportWidget* viewport, QWidget* parent)
    : QWidget(parent), m_viewport(viewport) {
    setObjectName(QStringLiteral("EnvironmentPanel"));
    buildUi();
}

DragNumberBox* EnvironmentPanel::addValueRow(QFormLayout* form, const QString& label, double min,
                                             double max, double step, double value) {
    auto* box = new DragNumberBox;
    box->setRange(min, max);
    box->setSingleStep(step);
    box->setDecimals(step >= 1.0 ? 0 : 2);
    box->setValue(value);
    form->addRow(label, box);
    return box;
}

void EnvironmentPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("EnvironmentContent"));
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(12, 12, 12, 12);
    col->setSpacing(12);

    // --- Environment ---
    QFormLayout* env = addGroup(col, tr("Environment"));
    buildEnvironmentSelector(env);
    m_rotation = addValueRow(env, tr("Rotation°"), 0.0, 360.0, 1.0, m_settings.environmentRotationDeg);

    // --- Exposure & Tone ---
    QFormLayout* tone = addGroup(col, tr("Exposure && Tone")); // && — a lone & becomes an accelerator underline
    m_exposure = addValueRow(tone, tr("Exposure"), 0.0, 3.0, 0.01, m_settings.exposure);
    m_tonemap = new QCheckBox(tr("ACES filmic tonemap"));
    m_tonemap->setChecked(m_settings.tonemap);
    tone->addRow(QString(), m_tonemap);

    // --- Image-Based Lighting ---
    QFormLayout* ibl = addGroup(col, tr("Image-Based Lighting"));
    m_diffuse = addValueRow(ibl, tr("Diffuse"), 0.0, 3.0, 0.01, m_settings.diffuseIntensity);
    m_specular = addValueRow(ibl, tr("Specular"), 0.0, 2.0, 0.01, m_settings.specularIntensity);
    m_ambientFill = addValueRow(ibl, tr("Ambient fill"), 0.0, 1.0, 0.01, m_settings.ambientFill);

    // --- Key Light ---
    QFormLayout* key = addGroup(col, tr("Key Light"));
    m_keyIntensity = addValueRow(key, tr("Intensity"), 0.0, 5.0, 0.05, m_settings.keyIntensity);
    m_keyAzimuth = addValueRow(key, tr("Azimuth°"), -180.0, 180.0, 1.0, m_settings.keyAzimuthDeg);
    m_keyElevation = addValueRow(key, tr("Elevation°"), 0.0, 85.0, 1.0, m_settings.keyElevationDeg);

    // --- Skin & Rim (PBR-mode accents) ---
    QFormLayout* accents = addGroup(col, tr("Skin && Rim"));
    m_subsurface = addValueRow(accents, tr("Subsurface"), 0.0, 1.0, 0.01, m_settings.subsurface);
    m_rim = addValueRow(accents, tr("Rim light"), 0.0, 1.5, 0.01, m_settings.rimIntensity);

    // --- Reset ---
    auto* reset = new QPushButton(tr("Reset to defaults"));
    reset->setObjectName(QStringLiteral("EnvironmentResetButton"));
    col->addWidget(reset, 0, Qt::AlignLeft);

    col->addStretch(1);
    scroll->setWidget(content);

    // --- Wiring: every control updates m_settings and pushes it live to the viewport ---
    connect(m_rotation, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.environmentRotationDeg = static_cast<float>(d);
        pushSettings();
    });
    connect(m_exposure, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.exposure = static_cast<float>(d);
        pushSettings();
    });
    connect(m_tonemap, &QCheckBox::toggled, this, [this](bool on) {
        m_settings.tonemap = on;
        pushSettings();
    });
    connect(m_diffuse, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.diffuseIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(m_specular, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.specularIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(m_ambientFill, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.ambientFill = static_cast<float>(d);
        pushSettings();
    });
    connect(m_keyIntensity, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.keyIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(m_keyAzimuth, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.keyAzimuthDeg = static_cast<float>(d);
        pushSettings();
    });
    connect(m_keyElevation, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.keyElevationDeg = static_cast<float>(d);
        pushSettings();
    });
    connect(m_subsurface, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.subsurface = static_cast<float>(d);
        pushSettings();
    });
    connect(m_rim, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.rimIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(reset, &QPushButton::clicked, this, &EnvironmentPanel::resetToDefaults);
}

void EnvironmentPanel::buildEnvironmentSelector(QFormLayout* form) {
    m_envButton = new QPushButton;
    m_envButton->setObjectName(QStringLiteral("EnvironmentHdriButton"));
    m_envButton->setCursor(Qt::PointingHandCursor);
    // Qt's style engine ignores padding-left for a left-aligned QPushButton label (the text is laid
    // out from the border box regardless of the QSS padding), so a transparent spacer icon provides
    // the left inset for the environment name instead — icon + icon/text spacing ≈ a 16px indent.
    QPixmap spacer(10, 10);
    spacer.fill(Qt::transparent);
    m_envButton->setIcon(QIcon(spacer));
    m_envButton->setIconSize(QSize(10, 10));
    m_envMenu = new QMenu(m_envButton);
    // The object name keys AppProxyStyle's PM_SmallIconSize override (a larger icon column for the
    // thumbnails) and lets the menu inherit the app-wide QMenu styling like every other menu.
    m_envMenu->setObjectName(QStringLiteral("HdriMenu"));
    m_envButton->setMenu(m_envMenu);
    // Rebuild on every open so .hdr files added/removed while the app runs show up immediately.
    connect(m_envMenu, &QMenu::aboutToShow, this, &EnvironmentPanel::rebuildEnvironmentMenu);
    form->addRow(tr("HDRI"), m_envButton);

    // Initial selection mirrors the viewport's startup default (VulkanWindow::defaultEnvironmentPath:
    // the stock panorama by name, else the first .hdr) without firing a redundant re-load — the
    // viewport already loads it itself.
    const QDir hdriDir(LibraryPaths::hdriDirectory());
    const QStringList files =
        hdriDir.entryList(QStringList{QStringLiteral("*.hdr")}, QDir::Files, QDir::Name);
    QString initial;
    for (const QString& file : files) {
        if (file.compare(QStringLiteral("Golden Gate Hills.hdr"), Qt::CaseInsensitive) == 0) {
            initial = file;
            break;
        }
    }
    if (initial.isEmpty() && !files.isEmpty()) {
        initial = files.first();
    }
    if (!initial.isEmpty()) {
        m_currentEnvPath = hdriDir.filePath(initial);
        m_envButton->setText(QFileInfo(initial).completeBaseName());
    } else {
        m_envButton->setText(tr("Procedural Studio")); // no panoramas yet — the built-in fallback
    }
}

void EnvironmentPanel::rebuildEnvironmentMenu() {
    m_envMenu->clear(); // deletes the previous QWidgetAction and its hosted widgets

    const QDir dir(LibraryPaths::hdriDirectory());
    const QStringList files =
        dir.entryList(QStringList{QStringLiteral("*.hdr")}, QDir::Files, QDir::Name);

    // The menu hosts one QWidgetAction whose widget is a scrollable thumbnail list plus a footer
    // link — a plain QMenu can't show a real scroll bar or cap itself at N rows, a QListWidget can.
    auto* container = new QWidget(m_envMenu);
    auto* column = new QVBoxLayout(container);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);

    if (!files.isEmpty()) {
        auto* list = new QListWidget(container);
        list->setObjectName(QStringLiteral("HdriList"));
        list->setIconSize(QSize(kHdriIconW, kHdriIconH));
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setUniformItemSizes(true);
        list->setFrameShape(QFrame::NoFrame);
        list->setMouseTracking(true); // menu-like hover highlight (see _environment.qss)

        for (const QString& file : files) {
            const QString base = QFileInfo(file).completeBaseName();
            const QString path = dir.filePath(file);
            auto* item = new QListWidgetItem(hdriIcon(dir, base), base);
            item->setData(Qt::UserRole, path);
            list->addItem(item);
            if (path.compare(m_currentEnvPath, Qt::CaseInsensitive) == 0) {
                list->setCurrentItem(item); // highlight the active environment
            }
        }

        // Exactly kHdriVisibleRows rows tall (fewer if the collection is smaller); the scroll bar
        // covers the rest. Width fits thumbnail + name + the scroll bar.
        const int rowHeight = list->sizeHintForRow(0);
        const int visible = std::min<int>(kHdriVisibleRows, list->count());
        list->setFixedHeight(visible * rowHeight + 4);
        const int scrollBarW =
            list->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, list);
        list->setFixedWidth(list->sizeHintForColumn(0) + scrollBarW + 8);
        if (list->currentItem() != nullptr) {
            list->scrollToItem(list->currentItem(), QAbstractItemView::PositionAtCenter);
        }

        connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            chooseEnvironment(item->data(Qt::UserRole).toString(), item->text());
            m_envMenu->close();
        });
        column->addWidget(list);
    } else {
        auto* none = new QLabel(tr("No .hdr files found"), container);
        none->setObjectName(QStringLiteral("HdriEmptyLabel"));
        none->setContentsMargins(8, 4, 8, 4);
        column->addWidget(none);
    }

    // Footer: take the user straight to where panoramas live, so adding one is a drop away.
    auto* open = new QPushButton(tr("Open HDRI Folder…"), container);
    open->setObjectName(QStringLiteral("HdriFolderLink"));
    open->setCursor(Qt::PointingHandCursor);
    open->setFlat(true);
    connect(open, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(LibraryPaths::hdriDirectory()));
        m_envMenu->close();
    });
    column->addWidget(open);

    auto* hostAction = new QWidgetAction(m_envMenu);
    hostAction->setDefaultWidget(container);
    m_envMenu->addAction(hostAction);
}

void EnvironmentPanel::chooseEnvironment(const QString& path, const QString& name) {
    m_currentEnvPath = path;
    m_envButton->setText(name);
    if (m_viewport) {
        m_viewport->setEnvironment(path);
    }
}

void EnvironmentPanel::pushSettings() {
    if (m_viewport) {
        m_viewport->setLightingSettings(m_settings);
    }
}

void EnvironmentPanel::resetToDefaults() {
    const LightingSettings defaults;
    // Setting the widgets (unblocked) fires each control's handler, which rebuilds m_settings from the
    // defaults and pushes — so the viewport lands back on the default look.
    m_rotation->setValue(defaults.environmentRotationDeg);
    m_exposure->setValue(defaults.exposure);
    m_tonemap->setChecked(defaults.tonemap);
    m_diffuse->setValue(defaults.diffuseIntensity);
    m_specular->setValue(defaults.specularIntensity);
    m_ambientFill->setValue(defaults.ambientFill);
    m_keyIntensity->setValue(defaults.keyIntensity);
    m_keyAzimuth->setValue(defaults.keyAzimuthDeg);
    m_keyElevation->setValue(defaults.keyElevationDeg);
    m_subsurface->setValue(defaults.subsurface);
    m_rim->setValue(defaults.rimIntensity);
    pushSettings(); // covers the case where every widget was already at its default (no signals fired)
}

} // namespace pose
