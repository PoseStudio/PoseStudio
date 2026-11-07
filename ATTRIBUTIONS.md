# Attributions

PoseStudio's user interface and content build on the work of these projects. Thank you!

## UI Iconography — Lucide Icons

The application's UI icons are based on [Lucide](https://lucide.dev/), used under the ISC License:

> ISC License
>
> Copyright (c) for portions of Lucide are held by Cole Bemis 2013-2022 as part of Feather
> (MIT). All other copyright (c) for Lucide are held by Lucide Contributors 2022.
>
> Permission to use, copy, modify, and/or distribute this software for any purpose with or
> without fee is hereby granted, provided that the above copyright notice and this permission
> notice appear in all copies.
>
> THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
> SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
> THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY
> DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
> CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE
> OR PERFORMANCE OF THIS SOFTWARE.

## Stock HDRI Panoramas — Poly Haven

The stock lighting environments (equirectangular HDR panoramas and their preview images) are
**CC0** (public domain) assets from [Poly Haven](https://polyhaven.com/). No attribution is
legally required by CC0 — this credit is given with gratitude. Consider
[supporting Poly Haven](https://www.patreon.com/polyhaven).

## Third-Party Libraries

PoseStudio is built on the libraries below. None of their code is vendored into this
repository: CMake fetches each one at a pinned version at configure time, and the parts the
app uses are compiled into the executable. Each is used under its own license, summarized here;
the authoritative text is the license file in the library's own repository.

| Library | Used for | License |
| --- | --- | --- |
| [Qt 6](https://www.qt.io/) | Application framework, UI, SQLite access, networking | LGPL v3 (linked dynamically) |
| [Vulkan SDK](https://vulkan.lunarg.com/) (Khronos Vulkan loader) | The 3D viewport's graphics API | Apache 2.0 |
| [GLM](https://github.com/g-truc/glm) | Vector and matrix math | MIT (or the Happy Bunny License) |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | GPU memory management | MIT |
| [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) | The OpenXR loader behind VR Mode | Apache 2.0 |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | Wavefront `.obj` / `.mtl` import | MIT |
| [ufbx](https://github.com/ufbx/ufbx) | FBX rigged-character import | MIT or Unlicense (public domain) |
| [cgltf](https://github.com/jkuhlmann/cgltf) | glTF / GLB / VRM import | MIT |
| [LightUSD](https://github.com/lighttransport/tinyusdz) (formerly TinyUSDZ) | USD / USDA / USDC / USDZ import | Apache 2.0 |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing (figure import, project files, VRM metadata) | MIT |
| [miniz](https://github.com/richgel999/miniz) | gzip / zlib inflate (figure files, `.spz` splats, `.exr`) | MIT |
| [stb_image](https://github.com/nothings/stb) | Radiance `.hdr` panorama decoding | MIT or public domain |
| [tinyexr](https://github.com/syoyo/tinyexr) | OpenEXR `.exr` panorama decoding | BSD 3-Clause |

LightUSD carries third-party code of its own (among them LZ4, Zstandard, pugixml, fast_float,
Dragonbox, nanoflann, xxHash and MikkTSpace), each under its own permissive license; its
[LICENSE.3rdparty](https://github.com/lighttransport/tinyusdz/blob/main/LICENSE.3rdparty)
lists them. PoseStudio builds it with every optional feature off, so the parts that come with
those features (OpenSubdiv, MaterialX) are not in the build.

The release package also ships the Qt runtime, the Microsoft Visual C++ runtime and the Khronos
Vulkan loader next to the executable. Their license texts, and those of every library above
(LightUSD's bundled code included), are in the package's `licenses/` folder, indexed by
`NOTICES.txt`; in the repository they live in [packaging/licenses/](packaging/licenses/).
