// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file vulkanwindow_script_metrics.cpp
 * @brief The scripted in-app test's METRICS — the numbers `expect` asserts on — and `report`.
 *
 * One of the three files of the script runner on VulkanWindow (see vulkanwindow_script.cpp, the
 * step driver, and ikscript.h). scriptMetric hands a name to the family that owns it — the
 * document's, the pictures', the Materials tab's, the widgets', and what is read off the figure —
 * and each family reads what a user would see, off the same state the application shows it from.
 * The metrics of a feature that has a translation unit of its own are forwarded there (`traced.`,
 * `lamp`). tools/ikscripts/README.md lists the metrics: one added here is added there.
 */

#include "vulkanwindow.h"

#include "ikscript.h"
#include "rendering/vulkanrenderer.h"
#include "scene/armature.h"
#include "scene/ik/ikrig.h"
#include "scene/mesh.h"
#include "scene/model.h"
#include "scene/scene.h"
#include "smoothnormals.h" // the scripted `matfaceted`
#include "vulkanwindow_script_detail.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <string>

namespace pose {

using namespace scriptdetail;

namespace {

/// How far two pictures of one size are apart: the mean difference of their channels, 0-255.
/// -1 where either is missing or their sizes differ.
double meanDifference(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull() || a.size() != b.size()) {
        return -1.0;
    }
    const QImage x = a.convertToFormat(QImage::Format_RGB888);
    const QImage y = b.convertToFormat(QImage::Format_RGB888);
    double       total = 0.0;
    for (int row = 0; row < x.height(); ++row) {
        const uchar* p = x.constScanLine(row);
        const uchar* q = y.constScanLine(row);
        for (int i = 0; i < 3 * x.width(); ++i) {
            total += std::abs(static_cast<int>(p[i]) - static_cast<int>(q[i]));
        }
    }
    return total / (3.0 * static_cast<double>(x.width()) * static_cast<double>(x.height()));
}

/// How ALIKE two pictures' changes are: the correlation, -1..1, of (a - base) with (b - base)
/// over the brightness of every pixel — what two pictures of one view ADD to a third. Where a
/// relief is shaded, +1 is the same relief lit the same way and -1 the relief turned over,
/// whatever their strengths. 0 where a picture is missing, sizes differ, or nothing changed.
double changeCorrelation(const QImage& a, const QImage& b, const QImage& base) {
    if (a.isNull() || b.isNull() || base.isNull() || a.size() != b.size() || a.size() != base.size()) {
        return 0.0;
    }
    const QImage x = a.convertToFormat(QImage::Format_RGB888);
    const QImage y = b.convertToFormat(QImage::Format_RGB888);
    const QImage z = base.convertToFormat(QImage::Format_RGB888);
    double       sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0, n = 0.0;
    for (int row = 0; row < x.height(); ++row) {
        const uchar* p = x.constScanLine(row);
        const uchar* q = y.constScanLine(row);
        const uchar* r = z.constScanLine(row);
        for (int i = 0; i < x.width(); ++i) {
            const double zero = r[3 * i] + r[3 * i + 1] + r[3 * i + 2];
            const double u = p[3 * i] + p[3 * i + 1] + p[3 * i + 2] - zero;
            const double v = q[3 * i] + q[3 * i + 1] + q[3 * i + 2] - zero;
            sx += u;
            sy += v;
            sxx += u * u;
            syy += v * v;
            sxy += u * v;
            n += 1.0;
        }
    }
    const double cov = sxy - sx * sy / n;
    const double var = (sxx - sx * sx / n) * (syy - sy * sy / n);
    return var > 0.0 ? cov / std::sqrt(var) : 0.0;
}

/// How far @p bone has moved since the press (mm), 0 for a bone the press did not record.
double movedSincePress(const Armature& arm, const IkScript& s, int bone) {
    return bone >= 0 && static_cast<std::size_t>(bone) < s.pressPositions.size()
               ? glm::length(arm.boneWorldPosition(static_cast<std::size_t>(bone)) -
                             s.pressPositions[static_cast<std::size_t>(bone)]) * 1000.0
               : 0.0;
}

/// The unit direction from @p bone's joint to its LONGEST child's (what `hang` and `swing` read a
/// limb's direction off). False for a bone with no child, or none a tenth of a millimetre away.
bool longestChildDirection(const Armature& arm, int bone, glm::vec3& direction) {
    int   child = -1;
    float longest = 0.0f;
    for (std::size_t i = 0; i < arm.boneCount(); ++i) {
        if (arm.boneParent(i) != bone) {
            continue;
        }
        const float len = glm::length(arm.boneWorldPosition(i) - arm.boneWorldPosition(static_cast<std::size_t>(bone)));
        if (len > longest) {
            longest = len;
            child = static_cast<int>(i);
        }
    }
    if (child < 0 || longest < 1.0e-4f) {
        return false;
    }
    direction = (arm.boneWorldPosition(static_cast<std::size_t>(child)) - arm.boneWorldPosition(static_cast<std::size_t>(bone))) / longest;
    return true;
}

} // namespace

/// A number a script can `expect` on — what `report` prints, and a few per-bone readings:
///   grab            grab point to cursor, mm (during a drag)   grabpx  the same on screen, pixels
///   hips, head      moved since the last press, mm          steps   steps taken in the drag
///   jump            worst single-tick move of any joint, mm  lowest  the lowest joint's height, mm
///   meshlow         the posed MESH's lowest point over the floor, mm (negative = flesh through it)
///   gaze            the head's look direction over the horizon, degrees (negative = down)
///   headroll        the head's sideways tilt, degrees
///   tilt.<bone>     a bone's tilt from upright, degrees      y.<bone>     its height, mm
///   hang.<bone>     a bone's direction (origin to its longest child) from straight DOWN, degrees
///   moved.<bone>    moved since the last press, mm           euler.<bone>.<x|y|z>  a posed channel
///   movedxz.<bone>  moved along the FLOOR since the last press, mm (a planted foot's place, heel lift aside)
///   selected.<bone> 1 if the selected joint is that bone or a bone of its rigid segment (a click's pick), else 0
///   lit.<bone>      1 if the selection highlight tints that bone's flesh (the selected joint, or its twist twin), else 0
///   palm.<bone>.<x|y|z>  a HAND's palm normal, that world component (where a laid hand faces)
///   figure.<x|y|z|turn|tip|tilt>  the figure's place (cm, world) and rotation (deg): the Transform tab's Figure group
///   dial.<forward|sideways|twist>  the selected joint's Transform-tab dial, as it reads now
///   dialmin.<kind>, dialmax.<kind>  that dial's range     dialjoint.<bone>  1 if the tab shows that joint
///   modal                  the dial the joint mouse mode turns (0/1/2 = bend / side / twist), -1 when off
///   figures                models in the scene          dirty  1 if the .pss document has unsaved changes
bool VulkanWindow::scriptMetric(const QString& name, double& value) {
    // THE RENDERED MODE: traced.on / .shown / .done / .sample / .samples / .updates / .scenes / .held.
    if (name.startsWith(QLatin1String("traced."), Qt::CaseInsensitive)) {
        return scriptTracedMetric(name, value);
    }
    // THE LIGHT OBJECTS: lamps, lampselected, lamp.<n>.<field> (scriptLampMetric).
    if (name.startsWith(QLatin1String("lamp"), Qt::CaseInsensitive)) {
        return scriptLampMetric(name, value);
    }
    // Every other name belongs to one family — asked in turn; the last takes what is read off the figure.
    bool mine = false;
    bool known = scriptDocumentMetric(name, value, mine);
    if (mine) {
        return known;
    }
    known = scriptPictureMetric(name, value, mine);
    if (mine) {
        return known;
    }
    known = scriptMaterialMetric(name, value, mine);
    if (mine) {
        return known;
    }
    known = scriptUiMetric(name, value, mine);
    if (mine) {
        return known;
    }
    return scriptFigureMetric(name, value);
}

/// The document and the run's folder: figures, dirty, selection, exists.<path>, count.<glob>.
/// @p mine false when the name is none of them (as for every family below).
bool VulkanWindow::scriptDocumentMetric(const QString& name, double& value, bool& mine) {
    IkScript&       s = *m_script;
    mine = true;
    // (The two document metrics need no figure: an empty scene is what they are asked about.)
    if (name.compare(QLatin1String("figures"), Qt::CaseInsensitive) == 0) {
        value = m_renderer ? static_cast<double>(scene().modelCount()) : 0.0;
        return true;
    }
    if (name.compare(QLatin1String("dirty"), Qt::CaseInsensitive) == 0) {
        value = isProjectDirty() ? 1.0 : 0.0;
        return true;
    }
    if (name.compare(QLatin1String("selection"), Qt::CaseInsensitive) == 0) {
        value = m_renderer ? static_cast<double>(scene().selectedModelIndex()) : -1.0; // -1 = nothing selected
        return true;
    }
    if (name.startsWith(QLatin1String("exists."), Qt::CaseInsensitive)) {
        // exists.<path>: 1 when that file or folder is in the run's folder (the path is
        // relative to it and may hold dots and slashes, no spaces).
        value = QFileInfo::exists(QDir(s.outDir).absoluteFilePath(name.mid(7))) ? 1.0 : 0.0;
        return true;
    }
    if (name.startsWith(QLatin1String("count."), Qt::CaseInsensitive)) {
        // count.<folder>/<pattern>: how many files and folders of that folder of the run's
        // folder match the pattern (`*`, `?`) — for names a script cannot spell (a space).
        const QFileInfo what(QDir(s.outDir).absoluteFilePath(name.mid(6)));
        value = static_cast<double>(what.absoluteDir().entryList({what.fileName()}, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).size());
        return true;
    }
    mine = false;
    return false;
}

/// Pictures read as numbers: shot., matfile., maticon., render.
bool VulkanWindow::scriptPictureMetric(const QString& name, double& value, bool& mine) {
    IkScript&       s = *m_script;
    mine = true;
    // THE LAST SHOT's picture, read as numbers: `shot.darkest` — its darkest pixel's brightness
    // (the three channels' mean, 0-255) — and `shot.below.<n>` — how many of its pixels are
    // darker than n. For a view framed so that everything in it is bright, they catch a dark
    // line that should not be there (a silhouette blended with the backdrop instead of with
    // the Gaussian splats behind it: tools/ikscripts/splats/edge.txt). -1 before any shot.
    if (name.startsWith(QLatin1String("shot."), Qt::CaseInsensitive)) {
        const QString what = name.mid(5).toLower();
        if (what == QLatin1String("fromref")) {
            value = meanDifference(s.shotImage, s.shotReference); // (see `shotref`)
            return true;
        }
        if (what == QLatin1String("likeref")) {
            value = changeCorrelation(s.shotImage, s.shotReference, s.shotBase); // (see `shotbase`)
            return true;
        }
        const bool    darkest = what == QLatin1String("darkest");
        bool          numbered = false;
        const double  limit = what.startsWith(QLatin1String("below.")) ? what.mid(6).toDouble(&numbered) : 0.0;
        if (!darkest && !numbered) {
            return false;
        }
        value = -1.0;
        if (!s.shotImage.isNull()) {
            double least = 255.0;
            double below = 0.0;
            for (int y = 0; y < s.shotImage.height(); ++y) {
                const uchar* row = s.shotImage.constScanLine(y); // (RGB888: three bytes a pixel)
                for (int x = 0; x < s.shotImage.width(); ++x) {
                    const double brightness = (row[3 * x] + row[3 * x + 1] + row[3 * x + 2]) / 3.0;
                    least = std::min(least, brightness);
                    below += brightness < limit ? 1.0 : 0.0;
                }
            }
            value = darkest ? least : below;
        }
        return true;
    }
    // THE LAST `savematerial` / `applymaterial`: matfile.ok, matfile.embedded, matfile.missing,
    // matfile.copied, matfile.reused, matfile.written, matfile.linked, matfile.relocated;
    // maticon.ok, maticon.engine (1 = the render engine made it), maticon.size, and the icon's
    // pixel at a place given in percent of its width and height: maticon.<r|g|b|a>.<x>.<y>.
    if (name.startsWith(QLatin1String("matfile."), Qt::CaseInsensitive)) {
        const QString what = name.mid(8).toLower();
        value = what == QLatin1String("ok") ? s.matFileOk
                : what == QLatin1String("embedded") ? s.matFileEmbedded
                : what == QLatin1String("missing") ? s.matFileMissing
                : what == QLatin1String("copied") ? s.matFileCopied
                : what == QLatin1String("reused") ? s.matFileReused
                : what == QLatin1String("written") ? s.matFileWritten
                : what == QLatin1String("linked") ? s.matFileLinked
                : what == QLatin1String("relocated") ? s.matFileRelocated
                : what == QLatin1String("materials") ? s.matFileMaterials
                : what == QLatin1String("notes") ? s.matFileNotes : 0.0;
        return true;
    }
    if (name.startsWith(QLatin1String("maticon."), Qt::CaseInsensitive)) {
        const QStringList part = name.mid(8).toLower().split(QLatin1Char('.'));
        if (part[0] == QLatin1String("ok")) {
            value = s.matIconOk;
        } else if (part[0] == QLatin1String("engine")) {
            value = s.matIconEngine;
        } else if (part[0] == QLatin1String("size")) {
            value = s.matIcon.isNull() ? 0.0 : s.matIcon.width();
        } else if (part.size() >= 3 && !s.matIcon.isNull()) {
            const int  x = qBound(0, static_cast<int>(part[1].toDouble() / 100.0 * s.matIcon.width()), s.matIcon.width() - 1);
            const int  y = qBound(0, static_cast<int>(part[2].toDouble() / 100.0 * s.matIcon.height()), s.matIcon.height() - 1);
            const QRgb p = s.matIcon.pixel(x, y);
            value = part[0] == QLatin1String("r") ? qRed(p) : part[0] == QLatin1String("g") ? qGreen(p)
                    : part[0] == QLatin1String("b") ? qBlue(p) : qAlpha(p);
        } else {
            value = 0.0;
        }
        return true;
    }
    // THE LAST `render`: render.ok (1 = it finished), render.seconds, render.mean (the picture's
    // mean display value, 0-255), render.covered (percent of pixels that are not the backdrop).
    if (name.startsWith(QLatin1String("render."), Qt::CaseInsensitive)) {
        const QString what = name.mid(7).toLower();
        if (what == QLatin1String("ok")) {
            value = s.renderOk;
        } else if (what == QLatin1String("seconds")) {
            value = s.renderSeconds;
        } else if (what == QLatin1String("mean")) {
            value = s.renderMean;
        } else if (what == QLatin1String("covered")) {
            value = s.renderCovered;
        } else if (what == QLatin1String("splats")) {
            value = s.renderSplats;
        } else if (what == QLatin1String("fromref")) {
            value = meanDifference(s.renderImage, s.renderReference); // (see `shotref`)
        } else if (what == QLatin1String("likeref")) {
            value = changeCorrelation(s.renderImage, s.renderReference, s.renderBase); // (see `shotbase`)
        } else if (what.startsWith(QLatin1String("probe."))) {
            // The last render where the probe's world point is seen: the mean of a 5 x 5 block,
            // as 8-bit display values (lum: the three channels' mean). -1 off the picture.
            value = -1.0;
            QPointF at;
            if (s.probeSet && !s.renderImage.isNull() && m_renderer && width() > 0 && height() > 0 &&
                projectToScreen(glm::vec3(s.probe[0], s.probe[1], s.probe[2]), at)) {
                const int cx = static_cast<int>(at.x() / width() * s.renderImage.width());
                const int cy = static_cast<int>(at.y() / height() * s.renderImage.height());
                if (cx >= 2 && cy >= 2 && cx < s.renderImage.width() - 2 && cy < s.renderImage.height() - 2) {
                    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
                    for (int y = cy - 2; y <= cy + 2; ++y) {
                        for (int x = cx - 2; x <= cx + 2; ++x) {
                            const QRgb pixel = s.renderImage.pixel(x, y);
                            r += qRed(pixel);
                            g += qGreen(pixel);
                            b += qBlue(pixel);
                            a += qAlpha(pixel);
                        }
                    }
                    const QString channel = what.mid(6);
                    value = channel == QLatin1String("r")   ? r / 25.0
                            : channel == QLatin1String("g") ? g / 25.0
                            : channel == QLatin1String("b") ? b / 25.0
                            : channel == QLatin1String("a") ? a / 25.0 // (a transparent render's coverage)
                                                            : (r + g + b) / 75.0;
                }
            }
        } else {
            return false;
        }
        return true;
    }
    mine = false;
    return false;
}

/// The Materials tab: zones, zone, matlinked, the smoothing's readings, matmap. / mattex. /
/// matstore., the overlays', zonename., mat. / matimported.
bool VulkanWindow::scriptMaterialMetric(const QString& name, double& value, bool& mine) {
    IkScript&       s = *m_script;
    mine = true;
    // THE MATERIALS TAB's metrics (materialparams.h): `zones` (the selected object's zone count),
    // `zone` (the selected zone's index, -1 with nothing selected), `zonename.<name>` (1 when that
    // zone is selected), `matlinked` (the other zones sharing the imported material),
    // `mat.<field>` / `matimported.<field>` (the selected zone's values in the tab's display units,
    // as they stand / as imported; `mat.color.<r|g|b>` the linear base color).
    if (name.compare(QLatin1String("zones"), Qt::CaseInsensitive) == 0) {
        const MaterialView view = materialView();
        value = view.valid ? static_cast<double>(view.zones.size()) : 0.0;
        return true;
    }
    if (name.compare(QLatin1String("zone"), Qt::CaseInsensitive) == 0) {
        const MaterialView view = materialView();
        value = view.valid ? static_cast<double>(view.zone) : -1.0;
        return true;
    }
    if (name.compare(QLatin1String("matlinked"), Qt::CaseInsensitive) == 0) {
        value = static_cast<double>(materialView().linkedZones);
        return true;
    }
    // THE SMOOTHING, read off the geometry the zone is drawn with: `matfaceted` (the share of its
    // triangles whose corners carry the triangle's own normal, in percent: 100 faceted, 0 smooth),
    // `matvertices` and `mattriangles` (its vertex and triangle counts) and `matfilekept` (1 while
    // the file's own geometry is kept beside a smoothed one).
    if (name.compare(QLatin1String("matfaceted"), Qt::CaseInsensitive) == 0 || name.compare(QLatin1String("matvertices"), Qt::CaseInsensitive) == 0 ||
        name.compare(QLatin1String("mattriangles"), Qt::CaseInsensitive) == 0 || name.compare(QLatin1String("matfilekept"), Qt::CaseInsensitive) == 0) {
        value = -1.0;
        const MaterialView view = materialView();
        const Model*       model = (m_renderer && view.valid) ? scene().modelAt(static_cast<std::size_t>(view.model)) : nullptr;
        if (model != nullptr && view.zone >= 0 && static_cast<std::size_t>(view.zone) < model->meshCount()) {
            const Mesh& mesh = model->mesh(static_cast<std::size_t>(view.zone));
            if (name.compare(QLatin1String("matfaceted"), Qt::CaseInsensitive) == 0) {
                value = 100.0 * static_cast<double>(facetedShare(mesh.cpuVertices(), mesh.cpuIndices()));
            } else if (name.compare(QLatin1String("matvertices"), Qt::CaseInsensitive) == 0) {
                value = static_cast<double>(mesh.cpuVertices().size());
            } else if (name.compare(QLatin1String("mattriangles"), Qt::CaseInsensitive) == 0) {
                value = static_cast<double>(mesh.cpuIndices().size() / 3);
            } else {
                value = (&mesh.fileVertices() != &mesh.cpuVertices()) ? 1.0 : 0.0;
            }
        }
        return true;
    }
    if (name.startsWith(QLatin1String("matmap."), Qt::CaseInsensitive) || name.startsWith(QLatin1String("mattex."), Qt::CaseInsensitive)) {
        // matmap.<slot>: 1 when the selected zone carries that map — the slot names are the
        // Materials tab's (materialMapName: basecolor, normal, bump, roughness, metalness,
        // specmask, translucency, emission, pore, cutout); mattex.<slot>: where the
        // slot's map comes from — 0 none, 1 the imported map, 2 a file chosen in the tab.
        const MaterialView view = materialView();
        if (!view.valid) {
            return false;
        }
        const bool    tex = name.startsWith(QLatin1String("mattex."), Qt::CaseInsensitive);
        const QString which = name.mid(7).toLower();
        MaterialMap   map = MaterialMap::BaseColor;
        if (!materialMapByName(which.toStdString(), map)) {
            return false;
        }
        const std::string& source = view.params.maps[static_cast<std::size_t>(map)].source;
        value = tex ? (source.empty() ? 0.0 : source == kImportedTexture ? 1.0 : 2.0) : (view.maps.has(map) ? 1.0 : 0.0);
        return true;
    }
    if (name.startsWith(QLatin1String("matstore."), Qt::CaseInsensitive)) {
        // matstore.<slot>: 1 when the slot's file lies in a TEXTURE STORE of this run's folder (a
        // `.textures` folder beside a material `savematerial` wrote: assets/assetstore.h) — what
        // an applied material's map must be, not the file the material was saved from.
        const MaterialView view = materialView();
        MaterialMap        map = MaterialMap::BaseColor;
        if (!view.valid || !materialMapByName(name.mid(9).toLower().toStdString(), map)) {
            return false;
        }
        const QString path = QDir::cleanPath(QString::fromStdString(view.mapPaths[static_cast<std::size_t>(map)]));
        const QString run = QDir::cleanPath(s.outDir);
        value = !path.isEmpty() && path.startsWith(run, Qt::CaseInsensitive) && path.contains(QLatin1String("/.textures/"), Qt::CaseInsensitive) ? 1.0 : 0.0;
        return true;
    }
    // THE OVERLAYS' metrics: `overlays` (how many the selected zone carries), `overlayzones` (the
    // zones the stack is painted on — those sharing the zone's color map, itself included),
    // `overlay.<n>.<field>` (the tab's units; `blend` the mode's index), `overlay.<n>.tint.<r|g|b>`
    // and `overlay.<n>.has.<color|bump|normal|displacement>` (1 when that image is set).
    if (name.compare(QLatin1String("overlays"), Qt::CaseInsensitive) == 0) {
        const MaterialView view = materialView();
        value = view.valid ? static_cast<double>(view.params.overlays.size()) : 0.0;
        return true;
    }
    if (name.compare(QLatin1String("overlayzones"), Qt::CaseInsensitive) == 0) {
        value = static_cast<double>(materialView().overlayZones.size());
        return true;
    }
    if (name.compare(QLatin1String("metalbaked"), Qt::CaseInsensitive) == 0) {
        // The Metallic value (percent) the selected zone's bound metalness was baked for — an
        // overlay's metal is painted on the zone's absolute metalness — or -1: not baked.
        const float baked = m_renderer ? scene().meshMetalBaked(scene().selectedModelIndex(), scene().selectedZoneIndex()) : -1.0f;
        value = baked < 0.0f ? -1.0 : static_cast<double>(baked) * 100.0;
        return true;
    }
    if (name.startsWith(QLatin1String("overlay."), Qt::CaseInsensitive)) {
        const MaterialView view = materialView();
        const QStringList  part = name.toLower().split(QLatin1Char('.'));
        bool               ok = false;
        const int          n = part.size() > 2 ? part[1].toInt(&ok) : -1;
        if (!view.valid || !ok || n < 0 || static_cast<std::size_t>(n) >= view.params.overlays.size()) {
            return false;
        }
        const MaterialOverlay& o = view.params.overlays[static_cast<std::size_t>(n)];
        if (part[2] == QLatin1String("tint") && part.size() > 3) {
            value = part[3] == QLatin1String("g") ? o.tint.y : part[3] == QLatin1String("b") ? o.tint.z : o.tint.x;
            return true;
        }
        if (part[2] == QLatin1String("has") && part.size() > 3) {
            OverlayElement element = OverlayElement::Color;
            if (!overlayElementByName(part[3].toStdString(), element)) {
                return false;
            }
            value = overlayElementSource(o, element).empty() ? 0.0 : 1.0;
            return true;
        }
        if (part.size() > 3 && part[2] == QLatin1String("named")) {
            // overlay.<n>.named.<name>: 1 if that is its name (case ignored, a space written _).
            value = QString::fromStdString(o.name).toLower().replace(QLatin1Char(' '), QLatin1Char('_')) == part.mid(3).join(QLatin1Char('.')) ? 1.0 : 0.0;
            return true;
        }
        OverlayField field = OverlayField::Opacity;
        if (!overlayFieldByName(part[2].toStdString(), field)) {
            return false;
        }
        value = overlayFieldValue(o, field);
        return true;
    }
    if (name.startsWith(QLatin1String("zonename."), Qt::CaseInsensitive)) {
        const MaterialView view = materialView();
        const QString      want = name.mid(9);
        value = (view.valid && view.zone >= 0 && static_cast<std::size_t>(view.zone) < view.zones.size() &&
                 QString::fromStdString(view.zones[static_cast<std::size_t>(view.zone)]).compare(want, Qt::CaseInsensitive) == 0)
                    ? 1.0
                    : 0.0;
        return true;
    }
    if (name.startsWith(QLatin1String("mat."), Qt::CaseInsensitive) || name.startsWith(QLatin1String("matimported."), Qt::CaseInsensitive)) {
        const MaterialView view = materialView();
        if (!view.valid) {
            return false;
        }
        const bool            imported = name.startsWith(QLatin1String("matimported."), Qt::CaseInsensitive);
        const MaterialParams& p = imported ? view.imported : view.params;
        const QString         rest = name.mid(imported ? 12 : 4).toLower();
        {
            // mat.color.<r|g|b> is the linear base color; mat.<color>.<r|g|b> any of the
            // material's colors by the tab's names (materialColorName).
            const int dot = rest.indexOf(QLatin1Char('.'));
            if (dot > 0) {
                const QString colorName = rest.left(dot);
                MaterialColor which = MaterialColor::Base;
                if (colorName == QLatin1String("color") || materialColorByName(colorName.toStdString(), which)) {
                    const glm::vec3 c = materialColorValue(p, which);
                    const QChar     channel = rest.size() > dot + 1 ? rest[dot + 1] : QLatin1Char('r');
                    value = channel == QLatin1Char('g') ? c.y : channel == QLatin1Char('b') ? c.z : c.x;
                    return true;
                }
            }
        }
        MaterialField field = MaterialField::Roughness;
        if (!materialFieldByName(rest.toStdString(), field)) {
            return false;
        }
        value = materialFieldValue(p, field);
        return true;
    }
    mine = false;
    return false;
}

/// The application's widgets and Preferences -> Selection: selstyle., uicount. / uivalue. /
/// uienabled. / uivisible.
bool VulkanWindow::scriptUiMetric(const QString& name, double& value, bool& mine) {
    mine = true;
    // PREFERENCES → SELECTION as the viewport holds it: `selstyle.outline.<r|g|b|opacity>` and
    // `selstyle.joint.<r|g|b|opacity>` — the color as display bytes (0-255), the opacity in percent.
    if (name.startsWith(QLatin1String("selstyle."), Qt::CaseInsensitive)) {
        const QStringList part = name.toLower().split(QLatin1Char('.'));
        if (part.size() == 3 && (part[1] == QLatin1String("outline") || part[1] == QLatin1String("joint"))) {
            const bool       outline = part[1] == QLatin1String("outline");
            const glm::vec3& linear = outline ? m_selectionStyle.outlineColor : m_selectionStyle.jointColor;
            const auto displayByte = [](float c) {
                c = std::clamp(c, 0.0f, 1.0f);
                return std::round((c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f) * 255.0f);
            };
            if (part[2] == QLatin1String("opacity")) {
                value = std::round((outline ? m_selectionStyle.outlineOpacity : m_selectionStyle.jointOpacity) * 100.0f);
                return true;
            }
            const int channel = part[2] == QLatin1String("r") ? 0 : part[2] == QLatin1String("g") ? 1 : part[2] == QLatin1String("b") ? 2 : -1;
            if (channel >= 0) {
                value = displayByte(linear[channel]);
                return true;
            }
        }
        return false;
    }
    // THE WIDGETS' metrics (the `ui` command's): `uicount.<id>` (how many widgets carry that
    // testId), `uivalue.<id>.<n>` (the n-th's value: a scrub field's, a checkbox's 0 / 1, a
    // picker's index, a label's number), `uienabled.<id>.<n>` and `uivisible.<id>.<n>` (shown, as
    // far as its window goes). An id may itself contain dots.
    if (name.startsWith(QLatin1String("uicount."), Qt::CaseInsensitive) || name.startsWith(QLatin1String("uivalue."), Qt::CaseInsensitive) ||
        name.startsWith(QLatin1String("uienabled."), Qt::CaseInsensitive) || name.startsWith(QLatin1String("uivisible."), Qt::CaseInsensitive)) {
        const int     dot = name.indexOf(QLatin1Char('.'));
        const QString what = name.left(dot).mid(2).toLower(); // count / value / enabled / visible
        QString       id = name.mid(dot + 1);
        int           index = 0;
        if (what != QLatin1String("count")) {
            const int last = id.lastIndexOf(QLatin1Char('.'));
            bool      isNumber = false;
            const int n = last > 0 ? id.mid(last + 1).toInt(&isNumber) : 0;
            if (isNumber) {
                index = n;
                id = id.left(last);
            }
        }
        notifyMaterialView(); // (the tabs follow the rendered frame: bring them up to date)
        bool ok = false;
        emit uiQueryRequested(what, id, index, &value, &ok);
        return ok;
    }
    mine = false;
    return false;
}

/// What is read off the figure: the Figure group's rows, and everything on the armature.
bool VulkanWindow::scriptFigureMetric(const QString& name, double& value) {
    IkScript&       s = *m_script;
    if (name.startsWith(QLatin1String("figure."), Qt::CaseInsensitive)) {
        // (The Figure group's rows: a posable figure's, or a selected STATIC model's — a splat
        // scene, an OBJ — which has no armature.)
        const JointTransform::Figure fig = jointTransform().figure;
        const QString                which = name.mid(7).toLower();
        value = which == QLatin1String("x") ? fig.x : which == QLatin1String("y") ? fig.y : which == QLatin1String("z") ? fig.z
                : which == QLatin1String("tip") ? fig.tipDeg : which == QLatin1String("tilt") ? fig.tiltDeg
                : which == QLatin1String("scale") ? fig.scalePct : which == QLatin1String("static") ? (fig.isStatic ? 1.0f : 0.0f)
                                                                                                   : fig.turnDeg;
        return true;
    }
    const Armature* arm = scene().figureArmature();
    if (arm == nullptr) {
        return false;
    }
    const auto moved = [&](int bone) { return movedSincePress(*arm, s, bone); };
    const auto axisOf = [&](int bone, const glm::vec3& local) {
        return glm::normalize(glm::mat3(arm->poseGlobal(static_cast<std::size_t>(bone))) * local);
    };
    const QStringList part = name.split(QLatin1Char('.'));
    const QString     key = part[0].toLower();
    const int         bone = part.size() > 1 ? scriptBone(arm, part[1].toStdString()) : -1;
    const int         head = scriptBone(arm, "head"); // (by role on a rig in another convention)
    if (key == QLatin1String("grab")) {
        glm::vec3 grab(0.0f);
        if (!s.dragging || !scene().ikGrabPointWorld(grab)) {
            return false;
        }
        value = glm::length(grab - s.cursorWorld) * 1000.0;
    } else if ((key == QLatin1String("dial") || key == QLatin1String("dialmin") || key == QLatin1String("dialmax")) &&
               part.size() > 1) {
        const JointTransform transform = jointTransform();
        const QString        which = part[1].toLower();
        const int            dial = which == QLatin1String("forward") ? 0 : which == QLatin1String("sideways") ? 1 : 2;
        if (!transform.valid || !transform.dials[dial].enabled) {
            return false;
        }
        value = key == QLatin1String("dial") ? transform.dials[dial].value
                : key == QLatin1String("dialmin") ? transform.dials[dial].minValue : transform.dials[dial].maxValue;
    } else if (key == QLatin1String("modal")) {
        value = m_modal.kind; // the dial the mouse turns (0/1/2), -1 with the mode off
    } else if (key == QLatin1String("dialjoint") && bone >= 0) {
        const JointTransform transform = jointTransform();
        value = transform.valid && transform.name == arm->boneName(static_cast<std::size_t>(bone)) ? 1.0 : 0.0;
    } else if (key == QLatin1String("palm") && bone >= 0 && part.size() > 2) {
        const glm::vec3 pn = arm->handPalmNormal(bone);
        if (glm::length(pn) < 0.5f) {
            return false;
        }
        const QString axis = part[2].toLower();
        value = axis == QLatin1String("x") ? pn.x : axis == QLatin1String("y") ? pn.y : pn.z;
    } else if (key == QLatin1String("grabpx")) {
        // ... on SCREEN, in pixels: what the user sees. (A knee pulled up in a front view is taken
        // FORWARD onto its thigh's reach: under the pointer, and 40cm from the cursor's plane.)
        glm::vec3 grab(0.0f);
        QPointF   at;
        if (!s.dragging || !scene().ikGrabPointWorld(grab) || !projectToScreen(grab, at)) {
            return false;
        }
        value = std::hypot(at.x() - s.cursorPx.x(), at.y() - s.cursorPx.y());
    } else if (key == QLatin1String("hips")) {
        value = moved(scriptBone(arm, "hip")); // (by the alias layer: any rig's hips)
    } else if (key == QLatin1String("head") && part.size() == 1) {
        value = moved(head);
    } else if (key == QLatin1String("group")) {
        value = static_cast<double>(scene().groupBones().size()) + (arm->selectedBone() >= 0 ? 1.0 : 0.0); // the selected joints
    } else if (key == QLatin1String("ingroup") && bone >= 0) {
        const int posed = arm->knownPosingBone(bone);
        value = (posed == arm->selectedBone() || arm->inGroup(posed)) ? 1.0 : 0.0;
    } else if (key == QLatin1String("steps")) {
        value = arm->ikRig() != nullptr ? arm->ikRig()->stepsTaken() : 0;
    } else if (key == QLatin1String("jump")) {
        value = s.jumpMax * 1000.0;
    } else if (key == QLatin1String("breastl") || key == QLatin1String("breastr")) {
        // THE SOFT BREASTS: the left / right breast's swing from the sculpt, degrees (0 with the
        // soft body switched off, or on a flat chest).
        value = static_cast<double>(scene().breastSwingDeg(key == QLatin1String("breastl") ? 0 : 1));
    } else if (key == QLatin1String("breasthangl") || key == QLatin1String("breasthangr")) {
        // THE SOFT BREASTS: how far the left / right breast is drawn out along its tether, mm (0
        // standing, where the tether is level; a level-chested bow reads 30 on a full breast).
        value = static_cast<double>(scene().breastHangMm(key == QLatin1String("breasthangl") ? 0 : 1));
    } else if (key == QLatin1String("breastsidel") || key == QLatin1String("breastsider")) {
        // THE SOFT BREASTS: how far the left / right breast has moved sideways, mm, + outward toward the armpit
        // (on her back the spread, on her side the fall toward the lower side).
        value = static_cast<double>(scene().breastSideMm(key == QLatin1String("breastsidel") ? 0 : 1));
    } else if (key == QLatin1String("breastupl") || key == QLatin1String("breastupr")) {
        // THE SOFT BREASTS: how far the left / right breast has moved along the chest, mm, + up toward
        // the head (a bow's lift, a jolt's or a landing's bounce; below 0 only by the floor's give).
        value = static_cast<double>(scene().breastUpMm(key == QLatin1String("breastupl") ? 0 : 1));
    } else if (key == QLatin1String("breastfields")) {
        // THE BONELESS BREASTS: how many of the figure's breasts are delta fields (a rig with no
        // breast bone: 2; a figure whose pectoral bones carry them, or a flat chest: 0).
        value = static_cast<double>(arm->breastFieldCount());
    } else if (key == QLatin1String("breastfieldl") || key == QLatin1String("breastfieldr")) {
        // ... and how far the left / right one's field moves the skin at its apex, mm: what the GPU
        // blends (the mass's displacement within the field's travel limit).
        value = static_cast<double>(arm->breastFieldMoveMm(key == QLatin1String("breastfieldl") ? 0 : 1));
    } else if (key == QLatin1String("buttl") || key == QLatin1String("buttr")) {
        // THE SOFT BUTTOCKS: how far the left / right cheek has moved from the sculpt, mm (0 at
        // rest in any pose — they never hang — and while the soft body is switched off).
        value = static_cast<double>(scene().buttockMoveMm(key == QLatin1String("buttl") ? 0 : 1));
    } else if (key == QLatin1String("lowest")) {
        // (Not the figure node and its like: parentless, or hanging straight off a parentless node
        // at the origin — the hip's own parent.)
        const int hip = scriptBone(arm, "hip");
        value = 1.0e9;
        for (std::size_t i = 0; i < arm->boneCount(); ++i) {
            bool body = false;
            for (int cur = static_cast<int>(i); cur >= 0 && !body; cur = arm->boneParent(static_cast<std::size_t>(cur))) {
                body = cur == hip;
            }
            if (body) {
                value = std::min(value, static_cast<double>(arm->boneWorldPosition(i).y) * 1000.0);
            }
        }
    } else if (key == QLatin1String("meshlow")) {
        // The posed MESH's lowest point over the floor, mm (the Ground button's measure: the skin,
        // CPU-skinned): negative is flesh through the floor, which no joint reading shows.
        float lowestY = 0.0f;
        if (!scene().figureGroundGap(lowestY)) {
            return false;
        }
        value = static_cast<double>(lowestY) * 1000.0;
    } else if (key == QLatin1String("gaze") && head >= 0) {
        value = glm::degrees(std::asin(glm::clamp(axisOf(head, glm::vec3(0.0f, 0.0f, 1.0f)).y, -1.0f, 1.0f)));
    } else if (key == QLatin1String("headroll") && head >= 0) {
        value = glm::degrees(std::asin(glm::clamp(axisOf(head, glm::vec3(1.0f, 0.0f, 0.0f)).y, -1.0f, 1.0f)));
    } else if (key == QLatin1String("tilt") && bone >= 0) {
        value = glm::degrees(std::acos(glm::clamp(axisOf(bone, glm::vec3(0.0f, 1.0f, 0.0f)).y, -1.0f, 1.0f)));
    } else if (key == QLatin1String("hang") && bone >= 0) {
        // (An upper arm hanging by the side reads 15-25; one held out in front 60 and more.)
        glm::vec3 dir(0.0f);
        if (!longestChildDirection(*arm, bone, dir)) {
            return false;
        }
        value = glm::degrees(std::acos(glm::clamp(-dir.y, -1.0f, 1.0f)));
    } else if (key == QLatin1String("swing") && bone >= 0) {
        // The bone's fore-aft angle from straight down, degrees, + forward of the figure (the hips'
        // heading): an upper arm swung forward reads +20, one hanging back -20.
        glm::vec3 dir(0.0f);
        const int hip = scriptBone(arm, "hip");
        if (!longestChildDirection(*arm, bone, dir) || hip < 0) {
            return false;
        }
        glm::vec3       fore = glm::mat3(arm->poseGlobal(static_cast<std::size_t>(hip))) * glm::vec3(0.0f, 0.0f, 1.0f);
        fore.y = 0.0f;
        fore = glm::length(fore) > 1.0e-4f ? glm::normalize(fore) : glm::vec3(0.0f, 0.0f, 1.0f);
        value = glm::degrees(std::atan2(glm::dot(dir, fore), -dir.y));
    } else if (key == QLatin1String("y") && bone >= 0) {
        value = arm->boneWorldPosition(static_cast<std::size_t>(bone)).y * 1000.0;
    } else if (key == QLatin1String("moved") && bone >= 0) {
        value = moved(bone);
    } else if (key == QLatin1String("selected") && bone >= 0) {
        value = arm->selectedBone() >= 0 && arm->sameRigidSegment(arm->selectedBone(), bone) ? 1.0 : 0.0;
    } else if (key == QLatin1String("lit") && bone >= 0) {
        // (What the mesh shader is told to tint: the highlighted joint and its twin — and what is lit
        // WITH the selection: the face rig under a selected head, a carpal under a selected hand.)
        value = arm->highlightBone() >= 0 && (bone == arm->highlightBone() || bone == arm->selectedHighlightTwin() ||
                                              arm->highlightedWithSelection(bone))
                    ? 1.0
                    : 0.0;
    } else if (key == QLatin1String("movedxz") && bone >= 0) {
        const std::size_t b = static_cast<std::size_t>(bone);
        if (b >= s.pressPositions.size()) {
            return false;
        }
        const glm::vec3 d = arm->boneWorldPosition(b) - s.pressPositions[b];
        value = static_cast<double>(glm::length(glm::vec2(d.x, d.z))) * 1000.0;
    } else if (key == QLatin1String("euler") && bone >= 0 && part.size() > 2) {
        const int axis = part[2].toLower() == QLatin1String("x") ? 0 : part[2].toLower() == QLatin1String("y") ? 1 : 2;
        value = arm->boneEuler(static_cast<std::size_t>(bone))[axis];
    } else {
        return false;
    }
    return true;
}

/// What a user would see, as numbers: where the grabbed point is against the cursor, what the
/// body did since the press, the feet, the steps.
void VulkanWindow::scriptReport(const char* label) {
    IkScript&       s = *m_script;
    const Armature* arm = scene().figureArmature();
    if (arm == nullptr) {
        return;
    }
    const auto moved = [&](int bone) { return static_cast<float>(movedSincePress(*arm, s, bone)); };
    glm::vec3 grab(0.0f);
    const bool hasGrab = s.dragging && scene().ikGrabPointWorld(grab);
    const int  hip = scriptBone(arm, "hip"); // (through the alias layer, as the metrics: any rig's hips and head)
    const int  head = scriptBone(arm, "head");
    const IkRig* rig = arm->ikRig();
    double gaze = 0.0;
    double lowest = 0.0;
    scriptMetric(QStringLiteral("gaze"), gaze);
    double meshLow = 0.0;
    scriptMetric(QStringLiteral("lowest"), lowest);
    scriptMetric(QStringLiteral("meshlow"), meshLow);
    say("[ikscript]   [%s] gaze %.1f deg | lowest joint %.0f mm | lowest skin %.0f mm | worst move: %s at tick %d\n", label, gaze,
        lowest, meshLow, s.jumpBone >= 0 ? arm->boneName(static_cast<std::size_t>(s.jumpBone)).c_str() : "-", s.jumpTick);
    say("[ikscript]   [%s] grab-to-cursor %.1f mm | hips moved %.1f mm, head %.1f mm | pelvis euler (%.1f %.1f %.1f) | "
        "steps %d | worst single-tick joint move %.1f mm\n",
        label, hasGrab ? glm::length(grab - s.cursorWorld) * 1000.0f : 0.0f, moved(hip), moved(head),
        hip >= 0 ? arm->boneEuler(static_cast<std::size_t>(hip)).x : 0.0f,
        hip >= 0 ? arm->boneEuler(static_cast<std::size_t>(hip)).y : 0.0f,
        hip >= 0 ? arm->boneEuler(static_cast<std::size_t>(hip)).z : 0.0f,
        rig != nullptr ? rig->stepsTaken() : 0, s.jumpMax * 1000.0f);
    // The posed channels, for a record of what the pose IS (bones turned more than half a degree).
    QString posed;
    int     listed = 0;
    for (std::size_t i = 0; i < arm->boneCount() && listed < 40; ++i) {
        const glm::vec3& e = arm->boneEuler(i);
        if (std::max({std::abs(e.x), std::abs(e.y), std::abs(e.z)}) > 0.5f && arm->boneWorldPosition(i).y > -1.0f) {
            const std::string& name = arm->boneName(i);
            // (Digits are noise here: by the reference figure's names — and, for a rig that names
            // them otherwise, by what the rig itself reads as a finger's or a toe's joint.)
            const RigRole role = rig != nullptr && rig->roles().built() ? rig->roles().role(static_cast<int>(i)) : RigRole::None;
            if (name.find("Toe") != std::string::npos || name.find("Carpal") != std::string::npos ||
                name.find("Index") != std::string::npos || name.find("Mid") != std::string::npos ||
                name.find("Ring") != std::string::npos || name.find("Pinky") != std::string::npos ||
                name.find("Thumb") != std::string::npos || role == RigRole::Finger || role == RigRole::Toe) {
                continue;
            }
            posed += QStringLiteral(" %1(%2 %3 %4)").arg(QString::fromStdString(name)).arg(e.x, 0, 'f', 0).arg(e.y, 0, 'f', 0).arg(e.z, 0, 'f', 0);
            ++listed;
        }
    }
    say("[ikscript]   [%s] posed:%s\n", label, qPrintable(posed));
}

} // namespace pose
