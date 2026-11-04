/**
 * @file vreyeview.h
 * @brief One eye's view of the scene for a VR frame: the matrices the renderer draws with.
 *
 * The one VR type the rendering core knows. The OpenXR layer (src/viewport/vr/) tracks the
 * headset and turns each eye's pose + field of view into these; VulkanRenderer::drawVrFrame
 * renders the scene once per eye through them exactly as it renders the desktop view through
 * the orbit Camera (Camera::setExplicitView). GLM only — no OpenXR types reach rendering/.
 */

#ifndef VREYEVIEW_H
#define VREYEVIEW_H

#include <glm/glm.hpp>

namespace pose {

struct VrEyeView {
    glm::mat4 view{1.0f};       ///< world -> eye (the headset's tracked eye pose, inverted)
    glm::mat4 projection{1.0f}; ///< the eye's asymmetric frustum, Vulkan clip space (Y down, depth 0..1)
};

} // namespace pose

#endif // VREYEVIEW_H
