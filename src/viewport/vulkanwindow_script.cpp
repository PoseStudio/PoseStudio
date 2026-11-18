/**
 * @file vulkanwindow_script.cpp
 * @brief The scripted in-app posing test (POSESTUDIO_IK_SCRIPT=<file>): IkScript's loader and the
 *        STEP DRIVER of the runner on VulkanWindow.
 *
 * One of the translation units of VulkanWindow — see vulkanwindow.h and ikscript.h. Where the
 * IK harness (tools/ikharness) drives the Armature alone, this drives the APP: a `press` is the
 * real picker at a pixel followed by the real press path, a `drag` moves a scripted cursor in
 * SCREEN space through the real camera-parallel drag plane, the 60 Hz timer solves, the release
 * settles, and `shot` saves the frame the renderer produced. Nothing is injected into the OS —
 * the window's own gesture functions are called — so it runs on a desktop that is in use, and
 * the frames are read back from the GPU, so the window need not even be visible.
 *
 * The runner is three files. THIS one runs the script: it reads the lines, logs each, hands it to
 * the command families, and owns everything that WAITS for the app — a drag's ticks, a release's
 * settle, a shot's frame, a `wait` — together with `expect` and the verdict. What a command DOES
 * is in vulkanwindow_script_commands.cpp, what a metric READS in vulkanwindow_script_metrics.cpp;
 * the commands of a feature that has a translation unit of its own live there (scriptRender,
 * scriptSaveMaterial / scriptApplyMaterial, scriptLamp).
 *
 * THE LOG IS A BASELINE: every `[ikscript]` line is compared byte for byte between runs. Change a
 * line's text or a number's format only on purpose.
 *
 * tools/ikscripts/README.md is the command reference.
 */

#include "vulkanwindow.h"

#include "ikscript.h"
#include "rendering/vulkanrenderer.h"
#include "scene/armature.h"
#include "scene/scene.h"
#include "vulkanwindow_script_detail.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pose {

using namespace scriptdetail;

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

QString IkScript::assetPath(const QString& name) const {
    if (!QFileInfo(name).isRelative()) {
        return name;
    }
    const QString assets = qEnvironmentVariable("POSESTUDIO_IK_SCRIPT_ASSETS");
    return QDir(assets.isEmpty() ? outDir : assets).filePath(name);
}

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
    // The command families (vulkanwindow_script_commands.cpp), asked in turn for a line that is
    // none of the driver's own.
    using Family = ScriptFlow (VulkanWindow::*)(const QString&, const QStringList&, const QString&);
    static constexpr Family kFamilies[] = {
        &VulkanWindow::scriptCameraCommand,   &VulkanWindow::scriptPoseCommand,     &VulkanWindow::scriptGestureCommand,
        &VulkanWindow::scriptMaterialCommand, &VulkanWindow::scriptDocumentCommand, &VulkanWindow::scriptUiCommand,
        &VulkanWindow::scriptPictureCommand,  &VulkanWindow::scriptLightingCommand,
    };
    while (s.next < s.lines.size()) {
        const QString     line = s.lines[s.next++];
        const QStringList w = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QString     cmd = w[0].toLower();
        const auto        num = [&](int i) { return i < w.size() ? w[i].toFloat() : 0.0f; };
        say("[ikscript] > %s\n", qPrintable(line));

        if (cmd == QLatin1String("wait")) {
            // wait <ticks>: that many 16 ms ticks (sixtieths of a second) — and each ALSO advances a fall or
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
            // budget runs out (2400, forty seconds, by default: the first render on a device
            // loads its kernels, and a slower card needs a thousand ticks for a picture a fast
            // one finishes in two hundred — 900, the first default, ran out on one by a few
            // ticks). `expect traced.done == 1` says which.
            s.waitTicks = w.size() > 1 ? std::max(1, static_cast<int>(num(1))) : 2400;
            s.waitForTraced = true;
            s.waitedTicks = 0;
            QTimer::singleShot(16, this, &VulkanWindow::scriptWaitTick);
            return;
        } else if (cmd == QLatin1String("expect") && w.size() >= 4) {
            // expect <metric> <op> <value>: the script as a TEST. (Metrics: scriptMetric.)
            double      value = 0.0;
            const bool  known = scriptMetric(w[1], value);
            // The wanted value: a number, or another METRIC's value (`expect mat.roughness ==
            // matimported.roughness`). A name nothing knows FAILS the expectation, on either side:
            // compared against a silent 0, a mistyped metric there made a gate that could not fire.
            bool       wantIsNumber = false;
            double     want = w[3].toDouble(&wantIsNumber);
            const bool wantKnown = wantIsNumber || scriptMetric(w[3], want);
            const QString op = w[2];
            const bool ok = known && wantKnown && (op == QLatin1String("<")    ? value < want
                                                   : op == QLatin1String("<=") ? value <= want
                                                   : op == QLatin1String(">")  ? value > want
                                                   : op == QLatin1String(">=") ? value >= want
                                                   : op == QLatin1String("==") ? std::abs(value - want) < 1.0e-3
                                                                               : false);
            ++s.expectations;
            s.failures += ok ? 0 : 1;
            if (known && wantKnown) {
                say("[ikscript]   EXPECT %s: %s = %.3f, wanted %s %s\n", ok ? "ok" : "FAILED", qPrintable(w[1]), value,
                    qPrintable(op), qPrintable(w[3]));
            } else {
                say("[ikscript]   EXPECT FAILED: unknown metric '%s'\n", qPrintable(known ? w[3] : w[1]));
            }
        } else if (cmd == QLatin1String("report")) {
            scriptReport(w.size() > 1 ? qPrintable(line.section(QLatin1Char(' '), 1)) : "report");
        } else if (cmd == QLatin1String("quit")) {
            break;
        } else {
            ScriptFlow flow = ScriptFlow::NotMine;
            for (const Family family : kFamilies) {
                flow = (this->*family)(cmd, w, line);
                if (flow != ScriptFlow::NotMine) {
                    break;
                }
            }
            if (flow == ScriptFlow::Wait) {
                return; // the event that ends the wait calls scriptStep again
            }
            if (flow == ScriptFlow::NotMine) {
                say("[ikscript]   ignored (unknown command, or no drag in flight)\n");
            }
        }
    }
    say("[ikscript] done: %d expectation(s), %d FAILED\n", s.expectations, s.failures);
    std::_Exit(std::min(s.failures, 100)); // diagnostic mode: no teardown; the exit code is the failures
}

/// One tick of a `wait` or a `tracedwait`: advances a fall or a landing bounce in flight (under a
/// script the fall keeps the wait's ticks, not the wall clock), ends a `tracedwait` whose picture is
/// finished, and either schedules the next tick or resumes the script.
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

/// One 60 Hz tick of a scripted move (called from the IK tick before its solve): advances the
/// cursor along the move, feeds it to the drag exactly as a mouse move would, records the
/// worst single-tick joint move (a POP over 10cm is logged), and resumes the script when the
/// move's ticks and hold are up.
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
                const int b = scriptBone(arm, nm.trimmed().toStdString());
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
                const int b = scriptBone(arm, nm.trimmed().toStdString());
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

} // namespace pose
