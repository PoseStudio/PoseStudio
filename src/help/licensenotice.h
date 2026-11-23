// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file licensenotice.h
 * @brief Help → License: PoseStudio's "Appropriate Legal Notices".
 *
 * The GNU AGPL v3 (section 0) calls an interactive program's copyright notice, its statement
 * that there is no warranty, that it may be conveyed under the license, and how to view the
 * license its "Appropriate Legal Notices"; a program that displays them obliges every modified
 * version to keep displaying them (section 5 d). PoseStudio's additional terms
 * (LICENSE-ADDITIONAL-TERMS.txt, part B) ask those versions to preserve the copyright notice and
 * the attribution line shown here. So the wording below is part of the license's working:
 * change it together with LICENSE-ADDITIONAL-TERMS.txt and LICENSING.md, never apart.
 */

#ifndef LICENSENOTICE_H
#define LICENSENOTICE_H

class QWidget;

namespace LicenseNotice {

/// The attribution the additional terms ask every copy and every version based on PoseStudio
/// to preserve (LICENSE-ADDITIONAL-TERMS.txt, part B, term 1) — verbatim.
inline constexpr const char* kAttribution =
    "PoseStudio by Ion Hatzithomas and the PoseStudio contributors - https://posestudio.org";

/// Show the legal notices in a window-modal message box over @p parent (the main window). Its
/// "View License" button opens LICENSE.txt next to the executable, or the license on GitHub in a
/// copy that has none (a build from source). The box deletes itself when closed; its object name
/// is "LicenseNotice" (the POSESTUDIO_LICENSE_SHOT lever in main.cpp finds it by it).
void show(QWidget* parent);

} // namespace LicenseNotice

#endif // LICENSENOTICE_H
