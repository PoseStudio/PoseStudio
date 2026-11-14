# The Asset Manager

The **Asset Manager** tab on the side panel is where your content lives. It browses any number of folders on disk as *asset libraries*, finds assets in them, and lets you gather assets from anywhere into **Favorites** and nested **Collections** without moving a file.

The panel has two parts, with a draggable divider between them: the **tree** above (search results, Favorites, Collections, and one root per library) and the **grid** below (the thumbnails of whatever you selected in the tree), with an info bar under the grid counting what it shows.

## Asset libraries

An asset library is simply a folder. PoseStudio starts with two:

- **Maquettes** — a small built-in library that ships with the application.
- **My PoseStudio Library** — created in your Documents folder on first launch, for your own content.

Add your own with **Edit → Preferences → Assets → Add Asset Folder…**, or with the **Add Asset Folder** link that appears in the tree when no library exists. Pick the *root* folder of a content library; everything beneath it is browsed. Remove a library from the same page — that only unregisters the folder, nothing on disk is touched. The tree lists the built-in library first, then yours by folder name, and skips a library whose folder has gone missing.

**Manage Asset Folders** in the tree's context menus is a shortcut to that Preferences page.

## What shows up as an asset

PoseStudio does not know your file formats; it knows thumbnails. **A file is an asset when an image with the same name sits beside it in the same folder**: `Elena.duf` next to `Elena.png` is an asset shown with that picture, and so is `chair.obj` next to `chair.jpg`. A file without a matching image is not listed, and an image without a matching file is just an image. Recognised image types are png, jpg, jpeg, bmp, webp, gif, tif and tiff; if several match, the largest is used. Material sidecars (`.mtl`) are never listed as assets of their own.

Most character content is delivered this way — every preset comes with its own preview — so a content folder added as a library is browsable at once.

A folder named `textures` is never listed, in the tree or in the grid: it is where a saved material keeps the pictures it uses ([Where the textures go](materials.md#where-the-textures-go)), not a place that holds assets. Do not give a category folder that name.

## Browsing

- **Expand a folder** in the tree with its arrow. Folders load as you open them.
- **Click a folder** to show its assets in the grid: subfolders first (as folder shortcuts), then the assets alphabetically. The **breadcrumb** above the grid shows where you are; click any segment to go there.
- **Folder icons** in the tree say whether a folder holds assets of its own, holds them only in subfolders, or holds none.
- **Double-click** an asset: a saved material (`.psm`) is applied to the selected object's zone — see [Saving and reusing a material](materials.md#saving-and-reusing-a-material); a model (`.obj`), a figure (`.duf`, `.dsf`, `.fbx`, `.glb` / `.gltf`, `.vrm`, `.usd` / `.usda` / `.usdc` / `.usdz`) or a Gaussian splat scene (`.ply`, `.splat`, `.spz`) imports into the viewport; a script link (`.dsa`) is followed — see [Script links](#script-links); anything else opens in whatever application your system uses for that file type. Double-click a folder shortcut to enter the folder.
- **Hover** over an asset for its file type, size, modification date and folder.
- **Refresh** (in every context menu) rescans after you change files on disk.

## Script links

Character content often comes with small **scripts** (`.dsa`) among its presets — a thumbnail labelled with the character's name in a *Characters* folder, a vendor's web address in each of the product's folders. Most of them are links: the character's script points at the folder that holds the character, the web-address one at the vendor's site. PoseStudio never runs a script, but it reads a plain-text one for its link and follows it when you double-click it:

- **A link to a folder** shows that folder — in the tree too, when it lies in one of your libraries. When the folder holds exactly **one character**, that character is imported into the viewport as well, as if you had double-clicked it. Clothing, hair and props are not characters: a link to a folder of boots shows the folder and leaves the choice to you, and so does a folder with several characters in it.
- **A link to a file** — a script that loads a preset — opens that file as its own double-click would.
- **A link to a web page** opens in your browser.

A script that is not a link (a program, a tool), an encrypted (`.dse`) or compiled (`.dsb`) script, or a link whose target has gone missing is explained in a message instead, which offers **Open in Default Application** if you want the application that owns the script to run it. The same scripts given to PoseStudio on the command line (or with *Open with*) are followed the same way.

A link written relative to a content library ("/People/…") is looked for in the library the script lies in, then in your asset libraries and the content folders you have located for figures (see [Importing Content](importing.md#when-the-figures-data-cannot-be-found)).

## Searching

Type in the field at the top and press `Enter` (or click the magnifier). The search matches file *and* folder names, case-insensitively, anywhere in every library. Results appear under **Search Results** at the top of the tree as the folders that contain matches; click one to see them in the grid. The clear button (or emptying the field) removes the results.

## Favorites

**Favorites** is one flat list of assets you want at hand. Right-click an asset and choose **Add To Favorites**, or drag it from the grid onto *Favorites* in the tree. Click *Favorites* to see the list; drag thumbnails within the grid to reorder it — the order is yours and is kept. **Remove From Favorites** takes an asset out again.

## Collections

**Collections** are named groups of assets that can nest as deep as you like — *Characters / Fantasy / Elves* — and hold assets from any library side by side. Creating and filling them:

- Right-click **Collections** → **New Collection**, or right-click a collection → **New Sub-Collection**. The new collection appears ready to be renamed; type the name and press `Enter`.
- Right-click an asset → **Add To Collection** and choose an existing collection or *New Collection*. Or **drag** the asset onto a collection in the tree — or onto the *Collections* header to make a new one for it.
- Right-click a **folder** → **Add To Collection** to make a collection out of that folder's own assets (subfolders are not included), as a new top-level collection or under an existing one.
- Inside a collection, right-click an asset for **Move To Collection**, **Copy To Collection** and **Remove From Collection**. Dragging an asset from one collection onto another moves it.
- Drag a collection onto another (or onto the header) to move it, with everything inside it.
- **Rename Collection** (or select it and press `F2`) renames it in place. **Delete Collection** is available only when the collection is empty.

Like Favorites, a collection's grid is **sortable**: drag thumbnails to set the order, and the info bar says *Sortable* to remind you. Hold a dragged asset over a collapsed collection for a moment and it opens, so you can drop deeper.

**Find In Library** and **Browse Folder**, offered on assets shown in Favorites, Collections or Search Results, jump the tree to the asset's real folder or open that folder in Explorer.

## The Materials Library

The **Materials Library** tab, right under **Materials** on the side panel, is the Asset Manager over one folder: the **Materials** folder of your *My PoseStudio Library* (`Documents/My PoseStudio Library/Materials`, or wherever you keep that library). It is the same browser — the tree, the grid, the breadcrumb, the search, Favorites, Collections, the drags and the context menus all work exactly as this page describes — and whatever the Asset Manager learns to do, the Materials Library does too. What is different follows from its looking at one folder:

- **One library.** The tree has a single folder root, *Materials*, which is open when you first show the tab, with its contents in the grid. Its subfolders are your categories — *Skin*, *Metals*, *Glass*, and whatever you make inside them. There is nothing to add or remove, so the context menus have no *Manage Asset Folders*. The folder is created if it does not exist yet.
- **Adding materials** is putting files into that folder: right-click a folder → **Browse Folder** opens it in Explorer, and **Refresh** shows what you put there. What counts as a material is the same rule as for every asset: a file with an image of the same name beside it, which becomes its thumbnail.
- **Search** looks in the Materials folder and nowhere else.
- **Collections are the library's own.** A collection made here groups materials and is listed here; the Asset Manager's collections are not shown in this tab, nor this tab's in the Asset Manager.
- **Favorites are shared.** There is one Favorites list, and the Materials Library shows the favorites that lie in the Materials folder — so a material you favorite in either tab is a favorite in both. Dragging the favorites into an order here rearranges the materials among themselves and leaves your other favorites where they are.

The Materials folder is part of *My PoseStudio Library*, which is one of the Asset Manager's libraries, so the same files can also be reached there; the tab is the short way in.

**Double-clicking a MaterialX material** (`.mtlx`) does the same with one of that file's materials — see [MaterialX materials](importing.md#materialx-materials-mtlx).

**Double-clicking a saved material** (`.psm`) puts it on the selected object: on the zone the Materials tab shows, as one undo step. Materials get here from the Materials tab's **Save** button, which writes the material and the picture of it on a ball that is its thumbnail — see [Saving and reusing a material](materials.md#saving-and-reusing-a-material). Any other file opens exactly as it does in the Asset Manager.

## Working with saved materials

A material saved by PoseStudio (`.psm`) is three things on disk — the file, its icon, and its texture files in the `textures` folder beside it ([Where the textures go](materials.md#where-the-textures-go)) — and the library moves them together, so you never have to do it by hand in a file manager. Right-click a saved material, in the Materials Library or the Asset Manager:

- **Rename…** gives the material and its icon a new name. Its textures stay where they are, and the material finds them as before.
- **Duplicate** makes "*name* copy" beside it, with a copy of the icon. The copy uses the same texture files; nothing is copied in `textures`.
- **Move To Folder…** moves the material and its icon to the folder you choose, and takes its textures along into that folder's `textures` (a file that folder already has is not copied again). What the old folder's `textures` then holds for no material is removed; a file another material there still uses stays.
- **Delete** asks, then moves the material and its icon to the trash, together with the texture files that no other material in the folder uses.
- **Export Package…** packs it for sharing — see below.

Favorites and collections follow a material that was renamed or moved, and drop one that was deleted.

These commands are offered for materials PoseStudio saved. Content from other applications' libraries is read where it lies; PoseStudio never renames, moves or deletes it.

## Packages: sharing materials

A **package** is an ordinary `.zip` file that holds materials with their icons and textures, laid out exactly as they lie in the library.

- **Export Package…** on a saved material packs that material, its icon and the texture files it uses. On a folder — in the tree, in the grid, or by right-clicking the grid's empty space for the folder shown — it packs the folder with everything in it, under the folder's name.
- **Install Package…** on a folder in the tree, or on the grid's empty space, unpacks a package into that folder. Files that are already there and identical are skipped; if some are there with different content, you are asked whether to **Replace** them or **Keep Existing**. A package that would place a file outside the folder you chose is refused, and nothing of it is installed.

Because a package is just a zip of the folder layout, unzipping one into a library folder with any other tool installs it just as well; **Refresh** then shows it.

## Context menus

Right-click almost anything for its commands. In brief:

| On | Commands |
| --- | --- |
| A library folder in the tree | Add To Collection, Browse Folder, Export Package, Install Package, Expand / Expand Branch / Collapse, Manage Asset Folders, Refresh |
| A folder under Search Results | Find In Library, Browse Folder, then the same as above |
| **Collections** | Expand / Collapse, New Collection, Manage Asset Folders, Refresh |
| A collection | Rename Collection, Delete Collection, Expand / Collapse, New Sub-Collection, Manage Asset Folders, Refresh |
| **Favorites** | Refresh |
| Empty tree space | Manage Asset Folders, Refresh |
| An asset in the grid | Open, Find In Library and Browse Folder (outside the library view), Add To Favorites, Add To / Move To / Copy To Collection, Remove From Collection or Favorites, Refresh — and for a saved material: Rename, Duplicate, Move To Folder, Export Package, Delete |
| A folder shortcut in the grid | Open, Find In Library, Browse Folder, Add To Collection, Export Package, Refresh |
| Empty grid space | Browse Folder, Export Package and Install Package (for a real folder), Refresh |

*Open* on a saved material applies it, as a double-click does. On any other asset it uses the system's default application; to import a model or figure, double-click it instead.

The Materials Library's menus are the same, without *Manage Asset Folders*.
