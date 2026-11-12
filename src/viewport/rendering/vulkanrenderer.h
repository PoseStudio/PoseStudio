/**
 * @file vulkanrenderer.h
 * @brief The frame orchestrator: command buffers, frame synchronisation, the screen-sized
 *        targets, and the acquire -> record -> submit -> present loop that runs the frame graph.
 *
 * This is the one file in rendering/ that sits ABOVE scene/ — it owns the Scene and the Grid and
 * is allowed to include their headers; every other rendering/ file stays below the scene layer.
 * Owns the swapchain and everything that depends on the surface (the offscreen HDR target, the
 * outline mask, the post chain), drives one frame per drawFrame() call, and transparently
 * rebuilds those when the window resizes or the surface goes out of date.
 *
 * A frame (recordCommandBuffer) is a fixed sequence of passes: the key-light SHADOW map -> the
 * selection OUTLINE mask (only with a selection) -> the offscreen HDR SCENE pass at the context's
 * MSAA count (backdrop, meshes, grid, overlays, resolved to a sampleable RGBA16F image plus a
 * separate specular image) -> the screen-space SSS blur and BLOOM (PBR mode only) -> the
 * single-sample swapchain pass, whose sole draw is the fullscreen COMPOSITE (tonemap + bloom +
 * outline). Sync is per frame-in-flight (kMaxFramesInFlight) for the acquire semaphore and
 * fence, per swapchain image for the render-finished semaphore.
 *
 * Like the rest of rendering/, this is intentionally Qt-free — it takes a plain
 * std::string shader directory — so the only Qt coupling in the whole viewport lives in
 * VulkanWindow / ViewportWidget.
 */

#ifndef VULKANRENDERER_H
#define VULKANRENDERER_H

#include "camera.h"
#include "selectionstyle.h"
#include "vreyeview.h"
#include "vulkancommon.h"
#include "vulkanhandles.h"

#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pose {

class VulkanContext;
class VulkanSwapchain;
class HdrTarget;
class OutlineMask;
class PostProcess;
class ShaderLibrary;
class TracedView;
struct SplatData;
class Grid;
class Scene;
struct ModelData;
struct BakedEnvironment;

/**
 * @class VulkanRenderer
 * @brief Renders frames for one window surface and manages its frame lifecycle.
 */
class VulkanRenderer {
public:
    /**
     * @param context      The device layer (must outlive the renderer).
     * @param initialExtent Window size in physical pixels at construction.
     * @param shaderDir    Directory holding the compiled *.spv files (see CMake).
     */
    VulkanRenderer(VulkanContext& context, VkExtent2D initialExtent, std::string shaderDir);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    /// Records and presents one frame. Cheaply no-ops while the window is zero-sized
    /// (e.g. minimised). Recreates the swapchain on its own when needed. Returns true when the
    /// caller should schedule one more frame (the swapchain was just rebuilt, or this frame was
    /// skipped mid-rebuild, so what's on screen doesn't yet reflect the current state) — the
    /// window renders on demand, not continuously, so this is the only self-rearm signal.
    bool drawFrame();

    /// A frame as presented, read back from the GPU: tightly packed 8-bit RGBA or BGRA rows, top
    /// row first, already display-encoded (the swapchain is an sRGB format).
    struct CapturedFrame {
        uint32_t                   width = 0;
        uint32_t                   height = 0;
        bool                       bgra = false;
        std::vector<unsigned char> pixels;
    };
    /// FRAME CAPTURE (the scripted test runner's eyes, VulkanWindow's script): the NEXT drawFrame()
    /// copies its composited swapchain image into host memory and waits for it — takeCapture()
    /// then hands it over. It reads what was rendered, not the desktop: an occluded window
    /// captures as well as a visible one. False from takeCapture when no capture is ready (none
    /// asked for, the swapchain is not readable, or the frame was skipped by a rebuild).
    void requestCapture() { m_captureRequested = true; }
    bool takeCapture(CapturedFrame& out);

    /// THE GAUSSIAN SPLATS AS LAYERS OF A RENDER. A path tracer cannot trace against a splat
    /// scene — it is a recorded picture — so a render takes the capture as the viewport draws
    /// it, at the render's own size, and composites (RenderSession): tightly packed RGBA floats,
    /// top row first, PREMULTIPLIED color with the coverage in alpha, DISPLAY-referred (the
    /// colors as the capture was trained, never touched by the scene's lighting).
    struct SplatLayers {
        uint32_t           width = 0;
        uint32_t           height = 0;
        std::vector<float> all;   ///< every splat, nothing in front of them: the PLATE the render stands on
        std::vector<float> front; ///< only the splats in FRONT of the scene's surfaces (and of the floor): laid back over the render
    };
    /// Draws them, offscreen, through the viewport camera as a picture of @p width × @p height
    /// frames it (the camera's height kept, as a render keeps it) and reads them back; blocks
    /// until the GPU has. The FRONT layer is depth-tested against every mesh as posed — the
    /// see-through ones from a twelfth of coverage — and, with @p cutAtFloor, holds nothing
    /// beyond the level plane at @p floorHeight: what lies under that plane is the floor the
    /// render's shadows fall on, and belongs to the plate. False without a splat scene. Throws
    /// VulkanError when the device cannot hold targets of that size.
    bool renderSplatLayers(uint32_t width, uint32_t height, bool cutAtFloor, float floorHeight, SplatLayers& out);

    /// Records the new physical-pixel size; the actual swapchain rebuild is deferred to
    /// the next drawFrame() so a burst of resize events coalesces into one rebuild.
    void notifyResize(VkExtent2D newExtent);

    /// Uploads an already-parsed model (geometry + decoded textures) to the scene. Call from the
    /// GUI thread (the upload blocks briefly). Throws on Vulkan failure. Parsing + image decoding
    /// happen in the Qt layer (VulkanWindow), keeping this core free of file/codec concerns.
    void addModel(const ModelData& data);
    /// Uploads a Gaussian splat scene (splatcloud.h) as a static model at @p transform.
    void addSplatModel(const SplatData& data, const glm::mat4& transform);

    /// Removes the model at @p index from the scene, waiting for the GPU to go idle first so its
    /// buffers/descriptors aren't freed while an in-flight frame still references them.
    void deleteModel(std::size_t index);

    /// Removes EVERY model (selection + active figure reset), with the same device-wait policy as
    /// deleteModel — File → New and Open start from this.
    void clearModels();

    /// The scene: selection, picking, shade mode, lighting dials, and the whole posing API
    /// (joints, full-body IK, pins, pose snapshots and files) are called on it directly — the
    /// renderer adds no policy of its own to any of them. Only the operations that DO (a device
    /// wait before freeing, the camera framing, the environment upload) stay as renderer methods.
    Scene&       scene() { return *m_scene; }
    const Scene& scene() const { return *m_scene; }
    /// Scale for screen-space UI sizes rendered by the engine (the selection outline's width):
    /// the window's device pixel ratio, so a 2px outline stays 2 LOGICAL pixels on a HiDPI
    /// display. Defaults to 1.
    void setUiScale(float scale) { m_uiScale = scale; }

    /// How the selection is marked (selectionstyle.h; Preferences → Selection): the outline's
    /// color and opacity go to the composite of the desktop chain and of VR Mode's, the joint
    /// highlight's to the Scene's camera block. Takes effect at the next frame; at an outline
    /// opacity of 0 the outline's mask pass is skipped altogether.
    void                  setSelectionStyle(const SelectionStyle& style);
    const SelectionStyle& selectionStyle() const { return m_selectionStyle; }

    /// Exposed so the window's input handlers can drive the view (orbit/pan/dolly).
    Camera& camera() { return m_camera; }

    /// "Frame selected": aims the camera at the selected model (every model, with no selection)
    /// and backs off so its bounds fill the view, keeping the orbit angles. False with nothing
    /// to frame (the camera is left alone).
    bool frameSelected();

    /// Uploads a CPU-baked lighting environment (SH + prefiltered specular; see bakeEnvironment) and
    /// swaps it in. The bake is done off the render thread by the Qt layer — keeping both the CPU work
    /// and the image codec out of this core. No-op if the scene doesn't exist yet.
    void applyBakedEnvironment(const BakedEnvironment& baked);

    // --- THE RENDERED MODE (rendering/tracedview.h) ---------------------------------------------
    /// The path-traced picture of the scene from now on: @p width × @p height linear RGBA floats,
    /// row 0 = top, the exposure applied (a render engine's image, as its interface hands it
    /// over). Copied; drawn from the next frame while setTracedShown(true).
    void setTracedImage(const float* rgba, uint32_t width, uint32_t height);
    /// Whether the frame shows the path-traced picture in place of the rasterized scene (it
    /// does only once there is one: until then, and whenever this is off, the scene is drawn as
    /// its shade mode says). The line overlays and the selection outline are drawn either way.
    void setTracedShown(bool shown) { m_tracedShown = shown; }
    bool tracedShown() const { return m_tracedShown; }
    /// Forgets the picture (leaving the mode, or a picture of a scene that is gone).
    void clearTracedImage();

    // --- VR Mode (the OpenXR session lives in src/viewport/vr/; this core sees only Vulkan) -----
    /// Enters VR: builds a second set of scene targets at the headset's per-eye size (@p eyeExtent)
    /// — HDR target, outline mask, post chain — and a composite for the headset's render pass
    /// (@p eyeRenderPass, over its swapchain images; @p encodeSrgb when their format is UNORM so
    /// the composite encodes). Waits for the device. Until endVr(), drawFrame() no longer renders
    /// the desktop camera: it MIRRORS the headset's left eye into the window instead.
    /// @p eyeDepthRenderPass (VK_NULL_HANDLE = none): the session's depth-only pass over its
    /// depth swapchain — the eye target then keeps its depth and resolves it per eye for the
    /// runtime's reprojection (needs MSAA: the resolve shader reads a multisampled image).
    /// @p reducedEffects skips the subsurface blur and bloom in the eyes (a lighter GPU).
    /// @p splatScale sizes the eyes' photo layer — the Gaussian splats — as a share of the eye
    /// (hdrtarget.h: the splats' fill is what a headset frame cannot afford; 1 = the eye's size).
    void beginVr(VkRenderPass eyeRenderPass, VkExtent2D eyeExtent, bool encodeSrgb,
                 VkRenderPass eyeDepthRenderPass = VK_NULL_HANDLE, bool reducedEffects = false,
                 float splatScale = 1.0f);
    /// Leaves VR (waits for the device, frees the eye targets); the desktop view is its own again.
    void endVr();
    bool vrActive() const { return m_vr != nullptr; }
    /// The eyes' photo layer scale as drawn now (HdrTarget::setPhotoScale: free to change between
    /// frames) — VR Mode's dynamic scale (vulkanwindow_vr.cpp) moves it with the GPU's time.
    void setVrSplatScale(float scale);
    /// Renders one VR frame: the key-light shadow map once, then each eye's scene through its
    /// tracked view (VrEyeView) into the eye targets, composited into @p eyeFramebuffers[eye]
    /// (the headset's framebuffer for the image the caller acquired), and — with a depth pass —
    /// its depth resolved into @p eyeDepthFramebuffers[eye] (null entries skip it). Submits on
    /// the graphics queue — one VR frame in flight: the previous one is waited for first. Throws
    /// VulkanError.
    void drawVrFrame(const VrEyeView views[2], const VkFramebuffer eyeFramebuffers[2],
                     const VkFramebuffer eyeDepthFramebuffers[2] = nullptr);

    // --- GPU timing (POSESTUDIO_GPU_PERF / POSESTUDIO_VR_PERF) ----------------------------------
    /// The GPU time of the last COMPLETED frame of a kind — the whole command buffer, and the
    /// Gaussian splat draw(s) within it (both eyes' in a VR frame) — in milliseconds, read off
    /// timestamp queries where that frame's fence is waited for. valid once such a frame has
    /// completed. Measured whenever the queue has timestamps (a few stamps a frame): the VR
    /// splat scale reads it; the two environment variables only PRINT it.
    struct GpuTimings {
        double frameMs = 0.0;
        double splatMs = 0.0;
        bool   valid = false;
    };
    const GpuTimings& lastGpuTimings() const { return m_gpuTimings; }     ///< the desktop frame's
    const GpuTimings& lastVrGpuTimings() const { return m_vrGpuTimings; } ///< the VR frame's

private:
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void recreateSwapchain();
    /// Records the whole frame graph for one frame — see the file banner and the definition.
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, VkBuffer captureInto);
    /// The view-dependent half of the frame graph (steps 2-4 of recordCommandBuffer: the outline
    /// mask, the HDR scene pass, SSS + bloom) for @p camera into the given targets, writing frame
    /// slot @p frameSlot's UBOs. Shared by the desktop frame and each VR eye. Returns whether the
    /// outline mask was drawn (the composite paints no outline otherwise).
    bool recordScenePasses(VkCommandBuffer cmd, const Camera& camera, uint32_t frameSlot, HdrTarget& hdr,
                           OutlineMask& outline, PostProcess& post, VkExtent2D extent,
                           bool postEffects = true, bool asyncSplatSort = false);
    /// THE RENDERED MODE's half of the frame graph, in place of recordScenePasses: the outline
    /// mask, then the HDR scene pass holding the path-traced picture and the line overlays over
    /// it (no shadow map, no meshes, no grid, no subsurface blur, no bloom — the picture has its
    /// own light). Returns whether the outline mask was drawn.
    bool recordTracedPasses(VkCommandBuffer cmd, const Camera& camera, uint32_t frameSlot, VkExtent2D extent);

    VulkanContext& m_context;

    std::unique_ptr<ShaderLibrary>   m_shaders; // kept: the VR post chain is built later from it

    /// The eye-sized targets + the headset composite of VR Mode (beginVr .. endVr).
    struct VrTargets;
    std::unique_ptr<VrTargets> m_vr;

    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<HdrTarget>       m_hdrTarget;   // offscreen HDR scene target (see hdrtarget.h)
    std::unique_ptr<OutlineMask>     m_outlineMask; // the selected model's silhouette coverage (see outlinemask.h)
    std::unique_ptr<PostProcess>     m_postProcess; // SSS + bloom + the tonemapping composite (+ the outline)
    std::unique_ptr<Scene>           m_scene; // every scene pass: shadow map, outline mask, the HDR
                                              // scene (backdrop, meshes, overlays) — see scene.h
    std::unique_ptr<Grid>            m_grid;  // floor grid overlay (drawn into the HDR pass after the scene)
    std::unique_ptr<TracedView>      m_traced; // the Rendered mode's picture (tracedview.h)
    bool                             m_tracedShown = false; // see setTracedShown
    float                            m_uiScale = 1.0f; // see setUiScale
    SelectionStyle                   m_selectionStyle;  // see setSelectionStyle

    // GPU timestamps (lastGpuTimings): kTimingQueries queries per SLOT — the frame's first stamp,
    // the splat draw's begin/end for each of up to two scene passes (the desktop's one; an eye
    // each in VR), the frame's last stamp — a slot per desktop frame in flight and one for the
    // VR frame; written only while timing is on, read back after the slot's fence.
    static constexpr uint32_t kTimingQueries = 6;
    static constexpr uint32_t kVrTimingSlot = static_cast<uint32_t>(kMaxFramesInFlight);
    void createTimingPool();
    void timingBegin(VkCommandBuffer cmd, uint32_t slot); // resets the slot (outside any render pass) + stamp 0
    void timingMark(VkCommandBuffer cmd, uint32_t query); // one of the slot's inner stamps
    void timingEnd(VkCommandBuffer cmd);                  // the slot's last stamp
    void timingRead(uint32_t slot, GpuTimings& into);     // after the slot's fence
    bool            m_gpuTimingOn = false;
    float           m_timestampPeriodNs = 0.0f;
    UniqueQueryPool m_timingPool;
    std::array<bool, kMaxFramesInFlight + 1> m_timingWritten{};
    uint32_t        m_timingSlot = 0; // the slot the frame being recorded stamps
    uint32_t        m_timingPass = 0; // its scene passes so far (recordScenePasses)
    GpuTimings      m_gpuTimings;
    GpuTimings      m_vrGpuTimings;

    UniqueCommandPool            m_commandPool;
    std::vector<VkCommandBuffer> m_commandBuffers; // one per frame-in-flight; freed with the pool

    // imageAvailable + inFlight are per frame-in-flight; renderFinished is per swapchain
    // image (a semaphore signalled at submit must not be reused until that image's
    // present completes, which is tracked per image, not per frame). RAII owners: destroyed
    // after the destructor body (which has already waited for the device to go idle).
    std::vector<UniqueSemaphore> m_imageAvailableSemaphores;
    std::vector<UniqueSemaphore> m_renderFinishedSemaphores;
    std::vector<UniqueFence>     m_inFlightFences;

    uint32_t   m_currentFrame = 0;
    VkExtent2D m_windowExtent = {0, 0};
    bool       m_framebufferResized = false;
    bool          m_captureRequested = false; ///< See requestCapture().
    CapturedFrame m_capture;                  ///< The last captured frame, until taken.

    Camera m_camera;
};

} // namespace pose

#endif // VULKANRENDERER_H
