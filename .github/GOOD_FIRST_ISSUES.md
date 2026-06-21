# Good First Issues

Drafts for the repo's first batch of contributor-facing issues. Copy each section
below into a new GitHub issue (Issues → New Issue), using the suggested title and
labels, then delete it from this file once it's live. Once this file is empty, it
should be deleted.

---

## 1. Stress-test the Asset Manager and report bugs

**Labels:** `good first issue`, `testing`

The Asset Manager (the left-side panel with the directory tree + asset grid) is the
most feature-complete part of PoseStudio right now, but it hasn't had broad usage
outside the core contributors. We need people to actually use it and break it.

**What to do**
1. Build PoseStudio (see README) and point it at a real asset library (or any folder
   full of nested subfolders + images).
2. Try the everyday flows: browsing folders, searching, creating Collections and
   nested sub-Collections, adding folders/assets to a Collection, renaming/deleting
   things, right-click context menus everywhere.
3. Restart the app between sessions — anything that *looked* like it saved but
   didn't survive a restart is a bug worth its own issue.
4. File one issue per bug you find, with repro steps and (if you can) a screenshot.
   Tag them `bug`.

**Good starting points for what "everything" includes**
- Directory tree: expand/collapse, Browse Folder, Find In Library
- Search Results
- Collections: create, rename, delete, nested sub-Collections, Add/Move/Copy To
  Collection, folder shortcuts inside a Collection
- The asset grid: thumbnails, tooltips, double-click to open

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

## 3. Build the Preferences UI on top of the existing PreferencesManager backend

**Labels:** `enhancement`, `help wanted`, `good first issue`

The data layer for preferences already exists (`src/preferencesmanager.h`/`.cpp`) —
a singleton cache backed by the `Preferences` SQLite table, with
`getValue(key, default)` / `setValue(key, value)` already implemented and loaded at
startup (`src/main.cpp`). What's missing is the actual UI.

The **Edit → Preferences** menu item already exists in `src/menumanager.cpp` but is
currently a disabled placeholder (`->setEnabled(false)`).

**Scope for a first pass**
1. A `PreferencesDialog` (QDialog) that reads/writes through the existing
   `PreferencesManager::instance()` API — no new persistence code needed.
2. Wire it up to the Edit → Preferences action and enable that menu item.
3. Pick a small, real starting set of preferences rather than trying to cover
   everything at once — e.g. theme/accent color, default asset library behavior, or
   thumbnail size (`Constants::GRID_ICON_DISPLAY_SIZE` is currently a hardcoded
   constant in `src/constants.h` and would be a good first candidate to make
   user-configurable).

This is a good first *subsystem* issue if you want something more substantial than
a bug fix but with a backend that already exists to lean on — you're building the
UI layer, not inventing the storage model.

---

## 4. Design and build an in-app Help/Documentation subsystem

**Labels:** `enhancement`, `help wanted`, `discussion`

The Help menu (`src/menumanager.cpp`) currently has disabled placeholders for
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
1. Wire up the existing Help menu actions in `src/menumanager.cpp` (currently all
   `setEnabled(false)`).
2. Whatever content/viewer mechanism is decided on above.

This is a bigger, less-defined task than the other issues here — best suited for
someone who wants to help shape a new subsystem from scratch rather than follow an
existing pattern. Comment if you want to propose a direction before starting.
