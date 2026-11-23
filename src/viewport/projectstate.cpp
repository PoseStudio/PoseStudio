// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file projectstate.cpp
 * @brief Implementation of ProjectState: the .pss document's path and unsaved flag, each setter
 *        announcing only a real transition. See projectstate.h.
 */

#include "projectstate.h"

namespace pose {

ProjectState::ProjectState(QObject* parent) : QObject(parent) {}

QString ProjectState::path() const { return m_path; }

void ProjectState::setPath(const QString& path) {
    if (m_path == path) return; // de-duplicate: no transition, no signal
    m_path = path;
    emit changed();
}

bool ProjectState::dirty() const { return m_dirty; }

void ProjectState::markDirty() {
    if (m_dirty) return; // pose edits re-mark a dirty scene on every commit — only the
    m_dirty = true;      // clean->dirty transition is worth announcing
    emit changed();
}

void ProjectState::setClean() {
    if (!m_dirty) return; // a successful save is what clears the flag; re-saving a clean
    m_dirty = false;      // scene must not re-announce itself
    emit changed();
}

} // namespace pose
