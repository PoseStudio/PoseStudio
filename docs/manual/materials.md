# Materials

The **Materials** tab on the side panel edits the surfaces of the selected object: a figure's skin, eyes, mouth and nails, an imported prop's materials. Every object imports with the materials its file describes, and this tab lets you change them with sliders, color swatches and texture slots — glossier skin, a metallic prop, a color tint, a see-through surface, a different map — without writing a shader. It is also where images are layered over the skin: [tattoos, marks and makeup](#overlays-tattoos-marks-and-makeup).

The **Save** button at the top of the tab saves the material you are looking at to a file, and the tab below it, **Materials Library**, lists the materials you have saved — see [Saving and reusing a material](#saving-and-reusing-a-material).

PoseStudio has **one material model**: a physically based surface in the metallic / roughness convention, the parameter set the industry's renderers share (the base layer, a coat, transmission, subsurface, sheen, emission, a thin film, and the maps that paint them). The viewport's PBR shading draws nearly all of it — the base layer, the coat, sheen, the thin film, transmission, emission, the detail layer and the maps that paint them; what it cannot (displacement, the subsurface's scattering color and distance) is kept with the material for the rendering engines to come, and the tab says which is which. There is no node editor and no second workflow to choose: files authored in the older specular / glossiness convention are converted when they are imported (see [Importing Content](importing.md#materials-and-the-specular--glossiness-conversion)).

## Selecting an object and a zone

Click an object in the viewport to select it — the blue outline shows which — and the tab shows its name and its **zones**. A zone is one material group of the object: a figure has a dozen or more (Torso, Face, Lips, Legs, Irises, Cornea, Teeth …), a prop has one per material in its file. The **Zone** picker lists them; pick one and the sections below show its material.

Clicking a surface selects its zone. Click the figure's cheek and the tab shows **Face**; click her thigh and it shows **Legs**; click the eye and it shows the sclera or the iris (the cornea is clear, so the click lands on what is behind it). The same click selects the joint under the cursor for posing, so the two tabs follow one gesture.

Nothing on the tab applies to a Gaussian splat scene: a capture carries its own color and light and has no material to edit.

## Editing matching zones together

A figure's skin is several zones that import with one and the same material — the preset applies its skin shader to the torso, the face, the arms, the legs, the ears and the lips alike. **Edit matching zones together** (on by default) applies every value change to all the zones that were imported with the same material as the one you are editing, so lowering the skin's roughness on the torso lowers it on the face and the limbs too. The note under the box says how many other zones follow. The eyes, the mouth and the nails have materials of their own and are not touched. Texture slots are always the zone's own: a map chosen for the torso goes on the torso.

Turn the box off to edit one zone alone — a tattooed arm glossier than the rest, say. The zones that match are decided by the material the file **imported** with, so a zone you edited apart earlier still follows the group when the group is edited again.

## Overlays: tattoos, marks and makeup

The **Overlays** section, under Base — folded while the zone has no overlay, open once it has one; folded, its heading shows a **+** while the zone has none, or how many it has; click the heading to open it — layers color and pictures over the skin: a tattoo, freckles, a birthmark, a bruise, veins, stretch marks, a scar — and makeup: lipstick, eye shadow, blush. An overlay is one of two things, or both:

- a **picture** painted on the zone's **UV layout** — the same square the zone's own texture maps are painted on, so an image made for a figure's torso map lands where it was painted on that figure's torso — with **transparency where the skin shows**. A PNG with an alpha channel is the usual file;
- a **color and a mask**: a flat color, and an image on the Opacity row that says where it goes. A lipstick or an eye shadow needs no picture at all.

PoseStudio composites the overlays into the zone's maps as you work. The zone's own maps are never changed: switch an overlay off or remove it and the skin is back exactly as it was.

### Adding an overlay

1. Click the surface the overlay is for — the thigh, the cheek — so the tab shows its zone.
2. Click **+ Add Overlay**. An empty overlay named *Overlay 1* appears as a **card** under the button. It asks for no file, and it changes nothing yet.
3. On the card, give the overlay what it is made of.

What to give it:

- For a tattoo, a bruise, freckles: click the small square on the **Base Color** row and choose the picture.
- For makeup: click the color on the **Base Color** row and pick it, then click the small square on the **Opacity** row and choose the mask.
- For relief — a scar's ridge, raised ink — click the small square on the **Bump** or the **Normal** row and choose its image.

The overlay shows on the figure as soon as it has something to show. A large image takes a moment to read the first time.

An overlay belongs to the **texture map**, not to the one zone. A figure's face, lips and ears are separate zones painted on one map, so a makeup overlay added with the face selected reaches the lips too, and shows in the Overlays section of each of those zones. The note under the cards lists the zones that share the map.

### An overlay's card

The card's top line has the overlay's **name** — click it to fold or unfold the card — a **switch** that turns the overlay off and on with every value kept, arrows that move it up and down among the other overlays, and **×** to remove it.

Overlays are named by their place as they are added — the third overlay of a zone is *Overlay 3*, whatever the first two have been renamed to — and keep their names when they are moved. To give one a name of your own, right-click its name and choose **Rename…**: the name becomes a text field. Press Enter, or click elsewhere, to keep what you typed; press Esc to leave the name as it was.

Below the top line are the overlay's controls; each is one undo step and has a button that returns it to its default. A **small square** on a row is that row's image. Click it to choose an image file, right-click it to remove the image or open its folder, rest the pointer on it for a larger view.

| Row | Range | What it does |
| --- | --- | --- |
| **Blend** | a list | How the overlay's color meets the skin under it — see *Matching the skin* below. **Multiply** is the default. |
| **Base Color** | a color, a picture | The overlay's color and, in the small square, its picture. With a picture the color multiplies it: white leaves it as painted, and a white or grey picture takes the color whole, so one black tattoo gives an ink of any color (raise Brightness first: black stays black under a color). With no picture the overlay *is* the color, where the Opacity's map lets it. |
| **Brightness** | −100 – 100 | Lightens or darkens the color before it is blended. |
| **Contrast** | −100 – 100 | Spreads the picture's values apart, or pulls them toward mid grey. |
| **Gamma** | 0.20 – 5.00 | The picture's midtones: above 1 lightens them, below 1 darkens them, black and white stay. |
| **Hue** | −180 – 180° | Turns the colors around the color wheel — a red rose to a blue one. Grey has no hue to turn; use Base Color for it. |
| **Saturation** | −100 – 100 | −100 is grey, 0 the overlay's own colors, 100 twice as vivid. |
| **Gloss** | −100 – 100 | Makes the surface glossier (above 0) or more matte (below 0) where the overlay is: lip gloss, a wet wound, powder. It follows the overlay's shape — the picture and the Opacity's map — but not the Opacity value, so a sheer lipstick is as glossy as an opaque one. |
| **Metallic** | −100 – 100 | Makes the surface more metallic (above 0) or less (below 0) where the overlay is: metallic ink, gold leaf, glitter — or, below 0, paint over a metal surface. The small square is an image that limits it *within* the overlay: gold accents on part of a tattoo. Like the gloss it follows the overlay's shape, not the Opacity value, and it leaves the zone's own Metallic value as it is. |
| **Opacity** | 0 – 100 | How much of the overlay's **color** shows. The small square is its map — the overlay's **mask**: see *Masks: the Opacity's map* below. The relief — Bump, Normal, Displacement — follows neither the value nor the map: it has its own images and strengths. |
| **Bump** | −400 – 400 | An image of its own, grayscale, for relief: a scar's ridge, raised ink, a groove. Mid grey is no change, lighter rises, darker sinks; where the image is transparent it does nothing. The number is its strength; a negative strength turns the relief inside out. |
| **Normal** | 0 – 400 | A tangent-space normal image for the same purpose, blended over the zone's normal map, with its strength. |
| **Displacement** | −400 – 400 | A grayscale height image for a renderer's displacement. Stored for rendering engines; the viewport does not displace. |
| **Offset X** | −100 – 100 | Moves the whole overlay sideways across the texture map, in percent of the map's width: positive to the right. See *Placing an overlay* below. |
| **Offset Y** | −100 – 100 | Moves it up (positive) or down the map, in percent of the map's height. |
| **Rotation** | −180 – 180° | Turns the whole overlay about its own centre: positive is clockwise. |
| **Scale** | 10 – 500 | The overlay's size, in percent of the size it was painted at. It grows and shrinks about its own centre. |

The adjustments are applied in this order: Gamma, Brightness and Contrast first (the picture's values), then Base Color (its color), then Hue and Saturation.

An overlay does not need every part:

- **A picture alone** is the usual tattoo or bruise.
- **A Base Color and an Opacity map, with no picture**, is a flat color in the map's shape: makeup.
- **A Base Color alone**, other than white, tints the whole zone: a tan, body paint. White with no picture and no map is nothing — which is what a newly added overlay is.
- **A Bump or a Normal image alone** is relief with no color of its own: a scar you can feel. To keep an overlay's relief and drop its color, set **Opacity** to 0.
- **A Metallic value alone** — with its map for a shape, or with none for the whole zone — turns the skin that is there to metal.

**Metallic ink and gold leaf.** A metal takes its color from the color under it, so give a metallic overlay the metal's color: for gold leaf from a black tattoo, set **Blend** to Normal, pick a gold **Base Color**, raise **Brightness** to 100 so the black ink takes the color, set **Metallic** to 100, and add **Gloss** for a polished leaf or leave it at 0 for a brushed one. The Metallic row's image limits the metal to part of the picture — the highlights of a design in gold, the rest in ink. Like a mask it is a grayscale image (white is metallic) or an image with transparency (its opaque part is), and it moves, turns and scales with the overlay.

### Masks: the Opacity's map

The small square on the **Opacity** row takes a **mask**: a picture of where the overlay shows, painted on the zone's UV layout like any overlay image. The Opacity value scales it, as the material's own Opacity scales its cutout map. Either kind of file works:

- a **grayscale image**: white where the overlay shows, black where it does not, grey in between;
- an **image with transparency**: its opaque part is the mask, whatever its color — so a tattoo's own PNG serves as a mask of its shape.

The map is the **shape of the overlay's color, gloss and metallic**. It does not touch the relief: a Bump, Normal or Displacement image goes by its own picture and strength alone. What it is for:

- **Makeup**: a Base Color and a map of the lips, the eyelids or the cheeks, with no picture at all. One map gives a lipstick of every color, in any Blend.
- **Part of a picture**: a map keeps a tattoo off an area, or fades out one end of a bruise.

The map is moved, turned and scaled with the rest of the overlay. If its file cannot be found, the overlay's color, gloss and metallic are left out until it can — they are not applied everywhere instead.

### Placing an overlay: move, turn, scale

The last four rows of a card put the overlay somewhere other than where it was painted, at another angle, at another size: a tattoo a little higher on the thigh, a bruise nearer the ribs and turned to follow them, a mole made smaller. Every image of the overlay goes together — its picture, its mask and its relief. The overlay is scaled and turned about its own centre and then moved, so the rows do not disturb one another, and the button beside each row returns it to the image as painted.

**Offset X** and **Offset Y** move it.

- The values are **percent of the texture map**. Offset X moves the overlay to the right across the map (negative: to the left), Offset Y up (negative: down). 0 and 0 is where the image was painted.
- **Drag** a row for a fine move — a full sweep of the field is 20 percent of the map — or **click it and type** a value for a long one. The range is a whole map each way.
- On the figure the overlay travels **across the surface the way that part of the body is laid out on the map**. On the front of the torso X is left and right and Y is up and down. Along a leg Y still runs up and down the leg, while X carries the overlay *around* it, and which way round depends on how the leg is unwrapped. When in doubt, nudge a row by a percent or two and watch which way the overlay goes.
- A map holds several pieces of the body side by side with empty space between them. An overlay moved past the edge of its piece **leaves the surface there**, and may come onto the piece next to it: a thigh tattoo moved half a map sideways lands on the other leg. What runs off the edge of the map itself is cut off; it does not come back in on the other side.

**Rotation** turns it, in degrees, about its own centre. Positive is clockwise as the map is seen — on the front of the torso, clockwise as you look at the figure; on a part of the body that is unwrapped mirrored it turns the other way. A full sweep of the field is half a turn, in steps of half a degree.

**Scale** sizes it, in percent of the size it was painted at, about the same centre: 50 is half as wide and as tall, 200 twice. A full sweep of the field is 100 percent.

How sharp a placed overlay is:

- At Rotation 0 and Scale 100 the overlay is painted pixel for pixel, exactly as sharp as its file, wherever the offset puts it. A quarter turn or half a turn (90, −90, 180) is exact too.
- At any other angle or size the image is resampled, which softens it very slightly. Scaled down it is filtered, so fine lines thin out rather than break up. Scaled far up it shows the resolution it was painted at; for a much larger tattoo, paint a larger image.

What a turn and a scale do to relief:

- A **Normal** image is turned with its slopes, so light falls on a turned scar as it should, and it keeps its slopes at any size: the relief grows and shrinks with the picture.
- A **Bump** image keeps its height at any size. Scaled up, the same height spreads over more skin and looks gentler; scaled down it looks sharper. Adjust its strength to taste.

### Matching the skin

Skin differs from character to character, and the same image has to sit right on all of them. The blend mode does most of that:

| Blend | Use it for | How it behaves |
| --- | --- | --- |
| **Multiply** | tattoos, bruises, freckles, moles, veins | Darkens the skin that is there. White in the image changes nothing, so the skin's own tone and grain show through and the image reads as *in* the skin on a pale and a dark character alike. |
| **Normal** | lipstick, face paint, anything opaque | Covers the skin with the image's own color. |
| **Soft Light** | blush, a faint mark, a tan line | A gentle tint that keeps the skin's light and dark. |
| **Overlay** | stronger tints | The same with more contrast. |
| **Screen** | scars, stretch marks, highlighter | Lightens. Black in the image changes nothing. |
| **Darken** / **Lighten** | special cases | Keeps whichever is darker, or lighter, of the image and the skin. |

Then adjust: lower **Opacity** for a faded tattoo or a healing bruise; raise **Brightness** or **Gamma** when an image is too heavy on pale skin; add **Saturation** when a color goes dull on dark skin; turn **Hue** to recolor a picture, or **Base Color** to color a white or grey one.

For makeup, an overlay with no picture is the most flexible: the **Opacity** row's map gives the shape and **Base Color** the color, **Normal** an opaque lipstick or **Multiply** a stain, **Soft Light** a blush, and **Gloss** the shine.

### Several overlays

A zone carries as many overlays as you like. The cards are listed in the order they are painted: the first card first, and a new overlay at the bottom of the list, painted over the ones above it. The arrows on a card move it up and down the list; the order matters wherever overlays overlap and one of them covers (Normal) rather than darkens.

### What overlays do not do

- An overlay is moved, turned and scaled **on the texture map**, not on the body: it cannot cross from one piece of the body to another in one piece, and where the map is laid out tighter or looser on the skin it shrinks or stretches with it. An image is still made for one figure's UV layout; a figure of another generation has another layout and needs images made for it.
- An overlay's Bump and Normal images are drawn in the viewport through the zone's normal map, so **Normal Strength** under Geometry scales them along with the zone's own normal map.
- Displacement images are stored for rendering engines, like the material's own displacement.

## The sections

The material is laid out in collapsible sections; click a heading to fold or unfold it. Every section carries a **switch** at the right of its heading: untick it and that part of the material is off while every value in it is kept, so you can compare with and without a coat, a sheen or a glow and have your numbers back when you tick it again. For the layers — Top Coat, Sheen, Thin Film, Transmission, Subsurface, Emission, Displacement, Detail — off means the layer contributes nothing. For the surface's own sections it means: **Base** off, no diffuse color and no metal, a black dielectric (glass, or a coat alone); **Specular** off, no dielectric reflection, a matte surface (the top coat still reflects); **Geometry** off, the normal, bump and occlusion maps at rest, a smooth surface (opacity and Double Sided are the shape and stay); **Tiling** off, the maps as painted. The switch is part of the material: saved with the project, one undo step, and applied to the matching zones when they are edited together. A `.duf` preset's Top Coat Enable arrives as this switch. **Base**, **Specular** and **Geometry** are open; the others open by themselves when the zone you pick uses them (a coat on skin, a glow on a lamp) and otherwise stay as you left them.

Every value is a **scrub field** like the Environment tab's dials: drag left or right across it to change it — with **Shift** held, in steps a tenth the size — or click it once to type a number (`Enter` confirms, `Esc` cancels). A color is a **swatch**: click it for a color picker, and the color changes on the figure as you choose (`Cancel` puts it back). The small button beside each row returns that one value to what the file imported with — never to a generic default. The figure changes as you scrub.

A row marked *stored for rendering engines* is saved with the material and written to the project file, but the viewport does not draw it. A **render** ([Rendering](rendering.md)) reads the material itself, and uses some of what the viewport cannot show: glass really bends and blurs what is behind it and takes its **Transmission Color** over the **Depth** you give it; skin scatters light under its surface as far as its **Scattering Distance**, **Scattering Direction** and the transmission color at its depth say. Displacement, the Detail layer, dispersion and the maps marked as stored are not rendered yet.

### Base

| Row | Range | What it does |
| --- | --- | --- |
| **Base Color** | a color | The surface's color. With a base-color map the color tints the map: white leaves the map as painted, a red tint reddens it. |
| **Roughness** | 0 – 100 | How matte the surface is: 0 is mirror-smooth, 100 fully rough. Over a roughness map it scales the map, so the map's variation (a glossy T-zone, a matte cheek) is kept. A skin material's two specular lobes follow it together. |
| **Metallic** | 0 – 100 | 0 is a dielectric — skin, plastic, cloth, wood; 100 a metal, whose reflections take on the base color and which has no diffuse color of its own. Values between are for maps that blend the two; a real material is one or the other. |
| **Diffuse Roughness** | 0 – 100 | How rough the diffuse surface is at the micro scale — cloth, chalk, the moon. 0 is a smooth Lambertian surface; towards 100 the shading flattens and brightens toward the light (Oren-Nayar's retro-reflection, on the key light). |

### Specular

The dielectric reflection — what a non-metal's surface mirrors. A metal's reflection is its base color and ignores these.

| Row | Range | What it does |
| --- | --- | --- |
| **Specular** | 0 – 100 | How strongly a dielectric reflects: **100 is the standard dielectric** (4 % at normal incidence — plastic, glass, varnish), 0 reflects nothing. Skin without a specular mask imports around 30 – 80 (real skin reflects less). Over a specular mask the value scales the mask, and a material whose mask carries the attenuation imports above 100 — the figure's skin reads about 120 with a mask whose texels average well under white — so the range follows the imported value. |
| **Specular Color** | a color | A tint on the dielectric reflection — white for nearly every real material; a colored tint is the look of some stylized or coated surfaces. The PBR Shaded mode tints its reflection with it. |
| **IOR** | 1.0 – 3.0 | The index of refraction: 1.5 is glass and the standard dielectric, water 1.33, skin about 1.4, a diamond 2.4. The viewport's reflectance follows it together with Specular: 1.5 is the 4 % the Specular row describes, 2.4 a diamond's 17 %. |
| **Anisotropy** / **Anisotropy Rotation** | 0 – 100 / 0 – 360° | How much the highlight is stretched along the surface's grain (brushed metal, hair) and which way the grain runs: 0° is along the texture's horizontal, 90° its vertical. The key light's highlight and the environment's reflection both stretch; the maps for the two are stored for rendering engines. |
| **Lobe 1 Roughness** / **Lobe 2 Roughness** / **Lobe Ratio** | 0 – 100 | Shown for a **dual-lobe** material only — skin, whose specular is a tight reflection (lobe 1) over a broad soft sheen (lobe 2), blended by the ratio (100 is lobe 1 alone; skin imports around 85). The PBR Shaded mode renders the two lobes; Roughness above scales both together, these set each. Lobe 1's map is the Roughness slot (shown on both lines), which scales both lobes. The source app's Dual Lobe Specular group maps onto these three plus Specular (its weight and reflectivity) and the Specular Mask, Roughness, Lobe 2 Roughness and Lobe Ratio slots (its maps). |

### Top Coat

| Row | Range | What it does |
| --- | --- | --- |
| **Weight** | 0 – 200 | A thin clear layer over the surface — the oil on skin, a varnish on a prop, a car paint's lacquer. 0 is none; skin materials import around 35 – 45. |
| **Roughness** | 0 – 100 | How sharp the top coat's reflections are: 0 glassy, 100 dull. |
| **Color** | a color | The coat's tint: what passes through it to the surface takes this color — a candy-apple red over metal flakes. White is a clear coat. A `.duf` preset's Top Coat Color, which tints that shader's coat reflection, arrives as its hue alone (its brightness is folded into the weight), so a grey coat color is a clear coat here. |
| **IOR** | 1.0 – 3.0 | The coat layer's index of refraction: 1.5 is the standard 4 % coat, a lacquer about 1.5 – 1.6. A `.duf` preset's coat is 1.5 unless its layering mode is Fresnel, where its Top Coat IOR is read. |
| **Bump Strength** | 0 – 400 | How strongly the coat's own bump or normal map (the Top Coat Bump slot — orange peel on a lacquer) perturbs the coat layer. Greyed without one. Stored for rendering engines. |
| **Anisotropy** / **Anisotropy Rotation** | 0 – 100 / 0 – 360° | How much the coat's highlight is stretched along a grain (brushed lacquer) and which way the grain runs. The maps are stored for rendering engines. |

Each row carries a map: Top Coat Weight and Top Coat Roughness (the viewport samples them), Top Coat Color, Top Coat Reflectivity (beside the IOR row — it scales the coat's reflectance per texel) and Top Coat Bump (stored for rendering engines).

### Sheen

The soft back-scatter of fibres at grazing angles — velvet, felt, peach fuzz: a glow that brightens toward the edges of a form, in the sheen's color, and takes that much of the light from the surface beneath it.

| Row | Range | What it does |
| --- | --- | --- |
| **Sheen** | 0 – 100 | How much of it there is; 0 is none. Imported from a `.duf` preset's Backscattering group, which is that shader's sheen. The weight and color maps are sampled; the roughness map is stored for rendering engines. |
| **Sheen Color** | a color | The fuzz's color, white for most cloth. |
| **Sheen Roughness** | 0 – 100 | How broad the sheen is: 0 a tight rim, 100 a soft bloom over the whole surface. |

### Thin Film

Iridescence — soap bubbles, oil on water, a beetle's shell: a thin film's interference colors replace the surface's plain reflection, shifting with the thickness and the angle of view. Over a metal the colors are vivid; over a dielectric they are faint, as a real film's are.

| Row | Range | What it does |
| --- | --- | --- |
| **Thin Film** | 0 – 100 | How much a thin surface film colors the reflection by interference. |
| **Thickness** | 0 – 2000 nm | The film's thickness; a soap bubble is a few hundred nanometres. A thickness map (black none, white this value) is sampled; a weight map is stored for rendering engines. |
| **Film IOR** | 1.0 – 3.0 | The film's index of refraction. |

### Transmission

Light passing *through* the material — glass, water, gems. The viewport shows what is behind the surface through it, tinted by the transmission color (deeper toward the edges of a solid, even across a thin-walled sheet), under the surface's own reflection; a transmissive zone casts no shadow. The view is not bent by refraction and a rough glass does not blur what is behind it — those need the frame behind the surface as a texture, a later round. **Opacity** under Geometry is the other way to see through a surface: a plain fade, with no reflection kept.

| Row | Range | What it does |
| --- | --- | --- |
| **Transmission** | 0 – 100 | 100 is clear glass, 0 an opaque solid. Its map scales it per texel. |
| **Transmission Color** | a color | The tint light takes on as it passes through (green bottle glass, amber). Its map is stored for rendering engines. |
| **Depth** | 0 – 1000 cm | Over how many centimetres the tint is reached inside the material; 0 applies it at the surface. Stored for rendering engines (the viewport applies the color as it is). |
| **Dispersion** | 0 – 100 | How much the index of refraction varies with wavelength — a diamond's fire. Stored for rendering engines. |
| **Thin Walled** | on / off | The surface is a single sheet (a window pane, a leaf, a soap bubble) with no solid interior: the tint is even across it, where a solid's deepens toward its edges. |

### Subsurface

| Row | Range | What it does |
| --- | --- | --- |
| **Translucency Weight** | 0 – 200 | How much light passes under the surface and comes back softened and reddened: skin's translucency, the warm light through ears and nostrils. 100 is the shader's full amount, 0 an opaque material. Scaled by the translucency color map's brightness where the zone has one; its Translucency Weight map (the per-region thickness: ears, nostrils, fingertips) is stored for rendering engines. |
| **Translucency Color** | a color | The color light takes on under the surface: reddish for skin, warm for wax, green for jade. With a translucency color map it tints the map. Stored for rendering engines; the viewport's skin profile is fixed. |
| **Scattering Color** | a color | How far each color of light scatters relative to the others — red deepest in skin, so its scattering color is reddish; white scatters all colors alike (the source app's SSS Color). Stored for rendering engines. |
| **Scattering Distance** | 0 – 100 cm | How far light travels under the surface before it comes back (the source app's Scattering Measurement Distance; skin about 0.1 – 0.3, wax a few centimetres). Stored for rendering engines. |
| **Scattering Direction** | −100 – 100 | Which way light scatters inside: 0 evenly, forward towards 100, back towards −100 (the source app's SSS Direction). Stored for rendering engines. |

### Emission

| Row | Range | What it does |
| --- | --- | --- |
| **Emission Color** | a color | The color the surface gives off. With an emission map it tints the map. |
| **Emission** | 0 – 100 | How brightly the surface glows: 0 is none, 1 the color at the scene's full radiance, more for a lamp or a screen. Shown in the **PBR Shaded** mode, where a bright emission blooms. |

### Geometry

| Row | Range | What it does |
| --- | --- | --- |
| **Opacity** | 0 – 100 | 100 is solid. Anything less is drawn see-through, blended over what is behind it — the eyes' cornea and moisture import this way. A cutout mask in the zone's Cutout slot still applies. |
| **Normal Strength** | 0 – 400 | How strongly the zone's normal map perturbs the surface: 100 is the map as painted, 0 flattens it, more exaggerates it. Greyed when the zone has no normal map and no overlay with a Bump or Normal image (the viewport draws an overlay's relief through the normal map). |
| **Bump Strength** | 0 – 400 | The same for a bump (height) map. Greyed without one. |
| **Ambient Occlusion** | 0 – 100 | How much a baked ambient-occlusion map (the Ambient Occlusion slot: grayscale, white is open) darkens the crevices. Greyed without one. The viewport applies it, over the occlusion it bakes per vertex at import, to the environment's light — a prop's authored occlusion where the vertex bake is coarse. |
| **Double Sided** | on / off | Render the back faces too — a leaf, a cloth card, an open shell. Stored for rendering engines; the viewport always lights both sides. |
| **Smooth by Angle** | on / off | Shade the surface smoothly, with normals PoseStudio works out from its faces instead of the file's — see [Smooth shading](#smooth-shading) below. On by default for a model whose file brings no smoothing of its own. |
| **Smoothing Angle** | 0 – 180 | How sharp an edge may be and still be smoothed over, in degrees. 60 rounds a sphere and keeps a cube's edges; 0 is faceted; 180 smooths everything. Greyed while Smooth by Angle is off (the angle is kept). |

### Smooth shading

A surface is shaded by the direction each point of it faces — its *normals* — and a model's file brings its own. Many files bring none worth the name: unless its author set the surface to smooth, a modelling program writes **one normal per face**, and such a model looks faceted in every program that draws it as written — a sphere shows each of its polygons, in the viewport and in a render alike.

PoseStudio therefore **smooths a model as it imports it when the file carries no smoothing of its own** — one normal per face, or no normals at all — and leaves a file with normals of its own exactly as it is. The import's line in the log says when it did.

**Smooth by Angle** is that switch, for each zone. On, PoseStudio works the normals out itself: faces that meet at less than the **Smoothing Angle** blend into one another, and an edge at that angle or sharper stays crisp. At the default of 60 degrees a ball is round, a can has a round side and flat ends, and a box keeps its edges. Lower the angle to keep gentler edges crisp (0 is fully faceted); raise it to round off sharper ones (past 90 a box turns into a pillow). Off, the zone is shaded as its file says — faceted if the file is, smooth if its author made it so.

- It shows in every shading mode (Flat Texture Shaded is faceted on purpose), in the Rendered mode and in a render: they all use the same normals.
- A surface is smoothed **across its zones**: two smoothed zones of one object meet without a seam.
- With **Edit matching zones together** on, a change of the smoothing reaches every zone of the object that was smoothed the same way — usually all of them, whatever their materials — and Reset Zone brings them back together. With it off, only the zone shown changes.
- It belongs to the **object**, not to the material: a material saved to the library does not carry it, and applying one does not change it. A project keeps it, and Undo steps over it like any other edit.
- Figures import with smooth normals of their own and are left as they are; a zone of one can still be switched to Smooth by Angle.
- Smoothing changes how light falls on a surface, not its shape: the **outline** of a coarse model stays polygonal.

### Displacement

A height map a renderer pushes the surface by, after subdividing it. Stored for rendering engines; the viewport does not displace. Opens by itself for a zone with a displacement map.

| Row | Range | What it does |
| --- | --- | --- |
| **Strength** | −100 – 100 cm | How far the Displacement map pushes the surface, from the map's black to its white; negative inverts it. Greyed without a map. Stored for rendering engines, which subdivide and displace the mesh — the viewport does not. A `.duf` preset's Minimum / Maximum Displacement import as this strength and the mid level below. |
| **Mid Level** | 0 – 100 | The map value that leaves the surface where it is: at 50 black sinks and white rises by half the strength each. Stored for rendering engines. |
| **Subdivision** | 0 – 5 | How many times a renderer subdivides the mesh before displacing it. Stored for rendering engines. |

### Detail

A small tiling normal map layered over the surface — a skin's pores, a cloth's weave, leather grain — repeated many times across the zone. Opens by itself for a zone that carries one.

| Row | Range | What it does |
| --- | --- | --- |
| **Weight** | 0 – 99 | The strength of the Detail Normal Map. Greyed without one. The source app's Detail Weight map is kept as the Detail Weight Mask slot, stored for rendering engines. |
| **Roughness** | 0 – 400 | A roughness multiplier by the layer's weight, so the pores read a little rougher than the skin between them; 100 is none. Its map is stored for rendering engines. |
| **Horizontal Tiles** / **Vertical Tiles** | 1 – 512 | How many times the detail map repeats across the zone, each way. |
| **Horizontal Offset** / **Vertical Offset** | −1000 – 1000 | Shifts the detail map across the zone, in repeats. |

### Tiling

The zone's UV repeat and shift, applied to every one of its maps together. Opens by itself for a zone whose material tiles.

| Row | Range | What it does |
| --- | --- | --- |
| **Horizontal Tiles** / **Vertical Tiles** | 0.01 – 1000 | How many times the zone's textures repeat across it — a tiled floor, a fabric print. 1 is the maps as painted. |
| **Horizontal Offset** / **Vertical Offset** | −1000 – 1000 | Shifts the textures across the zone, in repeats. |
| **Rotation** | −360 – 360° | Turns the textures across the zone, about the texture's origin after the tiling (a glTF texture transform's order). |

## Texture maps

Every map a material can carry has a **slot**, and each slot shows as a **small thumbnail beside the value it scales**: the base color map beside Base Color, the roughness map beside Roughness (and beside Lobe 1 Roughness, whose map it is), the cutout beside Opacity, the normal and bump maps beside their strengths, the emission map beside Emission Color, the top coat's maps beside the Top Coat rows, and so on through the tab. The one map that scales no value, the detail layer's Weight Mask, has a row of its own in the Detail section with the bound file's name beside it. An empty slot is a dark square.

Every thumbnail works the same way:

- **Click** it to replace the slot with an image file — PNG, JPEG, TIFF, BMP, TGA or WebP. The file is read the way an import reads its maps (large images are scaled to the viewport's working size) and the figure shows it at once. The dialog opens where you last chose a texture.
- **Rest the pointer** on it for a moment to see the map at a larger size, with the file's name under it.
- **Right-click** it for a menu: **Choose Image…**, **Remove** (the zone renders without that map, on its values alone), **Restore Imported Map**, and **Open File Location** (the map's folder in your system's file browser; an image packed inside the model's file has none).
- The **restore arrow** on the line puts the map back along with the value.

The slots, by section: Base Color, Roughness, Metalness and Diffuse Roughness in Base; Specular Mask, Specular Color, Anisotropy, Anisotropy Rotation, Lobe 2 Roughness and Lobe Ratio in Specular (Lobe 1's map is the Roughness slot); the Top Coat's Weight, Color, Roughness, Reflectivity, Bump, Anisotropy and Anisotropy Rotation; Transmission and Transmission Color; Translucency Weight and Translucency Color in Subsurface; Sheen, Sheen Color and Sheen Roughness; Emission; Thin Film and Thin Film Thickness; Cutout, Normal, Bump, Displacement and Ambient Occlusion in Geometry; the Detail Normal Map, Detail Weight Mask and Detail Roughness in Detail.

A base color, emission or tint map is read as a color image (sRGB); a normal map as data; a roughness, metalness, diffuse roughness, specular mask or cutout file as a grayscale map — a color file's red channel is used. The viewport samples the Base Color, Normal, Bump, Roughness, Metalness, Specular Mask, Translucency Color, Emission, Detail Normal Map and Cutout slots, and since the viewport round the Ambient Occlusion, Transmission, Top Coat Weight, Top Coat Roughness, Sheen, Sheen Color and Thin Film Thickness maps too (packed two to a texture behind the scenes, so a 4K map of each costs no more than before). The Diffuse Roughness, Specular Color, Anisotropy, Anisotropy Rotation, Lobe 2 Roughness, Lobe Ratio, Translucency Weight, Detail Weight Mask, Detail Roughness, Displacement, Transmission Color, Sheen Roughness, Thin Film weight and the Top Coat's Color, Reflectivity, Bump and Anisotropy maps are stored for rendering engines: the tab shows them and the project keeps them, the viewport does not sample them. A cutout mask is black where the surface is cut away and white where it is kept; a zone with a cutout is drawn in the see-through pass, as the eyelashes are.

The sliders scale the maps — Roughness multiplies a roughness map, Metallic a metalness map, the Base Color swatch tints the base color map — so the map's own variation is kept.

## Resetting

- The button beside a row returns that value to what the file imported with.
- **Reset Zone** returns the whole zone's values — and its matching zones', when editing them together — to the imported material. The texture slots are put back one by one with their arrows, and the overlays are removed with their own **×**.
- **Reset All Zones** returns every zone's values.

## Saving and reusing a material

A material you have made — a brushed gold, a car paint, a skin you tuned — can be saved to a file and put on any other object.

### Saving

Select the object and the zone, then click **Save**, at the top right of the tab across from the object's name. A file dialog opens in your Materials library — the **Materials** folder of *My PoseStudio Library*, or the folder inside it you saved into last. Choose a category folder if you keep them (*Metal*, *Glass*, *Skin* …), give the material a name, and save. Two files are written side by side:

- **The material**, `name.psm`: every value of the zone as the tab shows it, and for every texture slot that holds a map, the map's file — which is saved *with* the material: see [Where the textures go](#where-the-textures-go), below.
- **Its icon**, `name.png`: a picture of the material on a ball, 255 pixels square on a transparent background, with a little margin round the ball. The ball is **rendered by the render engine** — path traced, like a picture from [Render → Render Image](rendering.md) — so glass refracts, skin scatters and a coat lies over its base as they would in a render. It is lit by a small studio of its own that is built into PoseStudio — dark walls, a pale floor that falls away into dimness behind the ball, a large softbox above and to the left and a thin strip light to the right, all neutral grey — never by your scene's lighting or by a panorama from your HDRI folder. A polished ball is a mirror of the room it stands in, so the room is made for what a ball should show: chrome comes out black beside white with crisp reflections, a matte material shows its form, and a material's own color is the only color in its icon. The edge of a ball mirrors whatever is behind it, and the library shows icons on a dark background, so what is behind the ball is dim, and straight behind it dark: an icon's edge falls away into the background, never a pale or grey ring round it — whatever the material's color. Every icon is lit alike, on any computer, so two materials can be compared by their icons. Rendering it takes a moment — a second or two, longer the first time a graphics card is used in a session.

Saving over a material that already has an icon asks first: **Recreate Icon** renders it again from the material as it is now, **Keep Existing Icon** saves the material and leaves the picture — your own, perhaps — as it is, and **Cancel** saves nothing.

The saved material appears in the **Materials Library** tab at once.

What a saved material does not hold:

- **Overlays.** A tattoo or a makeup layer is painted on one object's layout and belongs to that object, not to the surface. They stay on the object (and in its [project file](project-files.md)); the material file and its icon are the surface without them.

### Where the textures go

A saved material takes its texture files with it. They are copied into a `textures` folder beside the material, in a subfolder named after it, and the material refers to them by paths relative to itself:

```
Materials/Fabric/
    Denim.psm
    Denim.png
    textures/
        Denim/
            denim_color.jpg
            denim_normal.png
```

So a material's folder is all the material needs. Move the folder, copy it to another computer, keep your library in a synced folder, zip it and send it to someone: the materials in it still find their maps. There is no central texture folder that has to be kept in step with your materials, and nothing to "install".

- **A file is stored once.** A second material saved into the same folder that uses the same image — a color variant that shares its normal map — refers to the file that is already there, whatever it is called; nothing is copied twice. A *different* image that happens to have the same name is given a name of its own (`color_2.png`), never written over the first.
- **A map that is already in place stays there**: one that lies right beside the material, or in that folder's `textures`.
- **An image that has no file of its own is written out** as a PNG named after the material and the slot (`Denim_roughness.png`): an image that lives inside a model's file (a `.glb`'s), and a map that is one channel of a packed image (a glTF file keeps roughness and metalness in one picture). Such an image is saved as PoseStudio holds it, which is at most 4096 pixels on a side.
- **Saving a material again under its own name** with other maps removes the copies it no longer uses (they go to the trash) — unless another material in the folder still uses them.
- The library browsers do not list `textures` folders: they hold pictures, not materials.

If you would rather have no copies — you keep your textures in one place and never move them — turn off **Copy texture files when saving a material** in [Preferences → Assets](preferences.md#assets). A material then refers to each file where it is, stops finding it if it moves, and will not work on another computer; and an image that has no file of its own is left out of the saved material (the save says how many).

To rename, move, duplicate or delete a saved material together with its files, or to send one to somebody, use the library's own commands: see [Working with saved materials](asset-manager.md#working-with-saved-materials) and [Packages](asset-manager.md#packages-sharing-materials).

A copy of PoseStudio without a render engine still writes an icon, drawn by a plainer built-in shader — the base color, metal, roughness and their maps, a coat, emission — so that the material is listed; it is a stand-in, not the rendered ball.

### Applying a saved material

Select an object, click the surface you want — the Materials tab shows which zone that is — and **double-click the material** in the Materials Library tab (or in the Asset Manager, which reaches the same folder). The zone takes the saved material: its values, and its maps in their slots. A slot the saved material has no map for is emptied — the file is the whole surface, so nothing of the old one shows through. The zone's overlays stay where they are.

- It is **one undo step**: `Ctrl+Z` gives the zone back exactly as it was.
- Only the zone shown is changed, whatever *Edit matching zones together* says: a material is put on one surface at a time.
- With nothing selected, a message says so and nothing happens.
- A map that is not where the material says is **looked for**: beside the material, in its `textures` folder, and then by its file name anywhere in the library the material lies in (the nearest one, if there are several). Only a map that is found nowhere leaves its slot empty, and a message says how many were missing.
- **Reset Zone** still returns the zone to what its own file imported with.

**MaterialX materials** (`.mtlx`, the format material libraries are published in) are applied the same way — double-click one in a library, or use **File → Import → .MTLX (MaterialX Material)**: see [MaterialX materials](importing.md#materialx-materials-mtlx). Save one after applying it and it becomes a material of your own library, with its images copied in beside it.

A material file is plain text (JSON) and can be read, compared or kept under version control.

## Undo and projects

Each scrub, each typed value, each color pick, each texture choice or clear and each reset is one undo step (`Ctrl+Z`), on the same history as pose and lighting edits — and so is each overlay added, removed, moved or adjusted. Undoing a material edit selects the object so you can see what changed.

A [project file](project-files.md) saves the zones whose material differs from the import — every value, every texture slot that was replaced (by its file's path) or cleared, and the zone's overlays with their images' paths and every control — and restores them when the project is opened, on the zone of that name, so a figure re-imported from its file gets its edited skin, its chosen maps and its tattoos back. A chosen texture whose file has since moved leaves the imported map in place, and an overlay whose image has moved is left unpainted (its card stays, so you can point it at the file again), each with a note in the log. A pose file does not carry materials.

## What the tab does not do

- It edits one object at a time. Two figures with the same skin are edited one after the other.
- The viewport draws the base layer (its reflectance from Specular, IOR and Specular Color, its anisotropy, its diffuse roughness), the coat (with its color, index and anisotropy), sheen, the thin film, transmission (the frame behind the surface through it, tinted — not bent, not blurred), the subsurface weight, emission, opacity, the detail layer (its map, weight, roughness multiplier, tiling and offsets), a bump map beside a normal map, and the UV tiling, offset and rotation. Displacement, the subsurface's color, scattering color, distance and direction, transmission depth and dispersion, and the maps listed above as stored are saved for rendering engines but not shown. Double Sided changes nothing in the viewport, which always lights both sides.
- The simpler shading modes show what they can of a material: the textured modes show the base color and the roughness, the matcap and clay modes show none of it. The **PBR Shaded** mode shows everything the viewport renders, and the data views (Albedo, Roughness Map, Specular Only) show one value at a time — see [Shading and Display](shading.md).
