## 📥 Download & Install

**Most people want this file → [`PoseStudio-{{VERSION}}-Windows-Setup.exe`](https://github.com/PoseStudio/PoseStudio/releases/download/v{{VERSION}}/PoseStudio-{{VERSION}}-Windows-Setup.exe)**

### 🛡️ Heads-up: Windows will warn you. Nothing is wrong with the download.

> [!IMPORTANT]
> When you run the installer, Windows shows a blue **“Windows protected your PC”** screen.
>
> ### 👉 Click **More info** → **Run anyway**. That's it.
>
> **This is not a virus warning.** Windows SmartScreen shows that screen for *every* installer it hasn't seen from a paid, certificate-signed publisher — PoseStudio is a free open-source project and hasn't bought a code-signing certificate yet. The warning is about our paperwork, not about the file.
>
> **What you can check instead, which a certificate would never give you:**
> - Every file below is compiled **in public** by [GitHub Actions](https://github.com/PoseStudio/PoseStudio/actions) straight from this repository — the build log and every line of source are open.
> - `SHA256SUMS.txt` (in the Assets list) lets you verify your download matches that build, byte for byte.
> - The installer needs **no administrator rights** and installs only into your own user account.
>
> 📖 [Screenshots of the warning and exactly what to click →](https://github.com/PoseStudio/PoseStudio/blob/main/docs/INSTALL.md#windows-protected-your-pc)

1. **Download** `PoseStudio-{{VERSION}}-Windows-Setup.exe` (from the **Assets** list below).
2. **Run it** and click through the blue warning as above (**More info → Run anyway**).
3. **Click through the installer** (no administrator password needed) and launch PoseStudio from the Start Menu.

Full step-by-step instructions, including screenshots of the SmartScreen warning and troubleshooting help: **[Installation Guide](https://github.com/PoseStudio/PoseStudio/blob/main/docs/INSTALL.md)**.

| You want… | Download |
|---|---|
| **The app, installed normally** (recommended) | `PoseStudio-{{VERSION}}-Windows-Setup.exe` |
| A no-install folder you can put anywhere | `PoseStudio-{{VERSION}}-Windows-Portable.zip` |
| To verify your download | `SHA256SUMS.txt` |

**System requirements:** 64-bit Windows 10 or 11, and a Vulkan-capable GPU (virtually any graphics card from 2016 onward) with reasonably current drivers.

**Privacy:** each time it starts, the app sends a small anonymous ping (a random install ID, the version, your OS/CPU type, installed-vs-portable, the Qt version, and the time — nothing else) so we can count active installs. Switch it off any time in Edit → Preferences → General.

## 💡 Quick Tips — the stuff that isn't obvious

- 🖱️ **Camera** — left-drag empty space to orbit, scroll to zoom, middle-drag to pan. The 🏠 button (top-right of the viewport) resets the view. Blender-style view keys: **1** / **3** / **7** for the front / right / top view (**Ctrl** for the opposite side; these are flat orthographic views — orbit to return to perspective), **9** flips the view, **5** is the Home view, **.** frames the selected object.
- 🔵 **Selection** — the selected object wears a blue outline. Click an object (or one of a figure's joints) to select it; click empty space to clear the selection. Orbiting never changes it. **Delete** removes the selected object.
- 🦴 **Posing — the magic one:** just grab the figure's joints directly (hand, elbow, knee, head…) and drag. There's no visible skeleton by default, but every joint is grabbable — the part you grab lights up blue — and the whole body follows naturally: feet stay planted, balance is automatic. Keep dragging sideways and the figure *steps and walks* to follow. Pull a hand beyond reach and the figure lifts off and hangs. Drag the hips downward for a crouch, down and forward for a kneel, down and back for a sit. **Ctrl+drag** moves just that limb (on the torso it bends the torso alone; on the hips it moves the whole figure); **Shift+click** selects several joints and **Shift+drag** moves them together; press **B**, **S** or **T** and move the mouse to bend, tilt or twist the selected joint (click to keep, Esc to cancel), or set it by number on the Transform tab.
- 📌 **Pin joints** — select a joint and press **P** (or right-click → Pin Joint). Pinned joints stay exactly put while you drag everything else — plant a hand on a table and crouch under it. Pins save with poses.
- 🎯 **Depth while posing** — roll the mouse wheel *while dragging* a joint to push it away from the camera or pull it toward you; it stays under the pointer, and the body follows.
- 🪞 **Mirror & reset** — Edit → **Mirror Pose** swaps the left and right sides; **Mirror Limb to Other Side** copies the selected arm or leg across; **Reset Selected Joint / Reset Limb / Reset Pose** put things back at rest. Right-click a joint for the per-joint ones. All undoable.
- 🎭 **Importing figures** — File → Import → .DUF for a native figure, or .FBX for a rigged character from another tool (it is read into PoseStudio's rig standard and poses the same way); or double-click one in the Asset Manager. If a native figure asks, point it at your content folder once — it's remembered.
- ⬇️ **Ground button** — drops a floating figure onto the floor. In actual free fall.
- 💾 **Poses** — File → Save Pose / Load Pose. **Ctrl+Z / Ctrl+Y** undo and redo — poses *and* lighting edits.
- 🌅 **Lighting** — Environment tab (right side) → click the environment name for the thumbnail picker. Drop your own `.hdr`/`.exr` files into `Documents\My PoseStudio Library\hdri` — subfolders become categories. And any number field: drag left/right to scrub, click to type.
- 🎨 **Shade modes** — the picker at the viewport's top-right: photoreal PBR, textured, cartoon, matcap, clay, lighting-only, silhouette, wireframes (including a hidden-line one), and data views for albedo, ambient occlusion, roughness, specular, normals, and UVs. View → Show Skeleton overlays the bone cage.

## 💬 We want your feedback!

This release exists so you can try PoseStudio and tell us what's confusing, broken, or missing:

- 🗣️ [Leave usability feedback](https://github.com/PoseStudio/PoseStudio/issues/new?template=usability_feedback.yml) — even "I couldn't figure out how to X" is gold
- 💬 [Join our Discord](https://discord.gg/SaKvt9aYCM) or [GitHub Discussions](https://github.com/PoseStudio/PoseStudio/discussions)
- 🐛 [Report a bug](https://github.com/PoseStudio/PoseStudio/issues)

See the [CHANGELOG](https://github.com/PoseStudio/PoseStudio/blob/main/CHANGELOG.md) for the detailed list of what's new in this version.

---
