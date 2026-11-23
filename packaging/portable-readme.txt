=====================================================================
 PoseStudio - Portable Edition
=====================================================================

Thanks for trying PoseStudio! This is the portable (no-install)
version. If you'd rather have a normal installer with a Start Menu
entry, grab PoseStudio-<version>-Windows-Setup.exe instead from:

    https://github.com/PoseStudio/PoseStudio/releases/latest


HOW TO RUN
----------
1. Put this PoseStudio folder anywhere you like
   (e.g. C:\Apps\PoseStudio or a USB drive).

2. Double-click PoseStudio.exe

   If Windows shows a blue "Windows protected your PC" screen,
   click "More info", then "Run anyway". The warning appears because
   we're a new open-source project without a paid code-signing
   certificate - the app is built in public by GitHub Actions
   directly from our source code:
   https://github.com/PoseStudio/PoseStudio


ONE-TIME SETUP: STOCK LIGHTING ENVIRONMENTS (RECOMMENDED)
---------------------------------------------------------
The "stock-content\hdri" folder next to this file contains 33 free
(CC0) HDR lighting environments from polyhaven.com. PoseStudio looks
for them in your Documents folder, so copy them there once:

1. Start PoseStudio once and close it (this creates your library).
2. Copy the CONTENTS of:   stock-content\hdri
   into:                   Documents\My PoseStudio Library\hdri
3. Restart PoseStudio. The Environment panel's HDRI picker will now
   list them all, with thumbnails.

(The installer edition does this step for you automatically.)


SYSTEM REQUIREMENTS
-------------------
- 64-bit Windows 10 or 11
- A Vulkan-capable GPU (virtually any graphics card from 2016 onward)
  with reasonably current drivers. If the 3D viewport shows an error
  message instead of a grid, update your graphics drivers.


FEEDBACK
--------
This build exists so you can try PoseStudio and tell us what is
confusing, broken, or missing:

- Usability feedback / bugs:
  https://github.com/PoseStudio/PoseStudio/issues
- Discussions:
  https://github.com/PoseStudio/PoseStudio/discussions
- Discord:
  https://discord.gg/SaKvt9aYCM

Full installation guide and troubleshooting:
  https://github.com/PoseStudio/PoseStudio/blob/main/docs/INSTALL.md

PoseStudio is free, open-source software under the GNU AGPL v3 with
the additional permission and terms in LICENSE-ADDITIONAL-TERMS.txt -
the license is LICENSE.txt next to this file. "PoseStudio" and its
logo are trademarks of Ion Hatzithomas. It is built on the Qt framework
(LGPL v3) and a few other open-source components; their licenses and
notices are in the licenses\ folder (start with licenses\NOTICES.txt).
  https://github.com/PoseStudio/PoseStudio
=====================================================================
