# Lights

The [Environment](environment.md) tab lights the scene with a panorama and one key light. The **Lights** tab adds lights of your own to it: a lamp beside the figure, a spotlight from above, a softbox for a portrait, a second sun. Each is an object in the scene — it stands somewhere, shines somewhere, and has a color and a brightness — and you can have as many as the picture needs.

Lights are part of the scene like the pose is: every change is an undo step (`Ctrl+Z`), and a [project file](project-files.md) saves them.

## The four kinds

| Kind | What it is | Use it for |
| --- | --- | --- |
| **Point Light** | Shines every way from one point. | A bulb, a candle, a glow inside the scene. |
| **Spot Light** | A point light held to a cone. | A stage light, a torch, a lamp with a shade — light where you aim it and nowhere else. |
| **Area Light** | A rectangle that shines from one face. | A softbox or a window: broad, soft light and soft shadows. |
| **Sun** | Light from one direction, the same everywhere, however far. | Daylight; a strong light from far away. Where it stands does not matter, only where it shines. |

## Adding a light

Choose **Add Light** on the Lights tab, or **Edit → Add Light**, and pick a kind. The new light stands two metres from the figure, above it and aimed at it, on your side of the scene — the first one a little to the left of where you are looking from, each further one a step round from the last, so three lights added in a row already stand around the figure. It is selected, so the tab shows its settings at once.

The *subject* a light is placed around, and aimed at, is the figure you are posing — with several figures, the one whose joint you clicked last. In a scene without a figure it is everything in the scene.

## Lights in the viewport

Every light draws a small **marker** where it stands: rings for a point light, a cone for a spotlight, a rectangle with a line showing which way it shines for an area light, a disc with rays for a sun. Markers are yellow; the selected light's is orange, and a light that is switched off is grey. Markers are never part of a render.

| Gesture | What it does |
| --- | --- |
| Click a marker | Selects the light. The Lights tab shows it. |
| Drag a marker | Moves the light, in the plane facing you. Turn the mouse wheel while dragging to move it nearer or farther. |
| Right-click a marker | **Aim at Subject**, **Switch Off** / **Switch On**, **Delete Light**. |
| `Delete` | Deletes the selected light. |
| `Esc` | Deselects it. |

**A light that is aimed at the figure stays aimed at it while you drag it.** Carry a spotlight around the figure and it goes on lighting her from each new place. A light you have turned to shine somewhere else keeps its direction when it is moved; **Aim at Subject** turns it back to the figure.

A drag is one undo step, however far it went.

## The Lights tab

The picker at the top lists the scene's lights and chooses which one the tab shows — the same selection as a click on a marker. **Add Light** adds one; **Delete Light** removes the one shown.

Every value is a scrub field: drag across it, hold **Shift** for fine steps, or click and type (see [Fine control](interface.md#value-fields-and-fine-control)). The small button beside a row returns that setting to its default.

| Setting | Kinds | Range | What it does |
| --- | --- | --- | --- |
| **Name** | all | | The light's name, as the picker lists it. |
| **Kind** | all | | Changes the light into another kind, where it stands. |
| **Switched on** | all | | Off, the light lights nothing and keeps its place and settings. |
| **Color** | all | | The light's color. Click the swatch to choose one. |
| **Intensity** | all | 0 – 100 | How bright it is — see below. |
| **Radius (cm)** | point, spot | 0 – 200 | The size of the source. A larger source casts softer shadows in a render; it is no brighter. |
| **Cone (°)** | spot | 1 – 179 | How wide the cone of light is, edge to edge. |
| **Edge (%)** | spot | 0 – 100 | How much of the cone its edge fades over: 0 is a hard-edged circle of light, 100 fades from the middle out. |
| **Width (cm)**, **Height (cm)** | area | 1 – 1000 | The rectangle's size. A larger light casts softer shadows in a render; the subject is no brighter. |
| **Size (°)** | sun | 0 – 45 | How large the sun is in the sky. A larger one casts softer shadows in a render. |
| **Casts shadows in a render** | all | | Off, the light lights without casting shadows — a fill light that adds no second shadow. |

The **Placement** group says where the light stands and where it shines:

| Setting | Range | What it does |
| --- | --- | --- |
| **Left-Right (cm)** | ±5000 | Where it stands: toward the figure's left (positive) or right. |
| **Up-Down (cm)** | 0 – 5000 | How high it stands over the floor. |
| **Back-Forward (cm)** | ±5000 | Where it stands: in front of the figure (positive) or behind. |
| **Turn (°)** | ±180 | Which way it shines, about the vertical. 0 shines toward the back of the scene — at a figure from her front. |
| **Tilt (°)** | ±90 | How far below the horizon it shines: 0 is level, 90 straight down, negative values shine upward. |
| **Aim at Subject** | | Turns the light to shine at the figure from where it stands. |

A point light shines every way, so it has no Turn or Tilt. A sun has them and no use for its position: its marker is only a handle.

## How bright a light is

**Intensity** is how brightly the light lights a surface that faces it **one metre away**, on the same scale as the Environment tab's key light: a light of intensity 1.25, a metre from the figure, lights her as the key light does at its default.

Light spreads as it travels. Twice as far away, a point, spot or area light lights a quarter as much — which is why a new light, two metres from the figure, starts at 4. Move a lamp closer and it gets brighter, quickly; move it away and raise its intensity to keep the same light with softer fall-off across the figure.

A **Sun** does not fade with distance: its intensity is its brightness everywhere, and it starts at 1.

A larger source — more Radius, a bigger area light — is not brighter. It is *softer*: the same light, arriving from more directions, with softer-edged shadows in a render.

## What the viewport shows, and what a render adds

The **PBR Shaded** mode lights the scene with your lights at once, so you can place them by eye: their color and brightness, the fall-off with distance, a spotlight's cone and its edge, the one face an area light shines from, and their highlights on skin and metal. Two things it leaves to the renderer:

- **Shadows.** The PBR Shaded mode draws shadows for the Environment tab's key light only. Your lights light the figure there without casting shadows.
- **Soft light and bounced light.** The size of a source, and the light that surfaces pass on to each other, show only in a path-traced picture.

To see the lights as they will render — with their shadows — switch the shading picker to **[Rendered](shading.md#the-rendered-mode)**, which path traces the scene in the viewport and follows every change you make, or make a [render](rendering.md).

The PBR Shaded mode lights the scene with up to **eight** lights — the first eight in the list that are switched on. The Rendered mode and a render use them all. The other shading modes (Texture Shaded, Cartoon Shaded, the form studies) do not use light objects; they show the markers and nothing else.

## Tips

- **A three-light portrait.** Add an area light: it stands to the front and left, the *key*. Add a second and lower its intensity to a third of the first: the *fill*, on the other side. Add a third, drag it behind the figure and raise it: the *rim*, which draws the edge of her hair and shoulder. Then turn the Environment tab's own key light down, or off (Intensity 0), so your lights are the ones doing the work.
- **A softer face.** Make the key an area light and make it bigger; bring it closer and lower its intensity.
- **A pool of light.** A spot light from above with a narrow cone and a soft edge; lower the Environment's **Diffuse** dial so the rest of the scene falls away.
- **Colored light.** Two lights of different colors from opposite sides — a warm one and a cool one — model a figure more than any one white light.
- The lights are on the same undo history as poses and the Environment dials: `Ctrl+Z` after a lighting change undoes that change, not your last pose edit.
