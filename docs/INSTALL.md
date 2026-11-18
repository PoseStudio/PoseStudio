# Installing PoseStudio

Welcome! This guide walks you through downloading, installing, and running PoseStudio on Windows — and what to do if something doesn't work. No technical knowledge is needed.

> **In a hurry?** Download [`PoseStudio-…-Windows-Setup.exe` from the latest release](https://github.com/PoseStudio/PoseStudio/releases/latest), run it (click **More info → Run anyway** if Windows warns you), and launch PoseStudio from the Start Menu. That's it.

---

## What you need

| | Requirement |
|---|---|
| **Operating system** | 64-bit Windows 10 or 11 |
| **Graphics card** | Any Vulkan-capable GPU — virtually every graphics card from 2016 onward (NVIDIA GTX 600+, AMD GCN+, Intel Skylake+) — with reasonably current drivers. Without one, PoseStudio still opens, but the 3D viewport shows an error instead of the scene |
| **Disk space** | About 500 MB |
| **Administrator rights** | **Not needed** — PoseStudio installs into your own user account |

macOS and Linux versions are planned but not available yet — the code is built to be portable, and packaged builds will follow. [Watch the repository](https://github.com/PoseStudio/PoseStudio) to be notified.

---

## Step 1 — Download

Go to the **[latest release page](https://github.com/PoseStudio/PoseStudio/releases/latest)** and, under **Assets**, download:

| You want… | Download this file |
|---|---|
| **The app, installed normally** (recommended) | `PoseStudio-<version>-Windows-Setup.exe` |
| A no-install folder you can put anywhere (USB stick, etc.) | `PoseStudio-<version>-Windows-Portable.zip` |
| To verify your download's integrity | `SHA256SUMS.txt` |

Your browser may warn that the file "isn't commonly downloaded." That's expected for a small project — choose **Keep** (in Chrome/Edge you may need to click the download, then **⋯ → Keep → Keep anyway**).

## Step 2 — Run the installer

Double-click the downloaded `PoseStudio-<version>-Windows-Setup.exe`.

### "Windows protected your PC"?

The first time you run it, Windows SmartScreen will likely show a blue screen saying **"Windows protected your PC"**. This is normal and expected:

1. Click **More info**
2. Click **Run anyway**

**Why does this happen?** Windows shows this warning for any program whose publisher hasn't bought a code-signing certificate (they cost hundreds of dollars a year). PoseStudio is a young open-source project and doesn't have one yet. You don't have to take our word for what's in the installer, though — it is built automatically and in public by [GitHub Actions](https://github.com/PoseStudio/PoseStudio/actions) directly from [the source code in this repository](https://github.com/PoseStudio/PoseStudio), and every release ships a `SHA256SUMS.txt` so you can verify your download matches what CI produced.

### The installer itself

Click through the wizard — accept the license (PoseStudio is free software under the GPL v3), optionally add a desktop icon, and click **Install**. There's no administrator password prompt: PoseStudio installs into your own user account.

At the end, leave **Launch PoseStudio** checked and click **Finish**.

## Step 3 — First launch

PoseStudio opens under its splash screen — click anywhere to dismiss it (the first time a version starts, a **What's New** window with that version's release notes opens next). You should then see the PoseStudio window: a 3D viewport with a floor grid on the left and a side panel on the right with its tabs — **Asset Manager**, **Transform**, **Materials**, **Materials Library**, **Soft Body Physics**, **Environment** and **Lights**. Some things to try (the built-in manual — **Help → User Manual**, or **F1** — has the rest):

- **Orbit the camera** — left-drag in the viewport; scroll to zoom; middle-drag to pan. Blender-style view keys: **1** / **3** / **7** for the front / right / top view (**Ctrl** for the opposite side; these are flat orthographic views — orbit to return to perspective), **9** flips the view, **5** is the Home view, **.** frames the selected object.
- **Import a figure or model** — File → Import, or double-click an asset in the Asset Manager. The selected object wears a blue outline; click another object to select it, or empty space to clear the selection.
- **Pose a figure** — grab any part of the figure (a hand, a foot, the hips, the head…) and drag: the whole body follows with full-body IK — the feet stay planted, she balances and steps, and the hips taken down and forward kneel her, down and back sit her. **Ctrl+drag** moves just the limb you grabbed (on the torso it bends the torso alone; on the hips it moves the whole figure as she is posed). To turn one joint, select it and press **B**, **S** or **T**, then move the mouse to bend, tilt or twist it (click to keep, **Esc** to cancel) — or set it by number on the **Transform** tab. **P** pins the selected joint. The manual's *Poses Step by Step* page shows how to make every pose.
- **Change the lighting** — open the **Environment** tab (right side) and pick a different HDRI environment from the thumbnail picker. The stock set of 33 lighting environments was installed for you.

### Where things end up

| What | Where | Safe to delete? |
|---|---|---|
| The application | `C:\Users\<you>\AppData\Local\Programs\PoseStudio` | Use the uninstaller instead |
| **Your content library** (HDRIs, your saved assets) | `Documents\My PoseStudio Library` | It's yours — but deleting it removes your content |
| Settings + asset database | `C:\Users\<you>\AppData\Roaming\PoseStudio` | Yes — resets the app to defaults |

### Privacy — the install ping

Each time it starts, PoseStudio sends one small request to `posestudio.io` saying that this installation exists. It carries a random install ID (generated on first launch, not tied to you), the app version, your operating system and CPU type, whether the app was installed or runs portable, the Qt library version it was built with, and the time of the ping — nothing else. No names, no file names, no usage data, and it never slows the app down or blocks anything. It's how we count active installs and see which versions people are on.

To switch it off: **Edit → Preferences → General** and untick *Send an anonymous install ping*. The same page shows your install ID and exactly what is sent.

A few seconds after it starts, PoseStudio also asks GitHub whether a newer release exists, and shows a message with a link to the download page when one does. Nothing but the request itself is sent. *Check for updates at startup*, on the same Preferences page, switches it off.

---

## The portable version

If you chose the `…-Portable.zip` instead:

1. Unzip it anywhere (e.g. `C:\Apps\PoseStudio` or a USB drive) and run `PoseStudio.exe` from the unzipped folder (same **More info → Run anyway** note as above).
2. **One-time step:** the zip includes the stock lighting environments in its `stock-content\hdri` folder, but the app looks for them in `Documents\My PoseStudio Library\hdri`. Copy the *contents* of `stock-content\hdri` there (start the app once first so the library folder exists). The `README.txt` in the zip repeats these instructions.

## Updating

Just download and run the new version's installer — it upgrades your existing install in place. Your content library, preferences, collections, and favorites are all kept. (Portable users: unzip the new version over — or next to — the old folder; your library and settings live outside it and are untouched.)

## Uninstalling

**Settings → Apps → Installed apps → PoseStudio → Uninstall** (or Start Menu → right-click PoseStudio → Uninstall).

The uninstaller deliberately does **not** delete your personal content or settings. For a complete removal, also delete:

- `Documents\My PoseStudio Library` (your content — including the stock HDRIs)
- `C:\Users\<you>\AppData\Roaming\PoseStudio` (settings + asset database)

---

## Troubleshooting

**The viewport shows an error message instead of a 3D grid.**
Your GPU driver's Vulkan support couldn't be initialized. Update your graphics drivers ([NVIDIA](https://www.nvidia.com/drivers) / [AMD](https://www.amd.com/en/support) / [Intel](https://www.intel.com/content/www/us/en/download-center/home.html)) and restart. This can also happen on virtual machines and on very old GPUs (pre-2016) without Vulkan support.

**The Environment panel's HDRI picker is empty.**
The stock environments live in `Documents\My PoseStudio Library\hdri`. The installer places them there automatically; portable users need the one-time copy step above. Any `.hdr` or `.exr` panorama you drop into that folder (subfolders become categories) appears in the picker immediately — free ones are at [Poly Haven](https://polyhaven.com/hdris).

**My antivirus flagged the download.**
Unsigned new executables sometimes trip heuristic scanners (the same root cause as the SmartScreen warning). Verify your download against `SHA256SUMS.txt` from the release page — in PowerShell: `Get-FileHash .\PoseStudio-<version>-Windows-Setup.exe` — and compare the hash. If it matches and your scanner still objects, you can [check the file on VirusTotal](https://www.virustotal.com) and report the false positive to your AV vendor.

**Something else?**
Ask in [GitHub Discussions](https://github.com/PoseStudio/PoseStudio/discussions) or on [Discord](https://discord.gg/SaKvt9aYCM) — we're friendly, and "this didn't work on my machine" reports are exactly what this release is for.

---

## 💬 Tell us what you think

PoseStudio is in early development, and this build exists **so you can try it and tell us about it**. Nothing is too small to mention — especially the moments where you got stuck or something surprised you:

- **[Leave usability feedback](https://github.com/PoseStudio/PoseStudio/issues/new?template=usability_feedback.yml)** — a short guided form
- **[Report a bug](https://github.com/PoseStudio/PoseStudio/issues)**
- **[GitHub Discussions](https://github.com/PoseStudio/PoseStudio/discussions)** for ideas and questions
- **[Discord](https://discord.gg/SaKvt9aYCM)** to chat with the community

Thank you for trying PoseStudio! 🎉
