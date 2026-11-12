/**
 * @file splatcomposite.h
 * @brief A render with GAUSSIAN SPLATS in it: the path-traced picture laid into the capture as a
 *        film composite lays a CG character into footage.
 *
 * A splat scene is a recorded picture — colors as they were photographed, with no surface a ray
 * could be traced against — so the render engine never sees it. What a render does instead:
 *
 *   - the viewport's own splat renderer draws the capture at the render's size, twice
 *     (RenderSplatLayers): ALL of it, and only the part in FRONT of the scene's surfaces;
 *   - the engine renders the scene over NOTHING, its floor's shadows kept as coverage
 *     (renderengineapi.h: Settings::transparentBackground + shadowAlpha);
 *   - this class puts the three together:
 *
 *         plate   = ALL over the backdrop                (the capture as the picture's background)
 *         under   = render over plate, in LINEAR light   (the figure's edges are antialiased
 *                                                         against the capture itself, and its
 *                                                         shadow darkens the capture's floor as
 *                                                         light does)
 *         picture = FRONT over display(under)            (what stands in front of the figure)
 *
 * The capture's colors are DISPLAY-referred and the render's are linear, so the plate is taken
 * through the inverse of the picture's display transform — exact where the capture covers the
 * pixel: display(inverse(c)) = c, the capture comes out of the render as it went into training —
 * and the front layer is laid over the finished display picture, as the viewport's composite
 * lays it. Qt-free (std + the shared parallel helper).
 */

#ifndef SPLATCOMPOSITE_H
#define SPLATCOMPOSITE_H

#include "renderscene.h"

#include <cstddef>
#include <vector>

namespace pose {

/// The viewport's last step for one channel, as floats: the ACES fit when @p tonemap, then the
/// sRGB curve. In: linear light (the exposure applied). Out: 0..1.
float displayEncode(float linear, bool tonemap);
/// Its inverse: the linear value that displayEncode takes to @p display (clamped to 0..1).
float displayDecode(float display, bool tonemap);

class SplatComposite {
public:
    /// How the picture is shown and what lies behind everything.
    struct View {
        bool  tonemap = true;
        float exposure = 1.0f;                    ///< the multiplier the render's pixels already carry
        float backdropColor[3] = {0.0f, 0.0f, 0.0f}; ///< the flat backdrop's radiance (before the exposure)
        bool  transparent = false;                ///< no backdrop: the picture keeps its coverage
    };

    /// Takes the layers (moved from) and prepares the plate — once: it is the same for every
    /// pass of the render.
    SplatComposite(RenderSplatLayers&& layers, const View& view);

    int width() const { return m_width; }
    int height() const { return m_height; }

    /// Composites one state of the render. @p render: the engine's picture — @p width() ×
    /// height() RGBA floats, top row first, linear with the exposure applied, PREMULTIPLIED,
    /// alpha = the scene's coverage over the floor shadow's darkness. Writes the display picture
    /// as 8-bit RGBA with STRAIGHT alpha (rows @p stride8 bytes apart; alpha 255 unless the view
    /// is transparent) and, where @p linearOut is given (it may be @p render itself), the same
    /// picture as linear premultiplied floats — what an OpenEXR file is saved from.
    void compose(const float* render, unsigned char* out8, std::size_t stride8, float* linearOut) const;

private:
    int                m_width = 0;
    int                m_height = 0;
    bool               m_tonemap = true;
    bool               m_transparent = false;
    std::vector<float> m_plate; ///< RGBA: linear (exposed), premultiplied by its coverage (1 over a backdrop)
    std::vector<float> m_front; ///< RGBA: display-referred, premultiplied
};

} // namespace pose

#endif // SPLATCOMPOSITE_H
