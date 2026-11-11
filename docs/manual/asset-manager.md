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

## Browsing

- **Expand a folder** in the tree with its arrow. Folders load as you open them.
- **Click a folder** to show its assets in the grid: subfolders first (as folder shortcuts), then the assets alphabetically. The **breadcrumb** above the grid shows where you are; click any segment to go there.
- **Folder icons** in the tree say whether a folder holds assets of its own, holds them only in subfolders, or holds none.
- **Double-click** an asset: a model (`.obj`), a figure (`.duf`, `.dsf`, `.fbx`, `.glb` / `.gltf`, `.vrm`, `.usd` / `.usda` / `.usdc` / `.usdz`) or a Gaussian splat scene (`.ply`, `.splat`, `.spz`) imports into the viewport; a script link (`.dsa`) is followed — see [Script links](#script-links); anything else opens in whatever application your system uses for that file type. Double-click a folder shortcut to enter the folder.
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

## Context menus

Right-click almost anything for its commands. In brief:

| On | Commands |
| --- | --- |
| A library folder in the tree | Add To Collection, Browse Folder, Expand / Expand Branch / Collapse, Manage Asset Folders, Refresh |
| A folder under Search Results | Find In Library, Browse Folder, then the same as above |
| **Collections** | Expand / Collapse, New Collection, Manage Asset Folders, Refresh |
| A collection | Rename Collection, Delete Collection, Expand / Collapse, New Sub-Collection, Manage Asset Folders, Refresh |
| **Favorites** | Refresh |
| Empty tree space | Manage Asset Folders, Refresh |
| An asset in the grid | Open (with the system's application), Find In Library and Browse Folder (outside the library view), Add To Favorites, Add To / Move To / Copy To Collection, Remove From Collection or Favorites, Refresh |
| A folder shortcut in the grid | Open, Find In Library, Browse Folder, Add To Collection, Refresh |
| Empty grid space | Browse Folder (for a real folder), Refresh |

*Open* on an asset always uses the system's default application; to import a model or figure, double-click it instead.
