<!--
Fritchii's "What's new" notes for this build: paste the release notes (the GitHub release body,
e.g. E:\OotRemakeEmulation\release\fritchii-vX\notes.md) here before the release build. The
download instructions at the top are dropped in the app. While this file holds only this comment,
no "What's new" window pops up after an update (Help > What's New says there are no notes yet).
-->
## ⬇️ Download this file: **`Fritchii-v5.1-Windows-x64.zip`**

On Fritchii v4, v4.1 or v5 you don't need to download anything: it offers this update when you open it. Otherwise unzip it anywhere and run `Fritchii-v5.1\lichii.exe`. Don't download "Source code (zip)" or "Source code (tar.gz)": they contain no emulator.

**Don't delete your shader cache** unless it's broken (see below). **NVIDIA players:** set Shader Cache Size to **Unlimited** once.

---

Fritchii v5.1, a fix-up for v5.

- **Dialogue text no longer turns into scraps of other textures.** Each new text box size could show garbage the first time it appeared.
- **Delete Shader Cache** in the Help menu (and when you right-click the game on the home screen), with a confirmation. Only for a cache that went bad: the shaders are prepared again on the next start.
- **4K and scaled displays:** Fritchii now uses exactly the scale set in Windows. 150% on a 4K screen became 200% (windows too big, dialogs cut off) and 125% became 100% (tiny text).

### Everything new in v5

Windows x64. Tested with The Legend of Zelda: Ocarina of Time (0400486024DEC000) on a Ryzen 7 7800X3D + RTX 4080: stable 60 fps in a lot of places, rarely under 45 even in the heaviest ones (Water Temple included), no shader stutter after the preparation.

#### New
- **Shaders are prepared once, before you play.** The first time you start OoT, a "Preparing shaders" screen builds the game's shaders at full speed (about 15 minutes on an 8-core CPU, once). After that, places you visit for the first time play like you've been there before: walking into a new area used to freeze for ~0.7 s per new shader, now it's 40-70 ms, and loads are shorter. A new driver or GPU prepares them again. Built on zortos's precompile work (zortos293/Lichii#895, #896).
- **New look.** Graphite theme, a home screen, an in-app "What's new", and Help > Export Log for bug reports.
- **In-game controls.** Move the mouse to the top of the game for Pause, Restart, Stop, Fullscreen and the menus.
- **New performance graph** (View > Performance Overlay): big fps, frame time, the lowest recent fps and a line against 60 (or 30). Loading screens leave a gap instead of a dip.

#### Faster
- **Moving NPCs and crowds no longer drag the frame rate down.** About +3 fps in Castle Town from each of the fixes behind it (render target relocation, image pool, an exact frame clock, cleanup of stale copies), and ~2.4 GB less video memory (8.8 to 6.4 GB in Castle Town).
- **~2.3 GB less RAM**, so 16 GB PCs have more room.
- **Windows 11: a precise game clock** built in. Players who got big gains from the GlobalTimerResolutionRequests registry tweak get it without the tweak.
- Shader translation moved off the GPU thread.
- Retrained profile-guided build on the new code.

#### Fixed
- **New players: "Press Start", chat bubbles and lighting** are right on an empty shader cache (they were missing or wrong while shaders were still building).
- **AMD: the red dotted line in the Royal Family's Tomb** (zortos293/Lichii#820).
- **Objects that could vanish for a whole session** in some games (clip distances, zortos293/Lichii#902).
- **Fastmem kept on PCs with little free memory** instead of silently running 20-25% slower (zortos293/Lichii#645).
- Less RAM while shaders are prepared (#878), stale data after some GPU writes (#700), a rare channel deadlock (#609), mods folders that don't exist (#686), Windows long paths (#576), and a few smaller ones (#918, #727, #610).

#### Shader cache
- The cache moves to version 54 (zortos's numbering), so caches from zortos's builds and Fritchii's are shared. The first start copies your old cache once (about +1.2 GB for OoT). The old `v52` file stays so you can roll back to v4; you can delete it later if you don't plan to.

#### Known
- AMD and Intel changes could not be tested on that hardware here: please report problems with your GPU model and the log (Help > Export Log).

Dynamic FPS by Dark (built-in). Thanks to zortos293 and every Lichii contributor whose work is in here, and to the **Ocarina of Forks** Discord for the moral support, the novel ideas and the debugging help.
