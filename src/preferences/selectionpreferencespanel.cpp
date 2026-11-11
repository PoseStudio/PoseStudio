/**
 * @file selectionpreferencespanel.cpp
 * @brief Implements the "Selection" preferences page.
 */

#include "selectionpreferencespanel.h"

#include "dragnumberbox.h"
#include "propertyrows.h"
#include "selectionsettings.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

/// The width of a row's control (the swatch, the opacity field): the pages' fields are short and
/// left-aligned, not stretched across the dialog.
constexpr int kFieldWidth = 150;

/// The swatch's look: its background IS the color, the caption — the color's hex name — in a
/// contrasting ink. (The Materials tab's swatches, under this page's own object name.) Its size
/// is said HERE, as the content box @p height and kFieldWidth leave inside the border and the
/// padding: a dialog's buttons carry a min-width and a padding from global.qss, and a style
/// sheet's size wins over setFixedSize.
void paintSwatch(QPushButton* swatch, const QColor& c, int height) {
    const bool dark = c.lightnessF() < 0.5;
    swatch->setText(c.name(QColor::HexRgb).toUpper());
    swatch->setStyleSheet(
        QStringLiteral("QPushButton#SelectionColorButton { background-color: %1; color: %2; border: 1px solid #4a4b4d; "
                       "border-radius: 4px; padding: 0px 8px; font-size: 12px; "
                       "min-width: %3px; max-width: %3px; min-height: %4px; max-height: %4px; }"
                       "QPushButton#SelectionColorButton:hover { border-color: #5b87cc; }")
            .arg(c.name(QColor::HexRgb), dark ? QStringLiteral("#e0e0e0") : QStringLiteral("#1b1c1e"))
            .arg(kFieldWidth - 18)
            .arg(height - 2));
}

/// A row's field: the control at its fixed width, its restore button snug on the right, the rest
/// of the row empty.
QWidget* fieldWithRestore(QWidget* control, QToolButton* restore) {
    auto* field = new QWidget;
    auto* row = new QHBoxLayout(field);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    row->addWidget(control);
    row->addWidget(restore);
    row->addStretch(1);
    return field;
}

} // namespace

SelectionPreferencesPanel::SelectionPreferencesPanel(QWidget* parent)
    : PreferencesPanel(tr("Selection"), parent) {
    setProperty("testId", QStringLiteral("selection.panel")); // (the scripted test's `ui call`)

    addDescription(
        tr("How the viewport marks what is selected. The two marks are set separately, and a change shows in the "
           "viewport at once — select an object, or click a joint, before opening Preferences to watch it."));

    m_marks[Outline].color = SelectionSettings::outlineColor();
    m_marks[Joint].color = SelectionSettings::jointColor();

    buildMark(Outline, tr("Object Outline"), QStringLiteral("selection.outline"),
              tr("The line drawn around the selected object. Switched off, or at an opacity of 0, no outline is drawn."));
    buildMark(Joint, tr("Joint Highlight"), QStringLiteral("selection.joint"),
              tr("The tint on the part of the body a selected joint moves. The tint keeps the surface's own light and "
                 "shadow, so the color sets its hue rather than its brightness — only a color near black darkens "
                 "the part. Switched off, or at an opacity of 0, a selected joint is not marked on the figure."));
}

void SelectionPreferencesPanel::buildMark(Mark mark, const QString& title, const QString& idPrefix,
                                          const QString& description) {
    MarkRows& rows = m_marks[mark];

    contentLayout()->addSpacing(mark == Outline ? 8 : 16);
    // --- The block's heading IS its switch: the checkbox in front of the title. Off, the mark is
    // not drawn — and its color and opacity are kept ---
    rows.enabled = new QCheckBox(title, this);
    rows.enabled->setObjectName(QStringLiteral("PreferencesSubheadingCheck"));
    rows.enabled->setChecked(mark == Outline ? SelectionSettings::outlineEnabled() : SelectionSettings::jointEnabled());
    rows.enabled->setToolTip(tr("Off, this mark is not drawn; its color and opacity are kept for when you switch it back on."));
    rows.enabled->setProperty("testId", idPrefix + QStringLiteral(".enabled"));
    contentLayout()->addWidget(rows.enabled);
    connect(rows.enabled, &QCheckBox::toggled, this, [this, mark](bool on) {
        if (mark == Outline) {
            SelectionSettings::setOutlineEnabled(on);
        } else {
            SelectionSettings::setJointEnabled(on);
        }
        announce(mark);
    });

    auto* form = new QFormLayout();
    form->setContentsMargins(0, 2, 0, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(6);

    // (The opacity field first: the swatch above it takes its height.)
    rows.opacity = new pose::DragNumberBox(this);
    rows.fieldHeight = rows.opacity->sizeHint().height();

    // --- Color: a swatch that opens the color dialog ---
    rows.swatch = new QPushButton(this);
    rows.swatch->setObjectName(QStringLiteral("SelectionColorButton"));
    rows.swatch->setCursor(Qt::PointingHandCursor);
    rows.swatch->setToolTip(tr("Click to choose the color."));
    rows.swatch->setProperty("testId", idPrefix + QStringLiteral(".color"));
    paintSwatch(rows.swatch, rows.color, rows.fieldHeight);
    connect(rows.swatch, &QPushButton::clicked, this, [this, mark]() { pickColor(mark); });
    QToolButton* restoreColor = pose::propertyrows::makeRestoreButton();
    restoreColor->setProperty("testId", idPrefix + QStringLiteral(".restore.color"));
    connect(restoreColor, &QToolButton::clicked, this,
            [this, mark]() { setColor(mark, SelectionSettings::defaultColor(), true); });
    form->addRow(rowLabel(tr("Color")), fieldWithRestore(rows.swatch, restoreColor));

    // --- Opacity: 0–100 %, announced as it is scrubbed and stored when the scrub ends ---
    const double defaultOpacity =
        mark == Outline ? SelectionSettings::kDefaultOutlineOpacity : SelectionSettings::kDefaultJointOpacity;
    rows.opacity->setRange(0.0, 100.0);
    rows.opacity->setSingleStep(1.0);
    rows.opacity->setDecimals(0);
    rows.opacity->setValue(mark == Outline ? SelectionSettings::outlineOpacity() : SelectionSettings::jointOpacity());
    rows.opacity->setFixedWidth(kFieldWidth);
    rows.opacity->setToolTip(tr("Drag to change, or click to type a value: 0 is invisible, 100 the color in full."));
    rows.opacity->setProperty("testId", idPrefix + QStringLiteral(".opacity"));
    connect(rows.opacity, &pose::DragNumberBox::editingStarted, this, [this, mark]() { m_marks[mark].scrubbing = true; });
    connect(rows.opacity, &pose::DragNumberBox::editingFinished, this, [this, mark]() {
        m_marks[mark].scrubbing = false;
        storeOpacity(mark);
    });
    connect(rows.opacity, &pose::DragNumberBox::valueChanged, this, [this, mark](double) {
        announce(mark);
        if (!m_marks[mark].scrubbing) {
            storeOpacity(mark); // set in one go (a restore): there is no gesture's end to wait for
        }
    });
    QToolButton* restoreOpacity = pose::propertyrows::makeRestoreButton();
    restoreOpacity->setProperty("testId", idPrefix + QStringLiteral(".restore.opacity"));
    connect(restoreOpacity, &QToolButton::clicked, this,
            [this, mark, defaultOpacity]() { m_marks[mark].opacity->setValue(defaultOpacity); });
    form->addRow(rowLabel(tr("Opacity")), fieldWithRestore(rows.opacity, restoreOpacity));

    contentLayout()->addLayout(form);
    addDescription(description);
}

void SelectionPreferencesPanel::setColor(Mark mark, const QColor& color, bool store) {
    if (!color.isValid()) {
        return;
    }
    MarkRows& rows = m_marks[mark];
    rows.color = QColor(color.rgb()); // (opaque: the opacity is its own row)
    paintSwatch(rows.swatch, rows.color, rows.fieldHeight);
    announce(mark);
    if (store) {
        if (mark == Outline) {
            SelectionSettings::setOutlineColor(rows.color);
        } else {
            SelectionSettings::setJointColor(rows.color);
        }
    }
}

void SelectionPreferencesPanel::storeOpacity(Mark mark) {
    if (mark == Outline) {
        SelectionSettings::setOutlineOpacity(m_marks[mark].opacity->value());
    } else {
        SelectionSettings::setJointOpacity(m_marks[mark].opacity->value());
    }
}

void SelectionPreferencesPanel::announce(Mark mark) {
    const MarkRows& rows = m_marks[mark];
    // (Switched off, the mark is announced at an opacity of 0: the viewport does not draw it.)
    const bool      on = rows.enabled == nullptr || rows.enabled->isChecked();
    const double    opacity = !on ? 0.0 : rows.opacity != nullptr ? rows.opacity->value() / 100.0 : 1.0;
    if (mark == Outline) {
        emit outlineChanged(rows.color, opacity);
    } else {
        emit jointHighlightChanged(rows.color, opacity);
    }
}

void SelectionPreferencesPanel::pickColor(Mark mark) {
    if (m_colorDialog) {
        m_colorDialog->raise();
        m_colorDialog->activateWindow();
        return;
    }
    m_picking = mark;
    m_colorAtOpen = m_marks[mark].color;
    // The app's own themed dialog (the native one ignores the dark theme). Every color it passes
    // through is shown in the viewport as it is picked; only an accepted one is stored.
    m_colorDialog = new QColorDialog(m_colorAtOpen, this);
    m_colorDialog->setOption(QColorDialog::DontUseNativeDialog, true);
    m_colorDialog->setWindowTitle(mark == Outline ? tr("Object Outline Color") : tr("Joint Highlight Color"));
    m_colorDialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_colorDialog, &QColorDialog::currentColorChanged, this,
            [this](const QColor& c) { setColor(m_picking, c, false); });
    connect(m_colorDialog, &QColorDialog::finished, this, [this](int result) {
        if (result == QDialog::Accepted) {
            const QColor chosen = m_colorDialog ? m_colorDialog->selectedColor() : QColor();
            setColor(m_picking, chosen.isValid() ? chosen : m_marks[m_picking].color, true);
        } else {
            setColor(m_picking, m_colorAtOpen, false); // Cancel: the color as it was (what is stored never changed)
        }
        m_colorDialog = nullptr;
    });
    m_colorDialog->open();
}

void SelectionPreferencesPanel::setOutlineRgb(int rgb) {
    setColor(Outline, QColor::fromRgb(static_cast<QRgb>(rgb) | 0xff000000u), true);
}

void SelectionPreferencesPanel::setJointRgb(int rgb) {
    setColor(Joint, QColor::fromRgb(static_cast<QRgb>(rgb) | 0xff000000u), true);
}
