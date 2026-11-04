# VR Mode

**View → VR Mode** puts the scene in a VR headset: the figure stands on your real floor at her true height, and you walk around her and look from any angle while the scene stays live. Pose her from the desk — a drag in the viewport, a dial on the Transform tab, a pose file — and the headset shows the result as it happens. The desktop viewport meanwhile shows what the headset's left eye sees.

The controllers walk you around the scene and let you grab joints with the pointer ray — see [The controllers](#the-controllers).

## What you need

PoseStudio talks to the headset through **OpenXR**, the standard every PC VR system speaks, so any of these works:

- A **Meta Quest** over **Quest Link** (a USB cable) or **Air Link** (Wi-Fi), with the Meta Quest Link app installed on the PC and set as the OpenXR runtime (the app does that itself).
- **Virtual Desktop**, streaming from the PC to the headset.
- **SteamVR**, with any headset SteamVR drives.

The Link app, Virtual Desktop or SteamVR does the streaming to the headset; PoseStudio only renders.

## Turning it on

1. Open **Edit → Preferences → VR** and turn on **Enable VR support**. PoseStudio connects to the headset runtime as it starts, so this takes a restart — the **View → VR Mode** entry offers the restart itself the first time you choose it.
2. Connect the headset *before* starting PoseStudio: start Quest Link or Air Link (or Virtual Desktop, or SteamVR), then start PoseStudio. A headset connected after launch needs another restart. The VR page in Preferences shows what the startup found under **Status**.
3. Choose **View → VR Mode** and put the headset on. You stand about two metres in front of the figure, facing her.

**View → Recenter VR View** puts you back in front of the figure, facing her, from wherever you have walked to. Choose **View → VR Mode** again to end it.

## The controllers

Both controllers draw a pointer ray. The bindings below are for Meta Touch controllers; Valve Index, Windows Mixed Reality and Vive controllers map the same way onto their own sticks and buttons.

| Control | What it does |
| --- | --- |
| **Left stick** | Walks you through the scene in the direction you are looking: forward and back, and sideways. |
| **Right stick, left or right** | Turns you on the spot, smoothly — faster the further you push. |
| **Trigger** | The left mouse button. Point the ray at a joint and pull: the joint is selected and, while you hold the trigger, moves with the ray — the whole body following, exactly as a drag in the viewport does. Let go and the figure settles. A pull on empty space selects the model the ray is on, or clears the selection. |
| **Right stick, up or down, while holding a joint** | Pushes the joint away along the ray or pulls it in — the mouse wheel's depth control. |
| **A** (right) / **X** (left) | Pins or unpins the selected joint, like the `P` key. |
| **Menu** (left controller) | Recenters you in front of the figure. |

The ray turns blue over a joint it can grab and orange while it carries one; it ends with a small marker on whatever it is on. Everything you do with the controllers is on the undo stack like the mouse's edits, and the desktop viewport shows it all meanwhile.

## Quality

Preferences → VR has a **Render scale**: the resolution each eye is rendered at, as a fraction of the headset's own recommended size (1.00). Lower it if the headset image stutters on a lighter GPU; raise it on a strong one for a sharper picture. It applies when VR Mode next starts.

## If VR Mode will not start

The message says why, and the VR page in Preferences says what the startup found:

- *No OpenXR runtime is installed* — install the Meta Quest Link app, Virtual Desktop or SteamVR, and make sure it is the active OpenXR runtime (each of them has a setting for that).
- *No headset was found* — the headset was not connected when PoseStudio started. Connect it (start Link / Air Link, Virtual Desktop or SteamVR), then restart PoseStudio.
- *The headset's GPU cannot render the viewport* — on a machine with two GPUs the headset is driven by one of them; PoseStudio needs that one to drive its own window too.

See also [Troubleshooting](troubleshooting.md).
