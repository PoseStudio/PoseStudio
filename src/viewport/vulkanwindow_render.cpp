/**
 * @file vulkanwindow_render.cpp
 * @brief VulkanWindow's side of a production render: the scene captured for a render engine.
 *        (A TU of VulkanWindow — see vulkanwindow.h for the class and its other TUs.)
 *
 * captureRenderScene() freezes what the viewport shows into a RenderSnapshot (rendersnapshot.h):
 *   - every mesh AS POSED (Model::bakePosedMesh: the vertex stage on the CPU), with its zone's
 *     material translated by scene/renderexport and its TEXTURES — the zone's own decoded
 *     images, shared, with the zone's OVERLAYS painted into the maps they touch exactly as the
 *     viewport's bindings carry them (overlaid(): the same compositor, the same cache), except
 *     that an overlay's bump stays in the bump map (the viewport alone folds it into its normal
 *     map);
 *   - the camera as framed, the key light as a sun, the environment — the panorama's FILE where
 *     one lights the scene (the engine reads it at full resolution) with the factor the viewport
 *     normalized it by, else the procedural studio's pixels — and the backdrop;
 *   - how the viewport shows it (exposure, tonemap, size).
 * Gaussian splat scenes are left out and counted: a path tracer has no triangles to trace there.
 */

#include "vulkanwindow.h"

#include "environment.h"
#include "environmentsource.h" // loadEnvironmentImage (a render's backdrop behind a splat scene)
#include "importformats.h"
#include "lightingsettings.h"
#include "materialparams.h"
#include "model.h"
#include "ikscript.h" // the scripted test's `render`
#include "renderexport.h"
#include "renderscene.h"
#include "rendersession.h"
#include "rendersnapshot.h"
#include "scene.h"
#include "vulkanrenderer.h"

#include "parallelfor.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include <QStringList>

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace pose {

const std::vector<glm::vec3>& VulkanWindow::studioEnvironmentPixels() const {
    // (Made once: see RenderSnapshot::environmentPixels.)
    if (!m_studioEnvironment) {
        EnvironmentImage studio = generateStudioEnvironment();
        normalizeEnvironmentLuminance(studio, kEnvironmentAverageLuminance); // (the pixels carry the factor)
        m_studioEnvironmentWidth = studio.width;
        m_studioEnvironmentHeight = studio.height;
        m_studioEnvironment = std::make_shared<const std::vector<glm::vec3>>(std::move(studio.pixels));
    }
    return *m_studioEnvironment;
}

void VulkanWindow::captureRenderLighting(RenderSnapshot& snapshotRef) const {
    using renderapi::ColorSpace;
    RenderSnapshot* const   snapshot = &snapshotRef;
    const Scene&            live = scene();
    const LightingSettings& lighting = live.lightingSettings();

    // --- The environment: the panorama that lights the scene now, or the procedural studio ---
    renderapi::Environment& environment = snapshot->description.environment;
    float                   luminanceScale = 1.0f;
    if (!m_appliedEnvironmentPath.isEmpty() && QFileInfo::exists(m_appliedEnvironmentPath)) {
        snapshot->environmentFile = QFileInfo(m_appliedEnvironmentPath).absoluteFilePath().toStdString();
        environment.file = snapshot->environmentFile.c_str();
        luminanceScale = m_appliedEnvironmentScale;
    } else {
        studioEnvironmentPixels(); // (made once: see RenderSnapshot::environmentPixels)
        snapshot->environmentPixels = m_studioEnvironment;
        renderapi::Image image;
        image.id = 1; // (no DecodedImage has this address)
        image.pixels = snapshot->environmentPixels->data();
        image.width = m_studioEnvironmentWidth;
        image.height = m_studioEnvironmentHeight;
        image.channels = 3;
        image.format = renderapi::PixelFormat::Float32;
        image.colorSpace = ColorSpace::Linear;
        image.name = "studio environment";
        environment.image = static_cast<int32_t>(snapshot->images.size());
        snapshot->images.push_back(image);
    }
    renderEnvironment(lighting, luminanceScale, environment);

    // --- The key light, the floor's shadows, the camera ---
    const renderapi::Light key = renderKeyLight(lighting);
    if (key.strength > 0.0f) {
        snapshot->lights.push_back(key);
    }
    // … and the light objects, every enabled one (the viewport shades with the first few; a
    // render traces them all, shadows included).
    for (const SceneLight& light : live.lights()) {
        if (light.enabled && light.intensity > 0.0f) {
            snapshot->lights.push_back(renderLight(light));
        }
    }
    snapshot->description.floorShadows = lighting.shadowsEnabled ? 1 : 0;
    snapshot->description.floorHeight = 0.0f;
}

std::shared_ptr<RenderSplatLayers> VulkanWindow::captureSplatLayers(int width, int height,
                                                                    const renderapi::Environment& environment, bool floorShadows) {
    if (!m_renderer || width < 1 || height < 1 || !scene().hasSplats()) {
        return nullptr;
    }
    // What lies within this of the floor's plane is the capture's FLOOR: it stays in the plate,
    // where the render's shadows fall on it, and out of the layer laid back over the picture.
    // (A reconstruction's floor is a few centimetres thick; a shoe on it is not the floor.)
    constexpr float kSplatFloorDepth = 0.04f;
    VulkanRenderer::SplatLayers drawn;
    try {
        if (!m_renderer->renderSplatLayers(static_cast<uint32_t>(width), static_cast<uint32_t>(height), floorShadows,
                                           kSplatFloorDepth, drawn)) {
            return nullptr;
        }
    } catch (const std::exception& e) {
        // (Targets of the render's size did not fit on the device, most likely: the render goes
        // without the capture, and says so.)
        qWarning("[render] the Gaussian splat layers could not be drawn at %dx%d: %s", width, height, e.what());
        return nullptr;
    }
    auto layers = std::make_shared<RenderSplatLayers>();
    layers->width = width;
    layers->height = height;
    layers->all = std::move(drawn.all);
    layers->front = std::move(drawn.front);

    // An ENVIRONMENT backdrop: the panorama as a render draws it behind the scene — along each
    // pixel's own ray, turned as the lighting is, at the backdrop's brightness — so that the
    // capture stands in front of the same backdrop a render without one shows.
    if (environment.backdrop != 0) {
        EnvironmentImage panorama;
        if (environment.file != nullptr && environment.file[0] != '\0') {
            if (std::optional<EnvironmentImage> loaded = loadEnvironmentImage(environment.file)) {
                panorama = std::move(*loaded);
            }
        } else if (m_studioEnvironment && m_studioEnvironmentWidth > 0 && m_studioEnvironmentHeight > 0) {
            panorama.width = m_studioEnvironmentWidth;
            panorama.height = m_studioEnvironmentHeight;
            panorama.pixels = *m_studioEnvironment;
        }
        if (panorama.width > 0 && panorama.height > 0) {
            Camera camera = m_renderer->camera();
            camera.setViewportSize(static_cast<float>(width), static_cast<float>(height));
            const float gain = environment.strength * environment.backdropStrength;
            const float c = std::cos(environment.rotation);
            const float s = std::sin(environment.rotation);
            layers->backdrop.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
            parallelFor(height, [&](int y) {
                float* out = &layers->backdrop[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3u];
                for (int x = 0; x < width; ++x, out += 3) {
                    // (Four rays a pixel: a panorama's fine detail — a softbox's grid — is
                    // filtered as a render's many samples filter it, not point-sampled.)
                    glm::vec3 radiance(0.0f);
                    for (int sub = 0; sub < 4; ++sub) {
                        const Ray ray = camera.screenPointToRay(static_cast<float>(x) + (sub & 1 ? 0.75f : 0.25f),
                                                                static_cast<float>(y) + (sub & 2 ? 0.75f : 0.25f),
                                                                static_cast<float>(width), static_cast<float>(height));
                        // (camera_ubo.glsl's rotateY: the direction the viewport samples its environment along.)
                        const glm::vec3 d(c * ray.direction.x + s * ray.direction.z, ray.direction.y,
                                          -s * ray.direction.x + c * ray.direction.z);
                        radiance += panorama.sample(d);
                    }
                    radiance *= 0.25f * gain;
                    out[0] = radiance.x;
                    out[1] = radiance.y;
                    out[2] = radiance.z;
                }
            });
        }
    }
    return layers;
}

std::shared_ptr<RenderSceneHolder> VulkanWindow::captureRenderLightingOnly() {
    if (!m_renderer) {
        return nullptr;
    }
    auto snapshot = std::make_shared<RenderSnapshot>();
    captureRenderLighting(*snapshot);
    const LightingSettings& lighting = scene().lightingSettings();
    const QSize             pixels = pixelExtent();
    snapshot->viewSettings.exposure = lighting.exposure;
    snapshot->viewSettings.tonemap = lighting.tonemap;
    snapshot->viewSettings.width = pixels.width();
    snapshot->viewSettings.height = pixels.height();
    snapshot->description.camera = renderCamera(m_renderer->camera());
    snapshot->link();
    return snapshot;
}

renderapi::Material VulkanWindow::captureZoneMaterial(RenderSnapshot& snapshot, OverlayBake& bake, int modelIndex, int meshIndex,
                                                      const char* name, const RenderMaterialContext& context, bool withOverlays) {
    using renderapi::ColorSpace;
    using renderapi::MapSlot;
    const Mesh& mesh = scene().modelAt(static_cast<std::size_t>(modelIndex))->mesh(static_cast<std::size_t>(meshIndex));

    renderapi::Material material = renderMaterial(mesh.material(), mesh.importedMaterial(), name, context);
    const auto          put = [&](MapSlot slot, int image, int channel) {
        material.maps[static_cast<int>(slot)].image = image;
        material.maps[static_cast<int>(slot)].channel = image >= 0 ? channel : -1;
    };
    // A slot's plain image and the channel its value rides in.
    const auto plainOf = [&](MaterialMap map, int* channel) {
        int        c = 0;
        const auto image = mesh.slotImage(map);
        if (image) {
            c = mesh.slotChannel(map);
        }
        if (channel != nullptr) {
            *channel = c;
        }
        return image;
    };
    // … with the zone's overlays painted in, as a renderer is handed it.
    const auto painted = [&](MaterialMap map, const std::shared_ptr<const DecodedImage>& base, int channel, float scale = 1.0f) {
        if (!withOverlays) {
            return base;
        }
        return overlaid(modelIndex, meshIndex, map, base, channel, bake, scale, /*forRenderer=*/true);
    };
    const std::string label = std::string(name) + ":";

    // Base color (sRGB), and the cutout: its own mask, or the base color image's alpha
    // where the import keeps the shape there (a glTF's alpha mode).
    const auto base = painted(MaterialMap::BaseColor, plainOf(MaterialMap::BaseColor, nullptr), 0);
    const int  baseImage = snapshot.imageOf(base, ColorSpace::SRgb, label + "base color");
    put(MapSlot::BaseColor, baseImage, -1);
    int maskChannel = 0;
    if (const auto mask = plainOf(MaterialMap::Opacity, &maskChannel)) {
        put(MapSlot::Opacity, snapshot.imageOf(mask, ColorSpace::Linear, label + "cutout"), maskChannel);
    } else if (mesh.hasOpacityMask() && baseImage >= 0) {
        put(MapSlot::Opacity, baseImage, 3);
    }

    // Roughness (the overlays' gloss in it) and metalness — where the overlays paint
    // metal, the zone's ABSOLUTE metalness (its Metallic value in the map: the material's
    // own value is then 1, the viewport's rule — see bindSlot).
    int        roughChannel = 0;
    const auto rough = plainOf(MaterialMap::Roughness, &roughChannel);
    const auto glossed = painted(MaterialMap::Roughness, rough, rough ? roughChannel : 0);
    put(MapSlot::Roughness, snapshot.imageOf(glossed, ColorSpace::Linear, label + "roughness"), rough ? roughChannel : 0);
    int  metalChannel = 0;
    auto metal = plainOf(MaterialMap::Metalness, &metalChannel);
    if (withOverlays && !overlaySignature(mesh.material().overlays, MaterialMap::Metalness).empty()) {
        const float value = mesh.material().baseEnabled ? mesh.material().metalness : 0.0f;
        const auto  baked = painted(MaterialMap::Metalness, metal, metal ? metalChannel : 0, value);
        if (baked != metal) {
            metal = baked;
            material.metallic = 1.0f;
        }
    }
    put(MapSlot::Metallic, snapshot.imageOf(metal, ColorSpace::Linear, label + "metalness"), metalChannel);

    // The surface maps: the normal map with the overlays' normals, the bump map with
    // their heights.
    const auto normal = painted(MaterialMap::Normal, plainOf(MaterialMap::Normal, nullptr), 0);
    put(MapSlot::Normal, snapshot.imageOf(normal, ColorSpace::Linear, label + "normal"), -1);
    const auto bump = painted(MaterialMap::Bump, plainOf(MaterialMap::Bump, nullptr), 0);
    put(MapSlot::Bump, snapshot.imageOf(bump, ColorSpace::Linear, label + "bump"), 0);

    // The rest are the zone's own maps, as they are.
    const auto plain = [&](MapSlot slot, MaterialMap map, ColorSpace space, bool color, const char* what) {
        int        channel = 0;
        const auto image = plainOf(map, &channel);
        put(slot, snapshot.imageOf(image, space, label + what), color ? -1 : channel);
    };
    plain(MapSlot::Specular, MaterialMap::SpecularMask, ColorSpace::Linear, false, "specular");
    plain(MapSlot::Emission, MaterialMap::Emission, ColorSpace::SRgb, true, "emission");
    plain(MapSlot::Subsurface, MaterialMap::Translucency, ColorSpace::SRgb, true, "translucency");
    plain(MapSlot::DetailNormal, MaterialMap::PoreDetail, ColorSpace::Linear, true, "detail normal");
    plain(MapSlot::CoatWeight, MaterialMap::CoatWeight, ColorSpace::Linear, false, "coat weight");
    plain(MapSlot::CoatRoughness, MaterialMap::CoatRoughness, ColorSpace::Linear, false, "coat roughness");
    plain(MapSlot::SheenWeight, MaterialMap::SheenWeight, ColorSpace::Linear, false, "sheen weight");
    plain(MapSlot::SheenColor, MaterialMap::SheenColor, ColorSpace::SRgb, true, "sheen color");
    plain(MapSlot::ThinFilmThickness, MaterialMap::ThinFilmThickness, ColorSpace::Linear, false, "thin film");
    plain(MapSlot::TransmissionWeight, MaterialMap::TransmissionWeight, ColorSpace::Linear, false, "transmission");
    return material;
}

std::shared_ptr<RenderSceneHolder> VulkanWindow::captureRenderScene() {
    if (!m_renderer) {
        return nullptr;
    }
    using renderapi::MapSlot;

    Scene&                  live = scene();
    const LightingSettings& lighting = live.lightingSettings();
    auto                    snapshot = std::make_shared<RenderSnapshot>();
    OverlayBake             bake;

    for (std::size_t mi = 0; mi < live.modelCount(); ++mi) {
        Model* model = live.modelAt(mi);
        if (model == nullptr) {
            continue;
        }
        if (model->splats() != nullptr) {
            ++snapshot->splatScenes;
            continue;
        }
        RenderMaterialContext context;
        context.subsurfaceDial = lighting.subsurface;
        context.translucencyAuthored = ImportFormats::isNativeFigureFile(QString::fromStdString(model->sourcePath()));
        const int modelIndex = static_cast<int>(mi);

        for (std::size_t k = 0; k < model->meshCount(); ++k) {
            const Mesh& mesh = model->mesh(k);
            const int   meshIndex = static_cast<int>(k);
            snapshot->geometry.emplace_back();
            if (!model->bakePosedMesh(k, snapshot->geometry.back())) {
                snapshot->geometry.pop_back();
                continue;
            }
            snapshot->names.push_back(mesh.name().empty() ? ("Zone " + std::to_string(k + 1)) : mesh.name());
            const char* name = snapshot->names.back().c_str();

            const renderapi::Material material =
                captureZoneMaterial(*snapshot, bake, modelIndex, meshIndex, name, context, /*withOverlays=*/true);

            renderapi::Mesh out;
            out.name = name;
            out.material = static_cast<int32_t>(snapshot->materials.size());
            snapshot->materials.push_back(material);
            snapshot->meshes.push_back(out);
        }
    }

    captureRenderLighting(*snapshot); // the environment, the key light, the light objects, the floor
    snapshot->description.camera = renderCamera(m_renderer->camera());

    const QSize pixels = pixelExtent();
    snapshot->viewSettings.exposure = lighting.exposure;
    snapshot->viewSettings.tonemap = lighting.tonemap;
    snapshot->viewSettings.width = pixels.width();
    snapshot->viewSettings.height = pixels.height();

    snapshot->link();

    // The scene's Gaussian splats: a render composites them from layers this window draws when
    // the render's size is known (captureSplatLayers).
    if (snapshot->splatScenes > 0) {
        const QPointer<VulkanWindow>  self(this);
        const renderapi::Environment  environment = snapshot->description.environment; // (file: the snapshot's string, which the session holds)
        const bool                    floorShadows = snapshot->description.floorShadows != 0;
        snapshot->splatSource = [self, environment, floorShadows](int width, int height) -> std::shared_ptr<RenderSplatLayers> {
            return self ? self->captureSplatLayers(width, height, environment, floorShadows) : nullptr;
        };
    }

    // POSESTUDIO_RENDER_TRACE=1: what a render engine is handed — every zone's material in the
    // interface's own terms, the lights, the environment. The first thing to read when a render
    // and the viewport disagree about a surface.
    static const bool trace = qEnvironmentVariableIsSet("POSESTUDIO_RENDER_TRACE");
    if (trace) {
        const renderapi::Environment& environment = snapshot->description.environment;
        const auto map = [](const renderapi::Material& m, MapSlot slot) {
            return m.maps[static_cast<int>(slot)].image >= 0 ? '+' : '-';
        };
        for (std::size_t i = 0; i < snapshot->meshes.size(); ++i) {
            const renderapi::Mesh&     mesh = snapshot->meshes[i];
            const renderapi::Material& m = snapshot->materials[static_cast<std::size_t>(mesh.material)];
            std::fprintf(stderr,
                         "[render] %-22s %7u tris  base %.2f %.2f %.2f  metal %.2f  rough %.2f  spec %.2f  ior %.2f  "
                         "opacity %.2f  transmission %.2f (%.2f %.2f %.2f at %.3f m, rough %.2f)%s  sss %.2f (%.4f %.4f %.4f m)  coat %.2f/%.2f  "
                         "sheen %.2f  emission %.2f  normal %.2f  bump %.2f  maps base%c cut%c rough%c metal%c spec%c "
                         "normal%c bump%c sss%c\n",
                         mesh.name != nullptr ? mesh.name : "", mesh.indexCount / 3, m.baseColor[0], m.baseColor[1],
                         m.baseColor[2], m.metallic, m.roughness, m.specular, m.ior, m.opacity, m.transmission,
                         m.transmissionColor[0], m.transmissionColor[1], m.transmissionColor[2], m.transmissionDepth,
                         m.transmissionRoughness,
                         (m.flags & renderapi::kMaterialThinWalled) != 0 ? " thin" : "", m.subsurface, m.subsurfaceRadius[0],
                         m.subsurfaceRadius[1], m.subsurfaceRadius[2], m.coat, m.coatRoughness, m.sheen, m.emissionStrength,
                         m.normalStrength, m.bumpStrength, map(m, MapSlot::BaseColor), map(m, MapSlot::Opacity),
                         map(m, MapSlot::Roughness), map(m, MapSlot::Metallic), map(m, MapSlot::Specular),
                         map(m, MapSlot::Normal), map(m, MapSlot::Bump), map(m, MapSlot::Subsurface));
        }
        for (const renderapi::Light& light : snapshot->lights) {
            static const char* const kKinds[] = {"sun", "point", "spot", "area"};
            std::fprintf(stderr,
                         "[render] %s light at %.2f %.2f %.2f  shining %.2f %.2f %.2f  color %.2f %.2f %.2f  strength %.2f  "
                         "angle %.1f deg  radius %.3f  size %.2f x %.2f\n",
                         kKinds[static_cast<int>(light.type) & 3], light.position[0], light.position[1], light.position[2],
                         light.direction[0], light.direction[1], light.direction[2], light.color[0], light.color[1],
                         light.color[2], light.strength, light.angle * 57.29578f, light.radius, light.sizeU, light.sizeV);
        }
        std::fprintf(stderr,
                     "[render] environment %s  strength %.3f  ambient %.3f  rotation %.1f deg  backdrop %d (%.3f %.3f %.3f x %.2f)  "
                     "floor shadows %d  exposure %.2f  tonemap %d\n",
                     environment.file != nullptr ? environment.file : "(studio)", environment.strength, environment.ambient,
                     environment.rotation * 57.29578f, static_cast<int>(environment.backdrop), environment.backdropColor[0],
                     environment.backdropColor[1], environment.backdropColor[2], environment.backdropStrength,
                     snapshot->description.floorShadows, snapshot->viewSettings.exposure,
                     snapshot->viewSettings.tonemap ? 1 : 0);
        std::fflush(stderr);
    }
    return snapshot;
}

// The scripted in-app test's `render` (vulkanwindow_script.cpp; tools/ikscripts/README.md): the
// scene as it stands, captured and rendered to its END through the render engine — the path the
// Render window takes, with a local event loop standing in for the user's wait — and saved.
// Not for the middle of a drag: the loop runs the application's timers too.
void VulkanWindow::scriptRender(const QStringList& words) {
    IkScript& s = *m_script;
    s.renderOk = 0.0;
    s.renderSeconds = 0.0;
    s.renderMean = 0.0;
    s.renderCovered = 0.0;
    s.renderSplats = 0.0;
    s.renderImage = QImage();
    const auto note = [](const QString& text) {
        std::fprintf(stderr, "[ikscript]   render: %s\n", qPrintable(text));
        std::fflush(stderr);
    };

    const std::shared_ptr<RenderSceneHolder> captured = captureRenderScene();
    if (!captured) {
        note(QStringLiteral("no renderer yet"));
        return;
    }
    QString engineError;
    if (RenderSession::engine(&engineError) == nullptr) {
        note(QStringLiteral("no render engine (%1)").arg(engineError));
        return;
    }

    // <name> [<width> <height>] [<samples>] [<device text>|cpu] [nodenoise] [transparent]
    RenderOptions options;
    options.width = captured->view().width & ~1;
    options.height = captured->view().height & ~1;
    options.samples = 64;
    options.deviceId = RenderSession::defaultDeviceId();
    QVector<int> numbers;
    for (int i = 2; i < words.size(); ++i) {
        bool      isNumber = false;
        const int value = words[i].toInt(&isNumber);
        if (isNumber) {
            numbers.push_back(value);
        } else if (words[i].compare(QLatin1String("cpu"), Qt::CaseInsensitive) == 0) {
            options.deviceId.clear();
        } else if (words[i].compare(QLatin1String("nodenoise"), Qt::CaseInsensitive) == 0) {
            options.denoise = false;
        } else if (words[i].compare(QLatin1String("transparent"), Qt::CaseInsensitive) == 0) {
            options.transparent = true;
        } else {
            for (const RenderDevice& device : RenderSession::devices()) {
                if (device.label().contains(words[i], Qt::CaseInsensitive)) {
                    options.deviceId = device.id;
                    break;
                }
            }
        }
    }
    if (numbers.size() >= 2) {
        options.width = numbers[0];
        options.height = numbers[1];
    }
    if (numbers.size() == 1 || numbers.size() >= 3) {
        options.samples = numbers.back();
    }

    RenderSession session;
    QEventLoop    loop;
    connect(&session, &RenderSession::ended, &loop, &QEventLoop::quit);
    QString error;
    if (!session.start(captured, options, &error)) {
        note(QStringLiteral("could not start (%1)").arg(error));
        return;
    }
    loop.exec();

    const renderapi::Status& status = session.status();
    s.renderSeconds = status.seconds;
    if (status.state != renderapi::State::Finished || !session.hasImage()) {
        note(QStringLiteral("did not finish (state %1) %2").arg(static_cast<int>(status.state)).arg(QString::fromUtf8(status.error)));
        return;
    }
    QDir(s.outDir).mkpath(QStringLiteral("render"));
    const QString path = QDir(s.outDir).filePath(QStringLiteral("render/") + words[1] + QStringLiteral(".png"));
    if (!session.save(path, &error)) {
        note(QStringLiteral("could not save %1 (%2)").arg(path, error));
        return;
    }
    // The picture in two numbers: its mean display value, and how much of it is not the backdrop
    // (the top-left corner's color, within a few levels).
    const QImage& image = session.image();
    const QRgb    corner = image.pixel(0, 0);
    double        sum = 0.0;
    qint64        covered = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb p = image.pixel(x, y);
            sum += (qRed(p) + qGreen(p) + qBlue(p)) / 3.0;
            if (qAbs(qRed(p) - qRed(corner)) + qAbs(qGreen(p) - qGreen(corner)) + qAbs(qBlue(p) - qBlue(corner)) > 12) {
                ++covered;
            }
        }
    }
    const double pixels = static_cast<double>(image.width()) * image.height();
    s.renderOk = 1.0;
    s.renderMean = sum / pixels;
    s.renderCovered = 100.0 * static_cast<double>(covered) / pixels;
    s.renderSplats = session.compositesSplats() ? 1.0 : 0.0;
    s.renderImage = image.copy();
    note(QStringLiteral("%1 x %2, %3 samples on %4: %5 s, mean %6, covered %7 % -> %8")
             .arg(options.width)
             .arg(options.height)
             .arg(options.samples)
             .arg(options.deviceId.isEmpty() ? QStringLiteral("the CPU") : options.deviceId)
             .arg(status.seconds, 0, 'f', 1)
             .arg(s.renderMean, 0, 'f', 1)
             .arg(s.renderCovered, 0, 'f', 1)
             .arg(path));
}

} // namespace pose
