# Releasing PoseStudio (maintainer guide)

This describes how downloadable Windows builds are produced and published. The short version: **push a `v*` tag and CI does everything else.**

## Cutting a release

1. **Bump the version** in `CMakeLists.txt` (`project(PoseStudio VERSION x.y.z …)`) and add the `## [x.y.z]` section to `CHANGELOG.md`. Merge to `main`. (The title bar, the About overlay and the install ping all read `Constants::APP_VERSION`, which CMake stamps from that `project()` line — there is no second copy of the number to edit.)
2. **Tag and push:**

   ```bash
   git tag v0.3.9
   git push origin v0.3.9
   ```

3. **Watch the [Actions tab](https://github.com/PoseStudio/PoseStudio/actions)** — the *Build Release* workflow takes roughly 15–20 minutes. When it finishes, the release is live on the [releases page](https://github.com/PoseStudio/PoseStudio/releases) with:
   - `PoseStudio-x.y.z-Windows-Setup.exe` — the Inno Setup installer (per-user, no admin needed)
   - `PoseStudio-x.y.z-Windows-Portable.zip` — the same build as a no-install folder
   - `SHA256SUMS.txt` — checksums for both
   - Release notes: a fixed "how to install" header (from `packaging/release-notes-template.md`) plus an auto-generated commit/PR list. **Edit the release afterward** to add human highlights for the version — the CHANGELOG section is a good source.

The tag must match the CMake version (tag `v0.3.9` ↔ CMake `0.3.9`) — the installer takes its version from the tag, and the two should agree with the CHANGELOG per its versioning note.

### Testing the pipeline without releasing

Run the workflow manually: **Actions → Build Release → Run workflow**. A manual run builds the installer and zip and uploads them as *workflow artifacts* (downloadable from the run page) but publishes nothing. Do this after any change to the workflow, the Inno script, or a Qt/Vulkan version bump.

## What the workflow does

`.github/workflows/release.yml`, on a `windows-latest` runner:

1. Installs Qt (version pinned in the workflow's `QT_VERSION` env — includes the `qtimageformats` module for webp/tiff thumbnail support) and the latest LunarG Vulkan SDK (silent install; CMake finds it via its `C:/VulkanSDK/*` fallback glob).
2. Builds Release with CMake/MSVC, passing the install-ping signing key from the `POSESTUDIO_PING_KEY` repository secret (see below). The existing post-build steps mirror `shaders/` and `Maquettes/` next to the exe.
3. Stages a deployable folder: exe + shaders + Maquettes, then `windeployqt --release --no-translations --no-compiler-runtime` adds the Qt runtime and plugins (platforms, sqldrivers, imageformats, styles, and `tls` — the Schannel backend the install ping's HTTPS request needs; check it is present in the staged folder whenever the Qt version changes). Then the **Visual C++ runtime**, app-local: every staged binary is scanned with `dumpbin /dependents` and exactly the `msvcp140*`/`vcruntime140*` DLLs they import are copied from the same Visual Studio's `VC\Redist\MSVC\<ver>\x64\Microsoft.VC14x.CRT` — windeployqt's `--compiler-runtime` was a no-op on the runner (no `VCINSTALLDIR`) and would only have yielded `vc_redist.x64.exe`, an admin-rights installer the per-user setup can't run; the UCRT is part of Windows 10/11 and isn't shipped. Then the **Vulkan loader**: `vulkan-1.dll` is copied from the runner's `System32` — the SDK installer's VulkanRT helper installs it there, version-matched to the SDK the app was built against — together with its license as `licenses\VulkanRT-License.txt` (`packaging/licenses/`). The exe imports the loader, and a machine whose display driver has no Vulkan support has none; without a bundled copy Windows refused to start the app before `main()`, so such users never saw the viewport's "no Vulkan" fallback and never sent an install ping. The step fails if the DLL is missing or isn't the Khronos loader; should a future SDK stop installing the runtime, take `x64\vulkan-1.dll` from LunarG's `https://sdk.lunarg.com/sdk/download/latest/windows/vulkan-runtime-components.zip` instead (it carries the same license file). Last, the **license texts**: the repo's `LICENSE` (GPL v3) goes next to the exe as `LICENSE.txt`, and `packaging/licenses/*.txt` into `licenses\` — LGPL v3 for Qt, the ICU and Mesa notices for the runtimes windeployqt ships beside it, the Vulkan loader's, the licenses of the open-source libraries compiled into the exe (GLM, VMA, the OpenXR loader with its JsonCpp, tinyobjloader, ufbx, cgltf, LightUSD with `LightUSD-third-party.txt` for the code it carries, nlohmann/json, miniz, stb, tinyexr), and `NOTICES.txt`, the index, rendered with `QT_VERSION` (and its major.minor) so it names the exact Qt shipped and its source archive. LGPL v3 §4 asks for that prominent notice, the LGPL and GPL texts, and a way to relink against a modified Qt — the separate Qt DLLs are that mechanism — and Help → About Qt (`QApplication::aboutQt()`) is the runtime notice §4(c) asks for. Add a file to `packaging/licenses/` (and a line to `NOTICES.txt`) whenever a new redistributable joins the package or a new library is compiled into the exe.
4. Downloads the **stock HDRI content pack** (see below) — the installer places it in the user's `Documents\My PoseStudio Library\hdri`; the portable zip carries it as `stock-content\hdri` with copy-me instructions.
5. Compiles `packaging/PoseStudio.iss` with Inno Setup (preinstalled on the GitHub runner image; the workflow falls back to `choco install innosetup` if it is missing), zips the portable variant, generates `SHA256SUMS.txt`, and publishes the GitHub Release. Only a tag build carries the install-ping signing key — a manual pipeline-test build ships keyless, so downloading and running its artifacts never counts as an install.

## The render engine (not in the release pipeline yet)

The Cycles render engine (`cmake/CyclesEngine.cmake`, `engines/cycles/`) is an opt-in part of the
build — `-DPOSESTUDIO_WITH_CYCLES=ON` — and **the release workflow does not build it yet**: a
released PoseStudio says "this copy has no render engine" under Render → Render Image. Shipping
it means, in the workflow and in `packaging/`:

- **Building it**: git-lfs on the runner (it has it), `-DPOSESTUDIO_WITH_CYCLES=ON` at configure
  — the fetch is about 0.6 GB, plus the CUDA compiler archives — and time for Cycles and its GPU
  kernels (a `cache` step keyed on the Cycles tag for `build/_deps/cycles-*` is worth its setup).
  Decide the kernel list (`POSESTUDIO_CYCLES_CUDA_ARCH`): the default covers the NVIDIA
  generations CUDA 13 supports, Turing and later; older cards need kernels built with an older
  CUDA release, which wants an older MSVC than the newest.
- **Staging it**: the mirrored `engines/cycles` folder goes into the package as it is (the app
  finds it next to the executable). It is about 160 MB; the debug-suffixed DLLs and the
  `api-ms-win-*` forwarders Cycles's install rules add can be pruned. The VC-runtime scan must
  read the engine's binaries too.
- **Licenses**: a `licenses/` entry and a `NOTICES.txt` section for Cycles (Apache 2.0) and for
  every precompiled library the engine folder carries — the list is in ATTRIBUTIONS.md; Blender's
  own `release/license` folder has the texts as Blender ships them.
- **NVIDIA's terms — releases ship the OptiX device (decided 2026-11-12).** The CUDA compiler
  is used at build time only (its redistributable archives; nothing of it is shipped but what a
  compiled kernel contains). The OptiX device is compiled against the OptiX SDK's headers, which
  are under NVIDIA's own license (the LICENSE file of `NVIDIA/optix-dev`, "Software Developer
  Kits, Samples and Tools License Agreement (with distribution rights)") — not an open-source
  one. Two sides to it:
  - *PoseStudio's side (GPL v3)* is settled by **`LICENSE-EXCEPTION.txt`**: an additional
    permission under section 7 to combine PoseStudio with NVIDIA's OptiX and CUDA software. It
    is the copyright holders' to grant: contributions come in under the CLA, which gives the
    project the right to relicense them — **check that every contributor with merged code has
    signed it** before the first release that carries the engine. The file ships next to
    `LICENSE.txt` in the package (the stage step copies it; the installer's license page is the
    GPL's).
  - *NVIDIA's side* is the distributor's to meet, release by release:
    **(1) notify NVIDIA before the first release — DONE, filed 2026-11-12** (section 1.5
    requires notification before use in a commercial application; PoseStudio is free, but the
    project reserves commercial licensing). The web form at `developer.nvidia.com/sw-notification`
    was submitted for PoseStudio with "NVIDIA OptiX SDK 9.0.0 (used through the OptiX device of
    the Cycles 5.3.0 renderer); CUDA Toolkit 13.5.0 (build-time compiler for the GPU kernels)"
    and a planned release date of December 1, 2026; the confirmation is kept with the project's
    records. File it again if the engine moves to another OptiX major version. **(2) Ship NVIDIA's license text** in
    `licenses/` with a `NOTICES.txt` section saying that the engine's OptiX device incorporates
    NVIDIA software, in binary form, for use on NVIDIA hardware, under that agreement — section
    1.1(ii) allows distribution in binary form only and asks that recipients be bound to terms
    consistent with it. **(3) Never ship the OptiX headers or SDK themselves** (the build fetches
    them; they are not in the repository and not in the package). Section 2.1(xii) forbids use
    that would make the SDK "subject to an Open Source License": the additional permission above
    exists so that PoseStudio's license makes no such claim on it. Blender ships the same OptiX
    device in its GPL builds.
  `-DPOSESTUDIO_CYCLES_OPTIX=OFF` remains the way to build without it (CUDA and the processor
  remain); such a build needs none of the above. This is a maintainer's reading, not legal
  advice — have counsel look at it once.

Use a `workflow_dispatch` run to prove the pipeline with the engine before a tagged release has it.

## The install-ping signing key

The app sends an anonymous install ping on every launch (`src/core/installping.h`), signed with an HMAC key that must never be committed. It reaches the build as the repository secret **`POSESTUDIO_PING_KEY`** (Settings → Secrets and variables → Actions), which the workflow passes to CMake as `-DPOSESTUDIO_PING_KEY=…`. Generate it once with `openssl rand -hex 32` (keep it to hex — it travels through a CMake compile definition) and use the same value as the server's verification key. If the secret is missing the workflow warns and the build ships with the ping disabled (an empty key never sends), so forks never ping. Rotating the key means updating the secret and the server together, then cutting a release — older builds keep signing with the old key and are rejected, which is the intended way to retire them from the count.

## The stock HDRI content pack

The 33 stock lighting environments (CC0, from [Poly Haven](https://polyhaven.com/), ~50 MB with their `.jpg` previews) are deliberately **not committed to the repo** — they'd bloat every clone forever. Instead they live as an asset on a dedicated, clearly-labeled pre-release:

- Tag: **`stock-content-v1`** — asset `PoseStudio-Stock-HDRIs-v1.zip`
- The zip's root contains the category folders (`Studios/`, `Nature/`, …) exactly as they should appear under `My PoseStudio Library\hdri`, plus an `ATTRIBUTION.txt`.
- It's marked *pre-release* so it never shows up as "Latest" to users.

**To update the pack:** build a new zip with the same internal layout, publish it as `stock-content-v2` (`gh release create stock-content-v2 --prerelease --title "Stock Content Pack v2 (build asset — not the app)" PoseStudio-Stock-HDRIs-v2.zip`), and bump `STOCK_CONTENT_TAG` in the workflow. Keep the old release around so old workflow refs stay reproducible. Keep it CC0-only content, and note additions in `ATTRIBUTIONS.md` if the source changes.

## The installer

`packaging/PoseStudio.iss` (Inno Setup 6.3+). Key decisions, so they aren't accidentally undone:

- **Per-user install** (`PrivilegesRequired=lowest` → `%LOCALAPPDATA%\Programs\PoseStudio`): no UAC prompt, friendliest for testers.
- **`AppId` is a fixed GUID — never change it**, or upgrades will stop replacing existing installs.
- Stock HDRIs install to `{userdocs}\My PoseStudio Library\hdri` with `onlyifdoesntexist uninsneveruninstall`: a file the user replaced is never clobbered, and **uninstall never touches the user's library**.
- No file associations are registered — `.duf`/`.obj` belong to other tools too; hijacking them would be hostile.

## Building the installer locally (optional)

CI is the source of truth, but for debugging: build Release, run `windeployqt --release --no-translations --no-compiler-runtime` on a staged copy of the exe (+ `shaders/`, `Maquettes/`, the imported `Microsoft.VC14x.CRT` DLLs, `vulkan-1.dll` from `System32`, `LICENSE.txt`, and `licenses\` from `packaging/licenses/`), download/unzip the stock pack, then:

```powershell
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" `
  /DAppVersion=0.0.0-local /DAppDir=<staged app dir> /DHdriDir=<hdri dir> /DOutDir=<out dir> `
  packaging\PoseStudio.iss
```

## Code signing (future)

Builds are unsigned, so users see the SmartScreen "Windows protected your PC" warning (the docs walk them through it). When download friction matters more than it does today, the cheapest legitimate path is **Azure Trusted Signing** (~$10/month); signing would slot into the workflow between the Inno step and publishing (sign both the exe inside the staging dir and the final Setup.exe). Until then, `SHA256SUMS.txt` + public CI builds are the integrity story.
