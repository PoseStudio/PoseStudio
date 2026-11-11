/**
 * @file assettreedelegate.h
 * @brief The item delegate that paints the Asset Manager's directory/collections tree.
 *
 * It fully overrides Qt's default item painting (don't expect standard QStyledItemDelegate
 * behaviour): the style engine draws only the selection/hover background, and the delegate
 * lays out the icon and text itself so separators can be slim hairlines, search-result rows
 * can grey their ancestor path prefix, and the inline rename editor can sit exactly over the
 * painted text. The separator color comes from the view's `separatorColor` Q_PROPERTY, which
 * _assetmanager.qss sets, so QSS drives this C++ painting.
 */

#ifndef ASSETTREEDELEGATE_H
#define ASSETTREEDELEGATE_H

#include <QStyledItemDelegate>

/**
 * @class AssetTreeDelegate
 * @brief Custom item delegate that completely overrides native Qt painting for the directory tree.
 * @details Responsible for rendering folder names, custom icons, and enforcing the geometry of the inline rename editor.
 */
class AssetTreeDelegate : public QStyledItemDelegate {
public:
    explicit AssetTreeDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

#endif // ASSETTREEDELEGATE_H
