# Changelog

## v1.1 (2026-10-21)

**New**
- Controls page in the settings menu: rebind every game button, both sticks and the menu key to any key, mouse button or controller button. There are separate Controller and Keyboard and mouse setups, and Reset all goes back to the defaults.
- HDR settings now have their own section under Display.
- New "Original game logic" option, on by default so the game behaves as it always did. Turning it off unlocks the extras:
  - Far-away enemies update less often, for a smoother frame rate in busy stages.
  - "Objects per stage" can go past the original game's limit.
- Turning "Original game logic" back on puts View distance and Object distance back to 100% straight away.
- A short hint on first start shows where the settings menu is.
- New SR icon.

**Fixes**
- Crackling and dropouts in the sound are fixed.
- "Skip any cutscene" no longer skips a cutscene by itself (for example the New Game intro) without a button press.
- The bug report key (F12 / Back + Start) is now off by default. Turn it on under System if you want it.
- Menu sliders follow the mouse while you drag them.
- Closing the menu no longer asks you to restart. Settings that need a restart are marked in the menu.
- Busy scenes with lots of objects no longer cut out, and a crash in the graphics code is fixed.
- More engine speed-ups (a few percent more frames per second in some stages).

## v1.0 (2026-10-21)

First release. You need your own copy of Shadow the Hedgehog for GameCube (USA version).

**Setup**
- On first start the game asks for your disc image (`.iso`, `.gcm`, or `.rvz` with Dolphin installed) or a disc folder extracted with Dolphin. It checks it's the right version, unpacks it, and starts. After that it goes straight into the game.

**Graphics**
- Any resolution, higher frame rates, and widescreen / ultrawide screen shapes (the HUD isn't stretched)
- Real-time shadows, with colour and brightness controls
- HDR
- Wider field of view and longer view distance
- Your own texture packs

**Gameplay**
- Skip any cutscene
- Movement tweaks, and your own saved movement presets
- Play as one of the game's other character models
- Optional harder bosses
- Cheats

**Settings**
- Everything is in an in-game menu (Back on a controller, Esc on the keyboard). The game keeps running while it's open.
- Quick setup at the top for resolution, screen shape, HDR and shadows
- Option to mute the game when its window is in the background

**Other**
- Controller and keyboard support
- Faster engine (about 20-40% more frames per second at 4K than earlier test builds)

**Known issues**
- 2-player mode hasn't been tested yet.
- Ambient occlusion is a work in progress and is switched off.
- European and Japanese discs aren't supported yet.
- The stage editor isn't included yet.
