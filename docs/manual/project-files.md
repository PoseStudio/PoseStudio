# Project Files

A project file (`.pss`) saves the whole scene — every figure with its pose and position, the environment, and the camera — as one small document you can reopen later or share. It is the way to keep a work in progress: close the app, come back, open the project, and the scene is exactly where you left it.

## Saving and opening

- **File → Save** (`Ctrl+S`) writes the scene to the project's file. The first save (and every save after **Save As**) asks for a place; afterwards it reuses the remembered path. If no path has been set, Save behaves as Save As.
- **File → Save As…** (`Ctrl+Shift+S`) picks a new `.pss` location and becomes the project's file from then on. The dialog opens in the folder you used last time; `.pss` is added to the name if you leave it off.
- **File → Open** (`Ctrl+O`) loads a `.pss` project. Everything currently in the scene is replaced by the project's content.

A save is tiny — kilobytes, not megabytes: the file stores references and numbers, never geometry or textures. Opening a project re-imports the figure files it names through the normal import pipeline and restores their poses, transforms, [material edits](materials.md) (the zones whose material differs from the import, by zone name — every value, and each texture slot that was replaced by a file or cleared; a replaced map is referenced by its path, like the figure file itself), the environment (HDRI and every lighting dial) and the camera framing on top.

## If a figure file has moved

A project names each figure's source file by path. If one of those files is missing when you open — moved, renamed, on another machine — PoseStudio asks for each missing file:

- **Choose File…** re-points the entry at the file you pick;
- **Skip** leaves that figure out and opens the rest of the project (a warning names what was skipped);
- **Cancel** aborts the open entirely.

The scene you had stays untouched until every referenced file is found or explicitly skipped, so a cancelled open never costs you anything.

## Unsaved changes

PoseStudio tracks whether the scene has changed since the last save — posing, moving a figure, editing a material, importing, deleting, and changing the environment all count; moving the camera does not (see [Preferences](preferences.md) to make it count). When there are unsaved changes, **New**, **Open** and closing the window ask first:

- **Save** writes the project and continues;
- **Discard Changes** discards the changes and continues;
- **Cancel** keeps you where you are.

## The file format

The file is versioned JSON and can be read by hand:

```json
{
  "format": "posestudio.project",
  "version": 1,
  "figures": [
    {
      "source": "C:/figures/hero.duf",
      "transform": {
        "translation": [0.0, 0.0, 0.0],
        "rotation": [0.0, 0.0, 0.0, 1.0],
        "scale": [1.0, 1.0, 1.0]
      },
      "rootBone": "hip",
      "rootTranslation": [0.0, -0.31, 0.12],
      "pose": [
        ["lShldrBend", -40.0, 0.0, 3.2],
        ["rShldrBend", -40.0, 0.0, -3.2]
      ],
      "pins": ["lHand"]
    }
  ],
  "environment": {
    "hdri": "C:/hdris/studio.hdr",
    "settings": { "exposure": 0.65, "keyIntensity": 1.25 }
  },
  "camera": {
    "target": [0.0, 1.0, 0.0],
    "yaw": 0.61,
    "pitch": 0.21,
    "distance": 4.2,
    "ortho": false
  }
}
```

Each figure row carries its pose the same way a `.pose` file does (bone rotations, the root's `@trans:` offset, pins), plus its transform — translation, rotation as a quaternion `[x, y, z, w]`, scale. The camera's `yaw` and `pitch` are in radians. Figure and panorama paths are absolute: a project opened on another computer, or after the files were moved, asks where each one is (above). Unknown fields are ignored on load, so files written by a newer version open in an older one; a file from a newer major version is rejected with an explanation rather than guessed at. A damaged file — bad JSON, a missing section, a non-numeric value — is rejected whole: the current scene is never half-replaced.
