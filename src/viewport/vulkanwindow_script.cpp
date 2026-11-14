/**
 * @file vulkanwindow_script.cpp
 * @brief The scripted in-app posing test (POSESTUDIO_IK_SCRIPT=<file>): IkScript's loader plus
 *        the runner on VulkanWindow.
 *
 * One of the translation units of VulkanWindow — see vulkanwindow.h and ikscript.h. Where the
 * IK harness (tools/ikharness) drives the Armature alone, this drives the APP: a `press` is the
 * real picker at a pixel followed by the real press path, a `drag` moves a scripted cursor in
 * SCREEN space through the real camera-parallel drag plane, the 60 Hz timer solves, the release
 * settles, and `shot` saves the frame the renderer produced. Nothing is injected into the OS —
 * the window's own gesture functions are called — so it runs on a desktop that is in use, and
 * the frames are read back from the GPU, so the window need not even be visible.
 *
 * tools/ikscripts/README.md is the command reference.
 */

#include "vulkanwindow.h"

#include "ikscript.h"
#include "rendering/vulkanrenderer.h"
#include "scene/armature.h"
#include "scene/ik/bonealiases.h"
#include "scene/ik/ikrig.h"
#include "scene/mesh.h"
#include "scene/model.h"
#include "scene/scene.h"
#include "smoothnormals.h" // the scripted `matfaceted`

#include "importformats.h" // the scripted `import`'s route by extension

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace pose {

std::unique_ptr<IkScript> IkScript::fromEnvironment() {
    const QString path = qEnvironmentVariable("POSESTUDIO_IK_SCRIPT");
    if (path.isEmpty()) {
        return nullptr;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // (Out, not on: a run that names a script the app cannot read must not sit there as an
        // ordinary app while its runner waits for a "done" that never comes - it did, on screen,
        // until someone noticed. The exit code is the runner's own "did not finish".)
        std::fprintf(stderr, "[ikscript] cannot read %s\n", qPrintable(path));
        std::fflush(stderr);
        std::_Exit(101);
    }
    auto script = std::make_unique<IkScript>();
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        const int hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0) {
            line.truncate(hash);
        }
        line = line.trimmed();
        if (!line.isEmpty()) {
            script->lines.push_back(line);
        }
    }
    script->outDir = qEnvironmentVariable("POSESTUDIO_IK_SCRIPT_OUT");
    if (script->outDir.isEmpty()) {
        script->outDir = QFileInfo(path).absolutePath();
    }
    QDir().mkpath(script->outDir);
    return script;
}

namespace {

void say(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fflush(stderr);
}

/// A script names bones the way the reference figure does; the other generations call the same
/// joints something else: the alias table the IK harness shares (scene/ik/bonealiases.h). The
/// name as given if the figure has it, else the first alternative it has, else the name.
std::string figureBoneName(const Armature* arm, const std::string& canonical) {
    return arm != nullptr ? aliasedBoneName(*arm, canonical) : canonical;
}
std::vector<glm::vec3> jointPositions(const Armature* arm) {
    std::vector<glm::vec3> out;
    if (arm != nullptr) {
        out.resize(arm->boneCount());
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = arm->boneWorldPosition(i);
        }
    }
    return out;
}

/// How far @p bone has moved since the press (mm), 0 for a bone the press did not record.
double movedSincePress(const Armature& arm, const IkScript& s, int bone) {
    return bone >= 0 && static_cast<std::size_t>(bone) < s.pressPositions.size()
               ? glm::length(arm.boneWorldPosition(static_cast<std::size_t>(bone)) -
                             s.pressPositions[static_cast<std::size_t>(bone)]) * 1000.0
               : 0.0;
}

} // namespace

/// The script's first step, once a posable figure — or, for a look at a static model alone (a
/// splat capture), any model — exists (polled: a command-line figure imports through the
/// progress dialog, whose event loop fires this timer mid-import).
void VulkanWindow::startScript() {
    IkScript& s = *m_script;
    // (As the bench: a command-line figure imports through the progress dialog, whose event loop
    // fires this timer mid-import. Poll until a posable figure, or any model, exists.)
    const auto ready = [&] { return m_renderer && (scene().hasPosableFigure() || scene().modelCount() > 0); };
    if (!ready() && s.retries++ < 120) {
        QTimer::singleShot(500, this, &VulkanWindow::startScript);
        return;
    }
    if (!ready()) {
        say("[ikscript] no figure or model: give one on the command line\n");
        std::_Exit(2);
    }
    say("[ikscript] %d commands, output in %s\n", static_cast<int>(s.lines.size()), qPrintable(s.outDir));
    scriptStep();
}

/// Resolves "<bone>[@share]" to the pixel a user would click: the joint itself, or the point that
/// share of the way along the bone's rigid limb segment. False when the bone is unknown or the
/// point is behind the camera.
bool VulkanWindow::scriptPixelOf(const QString& spec, QPointF& px) {
    const std::string bone = figureBoneName(scene().figureArmature(), spec.section(QLatin1Char('@'), 0, 0).toStdString());
    const QString     share = spec.section(QLatin1Char('@'), 1, 1);
    const int named = scene().selectBoneByName(bone);
    if (named < 0) {
        say("[ikscript] unknown bone '%s'\n", bone.c_str());
        return false;
    }
    if (share.isEmpty()) {
        // (The NAMED bone's own joint — not the selection's: a bone of the face rig selects the head,
        // and a click "at the eye" is a click at the eye's pixel.)
        const Armature* arm = scene().figureArmature();
        return arm != nullptr && projectToScreen(arm->boneWorldPosition(static_cast<std::size_t>(named)), px);
    }
    scene().setIkGrabOnSegment(share.toFloat());
    glm::vec3 point(0.0f);
    return scene().ikGrabPointWorld(point) && projectToScreen(point, px);
}

/// Runs script lines until one has to WAIT for the app — a drag's ticks (`drag`/`dragw`/`dragv`),
/// a release's settle, a shot's frame, a `wait` — and returns; the event that ends the wait
/// (scriptDragTick's last tick, scriptSettled, scriptFrameRendered, the wait's timer) schedules
/// the next call. At the last line it prints the verdict and exits with the failure count.
void VulkanWindow::scriptStep() {
    IkScript& s = *m_script;
    while (s.next < s.lines.size()) {
        const QString     line = s.lines[s.next++];
        const QStringList w = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QString     cmd = w[0].toLower();
        const auto        num = [&](int i) { return i < w.size() ? w[i].toFloat() : 0.0f; };
        say("[ikscript] > %s\n", qPrintable(line));

        if (cmd == QLatin1String("view") && w.size() > 1) {
            const QString v = w[1].toLower();
            if (v == QLatin1String("home")) {
                resetView();
            } else {
                const AxisView axis = v == QLatin1String("back")     ? AxisView::Back
                                      : v == QLatin1String("left")   ? AxisView::Left
                                      : v == QLatin1String("right")  ? AxisView::Right
                                      : v == QLatin1String("top")    ? AxisView::Top
                                      : v == QLatin1String("bottom") ? AxisView::Bottom
                                                                     : AxisView::Front;
                setAxisView(axis);
            }
        } else if (cmd == QLatin1String("orbit")) {
            // orbit <yawDeg> <pitchDeg>: what an orbit drag does (and, like one, back to perspective)
            m_renderer->camera().orbit(glm::radians(num(1)), glm::radians(num(2))); // (degrees in, radians to the camera)
            requestUpdate();
        } else if (cmd == QLatin1String("frame")) {
            frameSelected();
        } else if (cmd == QLatin1String("lookat") && w.size() > 4) {
            // lookat <x> <y> <z> <radius m>: the camera framed on a WORLD point — a splat scene's
            // face or hands, which no bone names (closeup frames a joint).
            m_renderer->camera().frame(glm::vec3(num(1), num(2), num(3)), std::max(0.02f, num(4)));
            requestUpdate();
        } else if (cmd == QLatin1String("closeup") && w.size() > 2) {
            // closeup <bone> <radius m>: the camera framed on that joint — a hand, a foot, the face
            // (what a user does with the wheel and a pan before posing a finger).
            const Armature* arm = scene().figureArmature();
            const int       bone = arm ? arm->boneIndex(figureBoneName(arm, w[1].toStdString())) : -1;
            if (bone >= 0) {
                m_renderer->camera().frame(arm->boneWorldPosition(static_cast<std::size_t>(bone)), std::max(0.02f, num(2)));
                requestUpdate();
            } else {
                say("[ikscript]   unknown bone\n");
            }
        } else if (cmd == QLatin1String("shade") && w.size() > 1) {
            setShadeMode(w[1].toInt());
        } else if (cmd == QLatin1String("pose") && w.size() > 1) {
            if (!loadPose(line.section(QLatin1Char(' '), 1).trimmed())) {
                say("[ikscript]   pose not loaded\n");
            }
        } else if (cmd == QLatin1String("reset")) {
            runPoseUtility(PoseUtility::ResetPose);
        } else if (cmd == QLatin1String("undo")) {
            undo();
        } else if (cmd == QLatin1String("redo")) {
            redo();
        } else if (cmd == QLatin1String("ground")) {
            groundFigure();
        } else if (cmd == QLatin1String("lift") && w.size() > 1) {
            // lift <metres>: the active figure raised that far off the floor, as posed — what a pose
            // that bends the knees, or a pose file, leaves a figure at. `ground` then drops her.
            closeOpenPoseEdits();
            scene().translateModelY(scene().activeFigureIndex(), num(1));
            requestUpdate();
        } else if (cmd == QLatin1String("fk") && w.size() >= 5) {
            // fk <bone> <dx> <dy> <dz>: the joint turned by that many degrees about its own
            // channels, as a Transform dial turns one (through the limits and the FK collision
            // stop), as one undoable edit.
            if (scene().selectBoneByName(figureBoneName(scene().figureArmature(), w[1].toStdString())) >= 0) {
                closeOpenPoseEdits();
                m_preEditPose = scene().capturePose();
                scene().nudgeSelectedBone(glm::vec3(num(2), num(3), num(4)));
                scene().finalizePose();
                commitPoseUndo();
                requestUpdate();
            } else {
                say("[ikscript]   unknown bone\n");
            }
        } else if (cmd == QLatin1String("move") && w.size() > 3) {
            // move <dx> <dy> <dz>: the whole figure moved by that, in world centimetres (the
            // Transform tab's Figure group: her left, up, forward), one undoable edit.
            if (beginJointTransformEdit()) {
                const JointTransform::Figure fig = jointTransform().figure;
                setFigurePosition(0, fig.x + num(1));
                setFigurePosition(1, fig.y + num(2));
                setFigurePosition(2, fig.z + num(3));
                endJointTransformEdit();
            } else {
                say("[ikscript]   move refused (a drag owns the pose)\n");
            }
        } else if (cmd == QLatin1String("scale") && w.size() > 1) {
            // scale <percent>: a STATIC model's uniform scale (the Figure group's Scale row; a
            // figure keeps her own size).
            if (beginJointTransformEdit()) {
                setFigureScale(num(1));
                endJointTransformEdit();
            } else {
                say("[ikscript]   scale refused (a drag owns the pose)\n");
            }
        } else if ((cmd == QLatin1String("turn") || cmd == QLatin1String("tip") || cmd == QLatin1String("tilt")) && w.size() > 1) {
            // turn / tip / tilt <degrees>: the whole figure's rotation about the vertical, about
            // her own lateral axis (forward positive), about her own forward axis (her left down
            // positive) — the Figure group's three rows.
            if (beginJointTransformEdit()) {
                setFigureRotation(cmd == QLatin1String("turn") ? 0 : cmd == QLatin1String("tip") ? 1 : 2, num(1));
                endJointTransformEdit();
            } else {
                say("[ikscript]   %s refused (a drag owns the pose)\n", qPrintable(cmd));
            }
        } else if (cmd == QLatin1String("select") && w.size() > 1) {
            // select <index>: the model selected, as a click on its box does (-1 = none) — the way a
            // script reaches a STATIC model's materials (it has no joint to click).
            scene().setSelectedModel(static_cast<int>(num(1)));
            requestUpdate();
        } else if (cmd == QLatin1String("zone") && w.size() > 1) {
            // zone <index|name>: the Materials tab's zone picker.
            bool ok = false;
            int  index = w[1].toInt(&ok);
            if (!ok) {
                const MaterialView view = materialView();
                index = -1;
                for (std::size_t i = 0; i < view.zones.size(); ++i) {
                    if (QString::fromStdString(view.zones[i]).compare(w[1], Qt::CaseInsensitive) == 0) {
                        index = static_cast<int>(i);
                        break;
                    }
                }
                if (index < 0) {
                    say("[ikscript]   zone '%s' unknown\n", qPrintable(w[1]));
                }
            }
            if (index >= 0) {
                setMaterialZone(index);
            }
        } else if (cmd == QLatin1String("material") && w.size() > 2) {
            // material <field> <value> | material <color> <r> <g> <b> (linear; `color` the base
            // color, else a materialColorName — specularcolor, coatcolor, sheencolor,
            // transmissioncolor, emissioncolor, subsurfacecolor, scattercolor): the selected zone's
            // material set the way the Materials tab does it (one bracketed, undoable edit; the
            // fields and their display units are the tab's — materialparams.h), reaching the zones
            // that share the imported material while `matlink` is on.
            MaterialParams p = materialView().params;
            bool           known = true;
            MaterialColor  which = MaterialColor::Base;
            if (w.size() > 4 && (w[1].compare(QLatin1String("color"), Qt::CaseInsensitive) == 0 ||
                                 materialColorByName(w[1].toLower().toStdString(), which))) {
                materialColorRef(p, which) =
                    glm::vec3(static_cast<float>(num(2)), static_cast<float>(num(3)), static_cast<float>(num(4)));
            } else {
                MaterialField field = MaterialField::Roughness;
                if (materialFieldByName(w[1].toLower().toStdString(), field)) {
                    setMaterialField(p, field, static_cast<float>(num(2)));
                } else {
                    known = false;
                    say("[ikscript]   material field '%s' unknown\n", qPrintable(w[1]));
                }
            }
            if (known && beginMaterialEdit()) {
                setSelectedMaterial(p, s.materialLinked);
                endMaterialEdit();
            }
        } else if (cmd == QLatin1String("matlink") && w.size() > 1) {
            // matlink on|off: the tab's "Edit matching zones together".
            s.materialLinked = w[1].compare(QLatin1String("off"), Qt::CaseInsensitive) != 0;
        } else if (cmd == QLatin1String("matreset")) {
            // matreset [all]: the tab's Reset Zone / Reset All Zones.
            resetSelectedMaterial(s.materialLinked, w.size() > 1 && w[1].compare(QLatin1String("all"), Qt::CaseInsensitive) == 0);
        } else if (cmd == QLatin1String("texture") && w.size() > 2) {
            // texture <slot> <path|none|imported>: the tab's Textures group — the selected zone's
            // slot replaced by an image file (no spaces in the path: the line is split on them),
            // cleared, or put back to the imported map; one undoable edit. The slot names are
            // materialMapName's (basecolor, normal, bump, roughness, metalness, specmask,
            // translucency, emission, pore, cutout).
            MaterialMap map = MaterialMap::BaseColor;
            if (!materialMapByName(w[1].toLower().toStdString(), map)) {
                say("[ikscript]   texture slot '%s' unknown\n", qPrintable(w[1]));
            } else {
                const QString what = w[2];
                QString       source;
                if (what.compare(QLatin1String("imported"), Qt::CaseInsensitive) == 0) {
                    source = QString::fromLatin1(kImportedTexture);
                } else if (what.compare(QLatin1String("none"), Qt::CaseInsensitive) != 0) {
                    // A relative path is a test asset's: against POSESTUDIO_IK_SCRIPT_ASSETS (the
                    // folder tools/materials/make_test_assets.py wrote), else the output folder.
                    source = what;
                    if (QFileInfo(what).isRelative()) {
                        const QString assets = qEnvironmentVariable("POSESTUDIO_IK_SCRIPT_ASSETS");
                        source = QDir(assets.isEmpty() ? s.outDir : assets).filePath(what);
                    }
                }
                if (!setMaterialTexture(map, source)) {
                    say("[ikscript]   texture '%s' could not be applied\n", qPrintable(what));
                }
            }
        } else if (cmd == QLatin1String("overlay") && w.size() > 1) {
            // THE OVERLAYS (materialoverlay.h), the way the tab's Overlays section edits the
            // selected zone's stack — every form one undoable edit, reaching every zone painted
            // on the zone's color map:
            //   overlay add [image]                     a new overlay on top: EMPTY, as the tab's
            //                                           + Add Overlay makes it, or with its picture
            //   overlay remove <n> | overlay move <n> <to>
            //   overlay <n> <field> <value>             enabled, opacity, brightness, contrast, gamma,
            //                                           hue, saturation, gloss, metallic,
            //                                           bumpstrength, normalstrength,
            //                                           displacementstrength, offsetx, offsety,
            //                                           rotation, scale (the tab's units),
            //                                           blend <name|index>
            //   overlay <n> tint <r> <g> <b>            the card's Base Color: display (sRGB) values 0..1
            //   overlay <n> name <text…>                its name (a new one is "Overlay N")
            //   overlay <n> map <color|opacity|metallic|bump|normal|displacement> <image|none>
            // (`map opacity` is the Opacity row's map: the overlay's mask.)
            // <n> counts from 0, the lowest layer. A relative image path is a test asset's (as
            // `texture`'s: POSESTUDIO_IK_SCRIPT_ASSETS, else the output folder).
            std::vector<MaterialOverlay> stack = materialView().params.overlays;
            const QString                what = w[1].toLower();
            const auto                   imagePath = [&](const QString& given) -> std::string {
                if (given.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0) {
                    return std::string();
                }
                if (QFileInfo(given).isRelative()) {
                    const QString assets = qEnvironmentVariable("POSESTUDIO_IK_SCRIPT_ASSETS");
                    return QDir(assets.isEmpty() ? s.outDir : assets).filePath(given).toStdString();
                }
                return given.toStdString();
            };
            bool changed = false;
            if (what == QLatin1String("add")) {
                MaterialOverlay overlay;
                overlay.name = "Overlay " + std::to_string(nextOverlayNumber(stack, "Overlay")); // (as the tab names one)
                if (w.size() > 2) {
                    overlay.color = imagePath(w[2]);
                }
                stack.push_back(std::move(overlay));
                changed = true;
            } else if (w.size() > 2) {
                const bool indexFirst = what != QLatin1String("remove") && what != QLatin1String("move");
                bool       ok = false;
                const int  n = (indexFirst ? w[1] : w[2]).toInt(&ok);
                if (!ok || n < 0 || static_cast<std::size_t>(n) >= stack.size()) {
                    say("[ikscript]   overlay %s: no such overlay (the zone has %d)\n", qPrintable(indexFirst ? w[1] : w[2]),
                        static_cast<int>(stack.size()));
                } else if (what == QLatin1String("remove")) {
                    stack.erase(stack.begin() + n);
                    changed = true;
                } else if (what == QLatin1String("move") && w.size() > 3) {
                    const int       to = std::clamp(w[3].toInt(), 0, static_cast<int>(stack.size()) - 1);
                    MaterialOverlay moved = std::move(stack[static_cast<std::size_t>(n)]);
                    stack.erase(stack.begin() + n);
                    stack.insert(stack.begin() + to, std::move(moved));
                    changed = true;
                } else if (indexFirst && w.size() > 3 && w[2].compare(QLatin1String("name"), Qt::CaseInsensitive) == 0) {
                    stack[static_cast<std::size_t>(n)].name = w.mid(3).join(QLatin1Char(' ')).toStdString();
                    changed = true;
                } else if (indexFirst && w.size() > 5 && w[2].compare(QLatin1String("tint"), Qt::CaseInsensitive) == 0) {
                    stack[static_cast<std::size_t>(n)].tint = glm::clamp(glm::vec3(num(3), num(4), num(5)), glm::vec3(0.0f), glm::vec3(1.0f));
                    changed = true;
                } else if (indexFirst && w.size() > 4 && w[2].compare(QLatin1String("map"), Qt::CaseInsensitive) == 0) {
                    OverlayElement element = OverlayElement::Color;
                    if (overlayElementByName(w[3].toLower().toStdString(), element)) {
                        overlayElementSource(stack[static_cast<std::size_t>(n)], element) = imagePath(w[4]);
                        changed = true;
                    } else {
                        say("[ikscript]   overlay element '%s' unknown\n", qPrintable(w[3]));
                    }
                } else if (indexFirst && w.size() > 3) {
                    OverlayField field = OverlayField::Opacity;
                    OverlayBlend blend = OverlayBlend::Multiply;
                    if (!overlayFieldByName(w[2].toLower().toStdString(), field)) {
                        say("[ikscript]   overlay field '%s' unknown\n", qPrintable(w[2]));
                    } else if (field == OverlayField::Blend && overlayBlendByName(w[3].toLower().toStdString(), blend)) {
                        stack[static_cast<std::size_t>(n)].blend = blend;
                        changed = true;
                    } else {
                        setOverlayField(stack[static_cast<std::size_t>(n)], field, num(3));
                        changed = true;
                    }
                }
            }
            if (changed && !setMaterialOverlays(stack)) {
                say("[ikscript]   overlay: no zone selected\n");
            }
        } else if (cmd == QLatin1String("ui") && w.size() > 2) {
            // ui click <id> [n] | ui set <id> <n> <value> | ui check <id> <n> <0|1> | ui pick <id> <n> <k>:
            // one of the application's widgets driven in-process — the n-th (from 0, in creation
            // order) carrying the dynamic property `testId` == <id> — through its own slot, as a
            // user's click, scrub or pick would run it; no desktop input. What a panel's `overlay`
            // / `material` commands cannot show: that ITS buttons are wired to the right thing.
            // (The tabs follow the rendered frame, which the script does not wait for: bring them
            // up to date first.)
            notifyJointTransform();
            notifyMaterialView();
            // (Also: ui call <id> <n> <slot> [int] — one of the widget's own slots or invokables
            // taking an int; ui type <id> <n> <text…> — a line edit's text; ui key <id> <n>
            // <enter|esc> — that key to the widget. What opens, fills and closes an inline editor.
            // And: ui scrub <id> <n> <% of its width> [shift] — a press, a drag that far and a
            // release on a scrub field, through its own mouse handlers; `shift` holds Shift (the
            // fine scrub).)
            emit uiActionRequested(w[1].toLower(), w[2], w.size() > 3 ? w[3].toInt() : 0, w.size() > 4 ? w[4].toDouble() : 0.0,
                                   w.size() > 4 ? w.mid(4).join(QLatin1Char(' ')) : QString());
        } else if (cmd == QLatin1String("dial") && w.size() > 2) {
            // dial <forward|sideways|twist> <value> [abs]: the selected joint's Transform-tab dial set
            // to that value, the way the tab does it (one bracketed, undoable edit); with a group the
            // others turn by the same change — or, `abs`, to the value itself (the restore buttons).
            const QString which = w[1].toLower();
            const int     dial = which == QLatin1String("forward") ? 0 : which == QLatin1String("sideways") ? 1 : 2;
            if (beginJointTransformEdit()) {
                setJointTransformDial(dial, w[2].toDouble(), w.size() > 3 && w[3].toLower() == QLatin1String("abs"));
                endJointTransformEdit();
            } else {
                say("[ikscript]   dial refused (a drag owns the pose)\n");
            }
        } else if (cmd == QLatin1String("modal") && w.size() > 1) {
            // modal <bend|side|twist>: the B / S / T key — a key event handed to the window's own
            // keyPressEvent, so the routing is the real one (the same key again drops the mode,
            // another of the three switches). The mode reads the desktop's cursor at the key; the
            // script's stands at the middle of the view instead: only its moves count.
            const QString which = w[1].toLower();
            const int     key = (which == QLatin1String("bend") || which == QLatin1String("forward")) ? Qt::Key_B
                                : (which == QLatin1String("side") || which == QLatin1String("sideways")) ? Qt::Key_S
                                                                                                          : Qt::Key_T;
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
            keyPressEvent(&press);
            m_modalCursor = QPointF(width() * 0.5, height() * 0.5);
            m_lastMousePos = m_modalCursor;
            say("[ikscript]   joint mouse mode %s\n", m_modalKind >= 0 ? "on" : "off");
        } else if (cmd == QLatin1String("app") && w.size() > 1) {
            // app <b|s|t|esc|enter|click|rclick> / app move <dxPx> <dyPx> [steps]: the same input
            // arriving at the APPLICATION with the viewport not focused — the Edit menu's action
            // for the key, and key and mouse events delivered to the main window, where the mode's
            // application-wide filter (JointModalInput) must pick them up. ViewportWidget makes them.
            // (The menu's actions are enabled by the joint the Transform state last ANNOUNCED, which
            // follows the rendered frame; the script does not wait for one: bring it up to date.)
            notifyJointTransform();
            const QString what = w[1].toLower();
            const bool    move = what == QLatin1String("move") && w.size() > 3;
            emit appInputRequested(what, move ? QPointF(num(2), num(3)) : QPointF(),
                                   move ? std::max(1, w.size() > 4 ? w[4].toInt() : 20) : 1);
            say("[ikscript]   joint mouse mode %s\n", m_modalKind >= 0 ? "on" : "off");
        } else if (cmd == QLatin1String("modalmove") && w.size() > 2) {
            // modalmove <dxPx> <dyPx> [steps]: the mouse moved that far (+y down) with no button
            // held, in that many move events (20 by default) through the window's mouseMoveEvent.
            const int     steps = std::max(1, w.size() > 3 ? w[3].toInt() : 20);
            const QPointF step(num(1) / steps, num(2) / steps);
            for (int k = 0; k < steps; ++k) {
                const QPointF at = m_lastMousePos + step;
                QMouseEvent   move(QEvent::MouseMove, at, mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                mouseMoveEvent(&move);
            }
        } else if (cmd == QLatin1String("modalkeep") || cmd == QLatin1String("modaldrop")) {
            // modalkeep [enter] / modaldrop [esc]: the left click (or Enter) that keeps the result /
            // the right click (or Esc) that puts the joint back — press AND release through the
            // window's handlers, so a click that leaked through the mode would show as a changed
            // selection, a drag, or the context menu it must not open.
            const bool keep = cmd == QLatin1String("modalkeep");
            if (w.size() > 1) {
                QKeyEvent press(QEvent::KeyPress, keep ? Qt::Key_Return : Qt::Key_Escape, Qt::NoModifier);
                keyPressEvent(&press);
            } else {
                const Qt::MouseButton button = keep ? Qt::LeftButton : Qt::RightButton;
                const QPointF         at = m_lastMousePos;
                QMouseEvent down(QEvent::MouseButtonPress, at, mapToGlobal(at), button, button, Qt::NoModifier);
                mousePressEvent(&down);
                QMouseEvent up(QEvent::MouseButtonRelease, at, mapToGlobal(at), button, Qt::NoButton, Qt::NoModifier);
                mouseReleaseEvent(&up);
            }
        } else if (cmd == QLatin1String("saveproject") && w.size() > 1) {
            // saveproject <name.pss>: File -> Save As into the run's folder (the .pss document).
            const QString path = QDir(s.outDir).filePath(w[1]);
            const bool    saved = saveProjectFile(path);
            if (saved) {
                setProjectPath(path); // as the menu's Save As does: the document's path, and clean
                setProjectClean();
            }
            say("[ikscript]   project %s: %s\n", saved ? "saved" : "NOT saved", qPrintable(path));
        } else if (cmd == QLatin1String("openproject") && w.size() > 1) {
            // openproject <name.pss>: File -> Open from the run's folder — the scene reset and every
            // figure re-imported and restored (the recovery dialog's re-pointing is not driven here).
            std::vector<std::string> missing;
            std::string              error;
            const int status = loadProjectFile(QDir(s.outDir).filePath(w[1]), {}, missing, error);
            say("[ikscript]   project %s%s%s\n", status == 0 ? "opened" : status == 1 ? "has missing sources" : "NOT opened: ",
                status < 0 ? error.c_str() : "", status == 1 ? (" " + missing.front()).c_str() : "");
        } else if (cmd == QLatin1String("new")) {
            resetToEmptyScene(); // File -> New's scene reset (the menu's prompt and panel reset are not driven here)
        } else if ((cmd == QLatin1String("outlinestyle") || cmd == QLatin1String("jointstyle")) && w.size() > 2) {
            // outlinestyle <rrggbb> <opacity%> / jointstyle <rrggbb> <opacity%>: the selection
            // outline's / the joint highlight's color (hex, no #: a # begins a comment) and opacity,
            // as Preferences → Selection hands them to the viewport — nothing is stored.
            const QColor color = QColor::fromString(QLatin1Char('#') + w[1]);
            if (!color.isValid()) {
                say("[ikscript]   %s: '%s' is no rrggbb color\n", qPrintable(cmd), qPrintable(w[1]));
            } else if (cmd == QLatin1String("outlinestyle")) {
                setSelectionOutline(color, static_cast<float>(num(2) / 100.0));
            } else {
                setJointHighlight(color, static_cast<float>(num(2) / 100.0));
            }
        } else if (cmd == QLatin1String("prefs") && w.size() > 1) {
            // prefs <tab>: the Preferences dialog opened on that tab, as Edit → Preferences opens it
            // (from the event loop: give it a `wait` before driving its widgets with `ui`; picture
            // it with `uishot`, close it with `ui click preferences.close`).
            emit preferencesRequested(w.mid(1).join(QLatin1Char(' ')));
        } else if (cmd == QLatin1String("renderwindow")) {
            // renderwindow [start]: the Render window, as Render → Show Render Window opens it —
            // with `start`, as Render → Render Image does (from the event loop: `wait` before
            // driving it with `ui`; `uienabled.render.stop.0` reads 1 while its render runs).
            emit renderWindowRequested(w.size() > 1 && w[1].compare(QLatin1String("start"), Qt::CaseInsensitive) == 0);
        } else if (cmd == QLatin1String("probe") && w.size() > 3) {
            // probe <x> <y> <z>: a WORLD point (metres) — `render.probe.<r|g|b|a|lum>` then reads the
            // last render's picture where that point is seen (through the viewport's camera as
            // it stands: a render of the viewport's own size frames what the viewport frames).
            s.probeSet = true;
            s.probe[0] = w[1].toFloat();
            s.probe[1] = w[2].toFloat();
            s.probe[2] = w[3].toFloat();
        } else if (cmd == QLatin1String("import") && w.size() > 1) {
            // import <file>: File → Import, without its dialogs — a model, a figure or a Gaussian
            // splat scene by its extension, added to the scene as it stands (a relative path
            // resolves like `texture`'s: POSESTUDIO_IK_SCRIPT_ASSETS, else the output folder).
            QString file = line.section(QLatin1Char(' '), 1).trimmed();
            if (QFileInfo(file).isRelative()) {
                const QString assets = qEnvironmentVariable("POSESTUDIO_IK_SCRIPT_ASSETS");
                file = QDir(assets.isEmpty() ? s.outDir : assets).filePath(file);
            }
            PendingImport import;
            import.path = file;
            import.kind = ImportFormats::isSplatFile(file)   ? PendingImport::Kind::Splat
                          : ImportFormats::isModelFile(file) ? PendingImport::Kind::Obj
                                                             : PendingImport::Kind::Figure;
            closeSettlingEdits();
            if (!runImport(import, /*showProgress=*/false)) {
                say("[ikscript]   %s was NOT imported\n", qPrintable(file));
            }
            requestUpdate();
        } else if (cmd == QLatin1String("delete")) {
            // delete: the Delete key — the selected light goes (one undo entry), else the selected
            // model (which clears the undo history).
            deleteSelectedObject();
            requestUpdate();
        } else if (cmd == QLatin1String("windowsize") && w.size() > 2) {
            // windowsize <width> <height>: the main window resized, as the user resizes it (logical
            // pixels). The viewport follows through its own resize events: `wait` a few ticks.
            emit scriptWindowSizeRequested(std::max(400, w[1].toInt()), std::max(300, w[2].toInt()));
        } else if (cmd == QLatin1String("uishot") && w.size() > 1) {
            // uishot <name>: ui/<name>.png of the application's widgets — the side tabs (start on
            // one with POSESTUDIO_TAB=<title>); in a folder of its own, beside the viewport's shots
            // and out of their contact sheet. The Transform tab is brought up to date first: it
            // follows the rendered frame, and the script does not wait for one here.
            notifyJointTransform();
            notifyMaterialView();
            notifyLights();
            QDir(s.outDir).mkpath(QStringLiteral("ui"));
            emit uiShotRequested(QDir(s.outDir).filePath(QStringLiteral("ui/") + w[1] + QStringLiteral(".png")));
        } else if (cmd == QLatin1String("pin") && w.size() > 1) {
            // The joint a CLICK there finds — and, since the pick is the SKIN's (2026-09-26), the named
            // bone by name where the click lands on another part of the body (a hanging hand covers
            // the hip from the side: the click pinned a finger, and a "seated, hips pinned" lean had no
            // seat), as press does.
            QPointF px;
            if (scriptPixelOf(w[1], px)) {
                const int picked = boneAt(px);
                const Armature* found = scene().figureArmature();
                const std::string name = figureBoneName(found, w[1].section(QLatin1Char('@'), 0, 0).toStdString());
                const int wanted = found ? found->knownPosingBone(found->boneIndex(name)) : -1; // (a face-rig bone's name: the head)
                if (wanted >= 0 && (picked < 0 || !found->sameRigidSegment(picked, wanted))) {
                    if (picked >= 0) {
                        say("[ikscript]   the pick found %s there, not %s: pinned by name instead\n",
                            found->boneName(static_cast<std::size_t>(picked)).c_str(), name.c_str());
                    }
                    scene().selectBoneByName(name);
                }
                if (scene().hasSelectedBone()) {
                    togglePinSelectedJoint();
                }
            }
        } else if (cmd == QLatin1String("sclick") && w.size() > 1) {
            // sclick <bone>: the Shift+CLICK at the bone's pixel — adds the joint to THE GROUP (as its
            // primary) or removes a member; prints the group afterwards.
            const Armature*        before = scene().figureArmature();
            const int              prevPrimary = before != nullptr ? before->selectedBone() : -1;
            const std::vector<int> prevGroup = scene().groupBones();
            QPointF px;
            if (scriptPixelOf(w[1], px)) {
                scene().restoreSelection(prevPrimary, prevGroup); // (the lookup selected the bone by name: see spress)
                closeOpenPoseEdits();
                const bool began = beginGroupGesture(px);
                if (began) {
                    const int clicked = m_ik.groupClickBone;
                    abortIkDrag();
                    if (clicked >= 0) {
                        scene().removeGroupBone(clicked);
                        m_ik.groupClickBone = -1;
                    }
                }
                const Armature* arm = scene().figureArmature();
                std::string     members = arm && arm->selectedBone() >= 0
                                              ? arm->boneName(static_cast<std::size_t>(arm->selectedBone()))
                                              : std::string("nothing");
                for (const int member : scene().groupBones()) {
                    members += " + " + (arm ? arm->boneName(static_cast<std::size_t>(member)) : std::string("?"));
                }
                say("[ikscript]   shift-click at px(%.0f %.0f): the group is %s\n", px.x(), px.y(), members.c_str());
                requestUpdate();
            }
        } else if (cmd == QLatin1String("unpinall")) {
            unpinAllJoints();
        } else if (cmd == QLatin1String("pick2d") && w.size() > 1) {
            scene().setPick2D(w[1] == QLatin1String("on")); // (the A/B probe: the 2D pick alone, as before 2026-09-26)
        } else if ((cmd == QLatin1String("clickat") && w.size() > 3) ||
                   (cmd == QLatin1String("clickpx") && w.size() > 2)) {
            // clickat <x> <y> <z>: a real LEFT CLICK — a press and its release, no move between —
            // at the pixel a WORLD point (metres) is seen at, through the window's own handlers:
            // exactly what a user's click there does (a joint taken and let go, the model under
            // the cursor selected, or the selection cleared). `selection` then reads the model.
            // clickpx <x%> <y%>: the same at a point of the VIEW, in percent of its width and
            // height from the top left (so a script does not depend on the window's size).
            QPointF    at;
            const bool byPixel = cmd == QLatin1String("clickpx");
            if (byPixel) {
                at = QPointF(num(1) * 0.01 * width(), num(2) * 0.01 * height());
            }
            if (m_renderer && (byPixel || projectToScreen(glm::vec3(num(1), num(2), num(3)), at))) {
                QMouseEvent down(QEvent::MouseButtonPress, at, mapToGlobal(at), Qt::LeftButton, Qt::LeftButton,
                                 Qt::NoModifier);
                mousePressEvent(&down);
                QMouseEvent up(QEvent::MouseButtonRelease, at, mapToGlobal(at), Qt::LeftButton, Qt::NoButton,
                               Qt::NoModifier);
                mouseReleaseEvent(&up);
                say("[ikscript]   click at px(%.0f %.0f): model %d is selected\n", at.x(), at.y(),
                    scene().selectedModelIndex());
            } else {
                say("[ikscript]   clickat: the point is not in view\n");
            }
            requestUpdate();
        } else if (cmd == QLatin1String("click") && w.size() > 1) {
            // click <bone>[@share] [dxPx dyPx]: the pick at the bone's pixel — moved by that many
            // pixels where given (+y down): the tip of a toe lies beyond its last joint's pixel.
            QPointF px;
            if (scriptPixelOf(w[1], px)) {
                px += QPointF(num(2), num(3));
                const int picked = boneAt(px);
                const Armature* arm = scene().figureArmature();
                say("[ikscript]   click at px(%.0f %.0f): picked %s\n", px.x(), px.y(),
                    picked >= 0 && arm ? arm->boneName(static_cast<std::size_t>(picked)).c_str() : "nothing");
                requestUpdate();
            }
        } else if ((cmd == QLatin1String("press") || cmd == QLatin1String("cpress") || cmd == QLatin1String("spress")) &&
                   w.size() > 1) {
            // The real press: the picker at the pixel, then the press path (IK drag + undo snapshot).
            // cpress is the Ctrl press (IkScope::Part): the SCOPED drag of the grabbed chain alone for
            // a limb or the head, the FIGURE MOVE for a grab of the body itself.
            const bool scoped = cmd == QLatin1String("cpress");
            // spress is the SHIFT press (THE GROUP): the pressed joint joins the selection as its primary
            // and the whole group is dragged through it.
            const bool shift = cmd == QLatin1String("spress");
            // (scriptPixelOf selects the named bone to find its pixel, which a plain press relies on;
            // the Shift press must find the selection AS IT WAS — a member pressed again is the
            // primary, a selected joint not pressed becomes a secondary — so it is put back.)
            const Armature*        before = scene().figureArmature();
            const int              prevPrimary = before != nullptr ? before->selectedBone() : -1;
            const std::vector<int> prevGroup = scene().groupBones();
            QPointF px;
            if (!scriptPixelOf(w[1], px)) {
                continue;
            }
            if (shift) {
                scene().restoreSelection(prevPrimary, prevGroup);
            }
            closeOpenPoseEdits();
            bool began = (shift ? beginGroupGesture(px) : beginJointGesture(px, scoped)) && m_ik.dragging;
            // The pick is honest: what it finds is what is under that pixel. On another body that is
            // not always the bone the script names — a broad character's hanging arm covers her chest
            // from the side, one hand covers the other — and the gesture that follows is then some
            // other gesture (a whole gallery of "chest" drags once hauled an upper arm about). When
            // the pick lands on ANOTHER PART of the body — the rig would drag a different joint than
            // the one named — the bone is grabbed by name instead, at the same point, and the log says
            // so. (A small bone OF the named one, a pectoral for the chest, is what a user's click
            // finds too, and stands: the rig drags the named joint.)
            {
                const Armature*   found = scene().figureArmature();
                const std::string name = figureBoneName(found, w[1].section(QLatin1Char('@'), 0, 0).toStdString());
                const QString     share = w[1].section(QLatin1Char('@'), 1, 1);
                const int         wanted = found ? found->knownPosingBone(found->boneIndex(name)) : -1; // (a face-rig bone's name: the head)
                // (A DIGIT drag has no rig effector: what it drags is the selected digit's joint.)
                const int         dragged = found && found->ikDigitDrag() ? found->selectedBone()
                                            : found && found->ikRig() ? found->ikRig()->dragEffector() : -1;
                if (!shift && began && found && wanted >= 0 && !found->sameRigidSegment(found->selectedBone(), wanted) &&
                    found->ikRig() && dragged != wanted) {
                    const std::string other = found->boneName(static_cast<std::size_t>(found->selectedBone()));
                    abortIkDrag();
                    scene().selectBoneByName(name);
                    if (!share.isEmpty()) {
                        scene().setIkGrabOnSegment(share.toFloat());
                    }
                    began = beginIkDrag(scoped);
                    if (began) {
                        m_preEditPose = scene().capturePose();
                    }
                    say("[ikscript]   the pick found %s there, not %s: grabbed by name instead\n", other.c_str(), name.c_str());
                }
            }
            if (!began) {
                say("[ikscript]   press at px(%.0f %.0f) began no drag\n", px.x(), px.y());
                continue;
            }
            const Armature* arm = scene().figureArmature();
            s.dragging = true;
            s.cursorPx = px;
            s.cursorWorld = m_ik.planePoint;
            s.jumpMax = 0.0f;
            s.jumpBone = -1;
            s.jumpTick = s.dragTicks = 0;
            s.pressPositions = s.lastPositions = jointPositions(arm);
            say("[ikscript]   pressed %s at px(%.0f %.0f), grab point (%.3f %.3f %.3f)\n",
                arm && arm->selectedBone() >= 0 ? arm->boneName(static_cast<std::size_t>(arm->selectedBone())).c_str() : "?",
                px.x(), px.y(), m_ik.planePoint.x, m_ik.planePoint.y, m_ik.planePoint.z);
            requestUpdate();
        } else if ((cmd == QLatin1String("drag") || cmd == QLatin1String("dragw") || cmd == QLatin1String("dragv")) &&
                   s.dragging) {
            // drag <dxPx> <dyPx> <moveTicks> <holdTicks>: the cursor moves in SCREEN space (logical
            // pixels, +y DOWN, from where it is) — through the real drag plane, as the mouse does.
            // dragw <dx> <dy> <dz> <moveTicks> <holdTicks>: the cursor's WORLD target moves (metres)
            // — the harness's gestures, for parity.
            // dragv <right> <up> <moveTicks> <holdTicks>: a SCREEN-space drag too, given in metres
            // along the view's right and up axes at the grab's depth ("10cm to screen-right").
            s.worldMove = cmd == QLatin1String("dragw");
            if (cmd == QLatin1String("dragv")) {
                const glm::mat4 view = m_renderer->camera().view();
                const glm::vec3 right(view[0][0], view[1][0], view[2][0]);
                const glm::vec3 up(view[0][1], view[1][1], view[2][1]);
                QPointF         to = s.cursorPx;
                projectToScreen(s.cursorWorld + right * num(1) + up * num(2), to);
                s.fromPx = s.cursorPx;
                s.toPx = to;
                s.moveTicks = std::max(1, static_cast<int>(num(3)));
                s.holdTicks = std::max(0, static_cast<int>(num(4)));
            } else if (s.worldMove) {
                s.fromWorld = s.cursorWorld;
                s.toWorld = s.cursorWorld + glm::vec3(num(1), num(2), num(3));
                s.moveTicks = std::max(1, static_cast<int>(num(4)));
                s.holdTicks = std::max(0, static_cast<int>(num(5)));
            } else {
                s.fromPx = s.cursorPx;
                s.toPx = s.cursorPx + QPointF(num(1), num(2));
                s.moveTicks = std::max(1, static_cast<int>(num(3)));
                s.holdTicks = std::max(0, static_cast<int>(num(4)));
            }
            s.tick = 0;
            s.moving = true;
            return; // the 60 Hz tick drives it (scriptDragTick), and comes back here when done
        } else if (cmd == QLatin1String("wheel") && s.dragging) {
            stepIkDepth(num(1), s.cursorPx);
        } else if (cmd == QLatin1String("release") && s.dragging) {
            scriptReport("release");
            s.dragging = false;
            if (m_ik.poseChanged) {
                beginIkRelease();
                s.awaitingSettle = true;
                requestUpdate();
                return; // onIkTick calls scriptSettled()
            }
            abortIkDrag(); // a press that never moved: a click
        } else if (cmd == QLatin1String("wait")) {
            // wait <ticks>: that many sixteenths of a second — and each tick ALSO advances a fall or
            // a landing bounce in flight (scriptWaitTick → onFallTick), so the fall keeps step with
            // the wait however long a `shot`'s GPU readback stalls the event loop: a repeating
            // 16 ms timer folds the periods it missed into ONE tick while a single-shot deadline
            // keeps wall time, and the landing gallery's knee reading at a fixed tick differed
            // from one run to the next until the two shared one clock (2026-11-09).
            s.waitTicks = std::max(1, static_cast<int>(num(1)));
            QTimer::singleShot(16, this, &VulkanWindow::scriptWaitTick);
            return;
        } else if (cmd == QLatin1String("tracedwait")) {
            // tracedwait [<max ticks>]: waits until the Rendered mode shows its FINISHED picture —
            // every sample of the scene, the lighting and the camera as they stand — or the tick
            // budget runs out (900, a quarter of a minute, by default: the first render on a
            // device loads its kernels). `expect traced.done == 1` says which.
            s.waitTicks = w.size() > 1 ? std::max(1, static_cast<int>(num(1))) : 900;
            s.waitForTraced = true;
            s.waitedTicks = 0;
            QTimer::singleShot(16, this, &VulkanWindow::scriptWaitTick);
            return;
        } else if (cmd == QLatin1String("expect") && w.size() >= 4) {
            // expect <metric> <op> <value>: the script as a TEST. (Metrics: scriptMetric.)
            double      value = 0.0;
            const bool  known = scriptMetric(w[1], value);
            // The wanted value: a number, or another METRIC's value (`expect mat.roughness ==
            // matimported.roughness`).
            bool   wantIsNumber = false;
            double want = w[3].toDouble(&wantIsNumber);
            if (!wantIsNumber && !scriptMetric(w[3], want)) {
                want = 0.0;
            }
            const QString op = w[2];
            const bool ok = known && (op == QLatin1String("<")    ? value < want
                                      : op == QLatin1String("<=") ? value <= want
                                      : op == QLatin1String(">")  ? value > want
                                      : op == QLatin1String(">=") ? value >= want
                                      : op == QLatin1String("==") ? std::abs(value - want) < 1.0e-3
                                                                  : false);
            ++s.expectations;
            s.failures += ok ? 0 : 1;
            if (known) {
                say("[ikscript]   EXPECT %s: %s = %.3f, wanted %s %s\n", ok ? "ok" : "FAILED", qPrintable(w[1]), value,
                    qPrintable(op), qPrintable(w[3]));
            } else {
                say("[ikscript]   EXPECT FAILED: unknown metric '%s'\n", qPrintable(w[1]));
            }
        } else if (cmd == QLatin1String("report")) {
            scriptReport(w.size() > 1 ? qPrintable(line.section(QLatin1Char(' '), 1)) : "report");
        } else if (cmd == QLatin1String("light") && w.size() > 2) {
            // light <dial> <value>: one of the Environment tab's dials, as the tab sets it (no undo
            // entry; the tab's own widgets are not told).
            LightingSettings lighting = m_deferred.lighting;
            const QString    dial = w[1].toLower();
            const float      value = w[2].toFloat();
            const QHash<QString, float LightingSettings::*> dials = {
                {QStringLiteral("exposure"), &LightingSettings::exposure},
                {QStringLiteral("diffuse"), &LightingSettings::diffuseIntensity},
                {QStringLiteral("specular"), &LightingSettings::specularIntensity},
                {QStringLiteral("ambient"), &LightingSettings::ambientFill},
                {QStringLiteral("key"), &LightingSettings::keyIntensity},
                {QStringLiteral("rotation"), &LightingSettings::environmentRotationDeg},
                {QStringLiteral("azimuth"), &LightingSettings::keyAzimuthDeg},
                {QStringLiteral("elevation"), &LightingSettings::keyElevationDeg},
                {QStringLiteral("subsurface"), &LightingSettings::subsurface},
                {QStringLiteral("rim"), &LightingSettings::rimIntensity},
                {QStringLiteral("backdropbrightness"), &LightingSettings::backdropBrightness},
                {QStringLiteral("backdropblur"), &LightingSettings::backdropBlur},
                {QStringLiteral("shadowintensity"), &LightingSettings::shadowIntensity},
                {QStringLiteral("shadowsoftness"), &LightingSettings::shadowSoftness},
            };
            if (const auto it = dials.constFind(dial); it != dials.constEnd()) {
                lighting.*(it.value()) = value;
            } else if (dial == QLatin1String("tonemap")) {
                lighting.tonemap = value != 0.0f;
            } else if (dial == QLatin1String("shadows")) {
                lighting.shadowsEnabled = value != 0.0f;
            } else if (dial == QLatin1String("backdrop")) {
                lighting.backdropMode = static_cast<int>(value);
            } else {
                say("[ikscript]   unknown light dial '%s'\n", qPrintable(w[1]));
            }
            setLightingSettings(lighting);
        } else if (cmd == QLatin1String("lamp") && w.size() > 1) {
            // lamp add <kind> | remove <n> | select <n> | aim <n> | click <n> | drag <n> <dx%> <dy%> |
            // <n> <field> <value>: the LIGHT OBJECTS (the Lights tab), each as its control or the
            // mouse does it — see scriptLamp.
            scriptLamp(w);
        } else if (cmd == QLatin1String("environment") && w.size() > 1) {
            // environment <file.hdr|.exr>: the Environment tab's panorama pick (a relative path
            // resolves like `texture`'s). The bake runs off-thread: `wait` for it.
            QString file = line.section(QLatin1Char(' '), 1).trimmed();
            if (QFileInfo(file).isRelative()) {
                const QString assets = qEnvironmentVariable("POSESTUDIO_IK_SCRIPT_ASSETS");
                file = QDir(assets.isEmpty() ? s.outDir : assets).filePath(file);
            }
            setEnvironmentFile(file);
        } else if (cmd == QLatin1String("savematerial") && w.size() > 1) {
            // savematerial <name.psm> [noicon] [link] [<size>]: the Materials tab's Save — the
            // selected zone's material written into the run's folder (<name> may carry folders),
            // its textures collected beside it (`link`: named where they are) and its ball icon
            // rendered as <name>.png (metrics: matfile.ok, matfile.copied / .reused / .written /
            // .linked / .embedded, maticon.ok, maticon.engine, maticon.size,
            // maticon.<r|g|b|a>.<x%>.<y%>).
            scriptSaveMaterial(w);
        } else if (cmd == QLatin1String("asset") && w.size() > 2) {
            // asset rename <file> <name> | duplicate <file> | move <file> <folder> | delete <file>
            //     | export <file or folder> <zip> | install <zip> <folder> [replace]:
            // the library browser's file operations on files of the run's folder (what its
            // context menu's entries do, without their dialogs; a delete is for good, not to
            // the trash). Every path is relative to the run's folder; a rename's <name> is a
            // name, and the file of a duplicate or a delete is the REST OF THE LINE (a duplicate's
            // name has a space in it). Read the outcome with `exists.<path>` / `count.<glob>`.
            QStringList words = w.mid(1);
            const QString op = words[0].toLower();
            if (op == QLatin1String("duplicate") || op == QLatin1String("delete")) {
                words = QStringList{op, words.mid(1).join(QLatin1Char(' '))};
            }
            const auto inRun = [&s](const QString& path) { return QDir::cleanPath(QDir(s.outDir).absoluteFilePath(path)); };
            words[1] = inRun(words[1]);
            if (words.size() > 2 && op != QLatin1String("rename")) {
                words[2] = inRun(words[2]);
            }
            if (op == QLatin1String("install") || op == QLatin1String("move")) {
                QDir().mkpath(words.value(2));
            } else if (op == QLatin1String("export")) {
                QDir().mkpath(QFileInfo(words.value(2)).absolutePath()); // (a save dialog names a folder that is there)
            }
            emit libraryOperationRequested(words);
        } else if (cmd == QLatin1String("applymaterial") && w.size() > 1) {
            // applymaterial <name.psm>: a saved material put on the selected zone, as a
            // double-click in the Materials Library does (metrics: matfile.ok, matfile.missing,
            // matfile.relocated). applymaterial <file.mtlx> [<material>]: a material of a
            // MaterialX document (the first, or the one named) the same way (matfile.materials:
            // how many the file holds; matfile.notes: what it could not carry, counted).
            scriptApplyMaterial(w);
        } else if (cmd == QLatin1String("render") && w.size() > 1) {
            // render <name> [<width> <height>] [<samples>] [<device text>|cpu] [nodenoise] [transparent]:
            // a PRODUCTION RENDER of the scene as it stands, through the render engine, run to its
            // end and saved as render/<name>.png (metrics: render.ok, render.seconds, render.mean,
            // render.covered).
            scriptRender(w);
        } else if (cmd == QLatin1String("shot") && w.size() > 1) {
            s.shotPath = QDir(s.outDir).filePath(w[1] + QStringLiteral(".png"));
            s.awaitingShot = true;
            m_renderer->requestCapture();
            requestUpdate();
            // A window that is MINIMIZED (or hidden) draws no frame, and the shot would wait for
            // good — a run started on a desktop in use, its window put away by the user, hung at
            // its next `shot` with nothing in the log. After 20 s the shot is given up as a
            // FAILURE (the exit code counts it) and the script goes on.
            const int serial = ++s.shotSerial;
            QTimer::singleShot(20000, this, [this, serial]() {
                if (m_script && m_script->awaitingShot && m_script->shotSerial == serial) {
                    m_script->awaitingShot = false;
                    ++m_script->failures;
                    say("[ikscript]   SHOT FAILED: no frame was drawn in 20 s (is the window minimized or hidden?) - %s\n",
                        qPrintable(m_script->shotPath));
                    scriptStep();
                }
            });
            return; // renderFrame calls scriptFrameRendered()
        } else if (cmd == QLatin1String("quit")) {
            break;
        } else {
            say("[ikscript]   ignored (unknown command, or no drag in flight)\n");
        }
    }
    say("[ikscript] done: %d expectation(s), %d FAILED\n", s.expectations, s.failures);
    std::_Exit(std::min(s.failures, 100)); // diagnostic mode: no teardown; the exit code is the failures
}

/// One 60 Hz tick of a scripted move (called from the IK tick before its solve): advances the
/// cursor along the move, feeds it to the drag exactly as a mouse move would, records the
/// worst single-tick joint move (a POP over 10cm is logged), and resumes the script when the
/// move's ticks and hold are up.
void VulkanWindow::scriptWaitTick() {
    if (!m_script) {
        return;
    }
    if (m_fall.timer != nullptr && m_fall.timer->isActive()) {
        onFallTick(); // (the timer's own timeouts are ignored under a script: see onFallTick)
    }
    if (m_script->waitForTraced) {
        // `tracedwait`: over as soon as the Rendered mode's picture is the finished one.
        ++m_script->waitedTicks;
        double done = 0.0;
        if (scriptTracedMetric(QStringLiteral("traced.done"), done) && done > 0.5) {
            std::fprintf(stderr, "[ikscript]   tracedwait: the picture was finished after %d ticks\n", m_script->waitedTicks);
            std::fflush(stderr);
            m_script->waitTicks = 0;
        } else if (m_script->waitTicks <= 1) {
            std::fprintf(stderr, "[ikscript]   tracedwait: NOT finished after %d ticks\n", m_script->waitedTicks);
            std::fflush(stderr);
        }
    }
    if (--m_script->waitTicks > 0) {
        QTimer::singleShot(16, this, &VulkanWindow::scriptWaitTick);
        return;
    }
    m_script->waitForTraced = false;
    scriptStep();
}

void VulkanWindow::scriptDragTick() {
    IkScript& s = *m_script;
    if (!s.moving) {
        return;
    }
    const int   t = ++s.tick;
    ++s.dragTicks;
    const float f = std::min(1.0f, static_cast<float>(t) / static_cast<float>(s.moveTicks));
    if (s.worldMove) {
        s.cursorWorld = glm::mix(s.fromWorld, s.toWorld, f);
        setIkTarget(s.cursorWorld);
        projectToScreen(s.cursorWorld, s.cursorPx);
    } else {
        s.cursorPx = s.fromPx + (s.toPx - s.fromPx) * static_cast<qreal>(f);
        glm::vec3 target;
        if (dragPlaneHit(s.cursorPx, target)) { // exactly mouseMoveEvent's IK branch
            setIkTarget(target);
            s.cursorWorld = target;
        }
    }
    // What the last tick's solve did (this runs at the top of the tick, before the next solve).
    const Armature* arm = scene().figureArmature();
    glm::vec3       grab(0.0f);
    if (arm != nullptr && t > 1 && scene().ikGrabPointWorld(grab)) {
        const std::vector<glm::vec3> now = jointPositions(arm);
        float       tickWorst = 0.0f;
        std::size_t tickBone = 0;
        for (std::size_t i = 0; i < now.size() && i < s.lastPositions.size(); ++i) {
            const float step = glm::length(now[i] - s.lastPositions[i]);
            if (step > tickWorst) {
                tickWorst = step;
                tickBone = i;
            }
            if (step > s.jumpMax) {
                s.jumpMax = step;
                s.jumpBone = static_cast<int>(i);
                s.jumpTick = s.dragTicks;
            }
        }
        if (tickWorst > 0.10f) { // a POP: more than 10cm in one tick is never a drag's pace
            say("[ikscript]   POP at tick %d: %s moved %.0f mm in one tick\n", s.dragTicks, arm->boneName(tickBone).c_str(),
                tickWorst * 1000.0f);
        }
        // The per-tick trace (POSESTUDIO_IK_SCRIPT_TICKS, POSESTUDIO_IK_SCRIPT_EULERS): the worst joint's
        // move every tick, and named bones' Euler channels — the app's twin of the harness's
        // IK_HARNESS_POSE_TRACE / IK_HARNESS_EULER_TICKS, for a pop only a scripted click makes.
        static const bool  kTickTrace = std::getenv("POSESTUDIO_IK_SCRIPT_TICKS") != nullptr;
        static const char* kEulerBones = std::getenv("POSESTUDIO_IK_SCRIPT_EULERS");
        if (kTickTrace) {
            say("[ikscript]   tick %d: worst %s %.1f mm | grab (%.3f %.3f %.3f) cursor (%.3f %.3f %.3f)\n", s.dragTicks,
                arm->boneName(tickBone).c_str(), tickWorst * 1000.0f, grab.x, grab.y, grab.z, s.cursorWorld.x, s.cursorWorld.y,
                s.cursorWorld.z);
        }
        if (kEulerBones != nullptr) {
            const QStringList names = QString::fromUtf8(kEulerBones).split(QLatin1Char(','), Qt::SkipEmptyParts);
            QString           row;
            for (const QString& nm : names) {
                const int b = arm->boneIndex(figureBoneName(arm, nm.trimmed().toStdString()));
                if (b >= 0) {
                    const glm::vec3 e = arm->boneEuler(static_cast<std::size_t>(b));
                    row += QStringLiteral(" %1(%2 %3 %4)").arg(nm.trimmed()).arg(e.x, 0, 'f', 1).arg(e.y, 0, 'f', 1).arg(e.z, 0, 'f', 1);
                }
            }
            say("[ikscript]   tick %d eulers:%s\n", s.dragTicks, row.toUtf8().constData());
        }
        // (... and POSESTUDIO_IK_SCRIPT_POS: named bones' world positions every tick, mm — which joint a
        // pop is in when no channel of its chain moved.)
        static const char* kPosBones = std::getenv("POSESTUDIO_IK_SCRIPT_POS");
        if (kPosBones != nullptr) {
            const QStringList names = QString::fromUtf8(kPosBones).split(QLatin1Char(','), Qt::SkipEmptyParts);
            QString           row;
            for (const QString& nm : names) {
                const int b = arm->boneIndex(figureBoneName(arm, nm.trimmed().toStdString()));
                if (b >= 0 && static_cast<std::size_t>(b) < now.size()) {
                    const glm::vec3 w = now[static_cast<std::size_t>(b)] * 1000.0f;
                    row += QStringLiteral(" %1(%2 %3 %4)").arg(nm.trimmed()).arg(w.x, 0, 'f', 0).arg(w.y, 0, 'f', 0).arg(w.z, 0, 'f', 0);
                }
            }
            say("[ikscript]   tick %d positions (mm):%s\n", s.dragTicks, row.toUtf8().constData());
        }
        s.lastPositions = now;
    }
    if (t >= s.moveTicks + s.holdTicks) {
        s.moving = false;
        QTimer::singleShot(0, this, &VulkanWindow::scriptStep); // after this tick's solve
    }
}

/// The release settle finished (finishIkSettle): a `release` line's wait is over.
void VulkanWindow::scriptSettled() {
    IkScript& s = *m_script;
    if (s.awaitingSettle) {
        s.awaitingSettle = false;
        QTimer::singleShot(0, this, &VulkanWindow::scriptStep);
    }
}

/// A frame was presented after a `shot` line: takes the GPU capture, saves it as a PNG
/// (re-requesting one when a swapchain rebuild skipped it) and resumes the script.
void VulkanWindow::scriptFrameRendered() {
    IkScript& s = *m_script;
    if (!s.awaitingShot) {
        return;
    }
    VulkanRenderer::CapturedFrame frame;
    if (!m_renderer->takeCapture(frame)) {
        m_renderer->requestCapture(); // (the frame was skipped by a swapchain rebuild: the next one)
        requestUpdate();
        return;
    }
    s.awaitingShot = false;
    QImage image(frame.pixels.data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
                 static_cast<int>(frame.width) * 4,
                 frame.bgra ? QImage::Format_ARGB32 : QImage::Format_RGBA8888);
    image = image.convertToFormat(QImage::Format_RGB888); // (drops the alpha the composite leaves)
    s.shotImage = image; // (its own pixels: the conversion copied them — `shot.darkest`, `shot.below.<n>`)
    const bool saved = image.save(s.shotPath);
    say("[ikscript]   shot %s (%ux%u)%s\n", qPrintable(s.shotPath), frame.width, frame.height,
        saved ? "" : " — NOT SAVED");
    QTimer::singleShot(0, this, &VulkanWindow::scriptStep);
}

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
    IkScript&       s = *m_script;
    // (The two document metrics need no figure: an empty scene is what they are asked about.)
    if (name.compare(QLatin1String("figures"), Qt::CaseInsensitive) == 0) {
        value = m_renderer ? static_cast<double>(scene().modelCount()) : 0.0;
        return true;
    }
    if (name.compare(QLatin1String("dirty"), Qt::CaseInsensitive) == 0) {
        value = isProjectDirty() ? 1.0 : 0.0;
        return true;
    }
    // THE RENDERED MODE: traced.on / .shown / .done / .sample / .samples / .updates / .scenes / .held.
    if (name.startsWith(QLatin1String("traced."), Qt::CaseInsensitive)) {
        return scriptTracedMetric(name, value);
    }
    // THE LIGHT OBJECTS: lamps, lampselected, lamp.<n>.<field> (scriptLampMetric).
    if (name.startsWith(QLatin1String("lamp"), Qt::CaseInsensitive)) {
        return scriptLampMetric(name, value);
    }
    // THE LAST `render`: render.ok (1 = it finished), render.seconds, render.mean (the picture's
    // mean display value, 0-255), render.covered (percent of pixels that are not the backdrop).
    // THE LAST SHOT's picture, read as numbers: `shot.darkest` — its darkest pixel's brightness
    // (the three channels' mean, 0-255) — and `shot.below.<n>` — how many of its pixels are
    // darker than n. For a view framed so that everything in it is bright, they catch a dark
    // line that should not be there (a silhouette blended with the backdrop instead of with
    // the Gaussian splats behind it: tools/ikscripts/splats/edge.txt). -1 before any shot.
    if (name.startsWith(QLatin1String("shot."), Qt::CaseInsensitive)) {
        const QString what = name.mid(5).toLower();
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
    if (name.compare(QLatin1String("selection"), Qt::CaseInsensitive) == 0) {
        value = m_renderer ? static_cast<double>(scene().selectedModelIndex()) : -1.0; // -1 = nothing selected
        return true;
    }
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
    if (name.startsWith(QLatin1String("matstore."), Qt::CaseInsensitive)) {
        // matstore.<slot>: 1 when the slot's file lies in a TEXTURE STORE of this run's folder (a
        // `textures` folder beside a material `savematerial` wrote: assets/assetstore.h) — what
        // an applied material's map must be, not the file the material was saved from.
        const MaterialView view = materialView();
        MaterialMap        map = MaterialMap::BaseColor;
        if (!view.valid || !materialMapByName(name.mid(9).toLower().toStdString(), map)) {
            return false;
        }
        const QString path = QDir::cleanPath(QString::fromStdString(view.mapPaths[static_cast<std::size_t>(map)]));
        const QString run = QDir::cleanPath(s.outDir);
        value = !path.isEmpty() && path.startsWith(run, Qt::CaseInsensitive) && path.contains(QLatin1String("/textures/"), Qt::CaseInsensitive) ? 1.0 : 0.0;
        return true;
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
    const int         bone = part.size() > 1 ? arm->boneIndex(figureBoneName(arm, part[1].toStdString())) : -1;
    const int         head = arm->boneIndex(figureBoneName(arm, "head")); // (by role on a rig in another convention)
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
        value = m_modalKind; // the dial the mouse turns (0/1/2), -1 with the mode off
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
        value = moved(arm->boneIndex(figureBoneName(arm, "hip"))); // (by the alias layer: any rig's hips)
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
        const int hip = arm->boneIndex(figureBoneName(arm, "hip"));
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
        int   child = -1;
        float longest = 0.0f;
        for (std::size_t i = 0; i < arm->boneCount(); ++i) {
            if (arm->boneParent(i) != bone) {
                continue;
            }
            const float len = glm::length(arm->boneWorldPosition(i) - arm->boneWorldPosition(static_cast<std::size_t>(bone)));
            if (len > longest) {
                longest = len;
                child = static_cast<int>(i);
            }
        }
        if (child < 0 || longest < 1.0e-4f) {
            return false;
        }
        const glm::vec3 dir = (arm->boneWorldPosition(static_cast<std::size_t>(child)) - arm->boneWorldPosition(static_cast<std::size_t>(bone))) / longest;
        value = glm::degrees(std::acos(glm::clamp(-dir.y, -1.0f, 1.0f)));
    } else if (key == QLatin1String("swing") && bone >= 0) {
        // The bone's fore-aft angle from straight down, degrees, + forward of the figure (the hips'
        // heading): an upper arm swung forward reads +20, one hanging back -20.
        int   child = -1;
        float longest = 0.0f;
        for (std::size_t i = 0; i < arm->boneCount(); ++i) {
            if (arm->boneParent(i) != bone) {
                continue;
            }
            const float len = glm::length(arm->boneWorldPosition(i) - arm->boneWorldPosition(static_cast<std::size_t>(bone)));
            if (len > longest) {
                longest = len;
                child = static_cast<int>(i);
            }
        }
        const int hip = arm->boneIndex(figureBoneName(arm, "hip"));
        if (child < 0 || longest < 1.0e-4f || hip < 0) {
            return false;
        }
        const glm::vec3 dir = (arm->boneWorldPosition(static_cast<std::size_t>(child)) - arm->boneWorldPosition(static_cast<std::size_t>(bone))) / longest;
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
    const auto index = [&](const char* name) { return arm->boneIndex(name); };
    const auto moved = [&](int bone) { return static_cast<float>(movedSincePress(*arm, s, bone)); };
    glm::vec3 grab(0.0f);
    const bool hasGrab = s.dragging && scene().ikGrabPointWorld(grab);
    const int  hip = index("hip");
    const int  head = index("head");
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
            if (name.find("Toe") != std::string::npos || name.find("Carpal") != std::string::npos ||
                name.find("Index") != std::string::npos || name.find("Mid") != std::string::npos ||
                name.find("Ring") != std::string::npos || name.find("Pinky") != std::string::npos ||
                name.find("Thumb") != std::string::npos) {
                continue; // (digits: noise here)
            }
            posed += QStringLiteral(" %1(%2 %3 %4)").arg(QString::fromStdString(name)).arg(e.x, 0, 'f', 0).arg(e.y, 0, 'f', 0).arg(e.z, 0, 'f', 0);
            ++listed;
        }
    }
    say("[ikscript]   [%s] posed:%s\n", label, qPrintable(posed));
}

} // namespace pose
