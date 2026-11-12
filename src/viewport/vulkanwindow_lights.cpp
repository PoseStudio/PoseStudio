/**
 * @file vulkanwindow_lights.cpp
 * @brief VulkanWindow's side of the LIGHT OBJECTS (scene/scenelight.h): the list as an undoable
 *        edit, adding and removing, the selection, and the mouse's drag of a light's marker.
 *        (A TU of VulkanWindow — see vulkanwindow.h for the class and its other TUs.)
 *
 * The list lives twice, like the lighting dials: remembered in DeferredSceneState (chosen before
 * the renderer exists, kept across a device loss, reset by File → New) and in the Scene, which
 * shades with it and draws the markers. Every change goes through applyLights so the two cannot
 * part.
 *
 * AN EDIT is a bracket, as a material's is: beginLightEdit snapshots the list, any number of
 * setLight calls follow (a scrub of a dial, a drag of a marker), endLightEdit pushes ONE undo
 * entry if the list ended other than it began. Adding and removing are their own brackets.
 *
 * THE MOUSE: a left press on a light's marker selects the light and begins a drag of it in the
 * camera-parallel plane through where it stands — the posing drag's own geometry, wheel for
 * depth included — and a press released without a move is the selection alone.
 */

#include "vulkanwindow.h"

#include "camera.h"
#include "scene.h"
#include "scenelight.h"
#include "vulkanrenderer.h"

#include <QPointF>
#include <QString>
#include <QStringList>

#include <cstdio>

#include <algorithm>
#include <cmath>

namespace pose {

std::vector<SceneLight> VulkanWindow::lights() const {
    return m_deferred.lights;
}

int VulkanWindow::selectedLight() const {
    return m_renderer ? scene().selectedLight() : -1;
}

void VulkanWindow::applyLights(std::vector<SceneLight> lights) {
    m_deferred.lights = lights;
    if (m_renderer) {
        scene().setLights(std::move(lights));
        requestUpdate();
    }
}

void VulkanWindow::selectLight(int index) {
    if (!m_renderer || dragInFlight()) {
        return;
    }
    if (index != scene().selectedLight()) {
        closeSettlingEdits(); // (a settle, an open dial or material edit: landed first, as a press does)
        scene().setSelectedLight(index);
        requestUpdate();
    }
}

glm::vec3 VulkanWindow::lightAimPoint() const {
    // What a new light is aimed at, and "Aim at Subject" too: the middle of the active figure —
    // of everything, with none — a little above its centre (a face, not a navel), or a standing
    // person's chest in an empty scene.
    if (m_renderer) {
        glm::vec3 lo;
        glm::vec3 hi;
        if (scene().subjectBounds(lo, hi)) {
            return glm::vec3(0.5f * (lo.x + hi.x), lo.y + 0.72f * (hi.y - lo.y), 0.5f * (lo.z + hi.z));
        }
    }
    return glm::vec3(0.0f, 1.2f, 0.0f);
}

int VulkanWindow::addLight(LightKind kind) {
    closeSettlingEdits();
    std::vector<SceneLight> list = m_deferred.lights;
    SceneLight              light;
    light.kind = kind;
    light.name = nextLightName(list, kind);
    light.intensity = kind == LightKind::Sun ? 1.0f : 4.0f;

    // Where a new light stands: two metres from the subject and above it, on the VIEWER's side
    // — the first a key light's place, 40 degrees to the left of where the user looks from, so
    // what it lights is what the user is looking at — each further one a step round from the
    // last, so several added in a row are a ring of lights and not a pile; and aimed at the
    // subject.
    const glm::vec3 target = lightAimPoint();
    float           toViewer = 0.0f; // the azimuth of the camera as seen from the subject (0 = +Z)
    if (m_renderer) {
        const glm::vec3 eye = m_renderer->camera().position() - target;
        if (std::abs(eye.x) > 1.0e-4f || std::abs(eye.z) > 1.0e-4f) {
            toViewer = std::atan2(eye.x, eye.z);
        }
    }
    const float azimuth = toViewer + glm::radians(-40.0f + 80.0f * static_cast<float>(list.size()));
    const float distance = 2.0f;
    light.position = target + glm::vec3(distance * std::sin(azimuth), 0.55f, distance * std::cos(azimuth));
    aimLightAt(light, target);

    const std::vector<SceneLight> before = list;
    list.push_back(light);
    applyLights(list);
    pushLightUndo(before);
    const int index = static_cast<int>(list.size()) - 1;
    if (m_renderer) {
        scene().setSelectedLight(index);
    }
    return index;
}

void VulkanWindow::removeLight(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_deferred.lights.size() || dragInFlight()) {
        return;
    }
    closeSettlingEdits();
    const std::vector<SceneLight> before = m_deferred.lights;
    std::vector<SceneLight>       list = before;
    list.erase(list.begin() + index);
    if (m_renderer) {
        scene().setSelectedLight(-1);
    }
    applyLights(list);
    pushLightUndo(before);
}

bool VulkanWindow::beginLightEdit() {
    if (dragInFlight()) {
        return false;
    }
    if (m_lightEdit) {
        return true;
    }
    closeSettlingEdits();
    m_lightEdit = true;
    m_preEditLights = m_deferred.lights;
    return true;
}

void VulkanWindow::setLight(int index, const SceneLight& light) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_deferred.lights.size()) {
        return;
    }
    if (!m_lightEdit && !beginLightEdit()) {
        return;
    }
    if (m_deferred.lights[static_cast<std::size_t>(index)] == light) {
        return;
    }
    std::vector<SceneLight> list = m_deferred.lights;
    list[static_cast<std::size_t>(index)] = light;
    applyLights(std::move(list));
}

void VulkanWindow::endLightEdit() {
    if (!m_lightEdit) {
        return;
    }
    m_lightEdit = false;
    if (m_preEditLights != m_deferred.lights) {
        pushLightUndo(m_preEditLights);
    }
    m_preEditLights.clear();
}

void VulkanWindow::aimLightAtSubject(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_deferred.lights.size()) {
        return;
    }
    SceneLight light = m_deferred.lights[static_cast<std::size_t>(index)];
    aimLightAt(light, lightAimPoint());
    const bool opened = !m_lightEdit;
    setLight(index, light);
    if (opened) {
        endLightEdit();
    }
}

void VulkanWindow::pushLightUndo(const std::vector<SceneLight>& before) {
    UndoEntry entry;
    entry.kind = UndoEntry::Kind::Lights;
    entry.lights = before;
    m_undoStack.push_back(std::move(entry));
    m_redoStack.clear();
    markProjectDirty(); // the lights are part of the document
}

void VulkanWindow::notifyLights() {
    const std::uint64_t revision = m_renderer ? scene().lightsRevision() : 0;
    if (revision != m_announcedLightsRevision) {
        m_announcedLightsRevision = revision;
        emit lightsChanged();
    }
}

// ---------------------------------------------------------------------------------------------
// The mouse: a light's marker pressed, dragged, let go
// ---------------------------------------------------------------------------------------------

int VulkanWindow::lightAt(const QPointF& localPos) const {
    if (!m_renderer) {
        return -1;
    }
    return scene().pickLight(m_renderer->camera(), static_cast<float>(localPos.x()), static_cast<float>(localPos.y()),
                             static_cast<float>(width()), static_cast<float>(height()));
}

bool VulkanWindow::beginLightGesture(const QPointF& localPos) {
    const int index = lightAt(localPos);
    if (index < 0) {
        return false;
    }
    scene().setSelectedLight(index);
    m_lightDrag = LightDrag();
    m_lightDrag.active = true;
    m_lightDrag.index = index;
    m_lightDrag.planePoint = m_deferred.lights[static_cast<std::size_t>(index)].position;
    // (The marker is picked within a finger's width of its point: the light keeps its offset
    // from the cursor, so a press beside the point does not make it jump.)
    glm::vec3 hit;
    m_lightDrag.grabOffset = lightPlaneHit(localPos, hit) ? m_lightDrag.planePoint - hit : glm::vec3(0.0f);
    // A LIGHT AIMED AT THE SUBJECT STAYS AIMED AT IT: carried round the figure it goes on
    // lighting her — a lamp is moved to light its subject from somewhere else — where a spotlight
    // that kept its angles shone past her after a hand's width of travel. One the user has turned
    // elsewhere (more than kLightTracksDeg off the aim point at the press) keeps its angles: it
    // is lighting something else on purpose.
    const SceneLight& pressed = m_deferred.lights[static_cast<std::size_t>(index)];
    const glm::vec3   toSubject = lightAimPoint() - pressed.position;
    const float       away = glm::length(toSubject);
    constexpr float   kLightTracksDeg = 3.0f;
    m_lightDrag.tracks = pressed.kind != LightKind::Point && away > 1.0e-4f &&
                         glm::dot(lightDirection(pressed), toSubject / away) > std::cos(glm::radians(kLightTracksDeg));
    return true;
}

bool VulkanWindow::lightPlaneHit(const QPointF& localPos, glm::vec3& outWorld) const {
    const glm::mat4 view = m_renderer->camera().view();
    const glm::vec3 planeNormal(view[0][2], view[1][2], view[2][2]); // toward the camera
    const Ray       ray = cursorRay(localPos);
    const float     denom = glm::dot(ray.direction, planeNormal);
    if (std::abs(denom) <= 1e-4f) {
        return false;
    }
    const float t = glm::dot(m_lightDrag.planePoint - ray.origin, planeNormal) / denom;
    if (t <= 0.0f) {
        return false;
    }
    outWorld = ray.origin + ray.direction * t;
    return true;
}

void VulkanWindow::lightDragMove(const QPointF& localPos) {
    if (!m_lightDrag.active || !m_renderer) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(m_lightDrag.index);
    glm::vec3         hit;
    if (index >= m_deferred.lights.size() || !lightPlaneHit(localPos, hit)) {
        return;
    }
    SceneLight light = m_deferred.lights[index];
    light.position = hit + m_lightDrag.grabOffset;
    light.position.y = std::max(light.position.y, 0.0f); // (not under the floor)
    if (m_lightDrag.tracks) {
        aimLightAt(light, lightAimPoint());
    }
    if (!m_lightDrag.moved) {
        m_lightDrag.moved = beginLightEdit();
        if (!m_lightDrag.moved) {
            return;
        }
    }
    setLight(m_lightDrag.index, light);
}

void VulkanWindow::lightDragDepth(float notches, const QPointF& cursorPos) {
    if (!m_lightDrag.active || !m_renderer) {
        return;
    }
    // The posing drag's depth control (stepIkDepth), for the plane the light is dragged in.
    Camera&         camera = m_renderer->camera();
    const glm::mat4 view = camera.view();
    const glm::vec3 towardCamera(view[0][2], view[1][2], view[2][2]);
    const float     distance = glm::dot(m_lightDrag.planePoint - camera.position(), -towardCamera);
    constexpr float kMinPlaneDistance = 0.10f;
    float           push = notches * kIkDepthPerWheelNotch * std::max(distance, kMinPlaneDistance);
    push = std::max(push, kMinPlaneDistance - distance);
    m_lightDrag.planePoint -= towardCamera * push;
    lightDragMove(cursorPos);
}

void VulkanWindow::endLightGesture() {
    if (!m_lightDrag.active) {
        return;
    }
    const bool moved = m_lightDrag.moved;
    m_lightDrag = LightDrag();
    if (moved) {
        endLightEdit(); // one undo entry for the whole drag
    }
}

// ---------------------------------------------------------------------------------------------
// The scripted test's side (POSESTUDIO_IK_SCRIPT; tools/ikscripts/README.md)
// ---------------------------------------------------------------------------------------------

namespace {

/// A light's number by its script name, in the Lights tab's units (centimetres, degrees,
/// percent). False for a name that is none.
bool lampNumber(SceneLight& light, const QString& field, float*& value, float& scale) {
    scale = 1.0f;
    const QString f = field.toLower();
    if (f == QLatin1String("x")) {
        value = &light.position.x;
        scale = 100.0f;
    } else if (f == QLatin1String("y")) {
        value = &light.position.y;
        scale = 100.0f;
    } else if (f == QLatin1String("z")) {
        value = &light.position.z;
        scale = 100.0f;
    } else if (f == QLatin1String("turn")) {
        value = &light.turnDeg;
    } else if (f == QLatin1String("tilt")) {
        value = &light.tiltDeg;
    } else if (f == QLatin1String("intensity")) {
        value = &light.intensity;
    } else if (f == QLatin1String("radius")) {
        value = &light.radius;
        scale = 100.0f;
    } else if (f == QLatin1String("cone")) {
        value = &light.coneDeg;
    } else if (f == QLatin1String("blend")) {
        value = &light.blend;
        scale = 100.0f;
    } else if (f == QLatin1String("width")) {
        value = &light.width;
        scale = 100.0f;
    } else if (f == QLatin1String("height")) {
        value = &light.height;
        scale = 100.0f;
    } else if (f == QLatin1String("sunangle")) {
        value = &light.sunAngleDeg;
    } else if (f == QLatin1String("r")) {
        value = &light.color.x;
    } else if (f == QLatin1String("g")) {
        value = &light.color.y;
    } else if (f == QLatin1String("b")) {
        value = &light.color.z;
    } else {
        return false;
    }
    return true;
}

LightKind lampKind(const QString& word) {
    const QString k = word.toLower();
    return k == QLatin1String("spot") ? LightKind::Spot : k == QLatin1String("area") ? LightKind::Area
                                                         : k == QLatin1String("sun") ? LightKind::Sun : LightKind::Point;
}

} // namespace

void VulkanWindow::scriptLamp(const QStringList& w) {
    const auto note = [](const QString& text) {
        std::fprintf(stderr, "[ikscript]   lamp: %s\n", qPrintable(text));
        std::fflush(stderr);
    };
    const QString verb = w[1].toLower();
    if (verb == QLatin1String("add")) {
        addLight(lampKind(w.value(2)));
        return;
    }
    const auto indexOf = [&](int word) { return w.value(word).toInt(); };
    const auto valid = [&](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= m_deferred.lights.size()) {
            note(QStringLiteral("no light %1").arg(index));
            return false;
        }
        return true;
    };
    if (verb == QLatin1String("remove")) {
        if (valid(indexOf(2))) {
            removeLight(indexOf(2));
        }
    } else if (verb == QLatin1String("select")) {
        selectLight(indexOf(2));
    } else if (verb == QLatin1String("aim")) {
        if (valid(indexOf(2))) {
            aimLightAtSubject(indexOf(2));
        }
    } else if (verb == QLatin1String("click") || verb == QLatin1String("drag")) {
        // The MOUSE on a light's marker: the real pick at the marker's pixel, the real drag through
        // the camera-parallel plane (dx, dy in percent of the viewport), the real release.
        const int index = indexOf(2);
        QPointF   px;
        if (!valid(index) || !projectToScreen(m_deferred.lights[static_cast<std::size_t>(index)].position, px)) {
            note(QStringLiteral("light %1 is not on screen").arg(index));
            return;
        }
        closeSettlingEdits();
        if (!beginLightGesture(px)) {
            note(QStringLiteral("the pick found no marker at %1, %2").arg(px.x()).arg(px.y()));
            return;
        }
        if (verb == QLatin1String("drag")) {
            const QPointF to = px + QPointF(w.value(3).toDouble() * 0.01 * width(), w.value(4).toDouble() * 0.01 * height());
            for (int step = 1; step <= 8; ++step) {
                lightDragMove(px + (to - px) * (step / 8.0));
            }
        }
        endLightGesture();
        requestUpdate();
    } else {
        // lamp <n> <field> <value> | lamp <n> kind <name> | lamp <n> color <r> <g> <b> |
        // lamp <n> enabled|shadow <0|1> | lamp <n> name <text>
        bool      isIndex = false;
        const int index = w[1].toInt(&isIndex);
        if (!isIndex || !valid(index) || w.size() < 4) {
            note(QStringLiteral("unknown command"));
            return;
        }
        SceneLight    light = m_deferred.lights[static_cast<std::size_t>(index)];
        const QString field = w[2].toLower();
        float*        value = nullptr;
        float         scale = 1.0f;
        if (field == QLatin1String("kind")) {
            light.kind = lampKind(w[3]);
        } else if (field == QLatin1String("enabled")) {
            light.enabled = w[3].toInt() != 0;
        } else if (field == QLatin1String("shadow")) {
            light.castShadow = w[3].toInt() != 0;
        } else if (field == QLatin1String("name")) {
            light.name = QStringList(w.mid(3)).join(QLatin1Char(' ')).toStdString();
        } else if (field == QLatin1String("color") && w.size() >= 6) {
            light.color = glm::vec3(w[3].toFloat(), w[4].toFloat(), w[5].toFloat());
        } else if (lampNumber(light, field, value, scale)) {
            *value = w[3].toFloat() / scale;
        } else {
            note(QStringLiteral("unknown field '%1'").arg(w[2]));
            return;
        }
        if (beginLightEdit()) {
            setLight(index, light);
            endLightEdit();
        }
    }
}

bool VulkanWindow::scriptLampMetric(const QString& name, double& value) const {
    const QString n = name.toLower();
    if (n == QLatin1String("lamps")) {
        value = static_cast<double>(m_deferred.lights.size());
        return true;
    }
    if (n == QLatin1String("lampselected")) {
        value = selectedLight();
        return true;
    }
    // lamp.<n>.<field>
    const QStringList parts = n.split(QLatin1Char('.'));
    if (parts.size() != 3 || parts[0] != QLatin1String("lamp")) {
        return false;
    }
    bool      ok = false;
    const int index = parts[1].toInt(&ok);
    if (!ok || index < 0 || static_cast<std::size_t>(index) >= m_deferred.lights.size()) {
        return false;
    }
    SceneLight light = m_deferred.lights[static_cast<std::size_t>(index)];
    float*     number = nullptr;
    float      scale = 1.0f;
    if (parts[2] == QLatin1String("kind")) {
        value = static_cast<int>(light.kind);
    } else if (parts[2] == QLatin1String("enabled")) {
        value = light.enabled ? 1.0 : 0.0;
    } else if (parts[2] == QLatin1String("shadow")) {
        value = light.castShadow ? 1.0 : 0.0;
    } else if (lampNumber(light, parts[2], number, scale)) {
        value = static_cast<double>(*number) * scale;
    } else {
        return false;
    }
    return true;
}

} // namespace pose
