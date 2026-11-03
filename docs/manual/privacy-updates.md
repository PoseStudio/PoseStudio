# Privacy and Updates

PoseStudio makes two small network requests, both optional, and nothing else ever leaves your computer. Your content, your poses and your settings are never uploaded.

## The anonymous install ping

Two seconds after it starts, an official PoseStudio build sends one signed message to `posestudio.io` saying that this installation exists. It carries:

- a random **install ID** created the first time a ping is sent (a UUID: not derived from your machine, your account or anything else about you),
- the **application version**,
- the **operating system** name and version, and the **CPU architecture**,
- whether the application was **installed or is running portable**,
- the **Qt library version**,
- the **time** of the ping.

Nothing else — no names, no file names, no usage data, no location. The server counts distinct install IDs, which is how the project learns how many people use it and which versions are out there. There is no daily cap; every launch pings, and the server's counting keeps that honest.

Turn it off with **Edit → Preferences → General → Send an anonymous install ping**. The same page shows your install ID. A build compiled from source has no ping key and sends nothing regardless of the setting — the page says so.

## The update check

Three seconds after it starts, PoseStudio asks GitHub for the project's latest release — a plain request to the public releases API with nothing but the application's name and version as its user agent. When a newer version exists you get a message:

- **Open Download Page** opens the release in your browser.
- **Skip This Version** silences the startup message for that version; a still newer one will be offered again.
- **Later** asks again next launch.

The message does not block you; keep working and answer it when you like. Turn the startup check off with **Edit → Preferences → General → Check for updates at startup**.

**Help → Check for Updates…** runs the same check whenever you want and always reports — a newer release, *"You are running the latest release"*, or why the check failed. It ignores the skip setting.

Updating is manual: download the new installer or zip from the release page and run it. The installer upgrades in place and keeps your library and settings.

## What's New after an update

The first time a new version starts, PoseStudio opens **What's New in PoseStudio x.y.z** as soon as you have clicked the splash screen away: the release notes of the version you are now running — what was added, changed and fixed — in a scrollable window of its own. It appears once per version (a fresh install shows it too, for the version installed). **Release Notes** in the window opens the full history in this manual; **Close** (or `Enter` / `Esc`) puts it away.

**Help → What's New…** opens the same window whenever you like. To stop it opening by itself, untick **Show this window after an update** in the window, or **Edit → Preferences → General → Show What's New after an update**.

## Where your data is

| Data | Location | Uploaded? |
| --- | --- | --- |
| Settings, asset libraries, collections, favorites | `%APPDATA%\PoseStudio\posestudio.db` | Never |
| Your content and HDR environments | Wherever you keep them; `Documents\My PoseStudio Library` by default | Never |
| Poses | The `.pose` files you save | Never |
| The install ID | In the settings database; shown on the General page | Only in the ping, if enabled |

Preferences → Factory Reset deletes the settings database, install ID included.
