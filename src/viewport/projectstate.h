// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file projectstate.h
 * @brief The runtime identity of the current `.pss` document: its last saved/opened path and
 *        whether the scene has unsaved changes.
 *
 * VulkanWindow used to carry these as two loose members touched from every edit choke point;
 * this object owns them so future document statuses (autosave, …) have one home. The mutators
 * de-duplicate: `changed()` fires only when a value ACTUALLY transitions, so listeners (the
 * window-title update) stay quiet while a pose edit re-marks an already-dirty scene. Reading
 * the state directly (VulkanWindow::isProjectDirty, the close prompt) needs no signal at all.
 */

#ifndef PROJECTSTATE_H
#define PROJECTSTATE_H

#include <QObject>
#include <QString>

namespace pose {

/**
 * @class ProjectState
 * @brief Owns the current document's path and dirty flag; emits `changed()` on real transitions.
 */
class ProjectState : public QObject {
    Q_OBJECT

public:
    explicit ProjectState(QObject* parent = nullptr);

    /// The .pss document last saved/opened ("" = new, never saved).
    QString path() const;
    void setPath(const QString& path);

    /// Whether the scene changed since the last save/open.
    bool dirty() const;
    void markDirty();
    void setClean();

signals:
    /// Emitted when the document state (path or dirty flag) actually changes.
    void changed();

private:
    QString m_path;            // "" = new, never saved
    bool    m_dirty = false;   // scene changed since the last save/open
};

} // namespace pose

#endif // PROJECTSTATE_H
