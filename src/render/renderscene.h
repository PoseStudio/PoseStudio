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

namespace pose {

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

    /// What of the scene a render engine cannot draw, counted for the user (Gaussian splat
    /// scenes are rasterized by the viewport alone).
    virtual int skippedSplatScenes() const = 0;
};

} // namespace pose

#endif // RENDERSCENE_H
