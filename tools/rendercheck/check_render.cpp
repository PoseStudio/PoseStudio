/**
 * @file check_render.cpp
 * @brief The render engine's guard (CTest `render-engine-check`): loads the engine library the
 *        way the application does and renders a small scene through the engine interface
 *        (src/render/renderengineapi.h), checking what comes back.
 *
 * It tests the BOUNDARY — the scene description, the conventions the interface states (v down,
 * row 0 = top, the camera's -Z, the sun's "to the light", the flat backdrop), start / status /
 * copyImage / cancel — with a scene made here: three spheres on the shadow-catching floor (a
 * checkered one from an in-memory texture, a metal one, a glass one) under a sun and a uniform
 * environment. No figure, no Vulkan, no window.
 *
 *   PoseStudioRenderCheck                      the checks, on the CPU (exit code = failures)
 *   PoseStudioRenderCheck --list               the engine's devices
 *   PoseStudioRenderCheck --device <text>      render on the first device whose name or id contains <text>
 *   PoseStudioRenderCheck --out <file.png>     save the image (with --size WxH, --samples N, --denoise)
 *
 * WITHOUT AN ENGINE (a build made without Cycles) it prints that and exits 0: the application is
 * complete without one, and so is its test suite.
 */

#include "renderengineapi.h"
#include "renderenginelibrary.h"

#include <QCoreApplication>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace pose::renderapi;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, const QString& detail = QString()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what, detail.isEmpty() ? "" : "  — ",
                detail.toUtf8().constData());
}

struct Vec3 {
    float x, y, z;
};
Vec3  operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3  cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3  normalized(Vec3 a) {
    const float l = std::sqrt(dot(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

/// A UV sphere with the application's conventions: v down from the top, tangent along +u.
struct SphereData {
    std::vector<float>    positions, normals, uvs, tangents;
    std::vector<uint32_t> indices;
};

SphereData makeSphere(Vec3 centre, float radius, int segments = 64, int rings = 32) {
    SphereData s;
    const float pi = 3.14159265358979f;
    for (int r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / rings; // 0 = the top
        const float theta = v * pi;
        for (int c = 0; c <= segments; ++c) {
            const float u = static_cast<float>(c) / segments;
            const float phi = u * 2.0f * pi;
            const Vec3  n{std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi)};
            s.positions.insert(s.positions.end(), {centre.x + radius * n.x, centre.y + radius * n.y, centre.z + radius * n.z});
            s.normals.insert(s.normals.end(), {n.x, n.y, n.z});
            s.uvs.insert(s.uvs.end(), {u, v});
            // dP/du, and the handedness that makes sign · (N × T) point along +v (down the image).
            s.tangents.insert(s.tangents.end(), {std::cos(phi), 0.0f, -std::sin(phi), -1.0f});
        }
    }
    for (int r = 0; r < rings; ++r) {
        for (int c = 0; c < segments; ++c) {
            const uint32_t a = static_cast<uint32_t>(r * (segments + 1) + c);
            const uint32_t b = a + static_cast<uint32_t>(segments + 1);
            // (Deliberately wound INWARD: the engine must wind triangles by their normals.)
            s.indices.insert(s.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }
    return s;
}

Mesh meshOf(const SphereData& s, const char* name, int material) {
    Mesh m;
    m.name = name;
    m.positions = s.positions.data();
    m.normals = s.normals.data();
    m.uvs = s.uvs.data();
    m.tangents = s.tangents.data();
    m.vertexCount = static_cast<uint32_t>(s.positions.size() / 3);
    m.indices = s.indices.data();
    m.indexCount = static_cast<uint32_t>(s.indices.size());
    m.material = material;
    return m;
}

/// The test scene and everything its pointers point at.
struct TestScene {
    SphereData                 left = makeSphere({-1.2f, 0.5f, 0.0f}, 0.5f);
    SphereData                 middle = makeSphere({0.0f, 0.5f, 0.0f}, 0.5f);
    SphereData                 right = makeSphere({1.2f, 0.5f, 0.0f}, 0.5f);
    std::vector<unsigned char> checker;
    std::vector<Image>         images;
    std::vector<Material>      materials;
    std::vector<Mesh>          meshes;
    Light                      sun; // the key light: a distant light
    Scene                      scene;
    Vec3                       eye{0.0f, 1.2f, 4.5f};
    Vec3                       target{0.0f, 0.45f, 0.0f};
    float                      fovY = 0.61f; // 35 degrees

    TestScene() {
        // A 64×64 sRGB checker whose TOP HALF is red/white and bottom half blue/white: a v flip
        // anywhere between here and the pixels shows as the wrong color on the sphere's top.
        const int size = 64;
        checker.resize(static_cast<std::size_t>(size) * size * 4);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const bool     white = ((x / 8) + (y / 8)) % 2 == 0;
                unsigned char* p = checker.data() + (static_cast<std::size_t>(y) * size + x) * 4;
                const bool     top = y < size / 2;
                p[0] = white ? 235 : (top ? 200 : 20);
                p[1] = white ? 235 : 20;
                p[2] = white ? 235 : (top ? 20 : 200);
                p[3] = 255;
            }
        }
        Image image;
        image.id = 1;
        image.pixels = checker.data();
        image.width = size;
        image.height = size;
        image.channels = 4;
        image.format = PixelFormat::UInt8;
        image.colorSpace = ColorSpace::SRgb;
        image.name = "checker";
        images.push_back(image);

        Material textured;
        textured.name = "checkered";
        textured.baseColor[0] = textured.baseColor[1] = textured.baseColor[2] = 1.0f;
        textured.roughness = 0.6f;
        textured.maps[static_cast<int>(MapSlot::BaseColor)].image = 0;
        Material metal;
        metal.name = "metal";
        metal.baseColor[0] = 1.0f;
        metal.baseColor[1] = 0.77f;
        metal.baseColor[2] = 0.34f;
        metal.metallic = 1.0f;
        metal.roughness = 0.25f;
        Material glass;
        glass.name = "glass";
        glass.baseColor[0] = glass.baseColor[1] = glass.baseColor[2] = 1.0f;
        glass.transmission = 1.0f;
        glass.transmissionColor[0] = 0.75f;
        glass.transmissionColor[1] = 0.95f;
        glass.transmissionColor[2] = 0.8f;
        glass.roughness = 0.02f;
        glass.ior = 1.5f;
        materials = {textured, metal, glass};

        meshes = {meshOf(left, "left", 0), meshOf(middle, "middle", 1), meshOf(right, "right", 2)};

        const Vec3 toSun = normalized({0.45f, 0.8f, 0.55f});
        sun.type = LightType::Sun;
        sun.direction[0] = -toSun.x; // (a light's direction is the way it SHINES)
        sun.direction[1] = -toSun.y;
        sun.direction[2] = -toSun.z;
        sun.color[0] = 1.0f;
        sun.color[1] = 0.96f;
        sun.color[2] = 0.9f;
        sun.strength = 1.25f;
        sun.angle = 0.03f;

        scene.images = images.data();
        scene.imageCount = static_cast<uint32_t>(images.size());
        scene.materials = materials.data();
        scene.materialCount = static_cast<uint32_t>(materials.size());
        scene.meshes = meshes.data();
        scene.meshCount = static_cast<uint32_t>(meshes.size());
        scene.lights = &sun;
        scene.lightCount = 1;
        scene.floorShadows = 1;
        scene.floorHeight = 0.0f;

        scene.environment.color[0] = scene.environment.color[1] = scene.environment.color[2] = 1.0f;
        scene.environment.strength = 0.35f;
        scene.environment.backdrop = 0;
        scene.environment.backdropColor[0] = 0.10f;
        scene.environment.backdropColor[1] = 0.20f;
        scene.environment.backdropColor[2] = 0.30f;

        lookFrom(eye);
    }

    /// Puts the camera at @p from, looking at the target.
    void lookFrom(Vec3 from) {
        eye = from;
        // Camera -> world, column-major: right, up, back (the camera looks down -Z), eye.
        const Vec3 back = normalized(eye - target);
        const Vec3 rightAxis = normalized(cross({0.0f, 1.0f, 0.0f}, back));
        const Vec3 up = cross(back, rightAxis);
        const float m[16] = {rightAxis.x, rightAxis.y, rightAxis.z, 0.0f, up.x, up.y, up.z, 0.0f,
                             back.x,      back.y,      back.z,      0.0f, eye.x, eye.y, eye.z, 1.0f};
        std::copy(m, m + 16, scene.camera.cameraToWorld);
        scene.camera.fovY = fovY;
    }

    /// The pixel a world point projects to (x right, y down from the top-left).
    void project(Vec3 p, int width, int height, int& px, int& py) const {
        const Vec3  back = normalized(eye - target);
        const Vec3  rightAxis = normalized(cross({0.0f, 1.0f, 0.0f}, back));
        const Vec3  up = cross(back, rightAxis);
        const Vec3  d = p - eye;
        const float depth = -dot(d, back);
        const float t = std::tan(0.5f * fovY);
        const float ndcX = dot(d, rightAxis) / (depth * t * (static_cast<float>(width) / height));
        const float ndcY = dot(d, up) / (depth * t);
        px = static_cast<int>((ndcX * 0.5f + 0.5f) * width);
        py = static_cast<int>((0.5f - ndcY * 0.5f) * height);
    }
};

struct Pixel {
    float r, g, b, a;
};

/// The mean of a (2·radius+1)² block of the image around a pixel.
Pixel meanAt(const std::vector<float>& rgba, int width, int height, int px, int py, int radius = 2) {
    Pixel sum{0, 0, 0, 0};
    int   count = 0;
    for (int y = py - radius; y <= py + radius; ++y) {
        for (int x = px - radius; x <= px + radius; ++x) {
            if (x < 0 || y < 0 || x >= width || y >= height) {
                continue;
            }
            const float* p = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            sum.r += p[0];
            sum.g += p[1];
            sum.b += p[2];
            sum.a += p[3];
            ++count;
        }
    }
    const float inv = count > 0 ? 1.0f / count : 0.0f;
    return {sum.r * inv, sum.g * inv, sum.b * inv, sum.a * inv};
}

/// Runs one render to its end (or @p cancelAfterMs), printing the engine's status as it changes.
State render(Engine* engine, const Scene& scene, const Settings& settings, std::vector<float>& rgba, bool verbose,
             int cancelAfterMs = -1, Status* last = nullptr) {
    rgba.assign(static_cast<std::size_t>(settings.width) * settings.height * 4, 0.0f);
    if (!engine->start(scene, settings)) {
        Status status;
        engine->status(status);
        if (last != nullptr) {
            *last = status;
        }
        return State::Failed;
    }
    Status  status;
    QString shown;
    int     waited = 0;
    for (;;) {
        engine->status(status);
        const QString text = QString::fromUtf8(status.text);
        if (verbose && text != shown) {
            shown = text;
            std::printf("    %5.1fs  %3.0f%%  %s\n", status.seconds, status.progress * 100.0f, status.text);
            std::fflush(stdout);
        }
        if (status.state == State::Finished || status.state == State::Cancelled || status.state == State::Failed) {
            break;
        }
        if (cancelAfterMs >= 0 && waited >= cancelAfterMs) {
            engine->cancel();
            cancelAfterMs = -1;
            continue;
        }
        QThread::msleep(20);
        waited += 20;
    }
    engine->copyImage(rgba.data(), settings.width, settings.height);
    if (last != nullptr) {
        *last = status;
    }
    return status.state;
}

/// Waits until an INTERACTIVE render has taken in scene @p sceneVersion and update @p updates,
/// rendered all its samples and shown them (the image's version then stands still), and copies
/// the image out at the size the engine reports. False on a failure or after @p timeoutMs.
bool waitInteractive(Engine* engine, uint64_t sceneVersion, uint64_t updates, std::vector<float>& rgba, Status& status,
                     int timeoutMs = 120000) {
    uint64_t lastVersion = 0;
    int      still = 0;
    for (int waited = 0; waited < timeoutMs; waited += 20) {
        engine->status(status);
        if (status.state == State::Failed || status.state == State::Cancelled || status.state == State::Finished) {
            return false;
        }
        const bool in = status.sceneVersion >= sceneVersion && status.updatesShown >= updates;
        const bool done = in && status.imageWidth > 0 && status.samples > 0 && status.sample >= status.samples;
        still = (done && status.imageVersion == lastVersion) ? still + 1 : 0;
        lastVersion = status.imageVersion;
        if (still >= 15) { // (300 ms with nothing new: the last update has been shown)
            rgba.assign(static_cast<std::size_t>(status.imageWidth) * status.imageHeight * 4, 0.0f);
            return engine->copyImage(rgba.data(), status.imageWidth, status.imageHeight);
        }
        QThread::msleep(20);
    }
    return false;
}

/// One flat quad in the plane y = @p height, @p half metres to each side, facing up.
struct QuadData {
    std::vector<float>    positions;
    std::vector<float>    normals;
    std::vector<float>    uvs;
    std::vector<uint32_t> indices;
};
QuadData makeQuad(float height, float half) {
    QuadData q;
    q.positions = {-half, height, -half, -half, height, half, half, height, half, half, height, -half};
    q.normals = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
    q.uvs = {0, 0, 0, 1, 1, 1, 1, 0};
    q.indices = {0, 1, 2, 0, 2, 3};
    return q;
}

unsigned char toDisplay(float linear) {
    // The viewport's own ACES fit and the sRGB curve: what the application shows.
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float x = std::max(linear, 0.0f);
    x = std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
    x = x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
    return static_cast<unsigned char>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f));
}

bool savePng(const QString& path, const std::vector<float>& rgba, int width, int height) {
    QImage image(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        unsigned char* row = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const float* p = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            row[4 * x] = toDisplay(p[0]);
            row[4 * x + 1] = toDisplay(p[1]);
            row[4 * x + 2] = toDisplay(p[2]);
            row[4 * x + 3] = 255;
        }
    }
    return image.save(path);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication  app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("PoseStudio"));
    const QStringList args = app.arguments();
    const auto        valueOf = [&](const QString& flag) {
        const int i = args.indexOf(flag);
        return (i >= 0 && i + 1 < args.size()) ? args[i + 1] : QString();
    };

    const QString directory = args.contains(QStringLiteral("--engine")) ? valueOf(QStringLiteral("--engine"))
                                                                         : pose::RenderEngineLibrary::cyclesDirectory();
    if (!pose::RenderEngineLibrary::present(directory)) {
        std::printf("render-engine-check: no render engine in %s — skipped (a build without Cycles).\n",
                    directory.toUtf8().constData());
        return 0;
    }
    QString error;
    const std::unique_ptr<pose::RenderEngineLibrary> library = pose::RenderEngineLibrary::load(directory, &error);
    if (library == nullptr) {
        std::printf("render-engine-check: FAILED to load the engine in %s: %s\n", directory.toUtf8().constData(),
                    error.toUtf8().constData());
        return 1;
    }
    Engine* engine = library->engine();
    std::printf("engine: %s %s  (%s)\n", engine->name(), engine->version(), directory.toUtf8().constData());

    std::vector<DeviceInfo> devices(static_cast<std::size_t>(std::max(engine->deviceCount(), 0)));
    for (std::size_t i = 0; i < devices.size(); ++i) {
        engine->device(static_cast<int32_t>(i), devices[i]);
        std::printf("  device %zu: [%d] %s   id %s\n", i, static_cast<int>(devices[i].type), devices[i].name, devices[i].id);
    }
    if (args.contains(QStringLiteral("--list"))) {
        return 0;
    }

    TestScene test;
    Settings  settings;
    settings.width = 320;
    settings.height = 180;
    settings.samples = 48;
    settings.denoise = 0;
    settings.exposure = 1.0f;
    QByteArray deviceId;
    const QString wantDevice = valueOf(QStringLiteral("--device"));
    if (!wantDevice.isEmpty()) {
        for (const DeviceInfo& info : devices) {
            if (QString::fromUtf8(info.name).contains(wantDevice, Qt::CaseInsensitive) ||
                QString::fromUtf8(info.id).contains(wantDevice, Qt::CaseInsensitive)) {
                deviceId = info.id;
                std::printf("rendering on: %s\n", info.name);
                break;
            }
        }
        if (deviceId.isEmpty()) {
            std::printf("no device matches \"%s\"\n", wantDevice.toUtf8().constData());
            return 1;
        }
        settings.device = deviceId.constData();
    }

    // --out: one picture, at the size and quality asked for.
    const QString out = valueOf(QStringLiteral("--out"));
    if (!out.isEmpty()) {
        const QStringList size = valueOf(QStringLiteral("--size")).split(QLatin1Char('x'));
        if (size.size() == 2) {
            settings.width = size[0].toInt();
            settings.height = size[1].toInt();
        } else {
            settings.width = 960;
            settings.height = 540;
        }
        if (!valueOf(QStringLiteral("--samples")).isEmpty()) {
            settings.samples = valueOf(QStringLiteral("--samples")).toInt();
        } else {
            settings.samples = 256;
        }
        settings.denoise = args.contains(QStringLiteral("--denoise")) ? 1 : 0;
        std::vector<float> rgba;
        Status             last;
        const State        state = render(engine, test.scene, settings, rgba, true, -1, &last);
        std::printf("state %d after %.1f s (%d samples)%s%s\n", static_cast<int>(state), last.seconds, last.sample,
                    last.error[0] ? ": " : "", last.error);
        if (state != State::Finished) {
            return 1;
        }
        return savePng(out, rgba, settings.width, settings.height) ? 0 : 1;
    }

    // --- The checks ---
    std::printf("render (%dx%d, %d samples):\n", settings.width, settings.height, settings.samples);
    std::vector<float> rgba;
    Status             last;
    const State        state = render(engine, test.scene, settings, rgba, false, -1, &last);
    check(state == State::Finished, "the render finishes", QString::fromUtf8(last.error));
    check(last.imageVersion > 0 && last.progress >= 0.999f, "its status reports the image and full progress");

    const int w = settings.width;
    const int h = settings.height;
    bool      finite = true;
    for (const float v : rgba) {
        finite = finite && std::isfinite(v);
    }
    check(finite, "every pixel is finite");

    // The flat backdrop, exactly (camera rays only): the top-left corner, far from everything.
    const Pixel corner = meanAt(rgba, w, h, 6, 6);
    check(std::abs(corner.r - 0.10f) < 0.01f && std::abs(corner.g - 0.20f) < 0.01f && std::abs(corner.b - 0.30f) < 0.01f,
          "the camera sees the flat backdrop color",
          QStringLiteral("%1 %2 %3").arg(corner.r, 0, 'f', 3).arg(corner.g, 0, 'f', 3).arg(corner.b, 0, 'f', 3));

    // The sun is up, to the right and toward the camera: a sphere's upper right is its lit side
    // (read on the matte, checkered sphere — wide blocks, so both hold the checker's mix).
    int px = 0, py = 0;
    test.project({-1.2f + 0.26f, 0.5f + 0.30f, 0.30f}, w, h, px, py);
    const Pixel lit = meanAt(rgba, w, h, px, py, 5);
    test.project({-1.2f - 0.30f, 0.5f - 0.26f, 0.30f}, w, h, px, py);
    const Pixel shade = meanAt(rgba, w, h, px, py, 5);
    const float litLuma = lit.r + lit.g + lit.b;
    const float shadeLuma = shade.r + shade.g + shade.b;
    check(litLuma > shadeLuma * 1.5f, "the sun lights the side that faces it",
          QStringLiteral("lit %1  shaded %2").arg(litLuma, 0, 'f', 3).arg(shadeLuma, 0, 'f', 3));
    test.project({0.0f + 0.2f, 0.5f + 0.2f, 0.42f}, w, h, px, py);
    const Pixel gold = meanAt(rgba, w, h, px, py);
    check(gold.r > gold.b * 1.5f, "the metal sphere is gold (its base color tints the reflection)",
          QStringLiteral("r %1  b %2").arg(gold.r, 0, 'f', 3).arg(gold.b, 0, 'f', 3));

    // The checkered sphere: red squares on its upper half, blue on its lower (v runs down).
    Pixel upper{0, 0, 0, 0};
    Pixel lower{0, 0, 0, 0};
    test.project({-1.2f, 0.5f + 0.27f, 0.42f}, w, h, px, py);
    upper = meanAt(rgba, w, h, px, py, 6);
    test.project({-1.2f, 0.5f - 0.27f, 0.42f}, w, h, px, py);
    lower = meanAt(rgba, w, h, px, py, 6);
    check(upper.r > upper.b * 1.15f && lower.b > lower.r * 1.15f, "the texture is the right way up (red above, blue below)",
          QStringLiteral("upper r %1 b %2, lower r %3 b %4")
              .arg(upper.r, 0, 'f', 3)
              .arg(upper.b, 0, 'f', 3)
              .arg(lower.r, 0, 'f', 3)
              .arg(lower.b, 0, 'f', 3));

    // The floor: invisible where it is lit, darker where the middle sphere's shadow falls. The
    // shadow lies opposite the sun — behind the sphere and to its left — so it is read beside the
    // sphere, a third of the way out from the shadow's middle toward the camera's side.
    const Vec3  toSun = normalized({0.45f, 0.8f, 0.55f});
    const float reach = 0.5f / toSun.y; // the sphere centre's shadow, on the floor
    const Vec3  across = normalized({toSun.z, 0.0f, -toSun.x}); // along the floor, square to the light
    const Vec3  side = across.z > 0.0f ? across : Vec3{-across.x, 0.0f, -across.z};
    const auto  shadowPoint = [&](float sphereX) {
        return Vec3{sphereX - toSun.x * reach + 0.35f * side.x, 0.0f, -toSun.z * reach + 0.35f * side.z};
    };
    Vec3 at = shadowPoint(0.0f);
    test.project(at, w, h, px, py);
    const Pixel shadow = meanAt(rgba, w, h, px, py);
    test.project({0.0f, 0.0f, 1.6f}, w, h, px, py);
    const Pixel open = meanAt(rgba, w, h, px, py);
    check(std::abs(open.g - 0.20f) < 0.03f, "the lit floor shows the backdrop through",
          QStringLiteral("%1 %2 %3").arg(open.r, 0, 'f', 3).arg(open.g, 0, 'f', 3).arg(open.b, 0, 'f', 3));
    check(shadow.g < open.g * 0.85f, "the floor catches the sphere's shadow",
          QStringLiteral("shadow %1  open %2").arg(shadow.g, 0, 'f', 3).arg(open.g, 0, 'f', 3));

    // The glass sphere lets the sun through to the floor: its shadow is lighter than the metal's.
    at = shadowPoint(1.2f);
    test.project(at, w, h, px, py);
    const Pixel glassShadow = meanAt(rgba, w, h, px, py);
    check(glassShadow.g > shadow.g * 1.1f && glassShadow.g < open.g * 0.97f, "glass casts a lighter shadow than metal — and casts one",
          QStringLiteral("glass %1  metal %2  open %3").arg(glassShadow.g, 0, 'f', 3).arg(shadow.g, 0, 'f', 3).arg(open.g, 0, 'f', 3));

    // Lit by the SUN ALONE the shadow is all but black: the sun is one of the lights the floor's
    // shadows are measured against. (A light Cycles does not count as the floor's own can only
    // brighten it — the sun's shadow then never shows, and with nothing else lit there is none.)
    {
        TestScene sunlit;
        sunlit.scene.environment.strength = 0.0f;
        Settings quick = settings;
        quick.samples = 32;
        std::vector<float> image;
        const State        s = render(engine, sunlit.scene, quick, image, false);
        at = shadowPoint(0.0f);
        sunlit.project(at, w, h, px, py);
        const Pixel dark = meanAt(image, w, h, px, py);
        sunlit.project({0.0f, 0.0f, 1.6f}, w, h, px, py);
        const Pixel lit = meanAt(image, w, h, px, py);
        check(s == State::Finished && std::abs(lit.g - 0.20f) < 0.03f && dark.g < lit.g * 0.25f,
              "lit by the sun alone, its shadow on the floor is all but black",
              QStringLiteral("shadow %1  open %2").arg(dark.g, 0, 'f', 3).arg(lit.g, 0, 'f', 3));
    }

    // A transparent background: the backdrop gone, alpha = coverage.
    {
        Settings alpha = settings;
        alpha.transparentBackground = 1;
        alpha.samples = 16;
        std::vector<float> image;
        const State        s = render(engine, test.scene, alpha, image, false);
        const Pixel        sky = meanAt(image, w, h, 6, 6);
        test.project({0.0f, 0.5f, 0.5f}, w, h, px, py);
        const Pixel solid = meanAt(image, w, h, px, py);
        check(s == State::Finished && sky.a < 0.01f && solid.a > 0.99f, "a transparent background leaves alpha = coverage",
              QStringLiteral("sky %1  sphere %2").arg(sky.a, 0, 'f', 3).arg(solid.a, 0, 'f', 3));
        // ... and the floor leaves no trace in it: not its surface, not the shadows on it.
        at = shadowPoint(0.0f);
        test.project(at, w, h, px, py);
        const Pixel noShadow = meanAt(image, w, h, px, py);
        check(s == State::Finished && noShadow.a < 0.02f, "… and no trace of the floor's shadows",
              QStringLiteral("alpha in the shadow %1").arg(noShadow.a, 0, 'f', 3));

        // WITH shadowAlpha the shadows are kept, as coverage of black: laid over a plate the
        // picture darkens it where the shadow falls, and nowhere else.
        alpha.shadowAlpha = 1;
        std::vector<float> kept;
        const State        s2 = render(engine, test.scene, alpha, kept, false);
        const Pixel        inShadow = meanAt(kept, w, h, px, py);
        test.project({0.0f, 0.0f, 1.6f}, w, h, px, py);
        const Pixel openFloor = meanAt(kept, w, h, px, py);
        test.project({0.0f, 0.5f, 0.5f}, w, h, px, py);
        const Pixel solidKept = meanAt(kept, w, h, px, py);
        const Pixel skyKept = meanAt(kept, w, h, 6, 6);
        check(s2 == State::Finished && inShadow.a > 0.10f && inShadow.g < 0.02f && openFloor.a < 0.05f && skyKept.a < 0.01f &&
                  solidKept.a > 0.99f,
              "with shadowAlpha the floor's shadow is coverage of black, for a plate under the picture",
              QStringLiteral("shadow a %1 g %2  open floor a %3  sky a %4  sphere a %5")
                  .arg(inShadow.a, 0, 'f', 3)
                  .arg(inShadow.g, 0, 'f', 3)
                  .arg(openFloor.a, 0, 'f', 3)
                  .arg(skyKept.a, 0, 'f', 3)
                  .arg(solidKept.a, 0, 'f', 3));
    }

    // A PANORAMA, laid out the application's way (u = atan2(x, -z) / 2π from the left edge, row 0
    // the zenith): -Z is red, +X green, +Z blue, -X yellow, the sky white. The camera looks
    // down -Z, so it sees RED behind the spheres — and, with the environment turned a quarter
    // (the viewport then samples along rotateY(d, 90°), which takes -Z to -X), YELLOW.
    {
        const int          pw = 64;
        const int          ph = 32;
        std::vector<float> panorama(static_cast<std::size_t>(pw) * ph * 3);
        for (int y = 0; y < ph; ++y) {
            for (int x = 0; x < pw; ++x) {
                const float u = (x + 0.5f) / pw;
                float       c[3] = {1.0f, 1.0f, 1.0f}; // the sky
                if (y >= ph / 4) {
                    const int sector = static_cast<int>(std::floor(u * 4.0f + 0.5f)) % 4; // 0: -Z, 1: +X, 2: +Z, 3: -X
                    const float colors[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}};
                    std::copy(colors[sector], colors[sector] + 3, c);
                }
                std::copy(c, c + 3, panorama.data() + (static_cast<std::size_t>(y) * pw + x) * 3);
            }
        }
        TestScene lit;
        Image     image;
        image.id = 2;
        image.pixels = panorama.data();
        image.width = pw;
        image.height = ph;
        image.channels = 3;
        image.format = PixelFormat::Float32;
        image.colorSpace = ColorSpace::Linear;
        image.name = "panorama";
        lit.images.push_back(image);
        lit.scene.images = lit.images.data();
        lit.scene.imageCount = static_cast<uint32_t>(lit.images.size());
        lit.scene.environment.image = 1;
        lit.scene.environment.strength = 1.0f;
        lit.scene.environment.backdrop = 1;
        lit.scene.lightCount = 0;
        Settings quick = settings;
        quick.samples = 16;
        std::vector<float> image0;
        std::vector<float> image90;
        const State        s0 = render(engine, lit.scene, quick, image0, false);
        lit.scene.environment.rotation = 1.5707963f;
        const State s90 = render(engine, lit.scene, quick, image90, false);
        // (Just above the horizon line at the image's centre top: the camera looks slightly down.)
        const Pixel ahead = meanAt(image0, w, h, w / 2, 4);
        const Pixel turned = meanAt(image90, w, h, w / 2, 4);
        check(s0 == State::Finished && ahead.r > 0.8f && ahead.g < 0.2f && ahead.b < 0.2f,
              "the panorama's -Z is behind a scene seen from +Z",
              QStringLiteral("%1 %2 %3").arg(ahead.r, 0, 'f', 2).arg(ahead.g, 0, 'f', 2).arg(ahead.b, 0, 'f', 2));
        check(s90 == State::Finished && turned.r > 0.8f && turned.g > 0.8f && turned.b < 0.2f,
              "the environment's rotation turns it the viewport's way",
              QStringLiteral("%1 %2 %3").arg(turned.r, 0, 'f', 2).arg(turned.g, 0, 'f', 2).arg(turned.b, 0, 'f', 2));
        // The white sky is overhead: the top of a sphere is lit white-ish, its underside is not.
        lit.project({0.0f, 0.99f, 0.05f}, w, h, px, py);
        const Pixel top = meanAt(image0, w, h, px, py + 3);
        check(top.r > 0.25f && top.g > 0.25f && top.b > 0.25f, "the panorama's top row is the zenith",
              QStringLiteral("%1 %2 %3").arg(top.r, 0, 'f', 2).arg(top.g, 0, 'f', 2).arg(top.b, 0, 'f', 2));

        // What the application sends by default: a panorama that LIGHTS (sampled as a light),
        // a flat backdrop behind, the sun — and the floor must still catch the sun's shadow.
        lit.scene.environment.rotation = 0.0f;
        lit.scene.environment.strength = 0.3f;
        lit.scene.environment.backdrop = 0;
        lit.scene.environment.backdropColor[0] = 0.10f;
        lit.scene.environment.backdropColor[1] = 0.20f;
        lit.scene.environment.backdropColor[2] = 0.30f;
        lit.scene.lightCount = 1;
        quick.samples = 48;
        std::vector<float> under;
        const State        su = render(engine, lit.scene, quick, under, false);
        Vec3               where = shadowPoint(0.0f);
        lit.project(where, w, h, px, py);
        const Pixel shaded = meanAt(under, w, h, px, py);
        lit.project({0.0f, 0.0f, 1.6f}, w, h, px, py);
        const Pixel clear = meanAt(under, w, h, px, py);
        check(su == State::Finished && std::abs(clear.g - 0.20f) < 0.03f && shaded.g < clear.g * 0.85f,
              "under a panorama's light the floor still shows the backdrop and catches the shadow",
              QStringLiteral("shadow %1  open %2").arg(shaded.g, 0, 'f', 3).arg(clear.g, 0, 'f', 3));
    }

    // THE LIGHT KINDS, each at a strength of 1 a metre above a white matte table: a strength is
    // the irradiance a light puts on a surface facing it — a sun's anywhere, the others' a metre
    // away on their axis (renderapi::Light) — so all four light the spot under them alike:
    // radiance = albedo × 1 / π. Twice as far a point light gives a quarter; outside its cone a
    // spot gives nothing; behind its face an area light gives nothing.
    {
        const float    kAlbedo = 0.8f;
        const QuadData table = makeQuad(0.5f, 3.0f);
        Material       matte;
        matte.name = "matte";
        matte.baseColor[0] = matte.baseColor[1] = matte.baseColor[2] = kAlbedo;
        matte.specular = 0.0f;
        matte.roughness = 1.0f;
        Mesh mesh;
        mesh.name = "table";
        mesh.positions = table.positions.data();
        mesh.normals = table.normals.data();
        mesh.uvs = table.uvs.data();
        mesh.vertexCount = 4;
        mesh.indices = table.indices.data();
        mesh.indexCount = 6;
        mesh.material = 0;

        TestScene bench; // (its camera; everything else is replaced)
        bench.target = {0.0f, 0.5f, 0.0f};
        bench.lookFrom({0.0f, 3.0f, 2.5f});
        bench.scene.materials = &matte;
        bench.scene.materialCount = 1;
        bench.scene.meshes = &mesh;
        bench.scene.meshCount = 1;
        bench.scene.images = nullptr;
        bench.scene.imageCount = 0;
        bench.scene.floorShadows = 0;
        bench.scene.environment.strength = 0.0f;
        bench.scene.environment.backdropColor[0] = 0.0f;
        bench.scene.environment.backdropColor[1] = 0.0f;
        bench.scene.environment.backdropColor[2] = 0.0f;

        Light light;
        light.position[0] = 0.0f;
        light.position[1] = 1.5f; // a metre above the table
        light.position[2] = 0.0f;
        light.direction[0] = 0.0f;
        light.direction[1] = -1.0f;
        light.direction[2] = 0.0f;
        light.strength = 1.0f;
        light.radius = 0.0f;
        light.angle = 0.6f;  // a spot's cone: 34 degrees across — half a metre wide on the table
        light.blend = 0.1f;
        light.sizeU = 0.1f;
        light.sizeV = 0.1f;
        bench.scene.lights = &light;
        bench.scene.lightCount = 1;

        Settings quick = settings;
        quick.samples = 64;
        const float expected = kAlbedo / 3.14159265f;
        int         cx = 0;
        int         cy = 0;
        bench.project({0.0f, 0.5f, 0.0f}, w, h, cx, cy);
        const auto under = [&](const std::vector<float>& image) { return meanAt(image, w, h, cx, cy, 1).g; };
        const auto within = [&](float value, float want) { return std::abs(value - want) < 0.06f * want; };

        std::vector<float> image;
        const char* const  names[4] = {"a sun", "a point light", "a spot light", "an area light"};
        const LightType    kinds[4] = {LightType::Sun, LightType::Point, LightType::Spot, LightType::Area};
        float              got[4] = {0, 0, 0, 0};
        bool               all = true;
        for (int k = 0; k < 4; ++k) {
            light.type = kinds[k];
            light.angle = kinds[k] == LightType::Sun ? 0.01f : 0.6f;
            all = all && render(engine, bench.scene, quick, image, false) == State::Finished;
            got[k] = under(image);
            all = all && within(got[k], expected);
        }
        check(all, "a strength of 1 lights the spot a metre under a sun, a point, a spot and an area light alike (albedo / pi)",
              QStringLiteral("sun %1  point %2  spot %3  area %4  expected %5 (%6, %7, %8, %9)")
                  .arg(got[0], 0, 'f', 4)
                  .arg(got[1], 0, 'f', 4)
                  .arg(got[2], 0, 'f', 4)
                  .arg(got[3], 0, 'f', 4)
                  .arg(expected, 0, 'f', 4)
                  .arg(QLatin1String(names[0]), QLatin1String(names[1]), QLatin1String(names[2]), QLatin1String(names[3])));

        // Twice as far: a quarter.
        light.type = LightType::Point;
        light.position[1] = 2.5f;
        render(engine, bench.scene, quick, image, false);
        const float far = under(image);
        check(within(far, expected * 0.25f), "a point light twice as far lights a quarter as much",
              QStringLiteral("%1, expected %2").arg(far, 0, 'f', 4).arg(expected * 0.25f, 0, 'f', 4));

        // A spot: nothing outside its cone (a metre to the side of a 34-degree cone's axis).
        light.type = LightType::Spot;
        light.position[1] = 1.5f;
        light.angle = 0.6f;
        render(engine, bench.scene, quick, image, false);
        int sx = 0;
        int sy = 0;
        bench.project({1.0f, 0.5f, 0.0f}, w, h, sx, sy);
        const float outside = meanAt(image, w, h, sx, sy, 1).g;
        check(under(image) > expected * 0.9f && outside < expected * 0.02f, "a spot light lights its cone and nothing outside it",
              QStringLiteral("inside %1  outside %2").arg(under(image), 0, 'f', 4).arg(outside, 0, 'f', 4));

        // An area light shines from ONE face: turned to shine up, the table under it is dark.
        light.type = LightType::Area;
        light.direction[1] = 1.0f;
        render(engine, bench.scene, quick, image, false);
        check(under(image) < expected * 0.02f, "an area light shines from one face only",
              QStringLiteral("%1 under a light that shines up").arg(under(image), 0, 'f', 4));
    }

    // AN INTERACTIVE RENDER (the viewport's): alive when its samples are done, and changed while
    // it runs — a camera, a whole new scene, a new size — on a SECOND engine, beside the first.
    {
        const std::unique_ptr<pose::RenderEngineInstance> second = library->createEngine();
        check(second != nullptr, "the library makes a second engine");
        Engine* live = second ? second->get() : nullptr;
        if (live != nullptr) {
            TestScene view;
            Settings  interactive = settings;
            interactive.interactive = 1;
            interactive.samples = 24;
            std::vector<float> first;
            Status             st;
            const bool started = live->start(view.scene, interactive) && waitInteractive(live, 1, 0, first, st);
            const Pixel cornerLive = started ? meanAt(first, w, h, 6, 6) : Pixel{0, 0, 0, 0};
            check(started && st.state == State::Rendering && st.imageWidth == w && st.imageHeight == h,
                  "an interactive render shows its samples and stays alive",
                  QStringLiteral("state %1  %2x%3  %4").arg(static_cast<int>(st.state)).arg(st.imageWidth).arg(st.imageHeight).arg(QString::fromUtf8(st.error)));
            check(started && std::abs(cornerLive.r - 0.10f) < 0.01f && std::abs(cornerLive.g - 0.20f) < 0.01f &&
                      std::abs(cornerLive.b - 0.30f) < 0.01f,
                  "its picture is the final render's (the backdrop, exactly)",
                  QStringLiteral("%1 %2 %3").arg(cornerLive.r, 0, 'f', 3).arg(cornerLive.g, 0, 'f', 3).arg(cornerLive.b, 0, 'f', 3));
            // … and the gold sphere where the final render has it.
            view.project({0.0f, 0.5f, 0.5f}, w, h, px, py);
            const Pixel goldLive = started ? meanAt(first, w, h, px, py, 3) : Pixel{0, 0, 0, 0};
            const Pixel goldFinal = meanAt(rgba, w, h, px, py, 3);
            check(started && std::abs(goldLive.r - goldFinal.r) < 0.25f * goldFinal.r + 0.02f && goldLive.r > goldLive.b * 1.5f,
                  "it renders the scene the final render does (the metal sphere)",
                  QStringLiteral("live %1 %2 %3  final %4 %5 %6")
                      .arg(goldLive.r, 0, 'f', 3)
                      .arg(goldLive.g, 0, 'f', 3)
                      .arg(goldLive.b, 0, 'f', 3)
                      .arg(goldFinal.r, 0, 'f', 3)
                      .arg(goldFinal.g, 0, 'f', 3)
                      .arg(goldFinal.b, 0, 'f', 3));

            // A CAMERA update: from the other side (-Z), where the textured sphere is on the RIGHT.
            view.lookFrom({0.0f, 1.2f, -4.5f});
            std::vector<float> moved;
            const uint64_t     afterCamera = live->update(view.scene, interactive, kUpdateCamera);
            const bool         movedOk = afterCamera == 1 && waitInteractive(live, 1, 1, moved, st);
            // Seen from behind the spheres have swapped sides: where the first picture had the
            // textured sphere (screen-left) there is now the GLASS one, and the picture as a whole
            // is another picture.
            view.project({1.2f, 0.5f, 0.0f}, w, h, px, py); // the glass sphere, from the new camera
            const Pixel glassNow = movedOk ? meanAt(moved, w, h, px, py, 3) : Pixel{0, 0, 0, 0};
            const Pixel thereBefore = started ? meanAt(first, w, h, px, py, 3) : Pixel{0, 0, 0, 0};
            double      difference = 0.0;
            if (movedOk && moved.size() == first.size()) {
                for (std::size_t i = 0; i < moved.size(); ++i) {
                    difference += std::abs(moved[i] - first[i]);
                }
                difference /= static_cast<double>(moved.size());
            }
            check(movedOk && px < w / 2 && difference > 0.01 &&
                      std::abs(glassNow.r - thereBefore.r) + std::abs(glassNow.g - thereBefore.g) + std::abs(glassNow.b - thereBefore.b) > 0.05f &&
                      st.sceneVersion == 1,
                  "a camera update moves the view without a new scene",
                  QStringLiteral("pixel %1,%2  now %3 %4 %5  before %6 %7 %8  mean difference %9  scene %10")
                      .arg(px)
                      .arg(py)
                      .arg(glassNow.r, 0, 'f', 3)
                      .arg(glassNow.g, 0, 'f', 3)
                      .arg(glassNow.b, 0, 'f', 3)
                      .arg(thereBefore.r, 0, 'f', 3)
                      .arg(thereBefore.g, 0, 'f', 3)
                      .arg(thereBefore.b, 0, 'f', 3)
                      .arg(difference, 0, 'f', 4)
                      .arg(st.sceneVersion));

            // A SCENE update: the metal turned blue, the glass sphere taken away — and the old
            // description's memory given up once the new one is in.
            view.lookFrom({0.0f, 1.2f, 4.5f});
            TestScene changed;
            changed.materials[1].baseColor[0] = 0.1f;
            changed.materials[1].baseColor[1] = 0.2f;
            changed.materials[1].baseColor[2] = 1.0f;
            changed.meshes.pop_back();
            changed.scene.meshCount = static_cast<uint32_t>(changed.meshes.size());
            std::vector<float> rebuilt;
            const uint64_t     afterScene = live->update(changed.scene, interactive, kUpdateScene);
            const bool         rebuiltOk = afterScene == 2 && waitInteractive(live, 2, 2, rebuilt, st);
            view = TestScene(); // (the first description's memory is gone: the engine must not read it)
            changed.project({0.0f, 0.5f, 0.5f}, w, h, px, py);
            const Pixel blue = rebuiltOk ? meanAt(rebuilt, w, h, px, py, 3) : Pixel{0, 0, 0, 0};
            changed.project({1.2f, 0.5f, 0.0f}, w, h, px, py);
            const Pixel gone = rebuiltOk ? meanAt(rebuilt, w, h, px, py, 2) : Pixel{0, 0, 0, 0};
            check(rebuiltOk && blue.b > blue.r * 1.5f, "a scene update rebuilds the materials (the metal is blue)",
                  QStringLiteral("%1 %2 %3").arg(blue.r, 0, 'f', 3).arg(blue.g, 0, 'f', 3).arg(blue.b, 0, 'f', 3));
            check(rebuiltOk && std::abs(gone.g - 0.20f) < 0.03f && std::abs(gone.r - 0.10f) < 0.03f,
                  "… and the geometry (the glass sphere is gone: the backdrop shows)",
                  QStringLiteral("%1 %2 %3").arg(gone.r, 0, 'f', 3).arg(gone.g, 0, 'f', 3).arg(gone.b, 0, 'f', 3));

            // A LIGHTING update: the sun and the environment taken away, another backdrop — the
            // geometry untouched. The metal sphere goes dark; the corner shows the new backdrop.
            TestScene dark;
            dark.scene.lightCount = 0;
            dark.scene.environment.strength = 0.0f;
            dark.scene.environment.backdropColor[0] = 0.30f;
            dark.scene.environment.backdropColor[1] = 0.10f;
            dark.scene.environment.backdropColor[2] = 0.20f;
            dark.scene.meshCount = 0; // (a lighting description brings no geometry)
            dark.scene.materialCount = 0;
            std::vector<float> relit;
            const uint64_t     afterLighting = live->update(dark.scene, interactive, kUpdateLighting);
            const bool         relitOk = afterLighting == 3 && waitInteractive(live, 3, 3, relit, st);
            const Pixel        cornerRelit = relitOk ? meanAt(relit, w, h, 6, 6) : Pixel{0, 0, 0, 0};
            changed.project({0.0f, 0.5f, 0.5f}, w, h, px, py);
            const Pixel unlit = relitOk ? meanAt(relit, w, h, px, py, 3) : Pixel{0, 0, 0, 0};
            check(relitOk && std::abs(cornerRelit.r - 0.30f) < 0.01f && std::abs(cornerRelit.g - 0.10f) < 0.01f && unlit.b < 0.01f,
                  "a lighting update relights the scene without rebuilding it",
                  QStringLiteral("corner %1 %2 %3  sphere %4 %5 %6  version %7")
                      .arg(cornerRelit.r, 0, 'f', 3)
                      .arg(cornerRelit.g, 0, 'f', 3)
                      .arg(cornerRelit.b, 0, 'f', 3)
                      .arg(unlit.r, 0, 'f', 3)
                      .arg(unlit.g, 0, 'f', 3)
                      .arg(unlit.b, 0, 'f', 3)
                      .arg(afterLighting));
            // … and back: the first lighting again (the scene description's own), the sphere blue.
            std::vector<float> relit2;
            TestScene          lightsBack;
            lightsBack.scene.meshCount = 0;
            lightsBack.scene.materialCount = 0;
            live->update(lightsBack.scene, interactive, kUpdateLighting);
            const bool  backOk = waitInteractive(live, 4, 4, relit2, st);
            const Pixel blueAgain = backOk ? meanAt(relit2, w, h, px, py, 3) : Pixel{0, 0, 0, 0};
            check(backOk && blueAgain.b > blueAgain.r * 1.5f && blueAgain.b > 0.05f, "… and back again (the materials were kept)",
                  QStringLiteral("%1 %2 %3").arg(blueAgain.r, 0, 'f', 3).arg(blueAgain.g, 0, 'f', 3).arg(blueAgain.b, 0, 'f', 3));

            // A new SIZE, through a camera update.
            Settings resized = interactive;
            resized.width = 200;
            resized.height = 120;
            std::vector<float> small;
            live->update(changed.scene, resized, kUpdateCamera);
            const bool  smallOk = waitInteractive(live, 4, 5, small, st);
            const Pixel cornerSmall = smallOk ? meanAt(small, 200, 120, 4, 4) : Pixel{0, 0, 0, 0};
            check(smallOk && st.imageWidth == 200 && st.imageHeight == 120 && std::abs(cornerSmall.g - 0.20f) < 0.01f,
                  "a camera update resizes the image", QStringLiteral("%1x%2").arg(st.imageWidth).arg(st.imageHeight));

            // Paused it renders nothing new; resumed it goes on; and a FINAL render runs beside it
            // on the first engine.
            live->setPaused(1);
            std::vector<float> beside;
            Settings           few = settings;
            few.samples = 8;
            check(render(engine, test.scene, few, beside, false) == State::Finished,
                  "a final render runs on the first engine beside a paused interactive one");
            live->setPaused(0);

            live->cancel();
            live->status(st);
            check(st.state == State::Cancelled, "cancel ends an interactive render",
                  QStringLiteral("state %1").arg(static_cast<int>(st.state)));
            check(live->update(changed.scene, interactive, kUpdateCamera) == 0, "… after which an update is refused");
        }
    }

    // Cancel: a render that would take minutes stops when told to, and reports it.
    {
        Settings longRender = settings;
        longRender.width = 1280;
        longRender.height = 720;
        longRender.samples = 100000;
        std::vector<float> image;
        const State        s = render(engine, test.scene, longRender, image, false, 1500);
        check(s == State::Cancelled, "cancel stops a render", QStringLiteral("state %1").arg(static_cast<int>(s)));
    }
    // … and the engine renders again afterwards.
    {
        Settings again = settings;
        again.samples = 8;
        std::vector<float> image;
        check(render(engine, test.scene, again, image, false) == State::Finished, "the engine renders again after a cancel");
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures;
}
