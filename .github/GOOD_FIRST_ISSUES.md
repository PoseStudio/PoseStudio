# Good First Issues

Drafts for the repo's first batch of contributor-facing issues. Copy each section
below into a new GitHub issue (Issues → New Issue), using the suggested title and
labels, then delete it from this file once it's live. Once this file is empty, it
should be deleted.

---

## 1. Stress-test the Asset Manager and report bugs

**Labels:** `good first issue`, `testing`

The Asset Manager (the left-side panel with the directory tree + asset grid) is one of the
most feature-complete parts of PoseStudio, but it hasn't had broad usage outside the core
contributors. We need people to actually use it and break it.

**What to do**
1. Build PoseStudio (see README) and point it at a real asset library (or any folder
   full of nested subfolders + images).
2. Try the everyday flows: browsing folders, searching, creating Collections and
   nested sub-Collections, adding assets to a Collection, favoriting assets,
   drag-reordering Favorites/Collections, renaming/deleting things, right-click
   context menus everywhere.
3. Restart the app between sessions — anything that *looked* like it saved but
   didn't survive a restart is a bug worth its own issue.
4. File one issue per bug you find, with repro steps and (if you can) a screenshot.
   Tag them `bug`.

**Good starting points for what "everything" includes**
- Directory tree: expand/collapse, Browse Folder, Find In Library
- Search Results
- Collections: create, rename, delete, nested sub-Collections, Add/Move/Copy To
  Collection, drag an asset onto a Collection node
- Favorites: add/remove, drag-to-reorder
- The asset grid: thumbnails, tooltips, double-click to import into the viewport

No code changes required for this issue — it's pure usage and bug reporting. Great
first contribution if you're not ready to dive into the C++/Qt code yet.

---

## 2. Use the Asset Manager and tell us what's missing or confusing

**Labels:** `good first issue`, `feedback`

Separate from bug-hunting (see the issue above), we want opinions: what's awkward,
what's missing, what would you expect that isn't there yet?

**What to do**
1. Use the Asset Manager for a real task — organizing an actual asset library into
   Collections the way you'd actually want to browse it.
2. Open a single issue (or a GitHub Discussion if you'd rather) with your notes.
   Some prompts, but don't feel limited to them:
   - What did you expect to be able to right-click and couldn't?
   - Where did the UI surprise you (in a bad way)?
   - What's missing from Collections/search/folder browsing that you'd want before
     this feels "done"?
   - Anything about thumbnails, tooltips, or layout that felt off?

This is feedback, not a bug report — vague is fine, "this felt clunky" is a useful
data point. No code required.

---

## 3. Add a View → Show Skeleton toggle for the posing overlay

**Labels:** `enhancement`, `help wanted`, `good first issue`

The viewport can draw a colored skeleton overlay (joint→parent line segments) over a
posable figure, but it's currently hidden with no way to turn it on: the renderer-side
plumbing (`Scene::setShowSkeleton` in `src/viewport/scene/scene.h`, forwarded through
`VulkanRenderer`/`VulkanWindow`/`ViewportWidget`) already exists and is deliberately
retained for exactly this feature — nothing in the UI sets it yet.

**Scope for a first pass**
1. Add a **View** menu to the menu bar (`src/shell/menumanager.cpp`) with a checkable
   "Show Skeleton" action.
2. Route it through `ViewportWidget` to the existing `setShowSkeleton` plumbing (see
   how the shade-mode setter travels the same path), and make sure the viewport
   repaints on toggle (`requestUpdate()` — see the event-driven-rendering note in
   `src/viewport/vulkanwindow.cpp`).
3. Optionally persist the choice via `PreferencesManager::instance()` so it survives
   restarts.

A small, well-bounded feature where every layer you need already has a worked example
to copy from — good if you want your first change to touch the real rendering path
without writing any Vulkan.

---

## 4. Design and build an in-app Help/Documentation subsystem

**Labels:** `enhancement`, `help wanted`, `discussion`

The Help menu (`src/shell/menumanager.cpp`) currently has disabled placeholders for
"Release Notes", "Tutorials", and "Support" — there's no backend or design for this
yet, so this issue is as much about deciding the approach as building it.

**Open questions to settle first** (let's discuss in the issue/Discussions before
code):
- In-app documentation viewer (rendering local Markdown/HTML), or just open links
  out to the website/Discord?
- Where does content live — bundled in `resources/` and shipped with the app, or
  fetched live from the docs site?
- Does "Release Notes" pull from `CHANGELOG.md` automatically, or get curated
  separately?

**Once direction is picked, rough scope**
1. Wire up the existing Help menu actions in `src/shell/menumanager.cpp` (currently
   all `setEnabled(false)`).
2. Whatever content/viewer mechanism is decided on above.

This is a bigger, less-defined task than the other issues here — best suited for
someone who wants to help shape a new subsystem from scratch rather than follow an
existing pattern. Comment if you want to propose a direction before starting.
