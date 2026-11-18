# .pss project file check

A standalone guard for the `.pss` project file codec (`src/viewport/scene/projectfile.cpp`).

## PoseStudioProjectFileCheck (`roundtrip.cpp`)

Round-trip and rejection checks for the file-format contract — 47 checks, every file they write
in the OS temp dir (`pss-rt/`, created up front):

- **The round trip.** A fully-populated `ProjectDocument` — a posed figure with a pin, a root
  translation, a transform and a material override (every value, a chosen and a cleared texture
  slot, three overlays, the smoothing, Flip Normal), a second figure at its defaults, an
  environment, four lights and a camera — is written, read back and compared field by field,
  bit for bit.
- **Rejections.** Bad JSON, a wrong format marker, a higher version and a missing file are
  refused; a file with no sections reads as the defaults.
- **Forward compatibility.** Unknown fields at every level are ignored, an unknown texture slot,
  overlay blend or light kind is read past, absent values keep their defaults; a malformed
  overlay or light, or lights that are no list, reject the file.
- **A name that is not valid UTF-8** (a zone or a bone named in another encoding) is written with
  the replacement character, and the file still reads.
- **The keys a zone names.** The reader records them, and a project from an older build, which
  names fewer values, leaves the ones it does not name at the re-imported zone's own.
- **Value ranges.** A number past its type's range (1e39 for a float) rejects the file; one
  outside the range its value can take is held to that range, and one inside it that the tab's
  rows do not reach (a default, an importer's value) is kept.

It compiles only the codec — `scene/projectfile.cpp` with the material's own (`materialjson.cpp`,
`materialparams.cpp`), over the header-only GLM and nlohmann/json — no Qt, no Vulkan — so it runs
anywhere the app builds. It is declared in `cmake/Tools.cmake` and registered with CTest:

```
ctest -C Release -R project-file-check
```

Exit code = number of failed checks (0 = green).

## The window's side is checked in the application

What the codec alone cannot show — a scene saved by the window, emptied with File → New and
opened again: the figure re-imported, its pose and pins restored, the document's
unsaved-changes flag, a bad file refused with the scene left intact — is driven in the real
application by the scripted check `tools/ikscripts/check_project.txt` (see
`tools/ikscripts/README.md` for how the scripts are run). A figure that comes back wrong from a
`.pss` file is isolated with the two together: this check says whether the file holds what was
saved, the script whether the window restores what the file holds.
