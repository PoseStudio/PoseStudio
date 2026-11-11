/**
 * @file modelimportservice.h
 * @brief The Qt-facing orchestration that turns a file path into a model in the scene.
 *
 * This is the seam between the Qt-free import/ subsystem and the Qt-free renderer core. It:
 *   1. picks the right importer for the file (ImporterRegistry),
 *   2. parses it into a format-neutral ModelData (MeshImporter::load),
 *   3. decodes each mesh's texture maps via QImage — from external paths OR embedded bytes — into
 *      tightly-packed shared RGBA8 images (all six material slots), baking opacity masks into the
 *      diffuse alpha and gutter-filling UV seams, so the renderer never needs an image codec,
 *   4. bakes per-vertex ambient occlusion + tangents on the final mesh, and
 *   5. uploads it (VulkanRenderer::addModel),
 * optionally driving a modal staged progress dialog through those phases.
 *
 * Steps 3-5 — everything after the format-specific parse — are the SAME for every source, so
 * they live here once (uploadModelData + the ImportProgress dialog/clock) and the figure path
 * (FigureImportService) calls them after its own parse: any new import path must go through
 * them too, or the decode-once texture sharing and the seam-killing gutter fill are lost.
 *
 * All the Qt (QImage decode, progress UI) lives here so both the importers and the renderer stay
 * Qt-free. VulkanWindow just forwards to it. Import runs synchronously on the GUI thread.
 */

#ifndef MODELIMPORTSERVICE_H
#define MODELIMPORTSERVICE_H

#include <QElapsedTimer>
#include <QString>

#include <memory>
#include <string>

class QImage;
class QProgressDialog;

namespace pose {

class VulkanRenderer;
struct DecodedImage;
struct ModelData;

/**
 * @class ModelImportService
 * @brief Stateless helper: load a model file into a renderer's scene. See file header.
 */
class ModelImportService {
public:
    /**
     * @class ImportProgress
     * @brief One import's modal staged progress dialog (optional) and its phase clock.
     *
     * Import runs synchronously on the GUI thread (parse -> texture decode -> bake -> GPU
     * upload), so a large model would otherwise freeze the UI with no feedback. Shown, the
     * dialog is ApplicationModal with no cancel button: each phase is a short, atomic, blocking
     * operation that can't be safely interrupted partway through. It pumps the event loop on
     * setValue(), so the bar/label repaint between phases. Not shown (the startup/command-line
     * drain, called from inside the expose/init path where that pump could re-enter it), the
     * object still keeps the clock so the timing log is identical either way. The dialog closes
     * when done() reaches the maximum, or with the object on an exception path.
     */
    class ImportProgress {
    public:
        /// @p show drives the dialog; @p windowTitle / @p readingLabel are its title and the
        /// busy-indicator label shown while the file parses. The dialog is forced on-screen and
        /// painted NOW: QProgressDialog's own auto-show is driven by a remaining-time estimate
        /// that can't see the upcoming blocking parse, so it would otherwise first appear
        /// mid-import.
        ImportProgress(bool show, const QString& windowTitle, const QString& readingLabel);
        ~ImportProgress();
        ImportProgress(const ImportProgress&) = delete;
        ImportProgress& operator=(const ImportProgress&) = delete;

        /// The parse finished: records its time and turns the busy indicator into a determinate
        /// bar at step 1 of kSteps.
        void parsed();
        /// Advances the bar one step under @p label (decode / bake / upload).
        void phase(const QString& label);
        /// Jumps the bar to its maximum, which closes the dialog.
        void done();

        qint64 elapsedMs() const { return m_clock.elapsed(); }
        qint64 parseMs() const { return m_parseMs; }

    private:
        static constexpr int kSteps = 5; // parse, decode, bake, upload, done

        std::unique_ptr<QProgressDialog> m_dialog; // null when not shown
        QElapsedTimer                    m_clock;
        qint64                           m_parseMs = 0;
        int                              m_step = 0;
    };

    /**
     * Loads @p path into @p renderer's scene. Logs and returns false on unsupported format or
     * parse/decode failure (a bad file drops only that import, never the app).
     * @param showProgress  When true (interactive imports), drives a modal staged QProgressDialog.
     *   Must be false for the startup/command-line drain called from inside the expose/init path,
     *   where the modal dialog's processEvents() could re-enter it.
     * @return true if a model was uploaded to the scene.
     */
    static bool importInto(VulkanRenderer& renderer, const QString& path, bool showProgress);

    /// Decodes every mesh's TextureSources (external paths OR embedded bytes) into shared
    /// DecodedImages via QImage, bakes opacity masks into the diffuse alpha, and gutter-fills each
    /// image against the union of its users' UVs. Each unique source is decoded ONCE (figure zones
    /// commonly share atlas files) and the decodes run across cores. Logs and leaves a mesh slot
    /// untextured if its image can't be decoded. Shared by the model and figure import paths so
    /// both decode textures identically — keep every import path behind this choke point, or the
    /// mip-bleed seams the gutter fill prevents come back.
    static void decodeModelTextures(ModelData& data);

    /// The post-parse half of every import: decodes @p data's textures (decodeModelTextures),
    /// bakes per-vertex AO + tangents on the final mesh, uploads it to @p renderer, and logs the
    /// per-phase timing as "[viewport] imported <kindPrefix><path> in ... (parse, decode,
    /// upload)<logSuffix>" — @p kindPrefix is "" for a model, "figure " for a figure; @p logSuffix
    /// trails the line (the figure's zone/corrective counts). @p progress must have had parsed()
    /// called. Throws on Vulkan failure (the caller's catch logs it).
    static void uploadModelData(VulkanRenderer& renderer, ModelData& data, ImportProgress& progress,
                                const char* kindPrefix, const QString& path,
                                const QString& logSuffix = QString());
};

/// THE TEXTURE SLOTS' helpers (the Materials tab, vulkanwindow_materials.cpp — the same decodes
/// and bakes the import runs, on one file): an image file decoded to RGBA8 (capped like an
/// import's), a cutout mask file (its gray in every channel), the base color with a mask baked
/// into its alpha (a white canvas at the mask's size without a base color; a copy of the base
/// without a mask; null with neither), the roughness / metalness parameter map (null with
/// neither), and a QImage mask as a DecodedImage. Null on a failed decode (logged).
std::shared_ptr<DecodedImage> decodeTextureFile(const std::string& path);
std::shared_ptr<DecodedImage> decodeMaskFile(const std::string& path);
std::shared_ptr<DecodedImage> maskToDecoded(const QImage& mask);
std::shared_ptr<DecodedImage> bakeCutoutIntoBaseColor(const std::shared_ptr<const DecodedImage>& base,
                                                      const std::shared_ptr<const DecodedImage>& mask);
/// The LAYER map (set 1 binding 8) from the four slots' plain images — the coat weight, the coat
/// roughness, the sheen weight and the thin-film thickness, each one's red channel into R, G, B
/// and A (255 where a map is absent); null without any of them.
std::shared_ptr<DecodedImage> buildLayerMapImage(const std::shared_ptr<const DecodedImage>& coatWeight,
                                                 const std::shared_ptr<const DecodedImage>& coatRoughness,
                                                 const std::shared_ptr<const DecodedImage>& sheenWeight,
                                                 const std::shared_ptr<const DecodedImage>& thinFilmThickness);
std::shared_ptr<DecodedImage> buildParameterMapImage(const std::shared_ptr<const DecodedImage>& rough, int roughChannel,
                                                     const std::shared_ptr<const DecodedImage>& metal, int metalChannel,
                                                     const std::shared_ptr<const DecodedImage>& ambientOcclusion,
                                                     const std::shared_ptr<const DecodedImage>& transmissionWeight);

} // namespace pose

#endif // MODELIMPORTSERVICE_H
