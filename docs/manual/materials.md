# Materials

The **Materials** tab on the side panel edits the surfaces of the selected object: a figure's skin, eyes, mouth and nails, an imported prop's materials. Every object imports with the materials its file describes, and this tab lets you change them with sliders, colour swatches and texture slots — glossier skin, a metallic prop, a colour tint, a see-through surface, a different map — without writing a shader.

PoseStudio has **one material model**: a physically based surface in the metallic / roughness convention, the parameter set the industry's renderers share (the base layer, a coat, transmission, subsurface, sheen, emission, a thin film, and the maps that paint them). The viewport's PBR shading draws nearly all of it — the base layer, the coat, sheen, the thin film, transmission, emission, the detail layer and the maps that paint them; what it cannot (displacement, the subsurface's scattering colour and distance) is kept with the material for the rendering engines to come, and the tab says which is which. There is no node editor and no second workflow to choose: files authored in the older specular / glossiness convention are converted when they are imported (see [Importing Content](importing.md#materials-and-the-specular--glossiness-conversion)).

## Selecting an object and a zone

Click an object in the viewport to select it — the blue outline shows which — and the tab shows its name and its **zones**. A zone is one material group of the object: a figure has a dozen or more (Torso, Face, Lips, Legs, Irises, Cornea, Teeth …), a prop has one per material in its file. The **Zone** picker lists them; pick one and the sections below show its material.

Clicking a surface selects its zone. Click the figure's cheek and the tab shows **Face**; click her thigh and it shows **Legs**; click the eye and it shows the sclera or the iris (the cornea is clear, so the click lands on what is behind it). The same click selects the joint under the cursor for posing, so the two tabs follow one gesture.

Nothing on the tab applies to a Gaussian splat scene: a capture carries its own colour and light and has no material to edit.

## Editing matching zones together

A figure's skin is several zones that import with one and the same material — the preset applies its skin shader to the torso, the face, the arms, the legs, the ears and the lips alike. **Edit matching zones together** (on by default) applies every value change to all the zones that were imported with the same material as the one you are editing, so lowering the skin's roughness on the torso lowers it on the face and the limbs too. The note under the box says how many other zones follow. The eyes, the mouth and the nails have materials of their own and are not touched. Texture slots are always the zone's own: a map chosen for the torso goes on the torso.

Turn the box off to edit one zone alone — a tattooed arm glossier than the rest, say. The zones that match are decided by the material the file **imported** with, so a zone you edited apart earlier still follows the group when the group is edited again.

## The sections

The material is laid out in collapsible sections; click a heading to fold or unfold it. Every section carries a **switch** at the right of its heading: untick it and that part of the material is off while every value in it is kept, so you can compare with and without a coat, a sheen or a glow and have your numbers back when you tick it again. For the layers — Top Coat, Sheen, Thin Film, Transmission, Subsurface, Emission, Displacement, Detail — off means the layer contributes nothing. For the surface's own sections it means: **Base** off, no diffuse colour and no metal, a black dielectric (glass, or a coat alone); **Specular** off, no dielectric reflection, a matte surface (the top coat still reflects); **Geometry** off, the normal, bump and occlusion maps at rest, a smooth surface (opacity and Double Sided are the shape and stay); **Tiling** off, the maps as painted. The switch is part of the material: saved with the project, one undo step, and applied to the matching zones when they are edited together. A `.duf` preset's Top Coat Enable arrives as this switch. **Base**, **Specular** and **Geometry** are open; the others open by themselves when the zone you pick uses them (a coat on skin, a glow on a lamp) and otherwise stay as you left them.

Every value is a **scrub field** like the Environment tab's dials: drag left or right across it to change it, or click it once to type a number (`Enter` confirms, `Esc` cancels). A colour is a **swatch**: click it for a colour picker, and the colour changes on the figure as you choose (`Cancel` puts it back). The small button beside each row returns that one value to what the file imported with — never to a generic default. The figure changes as you scrub.

A row marked *stored for rendering engines* is saved with the material and written to the project file, but the viewport does not draw it.

### Base

| Row | Range | What it does |
| --- | --- | --- |
| **Base Color** | a colour | The surface's colour. With a base-colour map the colour tints the map: white leaves the map as painted, a red tint reddens it. |
| **Roughness** | 0 – 100 | How matte the surface is: 0 is mirror-smooth, 100 fully rough. Over a roughness map it scales the map, so the map's variation (a glossy T-zone, a matte cheek) is kept. A skin material's two specular lobes follow it together. |
| **Metallic** | 0 – 100 | 0 is a dielectric — skin, plastic, cloth, wood; 100 a metal, whose reflections take on the base colour and which has no diffuse colour of its own. Values between are for maps that blend the two; a real material is one or the other. |
| **Diffuse Roughness** | 0 – 100 | How rough the diffuse surface is at the micro scale — cloth, chalk, the moon. 0 is a smooth Lambertian surface; towards 100 the shading flattens and brightens toward the light (Oren-Nayar's retro-reflection, on the key light). |

### Specular

The dielectric reflection — what a non-metal's surface mirrors. A metal's reflection is its base colour and ignores these.

| Row | Range | What it does |
| --- | --- | --- |
| **Specular** | 0 – 100 | How strongly a dielectric reflects: **100 is the standard dielectric** (4 % at normal incidence — plastic, glass, varnish), 0 reflects nothing. Skin without a specular mask imports around 30 – 80 (real skin reflects less). Over a specular mask the value scales the mask, and a material whose mask carries the attenuation imports above 100 — the figure's skin reads about 120 with a mask whose texels average well under white — so the range follows the imported value. |
| **Specular Color** | a colour | A tint on the dielectric reflection — white for nearly every real material; a coloured tint is the look of some stylized or coated surfaces. The PBR Shaded mode tints its reflection with it. |
| **IOR** | 1.0 – 3.0 | The index of refraction: 1.5 is glass and the standard dielectric, water 1.33, skin about 1.4, a diamond 2.4. The viewport's reflectance follows it together with Specular: 1.5 is the 4 % the Specular row describes, 2.4 a diamond's 17 %. |
| **Anisotropy** / **Anisotropy Rotation** | 0 – 100 / 0 – 360° | How much the highlight is stretched along the surface's grain (brushed metal, hair) and which way the grain runs: 0° is along the texture's horizontal, 90° its vertical. The key light's highlight and the environment's reflection both stretch; the maps for the two are stored for rendering engines. |
| **Lobe 1 Roughness** / **Lobe 2 Roughness** / **Lobe Ratio** | 0 – 100 | Shown for a **dual-lobe** material only — skin, whose specular is a tight reflection (lobe 1) over a broad soft sheen (lobe 2), blended by the ratio (100 is lobe 1 alone; skin imports around 85). The PBR Shaded mode renders the two lobes; Roughness above scales both together, these set each. Lobe 1's map is the Roughness slot (shown on both lines), which scales both lobes. The source app's Dual Lobe Specular group maps onto these three plus Specular (its weight and reflectivity) and the Specular Mask, Roughness, Lobe 2 Roughness and Lobe Ratio slots (its maps). |

### Top Coat

| Row | Range | What it does |
| --- | --- | --- |
| **Weight** | 0 – 200 | A thin clear layer over the surface — the oil on skin, a varnish on a prop, a car paint's lacquer. 0 is none; skin materials import around 35 – 45. |
| **Roughness** | 0 – 100 | How sharp the top coat's reflections are: 0 glassy, 100 dull. |
| **Color** | a colour | The coat's tint: what passes through it to the surface takes this colour — a candy-apple red over metal flakes. White is a clear coat. A `.duf` preset's Top Coat Color, which tints that shader's coat reflection, arrives as its hue alone (its brightness is folded into the weight), so a grey coat colour is a clear coat here. |
| **IOR** | 1.0 – 3.0 | The coat layer's index of refraction: 1.5 is the standard 4 % coat, a lacquer about 1.5 – 1.6. A `.duf` preset's coat is 1.5 unless its layering mode is Fresnel, where its Top Coat IOR is read. |
| **Bump Strength** | 0 – 400 | How strongly the coat's own bump or normal map (the Top Coat Bump slot — orange peel on a lacquer) perturbs the coat layer. Greyed without one. Stored for rendering engines. |
| **Anisotropy** / **Anisotropy Rotation** | 0 – 100 / 0 – 360° | How much the coat's highlight is stretched along a grain (brushed lacquer) and which way the grain runs. The maps are stored for rendering engines. |

Each row carries a map: Top Coat Weight and Top Coat Roughness (the viewport samples them), Top Coat Color, Top Coat Reflectivity (beside the IOR row — it scales the coat's reflectance per texel) and Top Coat Bump (stored for rendering engines).

### Sheen

The soft back-scatter of fibres at grazing angles — velvet, felt, peach fuzz: a glow that brightens toward the edges of a form, in the sheen's colour, and takes that much of the light from the surface beneath it.

| Row | Range | What it does |
| --- | --- | --- |
| **Sheen** | 0 – 100 | How much of it there is; 0 is none. Imported from a `.duf` preset's Backscattering group, which is that shader's sheen. The weight and colour maps are sampled; the roughness map is stored for rendering engines. |
| **Sheen Color** | a colour | The fuzz's colour, white for most cloth. |
| **Sheen Roughness** | 0 – 100 | How broad the sheen is: 0 a tight rim, 100 a soft bloom over the whole surface. |

### Thin Film

Iridescence — soap bubbles, oil on water, a beetle's shell: a thin film's interference colours replace the surface's plain reflection, shifting with the thickness and the angle of view. Over a metal the colours are vivid; over a dielectric they are faint, as a real film's are.

| Row | Range | What it does |
| --- | --- | --- |
| **Thin Film** | 0 – 100 | How much a thin surface film colours the reflection by interference. |
| **Thickness** | 0 – 2000 nm | The film's thickness; a soap bubble is a few hundred nanometres. A thickness map (black none, white this value) is sampled; a weight map is stored for rendering engines. |
| **Film IOR** | 1.0 – 3.0 | The film's index of refraction. |

### Transmission

Light passing *through* the material — glass, water, gems. The viewport shows what is behind the surface through it, tinted by the transmission colour (deeper toward the edges of a solid, even across a thin-walled sheet), under the surface's own reflection; a transmissive zone casts no shadow. The view is not bent by refraction and a rough glass does not blur what is behind it — those need the frame behind the surface as a texture, a later round. **Opacity** under Geometry is the other way to see through a surface: a plain fade, with no reflection kept.

| Row | Range | What it does |
| --- | --- | --- |
| **Transmission** | 0 – 100 | 100 is clear glass, 0 an opaque solid. Its map scales it per texel. |
| **Transmission Color** | a colour | The tint light takes on as it passes through (green bottle glass, amber). Its map is stored for rendering engines. |
| **Depth** | 0 – 1000 cm | Over how many centimetres the tint is reached inside the material; 0 applies it at the surface. Stored for rendering engines (the viewport applies the colour as it is). |
| **Dispersion** | 0 – 100 | How much the index of refraction varies with wavelength — a diamond's fire. Stored for rendering engines. |
| **Thin Walled** | on / off | The surface is a single sheet (a window pane, a leaf, a soap bubble) with no solid interior: the tint is even across it, where a solid's deepens toward its edges. |

### Subsurface

| Row | Range | What it does |
| --- | --- | --- |
| **Translucency Weight** | 0 – 200 | How much light passes under the surface and comes back softened and reddened: skin's translucency, the warm light through ears and nostrils. 100 is the shader's full amount, 0 an opaque material. Scaled by the translucency colour map's brightness where the zone has one; its Translucency Weight map (the per-region thickness: ears, nostrils, fingertips) is stored for rendering engines. |
| **Translucency Color** | a colour | The colour light takes on under the surface: reddish for skin, warm for wax, green for jade. With a translucency colour map it tints the map. Stored for rendering engines; the viewport's skin profile is fixed. |
| **Scattering Color** | a colour | How far each colour of light scatters relative to the others — red deepest in skin, so its scattering colour is reddish; white scatters all colours alike (the source app's SSS Color). Stored for rendering engines. |
| **Scattering Distance** | 0 – 100 cm | How far light travels under the surface before it comes back (the source app's Scattering Measurement Distance; skin about 0.1 – 0.3, wax a few centimetres). Stored for rendering engines. |
| **Scattering Direction** | −100 – 100 | Which way light scatters inside: 0 evenly, forward towards 100, back towards −100 (the source app's SSS Direction). Stored for rendering engines. |

### Emission

| Row | Range | What it does |
| --- | --- | --- |
| **Emission Color** | a colour | The colour the surface gives off. With an emission map it tints the map. |
| **Emission** | 0 – 100 | How brightly the surface glows: 0 is none, 1 the colour at the scene's full radiance, more for a lamp or a screen. Shown in the **PBR Shaded** mode, where a bright emission blooms. |

### Geometry

| Row | Range | What it does |
| --- | --- | --- |
| **Opacity** | 0 – 100 | 100 is solid. Anything less is drawn see-through, blended over what is behind it — the eyes' cornea and moisture import this way. A cutout mask in the zone's Cutout slot still applies. |
| **Normal Strength** | 0 – 400 | How strongly the zone's normal map perturbs the surface: 100 is the map as painted, 0 flattens it, more exaggerates it. Greyed when the zone has no normal map. |
| **Bump Strength** | 0 – 400 | The same for a bump (height) map. Greyed without one. |
| **Ambient Occlusion** | 0 – 100 | How much a baked ambient-occlusion map (the Ambient Occlusion slot: grayscale, white is open) darkens the crevices. Greyed without one. The viewport applies it, over the occlusion it bakes per vertex at import, to the environment's light — a prop's authored occlusion where the vertex bake is coarse. |
| **Double Sided** | on / off | Render the back faces too — a leaf, a cloth card, an open shell. Stored for rendering engines; the viewport always lights both sides. |

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

Every map a material can carry has a **slot**, and each slot shows as a **small thumbnail beside the value it scales**: the base colour map beside Base Color, the roughness map beside Roughness (and beside Lobe 1 Roughness, whose map it is), the cutout beside Opacity, the normal and bump maps beside their strengths, the emission map beside Emission Color, the top coat's maps beside the Top Coat rows, and so on through the tab. The one map that scales no value, the detail layer's Weight Mask, has a row of its own in the Detail section with the bound file's name beside it. An empty slot is a dark square.

Every thumbnail works the same way:

- **Click** it to replace the slot with an image file — PNG, JPEG, TIFF, BMP, TGA or WebP. The file is read the way an import reads its maps (large images are scaled to the viewport's working size) and the figure shows it at once. The dialog opens where you last chose a texture.
- **Rest the pointer** on it for a moment to see the map at a larger size, with the file's name under it.
- **Right-click** it for a menu: **Choose Image…**, **Remove** (the zone renders without that map, on its values alone), **Restore Imported Map**, and **Open File Location** (the map's folder in your system's file browser; an image packed inside the model's file has none).
- The **restore arrow** on the line puts the map back along with the value.

The slots, by section: Base Color, Roughness, Metalness and Diffuse Roughness in Base; Specular Mask, Specular Color, Anisotropy, Anisotropy Rotation, Lobe 2 Roughness and Lobe Ratio in Specular (Lobe 1's map is the Roughness slot); the Top Coat's Weight, Color, Roughness, Reflectivity, Bump, Anisotropy and Anisotropy Rotation; Transmission and Transmission Color; Translucency Weight and Translucency Color in Subsurface; Sheen, Sheen Color and Sheen Roughness; Emission; Thin Film and Thin Film Thickness; Cutout, Normal, Bump, Displacement and Ambient Occlusion in Geometry; the Detail Normal Map, Detail Weight Mask and Detail Roughness in Detail.

A base colour, emission or tint map is read as a colour image (sRGB); a normal map as data; a roughness, metalness, diffuse roughness, specular mask or cutout file as a grayscale map — a colour file's red channel is used. The viewport samples the Base Color, Normal, Bump, Roughness, Metalness, Specular Mask, Translucency Color, Emission, Detail Normal Map and Cutout slots, and since the viewport round the Ambient Occlusion, Transmission, Top Coat Weight, Top Coat Roughness, Sheen, Sheen Color and Thin Film Thickness maps too (packed two to a texture behind the scenes, so a 4K map of each costs no more than before). The Diffuse Roughness, Specular Color, Anisotropy, Anisotropy Rotation, Lobe 2 Roughness, Lobe Ratio, Translucency Weight, Detail Weight Mask, Detail Roughness, Displacement, Transmission Color, Sheen Roughness, Thin Film weight and the Top Coat's Color, Reflectivity, Bump and Anisotropy maps are stored for rendering engines: the tab shows them and the project keeps them, the viewport does not sample them. A cutout mask is black where the surface is cut away and white where it is kept; a zone with a cutout is drawn in the see-through pass, as the eyelashes are.

The sliders scale the maps — Roughness multiplies a roughness map, Metallic a metalness map, the Base Color swatch tints the base colour map — so the map's own variation is kept.

## Resetting

- The button beside a row returns that value to what the file imported with.
- **Reset Zone** returns the whole zone's values — and its matching zones', when editing them together — to the imported material. The texture slots are put back one by one with their arrows.
- **Reset All Zones** returns every zone's values.

## Undo and projects

Each scrub, each typed value, each colour pick, each texture choice or clear and each reset is one undo step (`Ctrl+Z`), on the same history as pose and lighting edits. Undoing a material edit selects the object so you can see what changed.

A [project file](project-files.md) saves the zones whose material differs from the import — every value and every texture slot that was replaced (by its file's path) or cleared — and restores them when the project is opened, on the zone of that name, so a figure re-imported from its file gets its edited skin and its chosen maps back. A chosen texture whose file has since moved leaves the imported map in place, with a note in the log. A pose file does not carry materials.

## What the tab does not do

- It edits one object at a time. Two figures with the same skin are edited one after the other.
- The viewport draws the base layer (its reflectance from Specular, IOR and Specular Color, its anisotropy, its diffuse roughness), the coat (with its colour, index and anisotropy), sheen, the thin film, transmission (the frame behind the surface through it, tinted — not bent, not blurred), the subsurface weight, emission, opacity, the detail layer (its map, weight, roughness multiplier, tiling and offsets), a bump map beside a normal map, and the UV tiling, offset and rotation. Displacement, the subsurface's colour, scattering colour, distance and direction, transmission depth and dispersion, and the maps listed above as stored are saved for rendering engines but not shown. Double Sided changes nothing in the viewport, which always lights both sides.
- The simpler shading modes show what they can of a material: the textured modes show the base colour and the roughness, the matcap and clay modes show none of it. The **PBR Shaded** mode shows everything the viewport renders, and the data views (Albedo, Roughness Map, Specular Only) show one value at a time — see [Shading and Display](shading.md).
