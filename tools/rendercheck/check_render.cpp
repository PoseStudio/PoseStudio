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
    SunLight                   sun;
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
        sun.direction[0] = toSun.x;
        sun.direction[1] = toSun.y;
        sun.direction[2] = toSun.z;
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
        scene.suns = &sun;
        scene.sunCount = 1;
        scene.floorShadows = 1;
        scene.floorHeight = 0.0f;

        scene.environment.color[0] = scene.environment.color[1] = scene.environment.color[2] = 1.0f;
        scene.environment.strength = 0.35f;
        scene.environment.backdrop = 0;
        scene.environment.backdropColor[0] = 0.10f;
        scene.environment.backdropColor[1] = 0.20f;
        scene.environment.backdropColor[2] = 0.30f;

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
        lit.scene.sunCount = 0;
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
        lit.scene.sunCount = 1;
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
