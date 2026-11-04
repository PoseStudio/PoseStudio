/**
 * @file viewportstrip.cpp
 * @brief Implementation of ViewportStrip. See viewportstrip.h.
 *
 * Both pickers are pose::MenuPickerButton — a push button opening a REAL QMenu — never a
 * QComboBox: that is the one way the dropdown looks and behaves exactly like the File/Edit/Help
 * menus (it inherits the global QMenu QSS in _menumanager.qss); see menupickerbutton.h for why
 * a styled QComboBox popup was tried and abandoned. The shader picker is a pure rendering of
 * scene/shademode.h's table (names, default, group separators); the view picker renders the
 * one view table below, which also drives its caption.
 */

#include "viewportstrip.h"

#include "jointmodebadge.h"
#include "menupickerbutton.h"
#include "scene/shademode.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QList>
#include <QMenu>
#include <QPoint>
#include <QPushButton>
#include <QSize>
#include <QVBoxLayout>

namespace pose {

namespace {

// The pickers' field style (the closed selector): the menu-bar surface + hover; the QMenu each
// opens is themed globally. text-align:left so the label sits left of the drop arrow, like a
// combo field. No cursor override on the strip's controls: Windows apps keep the standard arrow
// over buttons (the hand cursor reads as a hyperlink there).
QString pickerStyle(const QString& objectName, int minWidthPx) {
    return QStringLiteral("#%1 {"
                          "  background-color: #252627;"
                          "  color: #e8e8ea;"
                          "  border: 1px solid #555555;"
                          "  border-radius: 4px;"
                          "  padding: 5px 10px;"
                          "  min-width: %2px;"
                          "  font-size: 12px;"
                          "  text-align: left;"
                          "}"
                          "#%1:hover { background-color: #314D7A; color: #ffffff; }"
                          "#%1::menu-indicator { subcontrol-position: right center;"
                          "  subcontrol-origin: padding; right: 8px; }")
        .arg(objectName)
        .arg(minWidthPx);
}

// The icon buttons' style: the same surface/hover as the picker fields so the strip reads as
// one control. A checkable button (Skeleton) shows its state in the accent and takes a quieter
// hover so the two states stay distinguishable.
QString iconButtonStyle(const QString& objectName, bool checkable) {
    QString style = QStringLiteral("#%1 {"
                                   "  background-color: #252627;"
                                   "  border: 1px solid #555555;"
                                   "  border-radius: 4px;"
                                   "}")
                        .arg(objectName);
    if (checkable) {
        style += QStringLiteral("#%1:hover { background-color: #3a3d40; }"
                                "#%1:checked { background-color: #314D7A; }")
                     .arg(objectName);
    } else {
        style += QStringLiteral("#%1:hover { background-color: #314D7A; }").arg(objectName);
    }
    return style;
}

// Builds one square icon button squared off to the field height, so the strip aligns whatever
// the font metrics work out to.
QPushButton* makeIconButton(QWidget* parent, const QString& objectName, const QString& iconPath,
                            const QString& toolTip, int fieldHeight, bool checkable) {
    auto* button = new QPushButton(parent);
    button->setObjectName(objectName);
    button->setToolTip(toolTip);
    button->setIcon(QIcon(iconPath));
    button->setIconSize(QSize(16, 16));
    button->setCheckable(checkable);
    button->setStyleSheet(iconButtonStyle(objectName, checkable));
    button->setFixedSize(fieldHeight, fieldHeight);
    return button;
}

// The View picker's table: menu order, the preset each row selects, and the group separators.
// The same table supplies the caption for a preset (setViewPreset), so the menu label and the
// field label can't disagree. Free has no row — it is the detached "Perspective View" caption.
struct ViewEntry {
    const char* label;
    ViewPreset  preset;
    bool        separatorAfter;
};
constexpr ViewEntry kViewEntries[] = {
    {"Home View", ViewPreset::Home, true},      {"Top View", ViewPreset::Top, false},
    {"Bottom View", ViewPreset::Bottom, true},  {"Front View", ViewPreset::Front, false},
    {"Back View", ViewPreset::Back, true},      {"Left View", ViewPreset::Left, false},
    {"Right View", ViewPreset::Right, false},
};
constexpr int kViewEntryCount = static_cast<int>(sizeof(kViewEntries) / sizeof(kViewEntries[0]));

// Both picker tables (the shade modes in scene/shademode.h and the view entries above) are
// looked up at display time via QCoreApplication::translate("ViewportStrip", ...), so no
// source literal marks their names for the extraction tools. These no-ops keep every name
// present in a generated .ts catalog - without them an lupdate run would report the 25
// names as vanished and lrelease would drop their translations.
static const char* const kPickerNoops[] = {
    // Shade-mode picker names (scene/shademode.h's kShadeModes, 18 entries):
    QT_TRANSLATE_NOOP("ViewportStrip", "PBR Shaded"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Texture Shaded"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Flat Texture Shaded"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Cartoon Shaded"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Matcap"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Clay Shaded"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Lighting Only"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Silhouette"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Wireframe"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Hidden Line Wireframe"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Clay Shaded Wireframe"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Texture Shaded Wireframe"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Albedo"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Ambient Occlusion"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Roughness Map"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Specular Only"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Normals"),
    QT_TRANSLATE_NOOP("ViewportStrip", "UV Checker"),
    // View-picker names (kViewEntries above, 7 entries):
    QT_TRANSLATE_NOOP("ViewportStrip", "Home View"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Top View"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Bottom View"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Front View"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Back View"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Left View"),
    QT_TRANSLATE_NOOP("ViewportStrip", "Right View"),
};
// A mode or view added to either table must be added above too; the count guard catches that
// (a rename it cannot: the spelling is checked by nothing).
static_assert(static_cast<int>(sizeof(kPickerNoops) / sizeof(kPickerNoops[0]))
                  == kShadeModeCount + kViewEntryCount,
              "kPickerNoops must list every shade mode and every view name");

} // namespace

ViewportStrip::ViewportStrip(QWidget* owner)
    : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint) {
    // WA_ShowWithoutActivating so revealing the strip doesn't steal focus from the app; its
    // controls still take clicks normally. (See the header for why it is a Qt::Tool top-level.)
    setObjectName(QStringLiteral("ViewportShaderOverlay"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);

    // Two rows: the control strip, and under it (right-aligned, usually empty) the transient
    // axis-rotate badge. Hidden widgets take no space, so the strip is one row until a key is held.
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    column->addLayout(row);

    // QPushButton with text-align:left ignores padding-left, so the field caption gets a small
    // leading indent from a couple of spaces to keep it off the left border (the menu items,
    // which honour padding normally, stay unindented).
    const QString captionPrefix = QStringLiteral("  ");

    // The SHADER picker: a pure rendering of scene/shademode.h — names in table order, the
    // default (PBR) row checked, a separator after each row flagged separatorAfter.
    m_shaderPicker = new MenuPickerButton(this);
    m_shaderPicker->setObjectName(QStringLiteral("ShaderModeButton"));
    m_shaderPicker->setToolTip(tr("Viewport shading mode"));
    m_shaderPicker->setStyleSheet(pickerStyle(QStringLiteral("ShaderModeButton"), 168));
    // The table's names are the single source of truth (Qt-free const char*); they are looked up
    // at run time, so their .ts entries come from the i18n pipeline's table import, not lupdate.
    QList<MenuPickerButton::Item> shadeItems;
    for (const ShadeMode& mode : kShadeModes) {
        shadeItems.append({QCoreApplication::translate("ViewportStrip", mode.name),
                           mode.separatorAfter, QString()});
    }
    m_shaderPicker->setItems(shadeItems);
    m_shaderPicker->setCaptionPrefix(captionPrefix);
    m_shaderPicker->setCurrentIndex(kDefaultShadeMode);
    connect(m_shaderPicker, &MenuPickerButton::currentChanged, this, &ViewportStrip::shadeModeSelected);
    row->addWidget(m_shaderPicker);

    // The VIEW picker: the named camera views (the same ones the 1/3/7/5 keys reach), grouped by
    // axis, plus Home. Its caption READS the current view — "Perspective View" once the user
    // orbits away from a named one — via setViewPreset, so the keys, the View menu, and this
    // picker all agree on where the camera is.
    m_viewPicker = new MenuPickerButton(this);
    m_viewPicker->setObjectName(QStringLiteral("ViewPresetButton"));
    m_viewPicker->setToolTip(tr("Camera view"));
    m_viewPicker->setStyleSheet(pickerStyle(QStringLiteral("ViewPresetButton"), 132));
    QList<MenuPickerButton::Item> viewItems;
    // Same single-source rule as the shader picker: the table owns the labels, the pipeline's
    // table import owns their .ts entries.
    for (const ViewEntry& entry : kViewEntries) {
        viewItems.append({QCoreApplication::translate("ViewportStrip", entry.label),
                          entry.separatorAfter, QString()});
    }
    m_viewPicker->setItems(viewItems);
    m_viewPicker->setCaptionPrefix(captionPrefix);
    // The global QMenu::item padding is tight on the right (built for the long menu-bar
    // entries); these short labels need breathing room so the dropdown doesn't read as a
    // sliver, and the menu is at least as wide as its field, like a combo's popup.
    m_viewPicker->pickerMenu()->setStyleSheet(QStringLiteral("QMenu::item { padding: 6px 28px 6px 6px; }"));
    m_viewPicker->pickerMenu()->setMinimumWidth(m_viewPicker->sizeHint().width());
    connect(m_viewPicker, &MenuPickerButton::currentChanged, this, [this](int index) {
        if (index >= 0 && index < kViewEntryCount) {
            emit viewSelected(kViewEntries[index].preset);
        }
    });
    row->addWidget(m_viewPicker);

    // "Home": snap the camera back to the default perspective framing. "Ground": drop the figure
    // onto the floor plane — its CURRENT pose's lowest point lands on y = 0 (a knee bend lifts
    // the feet off the floor; this puts them back). "Skeleton": toggle the skeleton overlay
    // (the joint→parent bone lines drawn over the figure) — a persistent on/off view control
    // (unlike the one-shot Home/Ground), so it's checkable and shows its state; off by default,
    // joints are grabbed directly on the figure and stay clickable either way.
    const int fieldHeight = m_shaderPicker->sizeHint().height();
    m_homeButton = makeIconButton(this, QStringLiteral("ViewportHomeButton"),
                                  QStringLiteral(":/resources/icons/home.png"),
                                  tr("Reset to the default view"), fieldHeight, false);
    connect(m_homeButton, &QPushButton::clicked, this, &ViewportStrip::homeClicked);
    row->addWidget(m_homeButton);

    m_groundButton = makeIconButton(this, QStringLiteral("ViewportGroundButton"),
                                    QStringLiteral(":/resources/icons/ground.png"),
                                    tr("Move the selected figure to the ground"), fieldHeight, false);
    connect(m_groundButton, &QPushButton::clicked, this, &ViewportStrip::groundClicked);
    row->addWidget(m_groundButton);

    m_skeletonButton = makeIconButton(this, QStringLiteral("ViewportSkeletonButton"),
                                      QStringLiteral(":/resources/icons/skeleton.png"),
                                      tr("Toggle the skeleton overlay"), fieldHeight, true);
    // Only USER toggles reach the owner: setSkeletonChecked mirrors external state with the
    // button's signals blocked, so the View menu → owner → strip round-trip stays one hop.
    connect(m_skeletonButton, &QPushButton::toggled, this, &ViewportStrip::skeletonToggled);
    row->addWidget(m_skeletonButton);

    // The joint-mode badge, under the strip's right end (below the Skeleton button): visible
    // only while the joint mouse mode is on (B / S / T with a joint selected: setJointModeBadge).
    m_modeBadge = new JointModeBadge(this, fieldHeight);
    m_modeBadge->hide();
    // The VR badge (setVrBadge): the headset has the scene, this viewport mirrors its left eye.
    m_vrBadge = new QLabel(tr("VR"), this);
    m_vrBadge->setObjectName(QStringLiteral("VrBadge"));
    m_vrBadge->setFixedHeight(fieldHeight);
    m_vrBadge->setAlignment(Qt::AlignCenter);
    m_vrBadge->setStyleSheet(QStringLiteral("QLabel#VrBadge { color: #5b87cc; background: #252627; border: 1px solid "
                                            "#555555; border-radius: 4px; padding: 0 10px; font-weight: bold; }"));
    m_vrBadge->hide();
    auto* badgeRow = new QHBoxLayout();
    badgeRow->setContentsMargins(0, 0, 0, 0);
    badgeRow->setSpacing(6);
    badgeRow->addStretch(1);
    badgeRow->addWidget(m_vrBadge);
    badgeRow->addWidget(m_modeBadge);
    column->addLayout(badgeRow);

    adjustSize();
}

void ViewportStrip::anchorTo(const QWidget* container) {
    m_anchor = container;
    if (!container || !container->isVisible()) {
        return;
    }
    adjustSize();
    constexpr int margin = 12;
    const QPoint  topRight = container->mapToGlobal(QPoint(container->width(), 0));
    move(topRight.x() - width() - margin, topRight.y() + margin);
}

void ViewportStrip::setViewPreset(ViewPreset view) {
    for (int i = 0; i < kViewEntryCount; ++i) {
        if (kViewEntries[i].preset == view) {
            m_viewPicker->setCurrentIndex(i);
            return;
        }
    }
    m_viewPicker->setDetachedCaption(tr("Perspective View")); // Free: no named view
}

void ViewportStrip::setShadeMode(int mode) {
    m_shaderPicker->setCurrentIndex(mode); // mirrors the window; setCurrentIndex emits nothing
}

void ViewportStrip::setSkeletonChecked(bool on) {
    if (m_skeletonButton->isChecked() == on) {
        return;
    }
    m_skeletonButton->blockSignals(true);
    m_skeletonButton->setChecked(on);
    m_skeletonButton->blockSignals(false);
}

void ViewportStrip::setJointModeBadge(int kind) {
    const bool was = m_modeBadge->isVisibleTo(this);
    m_modeBadge->setMode(kind);
    m_modeBadge->setVisible(kind >= 0);
    if (was != (kind >= 0)) {
        anchorTo(m_anchor); // the strip grew or shrank by a row
    }
}

void ViewportStrip::setVrBadge(bool on) {
    if (m_vrBadge->isVisibleTo(this) == on) {
        return;
    }
    m_vrBadge->setVisible(on);
    anchorTo(m_anchor); // the badge row appeared or went away
}

} // namespace pose
