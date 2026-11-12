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

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QStringList>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>

namespace pose {

std::shared_ptr<RenderSceneHolder> VulkanWindow::captureRenderScene() {
    if (!m_renderer) {
        return nullptr;
    }
    using renderapi::ColorSpace;
    using renderapi::MapSlot;

    Scene&                  live = scene();
    const LightingSettings& lighting = live.lightingSettings();
    auto                    snapshot = std::make_shared<RenderSnapshot>();
    OverlayBake             bake;

    // An image of the scene as an entry of the description: one per image and encoding.
    std::map<std::pair<const DecodedImage*, int>, int> imageIndex;
    const auto imageOf = [&](const std::shared_ptr<const DecodedImage>& image, ColorSpace space, const std::string& label) -> int {
        if (!image || image->pixels.empty() || image->width == 0 || image->height == 0) {
            return -1;
        }
        const std::pair<const DecodedImage*, int> key(image.get(), static_cast<int>(space));
        if (const auto it = imageIndex.find(key); it != imageIndex.end()) {
            return it->second;
        }
        snapshot->imageOwners.push_back(image);
        snapshot->names.push_back(label);
        renderapi::Image out;
        out.id = (reinterpret_cast<std::uintptr_t>(image.get()) << 1) | static_cast<std::uintptr_t>(space);
        out.pixels = image->pixels.data();
        out.width = static_cast<int32_t>(image->width);
        out.height = static_cast<int32_t>(image->height);
        out.channels = 4;
        out.format = renderapi::PixelFormat::UInt8;
        out.colorSpace = space;
        out.name = snapshot->names.back().c_str();
        const int index = static_cast<int>(snapshot->images.size());
        snapshot->images.push_back(out);
        imageIndex.emplace(key, index);
        return index;
    };

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
                return overlaid(modelIndex, meshIndex, map, base, channel, bake, scale, /*forRenderer=*/true);
            };
            const std::string label = std::string(name) + ":";

            // Base color (sRGB), and the cutout: its own mask, or the base color image's alpha
            // where the import keeps the shape there (a glTF's alpha mode).
            const auto base = painted(MaterialMap::BaseColor, plainOf(MaterialMap::BaseColor, nullptr), 0);
            const int  baseImage = imageOf(base, ColorSpace::SRgb, label + "base color");
            put(MapSlot::BaseColor, baseImage, -1);
            int maskChannel = 0;
            if (const auto mask = plainOf(MaterialMap::Opacity, &maskChannel)) {
                put(MapSlot::Opacity, imageOf(mask, ColorSpace::Linear, label + "cutout"), maskChannel);
            } else if (mesh.hasOpacityMask() && baseImage >= 0) {
                put(MapSlot::Opacity, baseImage, 3);
            }

            // Roughness (the overlays' gloss in it) and metalness — where the overlays paint
            // metal, the zone's ABSOLUTE metalness (its Metallic value in the map: the material's
            // own value is then 1, the viewport's rule — see bindSlot).
            int        roughChannel = 0;
            const auto rough = plainOf(MaterialMap::Roughness, &roughChannel);
            const auto glossed = painted(MaterialMap::Roughness, rough, rough ? roughChannel : 0);
            put(MapSlot::Roughness, imageOf(glossed, ColorSpace::Linear, label + "roughness"), rough ? roughChannel : 0);
            int  metalChannel = 0;
            auto metal = plainOf(MaterialMap::Metalness, &metalChannel);
            if (!overlaySignature(mesh.material().overlays, MaterialMap::Metalness).empty()) {
                const float value = mesh.material().baseEnabled ? mesh.material().metalness : 0.0f;
                const auto  baked = painted(MaterialMap::Metalness, metal, metal ? metalChannel : 0, value);
                if (baked != metal) {
                    metal = baked;
                    material.metallic = 1.0f;
                }
            }
            put(MapSlot::Metallic, imageOf(metal, ColorSpace::Linear, label + "metalness"), metalChannel);

            // The surface maps: the normal map with the overlays' normals, the bump map with
            // their heights.
            const auto normal = painted(MaterialMap::Normal, plainOf(MaterialMap::Normal, nullptr), 0);
            put(MapSlot::Normal, imageOf(normal, ColorSpace::Linear, label + "normal"), -1);
            const auto bump = painted(MaterialMap::Bump, plainOf(MaterialMap::Bump, nullptr), 0);
            put(MapSlot::Bump, imageOf(bump, ColorSpace::Linear, label + "bump"), 0);

            // The rest are the zone's own maps, as they are.
            const auto plain = [&](MapSlot slot, MaterialMap map, ColorSpace space, bool color, const char* what) {
                int        channel = 0;
                const auto image = plainOf(map, &channel);
                put(slot, imageOf(image, space, label + what), color ? -1 : channel);
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

            renderapi::Mesh out;
            out.name = name;
            out.material = static_cast<int32_t>(snapshot->materials.size());
            snapshot->materials.push_back(material);
            snapshot->meshes.push_back(out);
        }
    }

    // --- The environment: the panorama that lights the scene now, or the procedural studio ---
    renderapi::Environment& environment = snapshot->description.environment;
    float                   luminanceScale = 1.0f;
    if (!m_appliedEnvironmentPath.isEmpty() && QFileInfo::exists(m_appliedEnvironmentPath)) {
        snapshot->environmentFile = QFileInfo(m_appliedEnvironmentPath).absoluteFilePath().toStdString();
        environment.file = snapshot->environmentFile.c_str();
        luminanceScale = m_appliedEnvironmentScale;
    } else {
        EnvironmentImage studio = generateStudioEnvironment();
        normalizeEnvironmentLuminance(studio, kEnvironmentAverageLuminance); // (the pixels carry the factor)
        snapshot->environmentPixels = std::move(studio.pixels);
        renderapi::Image image;
        image.id = 1; // (no DecodedImage has this address)
        image.pixels = snapshot->environmentPixels.data();
        image.width = studio.width;
        image.height = studio.height;
        image.channels = 3;
        image.format = renderapi::PixelFormat::Float32;
        image.colorSpace = ColorSpace::Linear;
        image.name = "studio environment";
        environment.image = static_cast<int32_t>(snapshot->images.size());
        snapshot->images.push_back(image);
    }
    renderEnvironment(lighting, luminanceScale, environment);

    // --- The key light, the floor's shadows, the camera ---
    const renderapi::SunLight key = renderKeyLight(lighting);
    if (key.strength > 0.0f) {
        snapshot->suns.push_back(key);
    }
    snapshot->description.floorShadows = lighting.shadowsEnabled ? 1 : 0;
    snapshot->description.floorHeight = 0.0f;
    snapshot->description.camera = renderCamera(m_renderer->camera());

    const QSize pixels = pixelExtent();
    snapshot->viewSettings.exposure = lighting.exposure;
    snapshot->viewSettings.tonemap = lighting.tonemap;
    snapshot->viewSettings.width = pixels.width();
    snapshot->viewSettings.height = pixels.height();

    snapshot->link();

    // POSESTUDIO_RENDER_TRACE=1: what a render engine is handed — every zone's material in the
    // interface's own terms, the lights, the environment. The first thing to read when a render
    // and the viewport disagree about a surface.
    static const bool trace = qEnvironmentVariableIsSet("POSESTUDIO_RENDER_TRACE");
    if (trace) {
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
        for (const renderapi::SunLight& sun : snapshot->suns) {
            std::fprintf(stderr, "[render] sun toward %.2f %.2f %.2f  color %.2f %.2f %.2f  strength %.2f  angle %.1f deg\n",
                         sun.direction[0], sun.direction[1], sun.direction[2], sun.color[0], sun.color[1], sun.color[2],
                         sun.strength, sun.angle * 57.29578f);
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
