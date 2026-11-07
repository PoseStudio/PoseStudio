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

1. Open **Edit → Preferences → VR** and turn on **Enable VR support**. PoseStudio connects to the headset runtime as it starts, so this takes a restart — the **View → VR Mode** entry offers the restart itself the first time you choose it, and your scene comes back with it: nothing you have posed is lost.
2. Connect the headset *before* starting PoseStudio: start Quest Link or Air Link (or Virtual Desktop, or SteamVR), then start PoseStudio. A headset connected after launch needs another restart; **View → VR Mode** offers that too, scene included. The VR page in Preferences shows what the startup found under **Status**.
3. Choose **View → VR Mode** — or click the **VR** button on the viewport's control strip, right of the view picker, or pick **VR Mode** at the bottom of that picker — and put the headset on. You stand about two metres in front of the figure, facing her. The VR button lights up blue while the headset has the scene.

**View → Recenter VR View** puts you back in front of the figure, facing her, from wherever you have walked to. To leave VR Mode press **Esc** at the desktop, click the lit VR button, or choose **View → VR Mode** again.

## The controllers

Both controllers draw a pointer ray. The bindings below are for Meta Touch controllers; Valve Index, Windows Mixed Reality and Vive controllers map the same way onto their own sticks and buttons.

| Control | What it does |
| --- | --- |
| **Left stick** | Walks you through the scene in the direction you are looking: forward and back, and sideways. |
| **Right stick, left or right** | Turns you on the spot, smoothly — faster the further you push. |
| **Trigger** | The left mouse button. Point the ray at a joint and pull: the joint is selected and, while you hold the trigger, moves with the ray — the whole body following, exactly as a drag in the viewport does. Let go and the figure settles. A pull on empty space selects the model the ray is on, or clears the selection. |
| **Trigger, with the hand at a joint** | A hand grab: reach out so the controller is at the joint (within about 15 cm) and pull — the joint comes along in your hand, wherever you move and turn it. |
| **Twist the controller while holding a joint** | Twists the joint about its own length, by the same angle — the Transform tab's Twist dial, turned by the wrist. |
| **Right stick, up or down, while holding a joint** | Pushes the joint away along the ray or pulls it in — the mouse wheel's depth control. |
| **A** (right) | Pins or unpins the selected joint, like the `P` key. |
| **B** (right) / **Y** (left) | Undo / Redo. |
| **X** (left) | The Ground button: drops the figure onto the floor. |
| **Menu** (left controller) | Recenters you in front of the figure. |

The ray turns blue over a joint it can grab and orange while it carries one; it ends with a small marker on whatever it is on, and a short stub at its root shows where the controller is. Everything you do with the controllers is on the undo stack like the mouse's edits, and the desktop viewport shows it all meanwhile — its VR button lit while VR Mode is on.

On a Valve Index the A and B buttons of each hand take the same roles (right A pins, right B undoes, left A grounds, left B redoes) and the left stick's click recenters; Windows Mixed Reality and Vive controllers use their trackpad clicks, grips and menu buttons.

## Quality

Preferences → VR has a **Render scale**: the resolution each eye is rendered at, as a fraction of the headset's own recommended size (1.00). Lower it if the headset image stutters on a lighter GPU; raise it on a strong one for a sharper picture. **Reduced effects in the headset** skips the skin's subsurface blur and the bloom for each eye, a bigger saving on a lighter GPU at the cost of some skin realism; the desktop view keeps the full look. With a Gaussian splat scene loaded, **Splat scale** (1.00 by default) is the most of each eye's size the splats are drawn at: drawing them is the dearest part of a headset frame, and their cost grows as you walk up to a capture, so PoseStudio lowers the size on the fly whenever a frame runs late and raises it back toward this setting as the GPU finds room — the capture softens a little rather than the picture stuttering. At 1.00 the capture is as sharp as the GPU can afford at the moment; at 0.50 the splats are drawn at half the eye's size in each direction, a quarter of the work. Lower it if the headset image still stutters with a capture loaded. With a capture loaded, **Reduced effects in the headset** makes the biggest difference of the three: on a laptop GPU the skin blur and the bloom cost as much as the whole rest of the frame, and with them off the capture keeps its full size. All three apply when VR Mode next starts.

PoseStudio also hands the headset each frame's depth, so when a frame is late the headset's own reprojection moves the picture against the real geometry rather than smearing it.

## If VR Mode will not start

The message says why, and the VR page in Preferences says what the startup found:

- *No OpenXR runtime is installed* — install the Meta Quest Link app, Virtual Desktop or SteamVR, and make sure it is the active OpenXR runtime (each of them has a setting for that).
- *No headset was found* — the headset was not connected when PoseStudio started. Connect it (start Link / Air Link, Virtual Desktop or SteamVR), then restart PoseStudio.
- *The headset's GPU cannot render the viewport* — on a machine with two GPUs the headset is driven by one of them; PoseStudio needs that one to drive its own window too.

See also [Troubleshooting](troubleshooting.md).
