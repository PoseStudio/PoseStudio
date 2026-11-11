/**
 * @file splatcloud.cpp
 * @brief The Gaussian splat cloud: upload, the far-first sort, the draw. See splatcloud.h.
 */

#include "splatcloud.h"

#include "camera.h"
#include "descriptorutil.h"
#include "parallelfor.h"
#include "splatimporter.h"
#include "vulkancontext.h"

#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

namespace pose {

namespace {

constexpr std::size_t kBins = 65536; ///< The depth is quantized to 16 bits for the counting sort.

/// A view that turned less than this since the last sort keeps the order: the order is a
/// function of the view direction alone, and so small a turn can swap only two splats whose
/// depths differ by less than the angle times their lateral separation — a millimetre or two
/// across a capture — while a tracked head turns by that much every frame it is held still.
constexpr float kResortDeg = 0.05f;
const float     kResortCos = std::cos(glm::radians(kResortDeg));

/// The chunks (= threads, through parallelfor.h) a sort of @p n splats runs over, at most @p cap.
int chunkCountFor(std::size_t n, int cap) {
    const unsigned hw = std::thread::hardware_concurrency();
    const int      workers = static_cast<int>(hw > 1 ? hw : 1);
    // A chunk per MILLION splats: measured 2026-11-05 on 600k (a 16-core laptop), one thread sorts
    // in 4-6 ms where ten took 20-32 — each pass spawns its threads afresh (parallelfor.h) and
    // every chunk zeroes and prefix-sums its own 65,536-bin histogram, so under a million the
    // threads cost more than they save.
    const int      byWork = static_cast<int>((n + 999999) / 1000000);
    return std::max(1, std::min({workers * 2, 32, std::max(1, cap), std::max(1, byWork)}));
}

int envChunks(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::max(1, std::atoi(value)) : fallback;
}

/// How many threads a sort runs over. parallelfor.h spawns a thread per chunk for each of a
/// sort's three passes, and a 16-core box spawning 48 threads a sort — several hundred a second
/// once a headset's every frame asks for one — spends as long spawning as sorting, and starves
/// the VR runtime's own threads beside it; a few threads do the sort in the same time with no
/// storm. POSESTUDIO_SPLAT_SORT_THREADS=<n> is the desktop frames' lever,
/// POSESTUDIO_SPLAT_WORKER_THREADS=<n> the VR worker's (the measuring levers behind the defaults).
const int kDesktopChunks = envChunks("POSESTUDIO_SPLAT_SORT_THREADS", 32);
const int kWorkerChunks = envChunks("POSESTUDIO_SPLAT_WORKER_THREADS", 4);

} // namespace

SplatCloud::SplatCloud(VulkanContext& context, const SplatData& data, VkDescriptorSetLayout setLayout)
    : m_context(context), m_count(data.count()), m_shDegree(data.shDegree) {
    const std::size_t n = m_count;
    // The GPU splats: the centre with its opacity, then the covariance Σ = (R S)(R S)ᵀ.
    std::vector<glm::vec4> gpu(n * 3);
    m_boundsMin = glm::vec3(std::numeric_limits<float>::max());
    m_boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec3& p = data.positions[i];
        const glm::vec4& q = data.rotations[i]; // (w, x, y, z)
        const glm::vec3& s = data.scales[i];
        const glm::mat3  R = glm::mat3_cast(glm::quat(q.x, q.y, q.z, q.w));
        const glm::mat3  M(R[0] * s.x, R[1] * s.y, R[2] * s.z); // R · diag(s), column by column
        const glm::mat3  cov = M * glm::transpose(M);
        gpu[i * 3] = glm::vec4(p, data.opacities[i]);
        gpu[i * 3 + 1] = glm::vec4(cov[0][0], cov[1][0], cov[2][0], cov[1][1]);
        gpu[i * 3 + 2] = glm::vec4(cov[2][1], cov[2][2], 0.0f, 0.0f);
        m_boundsMin = glm::min(m_boundsMin, p);
        m_boundsMax = glm::max(m_boundsMax, p);
    }
    if (n == 0) {
        m_boundsMin = m_boundsMax = glm::vec3(0.0f);
    }
    m_splatBuffer = createDeviceLocalBuffer(context, gpu.data(), sizeof(glm::vec4) * std::max<std::size_t>(gpu.size(), 1),
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    gpu.clear();
    gpu.shrink_to_fit();

    // The harmonics as halves, two to a uint, one run of (coefficients × 3) a splat.
    const std::size_t elements = static_cast<std::size_t>(data.coefficients()) * 3;
    m_shStride = static_cast<uint32_t>((elements + 1) / 2);
    std::vector<uint32_t> packed(std::max<std::size_t>(n * m_shStride, 1), 0);
    for (std::size_t i = 0; i < n; ++i) {
        const float* sh = &data.sh[i * elements];
        for (std::size_t e = 0; e < elements; e += 2) {
            const float second = e + 1 < elements ? sh[e + 1] : 0.0f;
            packed[i * m_shStride + e / 2] = glm::packHalf2x16(glm::vec2(sh[e], second));
        }
    }
    m_shBuffer = createDeviceLocalBuffer(context, packed.data(), sizeof(uint32_t) * packed.size(),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    packed.clear();
    packed.shrink_to_fit();

    // The order buffers (host-mapped, rewritten after every sort) and the sets that bind the three.
    const VkDevice device = context.device();
    m_pool = createDescriptorPool(device, {poolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kMaxFramesInFlight)},
                                  kMaxFramesInFlight);
    allocateDescriptorSets(device, m_pool.get(), setLayout, kMaxFramesInFlight, m_sets.data());
    const VkDeviceSize orderBytes = sizeof(uint32_t) * std::max<std::size_t>(n, 1);
    for (int f = 0; f < kMaxFramesInFlight; ++f) {
        const auto slot = static_cast<std::size_t>(f);
        m_orderBuffers[slot] = VulkanBuffer(context, orderBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        const VkDescriptorBufferInfo infos[3] = {
            bufferInfo(m_splatBuffer.handle()),
            bufferInfo(m_shBuffer.handle()),
            bufferInfo(m_orderBuffers[slot].handle(), orderBytes),
        };
        writeBufferDescriptors(device, m_sets[slot], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, infos, 3);
    }

    // The CPU side of the sort.
    m_positions = data.positions;
    m_world.resize(n);
    m_depth.resize(n);
    m_keys.resize(n);
    m_order.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        m_order[i] = static_cast<uint32_t>(i);
    }
    const int chunks = chunkCountFor(n, 32); // the most any sort runs over: the scratch is sized for it
    m_maxChunks = chunks;
    m_hist.resize(static_cast<std::size_t>(chunks) * kBins);
    m_chunkMin.resize(static_cast<std::size_t>(chunks));
    m_chunkMax.resize(static_cast<std::size_t>(chunks));
}

SplatCloud::~SplatCloud() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join(); // a sort in flight finishes first (milliseconds)
    }
}

void SplatCloud::sortForView(const glm::vec3& viewDir, const glm::mat4& model, std::vector<uint32_t>& out,
                             int wantedChunks) {
    const std::size_t n = m_count;
    if (n == 0) {
        return;
    }
    // POSESTUDIO_SPLAT_TRACE=1: the sort's cost per view change, the number that says when a
    // GPU sort is due.
    static const bool kTrace = std::getenv("POSESTUDIO_SPLAT_TRACE") != nullptr;
    const auto        t0 = std::chrono::steady_clock::now();
    out.resize(n);
    const int         chunks = std::clamp(wantedChunks, 1, m_maxChunks);
    const std::size_t chunkSize = (n + static_cast<std::size_t>(chunks) - 1) / static_cast<std::size_t>(chunks);
    auto range = [&](int c, std::size_t& lo, std::size_t& hi) {
        lo = static_cast<std::size_t>(c) * chunkSize;
        hi = std::min(n, lo + chunkSize);
    };
    // The centres through the model matrix, re-done when it moved.
    if (model != m_worldFor) {
        parallelFor(chunks, [&](int c) {
            std::size_t lo, hi;
            range(c, lo, hi);
            for (std::size_t i = lo; i < hi; ++i) {
                m_world[i] = glm::vec3(model * glm::vec4(m_positions[i], 1.0f));
            }
        });
        m_worldFor = model;
    }
    // The depth along the view direction (the view matrix's third row's direction; view z
    // points at the eye, so the distance is its negative — the row's constant term orders
    // nothing and is left out), and each chunk's range of it.
    parallelFor(chunks, [&](int c) {
        std::size_t lo, hi;
        range(c, lo, hi);
        float cmin = std::numeric_limits<float>::max();
        float cmax = std::numeric_limits<float>::lowest();
        for (std::size_t i = lo; i < hi; ++i) {
            const glm::vec3& w = m_world[i];
            const float      d = -(viewDir.x * w.x + viewDir.y * w.y + viewDir.z * w.z);
            m_depth[i] = d;
            cmin = std::min(cmin, d);
            cmax = std::max(cmax, d);
        }
        m_chunkMin[static_cast<std::size_t>(c)] = cmin;
        m_chunkMax[static_cast<std::size_t>(c)] = cmax;
    });
    float dmin = std::numeric_limits<float>::max();
    float dmax = std::numeric_limits<float>::lowest();
    for (int c = 0; c < chunks; ++c) {
        dmin = std::min(dmin, m_chunkMin[static_cast<std::size_t>(c)]);
        dmax = std::max(dmax, m_chunkMax[static_cast<std::size_t>(c)]);
    }
    if (!(dmax - dmin > 1e-6f)) {
        dmax = dmin + 1e-6f;
    }
    const float scale = static_cast<float>(kBins - 1) / (dmax - dmin);
    // The keys — far first: the farthest splat gets key 0 — and a histogram per chunk.
    parallelFor(chunks, [&](int c) {
        std::size_t lo, hi;
        range(c, lo, hi);
        uint32_t* hist = &m_hist[static_cast<std::size_t>(c) * kBins];
        std::fill(hist, hist + kBins, 0u);
        for (std::size_t i = lo; i < hi; ++i) {
            const float    t = (m_depth[i] - dmin) * scale;
            const uint32_t bin = static_cast<uint32_t>(std::clamp(t, 0.0f, static_cast<float>(kBins - 1)));
            const auto     key = static_cast<uint16_t>(kBins - 1 - bin);
            m_keys[i] = key;
            ++hist[key];
        }
    });
    // The prefix sums, bin-major across the chunks: each chunk's histogram becomes its starts.
    uint32_t running = 0;
    for (std::size_t b = 0; b < kBins; ++b) {
        for (int c = 0; c < chunks; ++c) {
            uint32_t& h = m_hist[static_cast<std::size_t>(c) * kBins + b];
            const uint32_t count = h;
            h = running;
            running += count;
        }
    }
    // The scatter: each chunk writes into its own starts, so the chunks never collide.
    parallelFor(chunks, [&](int c) {
        std::size_t lo, hi;
        range(c, lo, hi);
        uint32_t* start = &m_hist[static_cast<std::size_t>(c) * kBins];
        for (std::size_t i = lo; i < hi; ++i) {
            out[start[m_keys[i]]++] = static_cast<uint32_t>(i);
        }
    });
    if (kTrace) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[splat] sorted %zu splats far-first in %.1f ms over %d threads\n", m_count, ms, chunks);
    }
}

void SplatCloud::prepareFrame(const Camera& camera, const glm::mat4& model, uint32_t frameIndex, bool asyncSort) {
    if (m_count == 0) {
        return;
    }
    // The view DIRECTION (the view matrix's third row, which the sort reads the depth along):
    // the order is a function of it alone, so a dolly, a pan and a headset's second eye — the
    // same orientation, six centimetres over — re-sort nothing, and a turn under kResortDeg (a
    // tracked head's jitter, frame to frame) keeps the last order.
    const glm::mat4 view = camera.view();
    glm::vec3       dir(view[0][2], view[1][2], view[2][2]);
    const float     len = glm::length(dir);
    dir = len > 1e-6f ? dir / len : glm::vec3(0.0f, 0.0f, 1.0f);
    auto turnedFrom = [&](const glm::vec3& last) { return glm::dot(dir, last) < kResortCos; };

    std::unique_lock<std::mutex> lock(m_mutex);
    // A sort the worker finished since the last frame lands now: its order is the draw's.
    if (m_resultReady) {
        m_order.swap(m_orderBack);
        m_lastDir = m_resultDir;
        m_lastModel = m_resultModel;
        m_sorted = true;
        ++m_sortVersion;
        m_resultReady = false;
    }
    const bool stale = !m_sorted || turnedFrom(m_lastDir) || model != m_lastModel;
    if (stale && (!asyncSort || !m_sorted)) {
        // On this thread: a desktop frame (its view's own order, exactly), and the very first
        // order, which the first frame cannot do without. Never beside the worker: a request it
        // has not taken is withdrawn, a sort it is running waited out and superseded.
        m_wanted = false;
        m_cv.wait(lock, [&] { return !m_busy; });
        m_resultReady = false;
        lock.unlock();
        sortForView(dir, model, m_order, chunkCountFor(m_count, kDesktopChunks));
        lock.lock();
        m_lastDir = dir;
        m_lastModel = model;
        m_sorted = true;
        ++m_sortVersion;
    } else if (stale) {
        // VR Mode: the worker sorts for this view (the latest asked for wins) and this frame
        // draws the last order landed. A view the worker is already on is not asked for again.
        const bool asked = m_wanted || m_busy;
        if (!asked || turnedFrom(m_askDir) || model != m_askModel) {
            m_askDir = dir;
            m_askModel = model;
            m_wanted = true;
            if (!m_worker.joinable()) {
                m_worker = std::thread([this] { workerLoop(); });
            }
            m_cv.notify_all();
        }
    }
    lock.unlock();
    if (m_uploaded[frameIndex] != m_sortVersion) {
        std::memcpy(m_orderBuffers[frameIndex].mappedData(), m_order.data(), sizeof(uint32_t) * m_count);
        m_uploaded[frameIndex] = m_sortVersion;
    }
}

void SplatCloud::workerLoop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_cv.wait(lock, [&] { return m_quit || m_wanted; });
        if (m_quit) {
            return;
        }
        const glm::vec3 dir = m_askDir;
        const glm::mat4 model = m_askModel;
        m_wanted = false;
        m_busy = true;
        lock.unlock();
        sortForView(dir, model, m_orderWork, chunkCountFor(m_count, kWorkerChunks));
        lock.lock();
        m_orderBack.swap(m_orderWork); // (the hand-over: a result prepareFrame has not taken is superseded)
        m_resultDir = dir;
        m_resultModel = model;
        m_resultReady = true;
        m_busy = false;
        m_cv.notify_all(); // a frame waiting the sort out (prepareFrame's own-thread path)
    }
}

void SplatCloud::record(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex, const Camera& camera,
                        const glm::mat4& model) const {
    if (m_count == 0) {
        return;
    }
    SplatPushConstants push{};
    push.model = model;
    const glm::mat4& proj = camera.projection();
    const float      w = camera.viewportWidth();
    const float      h = camera.viewportHeight();
    const bool       ortho = proj[3][3] > 0.5f;
    // The focal lengths in pixels off the projection: for a perspective P[0][0] = 1 / (aspect ·
    // tan(fov/2)) and P[1][1] its negated twin (Vulkan's y), for an orthographic 1 / half-extent.
    push.focal = glm::vec4(std::abs(proj[0][0]) * w * 0.5f, std::abs(proj[1][1]) * h * 0.5f, w, h);
    // POSESTUDIO_SPLAT_SH_DEGREE=<0..3> caps the harmonics evaluated (a diagnostic: the color
    // with its view dependence off).
    static const int kDegreeCap = std::getenv("POSESTUDIO_SPLAT_SH_DEGREE") ? std::atoi(std::getenv("POSESTUDIO_SPLAT_SH_DEGREE")) : 3;
    // POSESTUDIO_SPLAT_TINY=1: every quad a pixel — the vertex stage's cost alone, the fill's
    // being the difference (a diagnostic for the perf readouts).
    static const bool kTiny = std::getenv("POSESTUDIO_SPLAT_TINY") != nullptr;
    push.params = glm::vec4(static_cast<float>(std::min(m_shDegree, kDegreeCap)), static_cast<float>(m_shStride),
                            kTiny ? 1.0f : 0.0f, ortho ? 1.0f : 0.0f);
    // The opacity compensation of the antialiased (mip-splatting) formulation — for EVERY scene
    // since 2026-11-05, not only one trained with it: without it a splat smaller than a pixel
    // keeps its whole trained opacity through the 0.3 px dilation, and a capture seen from a
    // distance — in a headset above all, at the photo layer's reduced scale — breaks into dark
    // speckles where the thin shading splats in front of the surface collapse into single
    // pixels; with it the capture stays smooth at any distance, and close up, where the splats
    // are pixels wide and the compensation tends to one, the picture is the same (1.4% of pixels
    // differ by over 8 of 255 on the real capture, the leggings' mottling among them).
    // POSESTUDIO_SPLAT_NO_AA=1: the compensation off (the A/B).
    static const bool kNoAa = std::getenv("POSESTUDIO_SPLAT_NO_AA") != nullptr;
    push.camPos = glm::vec4(glm::vec3(glm::inverse(model) * glm::vec4(camera.position(), 1.0f)), kNoAa ? 0.0f : 1.0f);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &m_sets[frameIndex], 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 4, static_cast<uint32_t>(m_count), 0, 0); // a strip of four, once a splat
}

} // namespace pose
