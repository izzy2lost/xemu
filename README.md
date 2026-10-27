# X1 BOX

<a href="https://play.google.com/store/apps/details?id=com.izzy2lost.x1box&pcampaignid=web_share">
  <img src="https://play.google.com/intl/en_us/badges/static/images/badges/en_badge_web_generic.png" alt="Get it on Google Play" height="80"/>
</a>

X1 BOX is an original Xbox emulator for Android. It also runs Sega Chihiro arcade games.

> [!NOTE]
> **4 GB of RAM or more is recommended.** Performance depends on your device and the game.

## Two Emulation Cores

Choose the core in Settings. You can also set a different core for each game.

- **xemu** emulates the full console. It runs the most games. Based on [xemu](https://xemu.app).
- **hakuX** is a faster core that runs fewer games. Based on [hakuX](https://github.com/rfandango/hakuX).

## Features

### Playing
- Game library with list, cover-grid and carousel views, plus optional online box-art lookup
- Settings for each game
- Save states with thumbnails, in several slots
- Fast forward
- Internal resolution scale up to 6x
- Widescreen and aspect ratio options
- Vulkan and OpenGL ES renderers
- Custom GPU drivers such as Turnip, on supported Adreno devices
- Cheats and mods (hakuX core)

### Controls
- On-screen controller with an editable layout. It hides when a physical controller connects.
- USB and Bluetooth controllers, with button remapping
- **Local multiplayer** with more than one controller

### Multiplayer and Online
- **System Link over LAN:** play System Link games with other X1 BOX devices on the same Wi-Fi. One device hosts and the others join, with host discovery built in. It also works with xemu on PC.
- **XLink Kai:** play System Link games with people over the internet through a PC running the XLink Kai Engine.
- **Insignia:** play on the Xbox Live replacement service. A setup guide in the app walks you through creating and recovering your account.

### Console Tools
- Setup wizard for your Xbox files
- Save manager to browse, export, import and delete game saves
- Xbox dashboard support, including importing a dashboard and booting it from the library
- HDD tools and an EEPROM editor for language, video modes, widescreen and MAC address
- ISO to XISO converter

### Sega Chihiro (Arcade)
- Chihiro mode with light-gun and driving controls
- Widescreen hacks for supported games

## Device Requirements

- Android 10 or newer
- A 64-bit ARM device (`arm64-v8a`)
- A GPU with Vulkan 1.1 or newer. The xemu core can also use OpenGL ES.
- 4 GB of RAM or more recommended

## Your Files

You must provide your own legally obtained games, dumped from your own discs.

The **xemu core** also needs these original Xbox files:

- MCPX Boot ROM
- Flash ROM (BIOS)
- Xbox hard disk image

The **hakuX core does not need them**. It has its own built-in system software, so you only need your games.

X1 BOX does not include any BIOS files or games.

## Project Origin

- Forked from [xemu](https://github.com/xemu-project/xemu)
- The hakuX core comes from [rfandango/hakuX](https://github.com/rfandango/hakuX)

## License

- The xemu-based emulator follows the xemu and QEMU licensing. It is mostly **GNU GPL v2**, with some GPLv2-compatible parts (for example BSD or MIT code in TCG). See `LICENSE`, `COPYING` and `COPYING.LIB`.
- The hakuX core in `android/hakux/core` is **GNU GPL v3 or later**. See `android/hakux/core/LICENSE`.

## Notes

- X1 BOX is not affiliated with Microsoft or Sega.
- Compatibility and performance vary by device and game.
