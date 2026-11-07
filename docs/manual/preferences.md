# Preferences

**Edit → Preferences** opens the settings dialog. Pages are listed down its left side; every setting takes effect as soon as you change it, and **Close** is the only button. Settings are kept in PoseStudio's database in your user profile, so they survive upgrades and moving the application.

## General

**Send an anonymous install ping** (on by default). Each time it starts, PoseStudio lets the project's server know that this installation exists. The page lists exactly what the ping carries — an install ID, the version, your operating system and CPU type, installer or portable, the Qt version and the time — and nothing else. Below the toggle the page shows your **Install ID**, or *not created yet* if no ping has been sent. Builds without a ping key say so and never send anything. See [Privacy and Updates](privacy-updates.md).

**Check for updates at startup** (on by default). A few seconds after launch PoseStudio asks GitHub for the latest release and tells you when a newer one exists. Nothing but the request is sent. **Help → Check for Updates…** does the same on demand.

**Show What's New after an update** (on by default). The first time a new version of PoseStudio starts, a window with that version's release notes opens once the splash screen is dismissed — see [Privacy and Updates](privacy-updates.md#whats-new-after-an-update). **Help → What's New…** opens the same window on demand.

**Count camera changes as unsaved** (off by default). Moving the camera — orbiting, panning, dollying, switching named views, framing the selection — does not mark a project dirty, so closing the window or starting a new scene after a camera move asks nothing. Turn this on if you want a changed framing to count as an unsaved change; the camera is always saved with the project file either way.
## Posing

The soft-body switches and dials — on and off, the breasts' Softness and Bounce, the buttocks' Softness — live in the side panel's **Soft Body Physics** tab (see [The Soft Body Physics tab](posing.md#the-soft-body-physics-tab)). They are remembered between sessions like a preference.

## VR

**Enable VR support** (off by default). PoseStudio connects to the headset runtime (Meta Quest Link, Virtual Desktop, SteamVR — anything that speaks OpenXR) as it starts, so turning this on takes a restart; **View → VR Mode** offers the restart itself the first time you choose it. **Status** says what the startup found: the headset and runtime that are ready, or why VR Mode cannot start. **Render scale** is the resolution each eye is rendered at, as a fraction of the headset's recommended size — lower it if the headset image stutters, raise it on a strong GPU. **Reduced effects in the headset** skips the skin's subsurface blur and the bloom per eye for a lighter GPU. **Splat scale** is the most of each eye's size a Gaussian splat scene is drawn at (1.00 by default): the splats are the dearest part of a headset frame, so PoseStudio lowers their size on the fly when a frame runs late and raises it back toward this as the GPU finds room. All three apply when VR Mode next starts. See [VR Mode](vr.md).

## Interface, Input, Navigation, System

These pages are placeholders: they name the settings that are planned (appearance, keyboard bindings, viewport navigation, performance and storage) but hold none yet.

## Assets

The list of your **asset libraries** — the folders the [Asset Manager](asset-manager.md) browses. The built-in *Maquettes* library is always present and is not listed here.

- **Add Asset Folder…** opens a folder chooser (starting next to the folder you added last). Pick the root folder of a content library. A folder that is already registered is refused with a note saying so.
- **Remove Selected** unregisters the selected folder. Nothing is deleted from disk, and any Favorites and Collections that point into it keep their entries.
- **Double-click** a library to jump the Asset Manager to it; the dialog closes so you can see it.

The Asset Manager updates as soon as you add or remove a library.

## Factory Reset

Wipes everything PoseStudio stores about you — asset libraries, collections, favorites and every preference — and restarts the application as if freshly installed. Your files on disk are untouched, including *My PoseStudio Library* and your HDRIs; only the database is reset (the default library is registered again at the restart, and the install ID is regenerated).

The button is disabled until you type **RESET** into the box, in any case. There is no further confirmation and no undo.
