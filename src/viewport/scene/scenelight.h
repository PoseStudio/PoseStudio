/**
 * @file scenelight.h
 * @brief A LIGHT OBJECT of the scene — a point, spot, area or sun light the user adds, places and
 *        aims — as plain data, and the few rules every reader of one shares.
 *
 * The lighting environment (the panorama, the key light that stands for its sun: lightingsettings.h)
 * lights a subject; light objects are what a scene is LIT with beyond it — a lamp in a room, a
 * softbox beside a face, a rim light behind a shoulder. The Scene keeps the list
 * (Scene::lights); the viewport's PBR mode shades with the first kMaxViewportLights of the
 * enabled ones, without shadows of their own; a render engine — and the Rendered viewport mode
 * that is one — traces them all, shadows and bounces included.
 *
 * ONE NUMBER FOR BRIGHTNESS, whatever the kind: the INTENSITY is the irradiance the light puts
 * on a surface facing it — a Sun's wherever the surface is; a Point's, a Spot's and an Area
 * light's ONE METRE from it, on its axis — in the units the key light's Intensity dial is in.
 * Twice as far, a quarter. So a light a metre from a face at the key light's intensity lights
 * it like the key light, and an Area light's size changes how soft its shadows are, not how
 * bright the subject is — a photographer's softbox.
 *
 * WHERE IT SHINES is two angles, as the key light's are: TURN, about the vertical (0 = toward
 * -Z, the way a light in front of a figure shines at her; positive counter-clockwise seen from
 * above, as the figure's own Turn), and TILT, below the horizon (90 = straight down). lightDirection() is the one reading of them, aimLightAt() its
 * inverse, lightFrame() the frame an Area light's rectangle lies in — the same rule the render
 * engine builds its light by (engines/cycles/scenebuilder.cpp), so the rectangle drawn is the
 * rectangle that shines.
 *
 * Qt-free (std + GLM): the Qt layer shows and edits these, the render core reads them.
 */

#ifndef SCENELIGHT_H
#define SCENELIGHT_H

#include <glm/glm.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace pose {

enum class LightKind : int {
    Point = 0, ///< shines every way from a point (a bulb, a candle)
    Spot = 1,  ///< a point light held to a cone (a stage light, a torch)
    Area = 2,  ///< a rectangle that shines from one face (a softbox, a window)
    Sun = 3,   ///< a distant light: the same direction and brightness everywhere
};
inline constexpr int kLightKindCount = 4;

/// The kind's name as the interface shows it.
inline const char* lightKindName(LightKind kind) {
    switch (kind) {
    case LightKind::Spot: return "Spot Light";
    case LightKind::Area: return "Area Light";
    case LightKind::Sun: return "Sun";
    default: return "Point Light";
    }
}

/// How many light objects the viewport's PBR mode shades with (the first enabled ones; a render
/// traces them all). The camera block carries exactly this many (cameraubo.h, camera_ubo.glsl).
inline constexpr int kMaxViewportLights = 8;

struct SceneLight {
    std::string name;
    LightKind   kind = LightKind::Point;
    bool        enabled = true;
    glm::vec3   position{0.0f, 1.8f, 1.5f}; ///< metres (a Sun's is only where its marker stands)
    float       turnDeg = 0.0f;             ///< where it shines, about the vertical (see the file comment)
    float       tiltDeg = 30.0f;            ///< … and below the horizon
    glm::vec3   color{1.0f, 1.0f, 1.0f};    ///< LINEAR rgb
    float       intensity = 4.0f;           ///< see ONE NUMBER FOR BRIGHTNESS
    float       radius = 0.05f;             ///< Point / Spot: the source's radius, metres (soft shadows in a render)
    float       coneDeg = 50.0f;            ///< Spot: the cone's full angle
    float       blend = 0.2f;               ///< Spot: the share of the cone its edge fades over
    float       width = 0.8f;               ///< Area: metres across (level)
    float       height = 0.8f;              ///< Area: metres up its face
    float       sunAngleDeg = 2.0f;         ///< Sun: its angular size (soft shadows in a render)
    bool        castShadow = true;          ///< in a render (the viewport's PBR mode draws no shadows for light objects)

    bool operator==(const SceneLight& o) const {
        return name == o.name && kind == o.kind && enabled == o.enabled && position == o.position && turnDeg == o.turnDeg &&
               tiltDeg == o.tiltDeg && color == o.color && intensity == o.intensity && radius == o.radius &&
               coneDeg == o.coneDeg && blend == o.blend && width == o.width && height == o.height &&
               sunAngleDeg == o.sunAngleDeg && castShadow == o.castShadow;
    }
    bool operator!=(const SceneLight& o) const { return !(*this == o); }
};

/// The unit direction @p light SHINES in (a Point light shines every way: its angles only turn
/// its marker).
inline glm::vec3 lightDirection(const SceneLight& light) {
    const float turn = glm::radians(light.turnDeg);
    const float tilt = glm::radians(light.tiltDeg);
    return glm::vec3(-std::sin(turn) * std::cos(tilt), -std::sin(tilt), -std::cos(turn) * std::cos(tilt));
}

/// Turns @p light to shine at @p target from where it is (left alone when it stands on it).
inline void aimLightAt(SceneLight& light, const glm::vec3& target) {
    const glm::vec3 to = target - light.position;
    const float     length = glm::length(to);
    if (length < 1.0e-5f) {
        return;
    }
    const glm::vec3 d = to / length;
    light.tiltDeg = glm::degrees(std::asin(glm::clamp(-d.y, -1.0f, 1.0f)));
    if (std::abs(d.x) > 1.0e-6f || std::abs(d.z) > 1.0e-6f) {
        light.turnDeg = glm::degrees(std::atan2(-d.x, -d.z));
    }
}

/// The frame a light lies in: `back` points from where it shines back at the light (the render
/// engine's object Z), `across` is level — an Area light's width — wherever the light does not
/// shine straight up or down, and `up` completes it (an Area light's height).
struct LightFrame {
    glm::vec3 across;
    glm::vec3 up;
    glm::vec3 back;
};
inline LightFrame lightFrame(const glm::vec3& shines) {
    LightFrame frame;
    frame.back = -shines;
    frame.across = std::abs(frame.back.y) < 0.999f ? glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), frame.back))
                                                   : glm::vec3(1.0f, 0.0f, 0.0f);
    frame.up = glm::cross(frame.back, frame.across);
    return frame;
}

/// The first free name of the form "<kind> N" among @p lights ("Spot Light 2").
inline std::string nextLightName(const std::vector<SceneLight>& lights, LightKind kind) {
    const std::string prefix = std::string(lightKindName(kind)) + " ";
    for (int n = 1;; ++n) {
        const std::string candidate = prefix + std::to_string(n);
        bool              taken = false;
        for (const SceneLight& light : lights) {
            taken = taken || light.name == candidate;
        }
        if (!taken) {
            return candidate;
        }
    }
}

} // namespace pose

#endif // SCENELIGHT_H
