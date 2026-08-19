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

The build-time library dependencies (Qt 6, Vulkan SDK, GLM, Vulkan Memory Allocator,
tinyobjloader, nlohmann/json, miniz, stb_image, tinyexr) are listed with links in the
[README's Tech Stack section](README.md#tech-stack--resources). Each is used under its own
license; none of their code is vendored into this repository — CMake fetches them at pinned
versions at configure time.
