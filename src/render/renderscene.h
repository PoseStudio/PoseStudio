/**
 * @file renderscene.h
 * @brief A scene description for a render engine together with everything it points at.
 *
 * renderapi::Scene is a tree of borrowed pointers (renderengineapi.h): whoever starts a render
 * must keep the meshes, materials and images alive until the render has ended. A
 * RenderSceneHolder is that promise as an object — the viewport makes one (a snapshot of the
 * scene as posed: ViewportWidget::captureRenderScene), and the render session holds it for as
 * long as its engine may read from it. What it holds is the holder's own business; the render
 * side sees only the description and what the viewport says about showing the image. Qt-free.
 */

#ifndef RENDERSCENE_H
#define RENDERSCENE_H

#include "renderengineapi.h"

#include <memory>
#include <vector>

namespace pose {

/// A scene's GAUSSIAN SPLATS for one render, drawn by the viewport's own splat renderer at the
/// render's size and through its camera (splatcomposite.h says what a render does with them).
/// Tightly packed floats, top row first.
struct RenderSplatLayers {
    int                width = 0;
    int                height = 0;
    std::vector<float> all;   ///< RGBA, premultiplied, DISPLAY-referred: every splat — the plate
    std::vector<float> front; ///< RGBA likewise: only the splats in front of the scene's surfaces and of the floor
    /// The backdrop a render draws behind everything, as RGB radiance a pixel (before the
    /// exposure) — for an ENVIRONMENT backdrop; empty for the scene's flat backdropColor.
    std::vector<float> backdrop;
};

class RenderSceneHolder {
public:
    virtual ~RenderSceneHolder() = default;

    /// The description. Its pointers live as long as this object.
    virtual const renderapi::Scene& scene() const = 0;

    /// How the viewport shows this scene — what a render is given so that it comes out as the
    /// viewport's picture, path traced: the Exposure dial (a multiplier on the image), whether
    /// the filmic tonemap is on, and the viewport's size in pixels (a render's default size).
    struct View {
        float exposure = 1.0f;
        bool  tonemap = true;
        int   width = 0;
        int   height = 0;
    };
    virtual View view() const = 0;

    /// The Gaussian splat scenes in the scene (a render engine cannot draw them: see
    /// splatLayers).
    virtual int skippedSplatScenes() const = 0;

    /// The scene's splats as layers of a render of @p width × @p height pixels — drawn NOW, by
    /// the viewport that made this holder, so it is asked right after the capture and on the
    /// same thread. Null when the scene has none, or when they could not be drawn (the render
    /// then goes without them, as it did before there were layers).
    virtual std::shared_ptr<RenderSplatLayers> splatLayers(int width, int height) {
        (void)width;
        (void)height;
        return nullptr;
    }
};

} // namespace pose

#endif // RENDERSCENE_H
