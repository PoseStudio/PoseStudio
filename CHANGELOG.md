# Changelog

All notable changes to the PoseStudio project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.3] - 2026-08-14
### Added
* **New Environment-tab controls.** The Key Light group gained **Azimuth°/Elevation°** sliders (the key light is finally movable — it was hardcoded front-right; all analytic shade modes follow it), and a new **Skin & Rim** group holds **Subsurface** (the strength of the skin-translucency terminator) and **Rim light** (a photographic back light that lifts the figure's dark side off the background). All live shader dials — no re-bake — and covered by Reset to defaults.
* **Default user library ("My PoseStudio Library").** On first launch the app creates a "My PoseStudio Library" folder in the user's Documents and registers it as an ordinary asset library (fully user-manageable; an existing library by that name — wherever the user keeps it — is honoured instead, and a deliberate later removal isn't recreated). This is the user-facing home for personal content the app itself consumes, starting with HDRI environments.
* **HDRI thumbnail menu.** The Environment tab's HDRI selector is now a drop-down showing a large (178×89) image **thumbnail of each panorama** next to its name — a same-basename image file in the same folder (any common format; the stock previews ship as `.jpg`), matched the same way the Asset Manager pairs thumbnails. The list shows six environments at a time with a scroll bar for the rest (the current one highlighted and scrolled into view), rebuilds every time it opens so files added while the app runs appear immediately, and ends with a left-aligned "Open HDRI Folder…" link that jumps straight to the folder to drop new panoramas into. The tab's numeric spin buttons also gained **+/−** glyphs so the increment/decrement direction reads at a glance.

### Changed
* **PBR mode: more photoreal skin.** Two root causes of the "blown-out front, reddish dark back" look are fixed. The cheap subsurface term used to be an additive red band that saturated across the *entire* shadow hemisphere — the whole unlit side rendered dark red; it's replaced by a physically-grounded **per-channel wrapped Lambert** on the key light (red wraps farthest past the terminator, then green, then blue — energy-conserving), which produces the warm terminator of translucent skin and decays to neutral darkness in shadow. The front was double-lit by the analytic key stacked on an environment that already contains the sky's light — the key's default intensity is reduced (0.85) and the wrap's energy conservation relaxes the lit side further. The shadow side is lifted by a higher default ambient fill (0.16) and a new dialable photographic **rim light**, so the figure's dark side stays readable and separates from the background instead of crushing.
* **HDRI environments now live in the user's asset library** (`My PoseStudio Library/hdri`) instead of a read-only folder next to the executable — a folder the user is accustomed to browsing, where adding an environment is just dropping an `.hdr` file (plus an optional same-basename preview image for the menu thumbnail). The viewport's default environment and the Environment tab both read this folder; `<appDir>/environment.hdr` still works as an explicit override, and with no panoramas present the built-in procedural studio takes over. The `resources/hdri` folder and its build-time mirror step are gone — HDRIs are user content, not build output (packaging/installers are responsible for placing the stock set into the user library).
* **Stock HDRI previews converted `.webp` → `.jpg`** — Qt only decodes `.webp` when the optional "Qt Image Formats" add-on is installed, so the bundled previews now use a format every Qt build decodes. User-supplied `.webp` previews still work where that add-on is present.

## [0.3.2] - 2026-08-14
### Changed
* **The viewport renders on demand, not continuously.** The render loop no longer re-arms itself every frame — a frame is drawn only when something changed (camera input, posing, imports, lighting/shade-mode changes, resize). Previously the GPU redrew at full swap rate even when idle, and the vsynced present throttled the whole GUI thread, making the entire app feel sluggish; the viewport now idles at zero GPU cost. A follow-up frame is self-scheduled only when the swapchain was just rebuilt. Un-obscuring the window also no longer forces a needless swapchain rebuild (only a real size change does).
* **Image-based-lighting bakes are multi-threaded.** The GGX specular prefilter and the BRDF-LUT integration (millions of samples, previously single-threaded) now run across all CPU cores — removing the multi-second startup freeze on first viewport expose and making Environment-panel HDRI switches several times faster. (New shared helper: `rendering/parallelfor.h`, used by every CPU bake.)
* **Faster figure import.** The per-corrective Catmull-Clark delta-field subdivision (~100+ full-mesh stencil passes, the dominant slice of subdivision's import cost) is parallelized across cores, and the skin-weight blending in subdivision no longer allocates a hash map per cage face/edge (~50k calls per import). Texture decode also got cheaper (see the Asset Manager note — the same decode path serves imports).
* **Snappier Asset Manager.** Thumbnails are now decoded *at* thumbnail size (letting the JPEG codec do the reduction) instead of decoding full-size images on the GUI thread and scaling down — eliminating browse-jank in folders with large preview images. The directory tree also stops rebuilding its state icons from resources on every repaint, and posing no longer allocates per mouse-move in the skinning update.

### Fixed
* **White dots in the centers of the eyes.** Material *colours* dialed by a preset were ignored in favour of the channel defaults (`current_value` vs. `value`) — so characters that dial their pupil diffuse to black rendered a light-grey pupil disc that read as a bright white dot. Dialed colours are now honoured; as a side effect **all figure textures render at their true brightness** (they were previously multiplied by a 75%-grey default tint).
* **White seam lines along UV boundaries when zoomed out.** Character textures leave the space between UV islands white with no padding, so the GPU mip chain averaged that background through every texture seam at viewing distance (clean when zoomed in, worse zoomed out). Decoded textures are now gutter-filled at import: the mesh's own UV coverage is rasterized and the unused background flooded with colours dilated from the islands (a pull-push fill, applied to diffuse and normal/bump maps alike), so every mip averages island colour into island colour. Covered texels are untouched — close-up rendering is byte-identical.
* **The mid-forearm joint no longer bends.** The figure format marks unposeable rotation channels with a `locked` flag (distinct from `clamped` limits); the importer now honours it, pinning those axes at rest. This locks the anatomically impossible bend at the mid-limb twist joints — most visibly the mid-forearm joint on both arms — while keeping their natural axial twist free within its authored range. The old name-based twist-bone heuristic missed this joint (its name doesn't say "twist" in most figure generations) and is now only a fallback for old figures that predate the `locked` flag.
* **Posing-overlay flicker.** The skeleton/gizmo overlay's vertex buffer was rewritten by the CPU each frame while the previous in-flight frame could still be reading it on the GPU; it's now one buffer per frame-in-flight.

## [0.3.1] - 2026-08-12
### Added
* **Image-based lighting (IBL).** The viewport is now lit by a whole HDR *environment* rather than a hand-tuned light rig, giving soft wrap light, natural rim, environment colour bleed, and real glossy reflections that stay consistent as the camera orbits. It's a CPU-baked split-sum implementation: spherical-harmonic diffuse irradiance + a GGX-prefiltered specular cubemap + a BRDF LUT, all baked from the environment at load. Materials gained a metalness channel.
* **Bundled CC0 HDRI environments.** A library of public-domain `.hdr` studio/outdoor panoramas ships in `resources/hdri/` and is mirrored next to the executable. The app loads a bundled default (`hdri/Golden Gate Hills.hdr`); drop an `environment.hdr` next to the executable to override it with any HDRI. A procedural neutral-studio environment is the built-in fallback when no `.hdr` is present. (`stb_image` is now a fetched build dependency, used only to decode `.hdr` in the app layer so the render core stays codec-free.)
* **Environment properties tab.** A new tab in the right-hand docker with live controls for the viewport lighting: HDRI selection (auto-populated from the bundled library), environment rotation, exposure, ACES tonemap toggle, image-based diffuse/specular levels, an ambient fill, and key-light intensity — plus a reset-to-defaults. Every control except the HDRI switch is an instant shader update (no re-bake); switching the HDRI re-bakes the IBL on a worker thread, so it never freezes the UI.

### Changed
* **Default shading is now PBR + IBL** (the "PBR" shade mode) instead of the 3-point-lit "Rendered" mode. The other shade modes are unchanged; the analytic key light still drives the PBR highlight, and the full key/fill/rim rig still drives the "Rendered" mode.
* **Default viewport framing.** A freshly loaded figure now sits centered and full-height in the view — the camera aims at mid-torso at a closer distance, instead of targeting the floor and pushing the figure high and far back.
* **Figure content-folder terminology.** The figure-import recovery flow now refers to a "Content Folder" (dialog titles, the "Locate Content Folder…" button, and messages) — clearer and free of third-party product branding.

### Fixed
* **Twist bones no longer bend.** The mid-limb "twist" joints (e.g. mid-femur, mid-forearm), which sit where a human has no real joint, are now locked to their axial-twist axis only — they can still twist to distribute limb rotation but can't fold the limb mid-bone.
* **Correct PBR key-light specular.** The analytic key light's specular used an over-darkened geometry term (roughness⁴ instead of roughness²); rough surfaces now get the right amount of specular shadowing.

## [0.3.0] - 2026-08-12
### Added
* **Rigged character figure import — the full pipeline.** PoseStudio can now import posable, morphable character figures from their native scene format (`.duf`/`.dsf` — a gzip-compressed JSON preset that references geometry, rig, skin-weight, morph, and UV data files spread across a content folder) and render them, textured, in bind pose. This is a large new subsystem (`src/viewport/import/figure/`), built as a Qt-free, **format-agnostic reader**: one reader handles many figure families and generations because the format is self-describing — geometry, bones, weights, morphs, and materials all come out of the files, so there is no per-figure branching. **File → Import → `.DUF (DUF File)`** opens it; a figure also loads by **double-clicking it in the Asset Manager grid**, from the command line / "open with", and a companion `.pose` file can be applied the same ways.
  * **Geometry + material zones:** polygons are triangulated and grouped per material zone, with the external UV set applied seam-aware. Physically-based materials carry a base color plus diffuse / normal / bump / roughness maps, and merge the `material_library` base a preset extends (so a preset that only *overrides* channels still resolves correctly). Transparency for the eye's clear shells (cornea / moisture) is keyed on the material's refraction weight, so the iris shows through.
  * **Character-shape morphs:** the preset's dialed control channels are walked through their driver formulas to the real head/body shape morphs, whose sparse deltas are **baked into the base vertices at import** — turning the generic base into the specific character (face + body). The character's overall height (a driven figure scale) is applied too.
  * **Joint-center adjustment:** the same shape morphs also carry driver formulas that shift each bone's rest position, so the **skeleton follows the character's new proportions** — posed limbs pivot at the right places and the joint gizmo sits on the body.
  * **Skeleton + GPU skinning:** the bone hierarchy and per-vertex top-4 skin weights are parsed and skinned on the GPU (linear-blend skinning through a per-model joint-matrix storage buffer). Posing is anatomically correct — each joint rotates in its own oriented frame and rotation order, so elbows/knees/shoulders bend about their true axes.
  * **Joint-driven pose correctives:** corrective morphs that fire as a function of bend angle to fix mesh crumpling ("candy-wrapper") at deep elbow/knee/shoulder/hip bends. They are discovered *structurally* (a morph driven by a joint rotation channel), so they work across figure generations regardless of file naming; each corrective's driver formula is translated into a small stack-program and re-evaluated from the live pose, re-morphing the affected mesh on the fly.
  * **Catmull-Clark subdivision:** the low-resolution base cage is smoothed into the render mesh (positions smoothed, UVs seam-preserving, skin weights blended) so limbs/torso/face read round rather than faceted. Because subdivision is a linear operator, each corrective's sparse deltas ride onto the dense mesh through the same stencils, so the runtime re-morph is unchanged.
  * **Skin shading:** cheap subsurface skin shading (wrap-lighting + a warm subsurface band), tangent-space normal-map support (a screen-space cotangent frame, no vertex tangents), and grayscale bump maps applied as a mip-stable texture-space perturbation — which is what gives the skin its surface grain.
  * **Build dependencies:** two more header-only libraries are fetched via `FetchContent` at pinned tags — **nlohmann/json** (JSON parsing, deliberately kept out of Qt so the importer core stays Qt-free) and **miniz** (gzip inflate for the compressed scene files). CMake now also enables the **C** language for miniz's C implementation sources.
* **Interactive posing UI.** An imported figure is posable directly in the viewport:
  * **Click-to-pose:** a left-click that lands on a joint selects it and enters posing (the drag rotates that joint); a click on empty space still orbits the camera.
  * **Rotate gizmo:** three axis rings (X/Y/Z) around the selected joint, drawn in the joint's own rotation frame; dragging a ring performs an axis-constrained rotation (including twist).
  * **Joint rotation limits:** each joint is clamped to its anatomical range of motion (read from the figure's per-axis rotation limits), so bends stay realistic — a knee can't hyper-extend, the neck won't over-rotate, etc.
  * **Pose undo / redo** (`Ctrl+Z` / `Ctrl+Y`) over committed pose edits.
  * **Pose save / load:** **File → Save Pose… / Load Pose…** round-trip the figure's joint rotations through a small plain-text `.pose` file; a `.pose` passed on the command line / "open with" applies onto the imported figure.
* **Viewport shader modes (a "shader picker").** A floating dropdown in the viewport's top-right corner switches between **twelve** shade modes instantly, with no pipeline swap: Rendered (skin/subsurface), **PBR** (physically-based — the photoreal mode), three Matcaps (Studio / Skin / Metal), Toon / Cel, Clay, Lighting-Only, Flat Shaded, Normals, Albedo (unlit), and UV Checker. The selector is a real menu, so it matches the app's menu styling exactly.
* **Anti-aliasing (MSAA).** The viewport renders with multisample anti-aliasing (up to 4×, device-dependent, with a single-sample fallback), so figure and model silhouettes are smooth instead of jagged.
* **Three-point studio lighting.** The scene is lit by a proper **key / fill / back (rim)** rig, world-fixed relative to the subject so lighting stays consistent as the camera orbits, giving imported figures dimensional, studio-portrait lighting. The PBR mode evaluates a full Cook-Torrance BRDF per light plus a hemisphere image-based ambient, a subsurface term for skin, and a fresnel back-rim, ACES-tonemapped.

### Changed
* **Content-folder resolution for figures.** The figure importer resolves a preset's cross-file references against the content-root folder(s): the folder auto-detected from the imported file's own location, **unioned** with any roots the user has configured. When a figure is browsed from a presets-only folder (with no co-located `data/` folder), the interactive import now offers a **"Locate Content Folder…"** prompt, remembers the chosen root, and retries — so those figures import instead of failing silently. A file that isn't a figure (pose / material / light presets share the extension) reports a clear message rather than doing nothing.
* **Skeleton overlay hidden by default.** Importing a figure no longer draws the bone cage over the character; the rotate gizmo is the visual affordance. Joints stay clickable (picking is independent of what's drawn), so a click on a joint still selects it and brings up the gizmo.
* **Menu styling refresh.** The menu-bar dropdown menus (and the viewport shader menu, which is itself a real menu) now share the Asset Manager's dark surface (`#252627`) and blue hover highlight (`#314D7A`), with rounded corners and no drop shadow.
* **Default viewport lighting upgraded** from a single directional key light to the three-point rig above; the mesh fragment shading (both the default Rendered mode and PBR) was reworked to use it.

## [0.2.0] - 2026-06-24
### Added
* **3D Viewport (Vulkan):** Introduced the foundation of a real, Vulkan-backed 3D viewport (`src/viewport/`), replacing the static dark placeholder widget in the main window. The viewport hosts a custom `QWindow` (not Qt's `QVulkanWindow`) so the app owns the device, swapchain, and render passes directly, and renders an infinite floor grid plus imported meshes (see below) through a real orbit camera with mouse/wheel navigation.
  * **Qt / engine boundary:** The subsystem is split into a thin Qt-facing layer (`ViewportWidget`, `VulkanWindow` — the only files allowed to include Qt) and a Qt-free rendering core under `rendering/` and `scene/` (pure Vulkan + std + GLM), keeping the engine reusable and testable.
  * **Layered Vulkan lifetime:** `VulkanContext` (instance/device/queues/VMA allocator) is created once; `VulkanSwapchain` (images, shared depth buffer, render pass, framebuffers) is the resolution-dependent layer rebuilt on resize; `VulkanRenderer` owns per-frame state and drives acquire → record → submit → present, with coalesced resize handling.
  * **Infinite floor grid:** Drawn as a single full-screen quad whose fragment shader reconstructs the world point on the `y=0` plane per pixel, with anti-aliased lines via `fwidth` derivatives, distance fade, and correct `gl_FragDepth` so scene geometry occludes it (`scene/grid.{h,cpp}` + `grid.vert/frag`).
  * **Orbit camera:** A pure-GLM orbit `Camera` with Vulkan-correct clip-space conventions (`scene/camera.{h,cpp}`). Drags drive the camera only when the press began inside the viewport, so opening a modal dialog (e.g. the Import file picker) over the native child window can't leak a button-held move and snap the view.
  * **Graceful degradation:** If `QVulkanInstance::create()` fails, the viewport falls back to an inline message instead of preventing the app from launching.
* **Viewport object context menu (right-click → Delete):** Right-clicking a model in the 3D viewport now picks the object under the cursor (a camera ray vs. each model's bounding box) and opens a context menu; its first action, **Delete**, removes that model from the scene. Deletion waits for the GPU to go idle before freeing the model's buffers/textures so it's safe mid-session. Picking is bounding-box level for now (fine for selecting whole imported objects).
* **Import menu:** **File → Import** is now a submenu enumerating the planned model formats — `.ABC`, `.BVH`, `.DAE`, `.FBX`, `.GLB`, `.OBJ`, `.PLY`, `.STL`, `.USD` — with only `.OBJ (Wavefront)` enabled today; the rest are disabled placeholders that will light up as each importer lands.
* **OBJ model import + textured mesh rendering path:** The first working importer — **File → Import → `.OBJ (Wavefront)`** opens a file dialog and loads the model into the 3D scene, lit by a directional key light + ambient fill, with its **diffuse texture (MTL `map_Kd`)** mapped on and tinted by `Kd` (geometry with no texture/material falls back to a light neutral grey, `#D1D1D1`), sitting correctly on the floor grid with depth. This stood up the entire mesh-rendering path the viewport previously lacked:
  * **GPU resources:** `VulkanBuffer` (`rendering/vulkanbuffer.{h,cpp}`) and `VulkanTexture` (`rendering/vulkanimage.{h,cpp}`, an sRGB sampled image with a generated mip chain) — move-only VMA RAII wrappers — plus `submitImmediate` (`rendering/vulkancommands.{h,cpp}`), the shared one-shot graphics-queue submit used for every load-time upload/transition.
  * **Lit, textured mesh pipeline:** A `Scene` (`scene/scene.{h,cpp}`) owning the mesh pipeline, a per-frame camera/lighting UBO (descriptor set 0), a shared set-1 layout + 1×1 white fallback texture, and the imported `Model` list; an interleaved `Vertex` (pos/normal/uv) format; per-draw push constants (`model` + `baseColor`); per-material diffuse texture in set 1 (`albedo = baseColor * texture`). `PipelineConfig` gained vertex-input, descriptor-set-layout, and cull-mode knobs. Two-sided shading (normal flipped per `gl_FrontFacing`) so inconsistent OBJ winding still reads correctly.
  * **Codec stays out of the core:** the Qt-free `loadObj()` (`scene/objloader.{h,cpp}`, over tinyobjloader) de-indexes geometry, generates smooth normals when absent, groups by material, and resolves `map_Kd` paths (including the quoted, space-containing absolute paths some exporters emit); the **Qt layer decodes the image files via `QImage`** and hands raw RGBA8 down — so the renderer needs no image-codec dependency yet supports every format Qt's plugins do. A texture that can't be found or decoded logs a warning and falls back to the base color rather than failing the import.
  * **Command-line / "open with":** any `.obj` path passed as a launch argument is imported on startup (queued until the renderer is ready), so the app works as a drag-onto-exe / file-association target too.
  * **Import progress indicator:** interactive imports show a modal, staged progress dialog (reading file → decoding textures → uploading to GPU), shown immediately and kept up for the whole import so large models no longer freeze the window with no feedback. The work is still synchronous on the GUI thread; the dialog repaints between phases.
  * **Faster import (batched GPU uploads):** a model's mesh buffers and textures are now uploaded through a single batched command submission (`ImmediateBatch`) instead of a separate submit + GPU fence-wait per buffer and per texture (~3 blocking round-trips per material group). On models with many material groups this is the dominant import cost, so this is a large speed-up. Trade-off: staging buffers for the whole model are held in memory until the one submit completes.
  * Non-diffuse PBR maps (normal/metal/rough) are a later step — OBJ doesn't natively carry them.
* **Build: Vulkan, GLM, VMA & tinyobjloader dependencies:** `CMakeLists.txt` now requires the LunarG Vulkan SDK (`find_package(Vulkan REQUIRED)` plus a check for the `glslc` compiler) and fetches GLM (`1.0.1`), AMD's Vulkan Memory Allocator (`v3.1.0`), and tinyobjloader (`v2.0.0rc13`) header-only via `FetchContent` at pinned tags — a fresh clone needs only Qt + the Vulkan SDK, nothing vendored. GLSL shaders under `src/viewport/shaders/` are compiled to SPIR-V via `glslc` and mirrored next to the executable on every build. CMake minimum raised to 3.21.

### Changed
* **"Add Asset Folder" remembers its location:** The folder browser now opens in the parent directory of the last asset folder you added (e.g. add `D:\PoseStudio\MyLibrary` and next time it starts in `D:\PoseStudio`), instead of always resetting to your home folder. Persisted in Preferences, so it survives restarts, and shared by both entry points (the Asset Manager and Preferences → Assets). Falls back to home if the remembered folder no longer exists.
* **Import remembers its location:** File → Import's file browser now opens in the folder of the last model you imported (persisted in Preferences, so it survives restarts) instead of always resetting to Documents. Falls back to Documents if that folder no longer exists.
* **Asset Manager ignores `.mtl` files:** Wavefront `.mtl` material libraries are sidecar files that belong to an `.obj`, not assets in their own right, so they're no longer listed as separate items in the asset grid (nor do they count toward a folder's asset hit-state / search results). Handled via a shared "ignored sidecar extensions" set, so more such types can be added later.
* **Double-click an `.obj` to import it:** Double-clicking a `.obj` asset in the grid now imports it into the 3D viewport (via the same path as File → Import and command-line "open with"), instead of opening it in the OS default application. Other file types still open in their default app, and double-clicking a folder still navigates into it.
* **Splash/About overlay is now a top-level window:** `SplashOverlay` was reworked from a child widget into a frameless, translucent top-level window owned by the main window and positioned over it. This is required because the new viewport is a native child window (`createWindowContainer`), which composites on top of ordinary overlay widgets — a child overlay would be hidden behind the viewport. As an *owned* window (rather than a globally always-on-top one) it stacks above PoseStudio's own window/viewport but still yields to other applications when they take focus.
* **Cross-platform foundation hardening:** With the renderer taking shape, started making good on PoseStudio's fully-cross-platform goal (Windows/macOS/Linux), fixing portability issues before they calcify. Note that only the Windows/MSVC build has actually been exercised so far — the macOS/Linux paths are correct by construction but not yet built or run.
  * **macOS Vulkan via MoltenVK:** Vulkan device creation now enables the `VK_KHR_portability_subset` device extension whenever the physical device advertises it. This is mandatory on MoltenVK — the Vulkan-on-Metal layer macOS/iOS run Vulkan through — and a no-op on desktop Windows/Linux drivers, so the renderer can bring up a spec-conformant device on Apple hardware.
  * **Windows-only icon resource guarded:** `resources/PoseStudio.rc` (the executable-icon resource) is now added to the build only under `if(WIN32)`. It's meaningless to the Clang/GCC toolchains used on macOS/Linux, where listing it unconditionally broke CMake configuration.
* **User database relocated to the per-user app-data directory:** `posestudio.db` now lives in `QStandardPaths::AppDataLocation` (e.g. `%APPDATA%/PoseStudio` on Windows, `~/Library/Application Support/PoseStudio` on macOS, `~/.local/share/PoseStudio` on Linux) instead of next to the executable — an installed app's own directory is read-only on macOS/Linux, and writing there also breaks code signing. A one-time *move*-migration relocates any existing exe-adjacent database into the new location on first launch (copy-then-delete) so existing libraries, collections, and favorites are preserved; Factory Reset additionally clears any lingering legacy file so it can't resurrect wiped data. The application name is now set at startup so the data path resolves deterministically.
* **Source tree reorganized by subsystem:** `src/` is now split into one folder per area — `core/` (database, preferences manager), `shell/` (menu bar, splash/About overlay, Fusion theming), `assets/` (Asset Manager), and `preferences/` (the dialog + every panel) — alongside the existing `viewport/`, extending the folder-per-subsystem layout the viewport introduced. Only `main.cpp` and the cross-cutting `constants.h` remain at the `src/` root. Each subsystem folder is on the compiler include path, so the existing bare-name `#include`s still resolve unchanged — a pure organizational move with no behavior change.

## [0.1.3] - 2026-06-23
### Added
* **Collection Asset Sorting:** Assets inside a Collection can now be manually drag-reordered, mirroring the behavior previously exclusive to Favorites. 
  * Added the `AssetCollectionItemSortOrder` column to the `AssetCollectionItems` table in `initialize.sql`.
  * Implemented an automatic database migration in `database.cpp` to seed existing rows with a stable initial order based on insertion order.
  * Collections now load in this manual order instead of alphabetically, and new items append to the end.
  * The info bar's "Sortable" label now dynamically appears while browsing a Collection.
* **Drag-and-Drop UX Enhancements:** * Replaced the per-item hover highlight with a precise, blue insertion drop-line indicator to clearly show where a dragged asset will land.
  * Implemented an edge auto-scroll feature that activates when dragging an asset near the top or bottom of the window to scroll to items above or below the fold.
  * Added a translucent ghost thumbnail that follows the cursor during a drag operation.
* **Asset Panel Info Bar:** Introduced a new dynamic info bar footer at the bottom of the asset panel. It conditionally displays "Assets: X", "Folders: X", and "Sortable" based strictly on the relevance of the currently viewed folder or collection.
* **Empty Node Dimming:** Implemented visual dimming (greyed-out text and empty folder icons) for directory tree nodes that contain no asset hits directly or within any of their descendant subdirectories.
* **Boolean Hit Cache:** Introduced a highly optimized, boolean-based hit cache (`directFolderHasHit`, `subtreeHasHit`) that automatically clears on model reset, supporting the new empty node dimming feature without the severe performance overhead of counting total assets.
* **Drag Asset Directly Onto Favorites/Collections:** Dragging an asset out of the grid and onto a tree node now files it there immediately — dropping on the Favorites root or any Collection node ADDs the asset there (or MOVEs it, with the source automatically removed, when dragged out of a Favorites/Collection view), and dropping it on the Collections header creates a new top-level collection and drops it straight into rename edit mode, exactly like the existing "New Collection" context-menu action. The target node lights up with the same highlight used for collection drag-reparenting while the drag is in progress.
* **Spring-Loaded Tree Expansion:** Hovering a dragged asset over a collapsed Collection node (or the Collections header) for a short dwell (`DRAG_AUTO_EXPAND_HOVER_MS`, 700ms) now auto-expands it, so a sub-collection can be reached as a drop target without first expanding the tree by hand.

### Changed
* **Drag-and-Drop Code Refactor:** Generalized the hand-rolled grid drag-reorder mechanism (ghost thumbnail, drop-line indicator, edge auto-scroll) to be shared seamlessly between Favorites and Collections. This is now gated behind a single `isSortableView()` check, with previously Favorites-specific methods renamed to generic equivalents (e.g., `beginFavoritesDrag` renamed to `beginGridDrag`, `persistGridOrder()`, etc.).
* **Folder-to-Collection Refactor:** Completely changed the behavior of adding a folder to a collection. Instead of creating a folder shortcut, it now automatically creates a new sub-collection named after the target folder, populated exclusively with the direct assets inside that folder (ignoring subfolders and their contents).
* **Library Root Sorting:** Updated the sorting logic for top-level library roots (e.g., Maquettes, My 3D Library) to sort alphabetically by folder name, rather than by their database insertion order.
* **Repository Sync:** Fast-forwarded the local `main` branch to match `origin/main`, pulling in recent upstream features including nested sub-collections, library empty-state, factory reset, dialog theming, and documentation updates.
* **Context Menu Performance:** Massively improved the performance of the "Expand Branch" context menu action by batching UI repaints (`setUpdatesEnabled(false)`) and leveraging Qt's native `expandRecursively()` method, preventing application freezes on large directory trees.
* **Database Migration Refactor:** Consolidated the repeated "check `PRAGMA table_info`, then `ALTER TABLE ADD COLUMN`" boilerplate in `database.cpp` into shared `tableHasColumn()`/`ensureColumn()` helpers (the latter also handles one-time seed queries for backfilling existing rows), so future additive schema changes are a single one-line call instead of a copy-pasted block.

### Fixed
* **Asset Sort Order Persistence:** Fixed a root-cause bug where `buildAssetHits` grouped results by folder and returned them in hash-iteration order, destroying the database's visual sort order. It now builds a path-to-hit map and emits results in the strict input order.
* **Drag-and-Drop Reshuffling:** Fixed an issue where the asset grid would live-shuffle incorrectly mid-drag. The original item now stays in place during the drag, and the layout remains stable until the drop target is computed upon release.
* **Info Bar Formatting:** Corrected asymmetric padding and set explicit vertical alignment to perfectly center the text within the new Info Bar footer, adding appropriate spacing before the "Sortable" indicator.
* **Search Result Folder Naming:** Fixed an issue where adding a folder to a collection directly from search results incorrectly saved the full breadcrumb path (e.g., `Characters / Female / Hair`) instead of isolating just the leaf folder name (`Hair`).
* **Header Breadcrumb Clipping:** Increased the right-side padding margin (`kMargin` adjusted to 65px) in the asset window header to guarantee that the breadcrumb asset count suffix does not get clipped off-screen when the window is resized to be very small.
* **Breadcrumb "Open" Action:** Right-clicking the asset window header's breadcrumb now only enables "Open" when the click actually targets a clickable ancestor folder link; right-clicking the bright current-folder segment (which isn't a link) now correctly leaves it greyed out instead of re-opening the folder you're already in.
* **Preferences Tab List Row Height:** Preferences dialog nav items now get an explicit row size hint instead of relying on QSS `::item` padding alone, fixing an oversized hover/selection highlight that bled into neighboring rows.

### Removed
* **Tree Node Hit Counts:** Completely removed the numeric asset "Hit" counts (previously displayed in parentheses) from tree nodes to clean up the UI and eliminate the expensive, synchronous recursive disk/database scanning operations that were slowing down the application.
* **Header Asset Count:** Removed the blue `(x)` numeric count suffix from the Asset Panel Header title to simplify the UI and defer to the new Info Bar footer.
* **Folder Shortcuts Mechanism:** Completely abandoned and removed all code associated with the old folder shortcut feature, including deleting the `AssetCollectionFolders` table from the SQL schema, removing it from migration scripts, and eliminating related context menu actions.

## [0.1.2] - 2026-06-21

### Added
- **Find In Library / Browse Folder for Collections & Search Results:** Folders and assets shown anywhere under Collections or Search Results (at any nesting depth) now surface "Find In Library" and "Browse Folder" at the top of their context menus, jumping to the item's real location in the physical library tree. Added a new `tree.png` icon for the action.
- **Nested Sub-Collections:** Collections can now contain other Collections, arbitrarily deep (e.g. `Characters > Female`, `Clothing > Shirts`). Any collection — top-level or nested — can hold its own asset items and folder shortcuts alongside further sub-collections. Added a "New Sub-Collection" action to each collection's context menu, and a "New Collection" action to the Collections root's context menu (`add-col.png`).
- **Full-Path Collection Picker:** "Add/Move/Copy To Collection" menus now list every collection by its full path (e.g. "Clothing / Shirts") instead of a bare name, so any nested collection is selectable directly from a flat list.
- **Factory Reset Database menu action:** Added Help → "Factory Reset Database..." with a confirmation dialog; on confirm it wipes and rebuilds `posestudio.db` from `initialize.sql` and relaunches the app.
- **"Add Asset Folder" empty-state link:** When no asset library is configured, a clickable "Add Asset Folder" link appears in the directory tree panel, opening a native folder picker and registering the chosen folder as a new library.
- **`sub-collection.png` icon:** All Collection nodes (top-level and nested) now render with a dedicated icon instead of sharing the physical-folder hit-state icons.

### Changed
- **Collection name uniqueness is now per-parent, not global:** "New Collection"/"New Sub-Collection" actions auto-generate a unique sibling name ("New Collection (2)", etc.) instead of silently reusing an existing collection of the same name under the same parent.
- **Dialog styling:** `QMessageBox`/`QDialog` now follow the app's dark theme (background, text color, button styling) instead of the default light Fusion appearance.
- **All icons replaced with Lucide Icons:** Every icon in `resources/icons/` has been re-sourced from the [Lucide](https://lucide.dev/) icon library for a more consistent visual language. Lucide will be the default icon source for new icons going forward.

### Fixed
- **Sub-collection renames not persisting:** The rename handler only wrote to the database for top-level collections; renaming a nested sub-collection updated the tree but reverted on restart. It now keys off the collection's own ID regardless of nesting depth.
- **Adding a folder to a Collection not persisting:** Root cause was that the `AssetCollectionFolders` table itself didn't exist in databases created before that table was added to the schema, so every insert was silently failing at the SQL `prepare()` step. Added a migration that creates the table (and its indexes) if missing.
- **Missing `AssetCollectionParentID` migration for existing databases:** Added an idempotent `ALTER TABLE` so existing installs gain the column needed for nested collections without requiring a factory reset.

### Removed
- **Unused icons:** Removed `favorites.png` and `favorite-item.png` (leftover from the removed Favorites feature) from `resources.qrc` and disk.

### Internal
- Added `CLAUDE.md`, project guidance for future Claude Code sessions covering build commands and architecture.

## [0.1.1] - 2026-06-19

### Added
- **Clickable Breadcrumb Navigation:** The asset panel's folder header is a real breadcrumb again — ancestor segments are clickable links (with hover underline) that jump straight to that folder. Segments are fit dynamically to the available header width, fitting as many trailing folders as possible and collapsing whatever doesn't fit (including the library name) into a single leading "...".
- **Folder Context Menu on Breadcrumb:** Right-clicking a breadcrumb segment (or the current folder) now offers "Open" and "Browse Folder", targeting whichever link is under the cursor.
- **Search Result De-duplication:** A search result that is itself a subfolder of another result already in the list is dropped, since it's already reachable by expanding that ancestor in the tree.
- **Search Result Path Truncation:** Long search result paths are capped to their last 4 segments, prefixed with "..." instead of showing the full path.
- **Folder Context Menu in Asset Grid:** Right-clicking a subfolder shown in the asset grid now offers Open, Add To Collection, Browse Folder, and Refresh — matching the functionality already available for individual assets.

### Changed
- **Breadcrumb Header Never Resizes the Window:** The folder header label no longer grows to fit long paths; it elides instead, so dragging the splitter or resizing the window is no longer fought by the label's content.
- **Tree Selection Reflects Current Folder:** Navigating away from a folder via a breadcrumb link, or by opening a subfolder from the asset grid, now clears the tree's selection/highlight instead of leaving the previous folder looking selected.
- **Two-Line Asset Labels:** Asset and folder names in the grid now word-wrap to two lines (eliding the second line with "..." if still too long) instead of being cut off after one line, with a deliberate, consistently-sized gap between the thumbnail and its label.
- **Asset Grid Row Spacing:** Added breathing room between grid rows via `QListWidget::setSpacing`.

### Removed
- **Combined View:** Removed the virtual "Combined View" tree node that aggregated subdirectories across all enabled asset libraries (`COMBINED_ROOT` / `COMBINED_DIR_*`). Folder navigation, search, and "Go to Folder" now operate solely against each library's own physical directory tree. Collections folder shortcuts pointing at virtual Combined View paths are no longer supported.

### Fixed
- **Grid Cell Sizing Drift:** `AssetGridDelegate`'s cell-size calculation and its actual paint logic had drifted out of sync (1.5 vs. 2 lines of text height), intermittently compressing the gap between a thumbnail and its label. The two are now derived from the same formula.

### Internal
- Project-wide cleanup pass: removed dead code left over from the now-deleted Favorites feature and other unused constants/functions, deduplicated repeated context-menu-building and icon-loading logic into shared helpers, moved a fully-inlined tree delegate implementation out of the header to restore the project's intended fast-compile structure, and rewrote stale or inaccurate comments throughout.

## [0.1.0] - 2026-06-16

### Added
- **Asset Search:** Added a persistent search bar (QLineEdit + clear button + search button) above the Collections node in the tree panel. Searching scans all enabled asset library directories recursively and populates a dedicated "Search Results" root node.
- **Search Results Tree:** Results are displayed as a proper navigable hierarchy — shared ancestor folders are merged so the full path context is preserved, not compacted into a single flat string.
- **Full Subtree Browsing from Search:** Matched folders in the search results are fully expandable via the existing lazy-load mechanism, allowing the user to browse the entire subtree beneath a hit without leaving the search results panel.
- **NoHit Filtering in Search:** Folders that contain no assets anywhere in their subtree (greyed "empty" folders) are automatically excluded from search results.
- **Search Visual Feedback:** While a search is running, a "Searching…" placeholder appears in the tree, the search controls are disabled, and the cursor changes to the system wait cursor. Results replace the placeholder atomically when complete.
- **Search Root Iconography:** The Search Results root node displays `search.png` when results are populated and `search-g.png` when empty, matching the grey/bright state of its text color.
- **Clear Search Button:** A dedicated clear button (`clear.png`) sits between the input and search button. Clicking it clears the input and collapses the Search Results node instantly.
- **Folder Shortcuts in Collections:** Users can now add any folder (physical library folder or Combined View virtual folder) directly to a Collection via the right-click context menu. The folder appears as a navigable child node under the collection, functioning identically to a folder shortcut.
- **"New Collection" Quick-Add:** All "Add to Collection" flyout menus (grid items and folder nodes) now have a "New Collection" entry pinned at the top with a separator below it. Clicking it creates a collection named "New Collection" (or reuses it if it already exists) and immediately adds the item.
- **Delete Collection:** Added a "Delete Collection" context menu item directly below "Rename Collection". The action is only enabled when the collection is completely empty (no asset items and no folder shortcuts), preventing accidental data loss.
- **Tri-State Folder Icons:** Folder nodes in the tree now use three distinct icons to communicate asset presence at a glance: `folder-full.png` (assets exist directly in this folder), `folder-hit.png` (assets exist in a subfolder), and `folder-empty.png` (no assets anywhere in the subtree).
- **Asynchronous Hit Detection:** Folder hit-state scanning is now fully deferred. On first paint, nodes display a bright default icon immediately. A `QPersistentModelIndex` queue processes one folder per event-loop tick via `QTimer::singleShot(0)`, emitting targeted `dataChanged` signals so icons and colors fill in progressively without blocking the UI.

### Changed
- **Collections Replace Shortcuts/Favorites:** The Shortcuts and Favorites systems have been removed entirely. Their functionality (grouping folders independently of physical location) is now unified under Collections, which supports both individual asset items and folder shortcuts in the same collection.
- **Alphabetical Collection Ordering:** Collection nodes are automatically sorted alphabetically on initial load, after a rename, and after a new collection is created.
- **Faster Tree Expansion:** Child nodes are now inserted via a single `item->appendRows(QList<QStandardItem*>)` call wrapped in `setUpdatesEnabled(false/true)`, replacing per-item `appendRow` calls to eliminate redundant `rowsInserted` signal emissions during expansion.
- **Database Cleanup:** Removed the unused `AssetFavorites` table and its associated index from `initialize.sql`.

### Fixed
- **Collection Staying Grey After Folder Add:** Adding a folder shortcut to a collection now correctly calls `invalidateAndRefresh` on the collection's cache entry, so its icon and color update immediately without requiring a manual refresh.
- **Combined View Folders in Collections (Not Found):** Virtual `COMBINED_DIR_` paths no longer trigger a filesystem existence check (which would always fail). The system now detects the `COMBINED_DIR_` prefix and uses library database queries to validate and display the folder correctly.
- **Search Results Root Greyed:** The Search Results node is now correctly bright when results are present. The proxy model evaluates `rowCount()` on the source item at paint time rather than attempting a `folderHitState` call on a non-filesystem path.

## [0.0.9] - 2026-06-06
### Added
- **Interactive Tooltips:** Implemented `CustomToolTip` class (inherited from `QWidget` with a `QLabel` child) to replace standard native tooltips. This allows for hoverable, interactive rich-text panels that remain open when the user moves their mouse over them.
- **Tooltip Grace Period:** Added custom `eventFilter` and `QTimer` logic to the thumbnail grid, providing a 400ms "grace period" for the user to move the mouse into the tooltip before it closes.
- **Context Menu Expansion:** Integrated dynamic "Add to Collection" sub-menu into the grid right-click context menu, featuring live database polling.
- **Smart "Browse Folder" Routing:** Added a highly efficient `QDirIterator` peek algorithm to virtual "Combined View" nodes, conditionally displaying a "Browse Folder" context action only if exactly one mapped physical folder actually contains files.
- **Double-Click Action:** Wired `QListWidget::itemDoubleClicked` signal to the open-asset logic, synchronizing double-click behavior with the context menu's "Open" action.

### Changed
- **UI Architecture:** Migrated all color definitions from hardcoded RGB values to centralized `Constants` in `constants.h` (e.g., `COLOR_ACCENT_BLUE`, `COLOR_THUMB_BG_START`, etc.), ensuring theme consistency across C++ and QSS.
- **Tooltip UX:** - Disabled OS-level tooltip animations (fade/slide) via `QApplication::setEffectEnabled` for a snappier, professional feel.
    - Implemented a custom `QProxyStyle` to override native `SH_ToolTip_WakeUpDelay` and `SH_ToolTip_FallAsleepDelay`, allowing for immediate resetting of delays when moving between items.
- **Context Menu UX:** Overrode native `PM_SubMenuOverlap` via custom `QProxyStyle` to introduce a 4-pixel visual gap between parent and child menus, overriding default native OS overlap rules.
- **QSS Modularity:** Transitioned widget-specific C++ stylesheet injections to a dedicated `_assetmanager.qss` file to prevent CSS specificity wars and isolate component styles.
- **Refactoring:** - Decoupled `AssetManagerWidget` logic from native tooltips to support custom, interactive floating widgets.
    - Updated `QListWidgetItem` data handling to securely store metadata (`Qt::UserRole` for paths and `Qt::UserRole + 1` for HTML content).

### Fixed
- **Rendering Artifacts:** Applied `Qt::WA_TranslucentBackground` to the custom tooltip and dynamically spawned sub-menus to eliminate rigid rectangular background artifacts around rounded CSS borders.
- **Selection Highlight Conflicts:** Prevented Qt from auto-tinting `QIcon` images upon selection, and resolved CSS specificity overrides to ensure a crisp, dark gray background with a blue accent border on selected items.
- **Memory/Build Issues:** - Resolved `QListWidgetItem` syntax errors by adding proper forward declarations in `assetmanagerwidget.h`.
    - Fixed CMake `AutoMoc` errors by correctly including `"assetmanagerwidget.moc"` in the C++ file for nested classes.
    - Fixed logic leak in mouse-move event tracking by introducing `activeToolTipItem` state tracking to prevent stuck tooltips.

## [0.0.8] - 2026-06-04
### Added
- **Virtual Collections System:** Implemented a database-backed "Collections" architecture, allowing users to group and organize 3D assets entirely independently of their physical hard-drive locations.
- **Dynamic QSS Properties:** Subclassed standard Qt UI components (e.g., `AssetTreeView`) to expose custom C++ `Q_PROPERTY` variables, successfully bridging pure CSS stylesheets with highly customized C++ `QPainter` drawing logic.
- **Context-Aware Right-Click Menus:** Built highly specific context menus that dynamically adapt based on the clicked node type (e.g., Root Nodes, Virtual Collections, Physical Folders). 
- **Hierarchy Protection:** Implemented intelligent tree-traversal logic to detect if a folder is a nested child of an existing Favorite, preventing redundant database entries.
- **Global UI Terminology Framework:** Abstracted core UI nomenclature (like "Favorites" and "Collections") into global constants (`constants.h`), establishing a robust foundation for future internationalization (i18n) and user-customization.
- **Contextual Iconography:** Integrated a full suite of custom icons (Expand, Collapse, Rename, Browse, Refresh) into the new context menus to improve navigation speed and visual hierarchy.

### Changed
- **Proxy Model Abstraction:** Upgraded the `AssetFolderProxyModel` to seamlessly route, cache, and serve both physical disk directory scans and SQLite database queries without the frontend grid renderer knowing the difference.
- **Inline Editor Geometry:** Overrode `updateEditorGeometry` in the item delegate to perfectly align Qt's native `QLineEdit` text box with custom-painted C++ text margins during renaming.
- **Root Node Protections:** Stripped native OS edit privileges (F2/slow double-click) from structural tree headers ("Favorites", "Collections") to prevent accidental renaming.
- **Open-Source Code Sweep:** Performed a massive cleanup of `assetmanagerwidget.cpp` and `main.cpp`, grouping internal logic and applying professional Doxygen-style blocks to assist new open-source contributors.

### Fixed
- **Fatal Startup Crash:** Resolved a silent Windows access violation (segfault) where the database was attempting to populate the UI grid before the widget pointers had been fully instantiated in memory.
- **Menu Separator Layout:** Fixed native Qt Fusion style quirks by explicitly targeting and padding `QMenu::icon` and `QMenu::separator` in the global `.qss` file.

## [0.0.7] - 2026-06-01
### Added
- **Asset Manager Grid UI:** Implemented a responsive `QListWidget` (Icon Mode) to visually display 3D assets and paired thumbnails.
- **Smart Recursive Scanning:** Added a UI toggle allowing users to choose between scanning only the root directory or recursively scanning all subfolders.
- **Non-Blocking Progress Dialog:** Integrated `QProgressDialog` into the scanning engine to prevent UI lockups during massive directory traversals. Includes a safe "Cancel" feature and dynamic text truncation for deep folder paths.
- **Procedural Thumbnail Rendering:** Engineered a custom `QPainter` canvas to mathematically bake a dark-theme gradient and layout padding directly into the image buffer, bypassing native CSS layout limitations.
- **High-Quality Image Scaling:** Thumbnails are now dynamically loaded into RAM and scaled using bilinear filtering (`Qt::SmoothTransformation`) to preserve aspect ratios without pixelation.
- **Alphabetical Sorting:** The Asset Library now automatically sorts discovered assets from A to Z prior to rendering.

### Changed
- **Main Boot Sequence:** Overhauled `main.cpp` to establish a clear, modular bootstrap sequence (Database -> Preferences -> Theming -> Layout). 
- **Safe Database Defaults:** Changed the `initDb` factory reset flag default to `0` to prevent accidental database wiping on standard launches.
- **CMake OS Agnosticism:** Removed hardcoded local Qt paths from `CMakeLists.txt` and added command-line instructions, ensuring the project builds cleanly across different contributor operating systems.
- **Grid Styling:** Replaced default Windows/macOS scrollbars with a custom dark-theme CSS implementation and added premium hover/selection states to the asset grid.

### Fixed
- **Text Padding Jitter:** Solved a known Qt layout bug where CSS `padding-top` shifted the entire thumbnail bounding box rather than the internal text.
- **Sorting Performance:** Relocated the `sortItems()` call outside the main scanning loop to prevent redundant UI recalculations and massively improve load times.
- **Application Exit Codes:** Corrected a bug in `main.cpp` where a failed database connection returned `false` (0/Success) instead of `-1` (Failure) to the operating system.

### Documentation
- **Open-Source Code Sweep:** Applied professional, Doxygen-style header comments to all core C++ files to explain architectural intent to future contributors.
- **File Architecture:** Documented the exact purpose and grouping of the Qt Resource (`.qrc`), Stylesheet (`.qss`), and SQL initialization files.

## [0.0.6] - 2026-05-31
### Added
- **PreferencesManager Singleton:** Implemented a fast, thread-safe memory cache for instant application-wide access to user settings without repetitive database querying.
- **AssetManagerWidget:** Created a standalone, modular C++ UI class for the upcoming 3D asset library, establishing a blueprint for future decoupled UI components.
- **Qt Resource Bundling:** Integrated `resources.qrc` to securely embed global stylesheets and database initialization scripts directly into the executable for guaranteed portability.

### Changed
- **Workspace UI:** Re-architected the main `QSplitter` layout to anchor the Properties and Assets tabs to the right side of the screen, adopting an industry-standard layout for 3D software.
- **Database Initialization:** Overhauled the SQLite startup sequence to dynamically target the active executable path, allowing for robust "factory reset" database regeneration.
- **Preferences Schema:** Upgraded the database schema to utilize a `UNIQUE` constraint on preference names, supporting safe, non-destructive `UPSERT` saves.
- **Global Styling:** Migrated all inline C++ styles into a global `.qss` file, utilizing targeted Object IDs for clean, scalable theme management.

### Fixed
- Resolved OS file-locking crashes during database regeneration by ensuring active connections are explicitly closed before file deletion.
- Corrected raw SQL insertion syntax in `initialize.sql` to properly escape strings and target correct tables instead of indexes.

## [0.0.5] - 2026-05-31
### Changed
- Refactored project structure to cleanly isolate source code within a dedicated `src/` directory.
- Modularized core functions to separate UI logic from initialization routines.

## [0.0.4] - 2026-05-30
### Added
- Created the main application splash screen overlay.

## [0.0.3] - 2026-05-30
### Added
- Built the foundational main application window instance.
- Implemented the initial top-level file menu architecture.

## [0.0.2] - 2026-05-29
### Added
- Created initial "Hello World" C++ Qt application package.
- Integrated SQLite core library into the application.
- Created initial database table `.sql` file and initialization function.

## [0.0.1] - 2026-05-24
### Added
- Initial project creation and repository initialization.
- Official PoseStudio project logo.
- Foundational `README.md` establishing the project vision as an open-source 3D character platform.
- Community funding configurations for GitHub Sponsors and Patreon.
- Core open-source community files including `LICENSE`, `.gitignore`, and `CODE_OF_CONDUCT.md`.
- Associated domain linkages for posestudio.org.
- Updated Code of Conduct.