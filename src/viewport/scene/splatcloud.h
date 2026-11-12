/**
 * @file splatcloud.h
 * @brief GAUSSIAN SPLATS on the GPU: a scene's splats as storage buffers, the per-view far-first
 *        draw order sorted on the CPU, and the one instanced-quad draw the splat pipeline makes
 *        of them (shaders/splat.vert + splat.frag; the pipeline and its set layout are
 *        ScenePipelines'). A Model holds one (Model::splats) in place of meshes — a splat scene
 *        is static, unlit and never a figure — and the Scene draws every model's cloud after the
 *        opaque meshes, before the transparent ones.
 *
 * THE DATA: per splat three vec4 (set 1 binding 0) — the centre with its opacity, then the 3D
 * covariance Σ = R S Sᵀ Rᵀ from its rotation and scales (xx xy xz yy | yz zz) in the splat's own
 * frame, carried through the model matrix in the shader — and its spherical harmonics as halves
 * packed in pairs (binding 1; one coefficient-major run of 3, 12, 27 or 48 halves a splat). THE
 * ORDER (binding 2, one host-mapped buffer per frame in flight): a translucent splat is blended
 * over what lies behind it, so the splats are drawn FAR FIRST along the view direction — a
 * counting sort on a 16-bit quantized depth, parallel over chunks (parallelfor.h), re-done only
 * when the view DIRECTION turned — by more than a twentieth of a degree: the order is a function
 * of the direction alone, so a dolly, a pan, a tracked head's jitter and a headset's second eye
 * re-sort nothing, and an event-driven frame that only posed the figure reuses the last order —
 * or the model transform moved; 4 million splats sort in a few milliseconds on eight cores. A
 * desktop frame sorts on its own thread (an orbit tick's frame draws the order of its own view,
 * exactly); VR MODE's frames hand the sort to a WORKER THREAD and draw the last order landed — a
 * frame or two behind the head at most, which no eye can tell — so a frame never waits on it
 * (sorted on the GUI thread per head turn, half a million splats took 10 ms of every 14 ms frame
 * and the headset showed every other frame, late). Vertex buffers there are none: the draw is a
 * four-vertex triangle strip instanced once a splat, gl_InstanceIndex the sorted slot and
 * gl_VertexIndex the quad corner — four vertex invocations a splat where six plain vertices
 * were six. The draw goes into the PHOTO PASS (hdrtarget.h), single-sample, after the scene.
 *
 * Qt-free (Vulkan + std + GLM).
 */

#ifndef SPLATCLOUD_H
#define SPLATCLOUD_H

#include "vulkanbuffer.h"
#include "vulkancommon.h"
#include "vulkanhandles.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace pose {

class Camera;
class VulkanContext;
struct SplatData;

/// The splat pipeline's push block — must stay byte-identical to splat.vert's `PC`.
struct SplatPushConstants {
    glm::mat4 model;
    glm::vec4 focal;  ///< x = fx, y = fy (pixels), z = viewport width, w = height
    glm::vec4 params; ///< x = SH degree, y = harmonics uints per splat, z = the tiny-quad probe (POSESTUDIO_SPLAT_TINY, 0/1), w = orthographic (0/1)
    glm::vec4 camPos; ///< xyz = the eye in the splat's own (model) frame; w = 1 for the opacity compensation (every scene; 0 = the A/B off)
    glm::vec4 cut;    ///< THE FLOOR CUT: x = a world height, y = +1 / -1 (the eye above / below that plane: splats beyond it are left out) or 0 = none
};
static_assert(sizeof(SplatPushConstants) == 128, "must match splat.vert's push block");

class SplatCloud {
public:
    /// Uploads @p data (one blocking transfer per buffer) and allocates the per-frame order
    /// buffers and descriptor sets from @p setLayout (ScenePipelines::splatSetLayout).
    SplatCloud(VulkanContext& context, const SplatData& data, VkDescriptorSetLayout setLayout);
    ~SplatCloud();

    SplatCloud(const SplatCloud&) = delete;
    SplatCloud& operator=(const SplatCloud&) = delete;

    std::size_t      count() const { return m_count; }
    int              shDegree() const { return m_shDegree; }
    const glm::vec3& boundsMin() const { return m_boundsMin; } ///< The centres' local AABB.
    const glm::vec3& boundsMax() const { return m_boundsMax; }

    /// Sorts the splats far-first for @p camera's view of the cloud at @p model (when the view
    /// direction turned or the model moved since the last sort) and writes the order into frame
    /// slot @p frameIndex's buffer if that slot is behind. Call before recording the frame (GUI
    /// thread). With @p asyncSort the sort runs on the worker thread and this frame draws the
    /// last order landed (VR Mode's frames, which come at the headset's rate); without it the
    /// sort runs here, so the frame draws the order of its own view.
    void prepareFrame(const Camera& camera, const glm::mat4& model, uint32_t frameIndex, bool asyncSort = false);

    /// Records the draw: binds this frame slot's set at index 1 (the caller has bound the splat
    /// pipeline and the camera set at 0), pushes the constants (the focal lengths off the
    /// camera's projection and viewport, the eye in the splat frame) and draws six vertices a
    /// splat (into the pass's photo layer: the colors as trained, the composite lays them over).
    /// @p cut is the push block's floor cut (SplatPushConstants::cut): zero draws every splat.
    void record(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex, const Camera& camera,
                const glm::mat4& model, const glm::vec4& cut = glm::vec4(0.0f)) const;

    /// WHAT A CLICK IS ON: the cloud's opacity along a ray, as the pixel under the cursor shows
    /// it. The ray is given in the cloud's OWN frame (@p origin, and @p direction the world's
    /// unit direction taken through the inverse model matrix, NOT re-normalized — the parameter
    /// along it is then the world's distance, comparable with a mesh's hit). Each splat the ray
    /// passes within its 3-sigma footprint weighs in with its opacity times its Gaussian at the
    /// ray's closest approach — the value the splat shader gives that pixel — and they are
    /// accumulated near to far as the "over" blend accumulates them. @p opacityOut is the total
    /// (what the outline's mask holds there: its half contour is the selection outline);
    /// @p tOut is where the accumulated opacity reaches a half — where the capture is what the
    /// eye sees along this ray — or, for a fainter total, where half of it has gathered. False
    /// under kPickFaintOpacity in all: the cursor is on nothing of this cloud.
    bool pick(const glm::vec3& origin, const glm::vec3& direction, float& tOut, float& opacityOut) const;
    /// Under this much accumulated opacity a ray is on nothing of the cloud (a stray floater).
    static constexpr float kPickFaintOpacity = 0.10f;

private:
    /// Sorts the cloud far-first along @p viewDir at @p model into @p out, over @p chunks
    /// chunks = threads (the scratch vectors are shared: one sort at a time, on whichever thread).
    void sortForView(const glm::vec3& viewDir, const glm::mat4& model, std::vector<uint32_t>& out, int chunks);
    void workerLoop();

    VulkanContext& m_context;
    std::size_t    m_count = 0;
    int            m_shDegree = 0;
    uint32_t       m_shStride = 2; ///< Harmonics uints per splat.
    glm::vec3      m_boundsMin{0.0f};
    glm::vec3      m_boundsMax{0.0f};

    // The CPU side of a click (pick): per splat the covariance (xx xy xz yy yz zz, as the GPU has
    // it), the opacity and the footprint's reach (three of its largest standard deviation) —
    // 32 bytes a splat; the centres are m_positions below.
    struct PickSplat {
        float cov[6];
        float opacity;
        float reach;
    };
    std::vector<PickSplat> m_pick;

    // The CPU side of the sort.
    std::vector<glm::vec3> m_positions; ///< Local centres.
    std::vector<glm::vec3> m_world;     ///< ... through the model matrix they were last sorted at.
    glm::mat4              m_worldFor{0.0f};
    std::vector<float>     m_depth;
    std::vector<uint16_t>  m_keys;
    std::vector<uint32_t>  m_order;
    std::vector<uint32_t>  m_hist;
    std::vector<float>     m_chunkMin, m_chunkMax;
    int                    m_maxChunks = 1; ///< what the scratch above is sized for
    glm::vec3              m_lastDir{0.0f}; ///< The view direction the order was last sorted along.
    glm::mat4              m_lastModel{0.0f};
    bool                   m_sorted = false;
    std::uint64_t          m_sortVersion = 0;
    std::array<std::uint64_t, kMaxFramesInFlight> m_uploaded{};

    // The sort worker (VR Mode): one thread, started on the first asynchronous request; the
    // latest view asked for is sorted next into m_orderWork — the worker's own, touched by no
    // other thread — and handed over, under the lock, into m_orderBack, which prepareFrame swaps
    // in on the GUI thread. (Sorted straight into m_orderBack, as it was until 2026-11-07, a
    // request queued while the worker was busy started the next sort into the very vector that
    // held the finished, unconsumed result, while the GUI thread swapped it out: in VR, during a
    // head turn, the drawn order could be half of one sort and half of the next.) m_mutex guards
    // every member below but m_orderWork, and the request / result flags; the scratch vectors
    // above are the sorting thread's alone (the GUI thread sorts only with the worker idle).
    std::thread             m_worker;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    bool                    m_quit = false;
    bool                    m_wanted = false;      ///< A request waits for the worker.
    bool                    m_busy = false;        ///< The worker is sorting.
    bool                    m_resultReady = false; ///< m_orderBack holds a finished sort.
    glm::vec3               m_askDir{0.0f};
    glm::mat4               m_askModel{1.0f};
    glm::vec3               m_resultDir{0.0f};
    glm::mat4               m_resultModel{1.0f};
    std::vector<uint32_t>   m_orderBack; ///< A finished sort, waiting for prepareFrame (guarded by m_mutex).
    std::vector<uint32_t>   m_orderWork; ///< The sort in progress: the worker thread's alone.

    // The GPU side. Declaration order = reverse destruction: the pool after the buffers its sets
    // describe.
    UniqueDescriptorPool                           m_pool;
    VulkanBuffer                                   m_splatBuffer; ///< 3 vec4 a splat, device-local.
    VulkanBuffer                                   m_shBuffer;    ///< The packed halves, device-local.
    std::array<VulkanBuffer, kMaxFramesInFlight>   m_orderBuffers; ///< One host-mapped order a frame slot.
    std::array<VkDescriptorSet, kMaxFramesInFlight> m_sets{};
};

} // namespace pose

#endif // SPLATCLOUD_H
