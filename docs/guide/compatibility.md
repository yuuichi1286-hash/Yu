# Compatibility and FAQ

## Supported Devices

LeviLauncher for Android targets Android 9.0 or later on ARM64 devices. Devices with more
available RAM and storage will have a better experience, especially when
managing multiple versions or large worlds.

## Minecraft Requirement

LeviLauncher for Android requires the official Minecraft Bedrock Edition app from Google
Play. It is designed for legitimate players and does not provide a Minecraft
license.

If Minecraft is not installed, installed from an unsupported source, or too old
for the current launcher behavior, the launcher may stop before starting the
game.

## Minecraft Versions

Minecraft 1.21.80 and later are tested. Older versions can still be launched:
the launcher shows a warning first, because they may crash and some built-in
mods may not work with them. Imported versions should use version isolation
when their data should remain separate from the installed version.

## Modified APKs

Modified Minecraft APKs, for example builds with a changed renderer, can be
imported. An APK is accepted when it has the official package name or when it
contains `libminecraftpe.so`, so repacked APKs with a different package name
work too. Extra native libraries that the modified game depends on are loaded
from the APK automatically. If the launcher cannot read the version from the
APK, the suggested name is `Minecraft_custom`; rename it to include the game
version (for example `Minecraft_1.21.100_vulkan`) so the launcher picks the
right loading path for that version.

The official Minecraft app from Google Play is still required.

## Storage and Migration

The launcher may need to migrate older data directories before continuing. Do
not delete the old directory until the migration succeeds and you have confirmed
that worlds, packs, and settings are available.

Backups are stored under the launcher-managed backup location. For important
worlds, keep another copy outside app storage.

## Native Mods

Native mods run native code in the game process. Install only mods from
sources you trust, and test them with an isolated version first.

## Common Questions

### Why does the launcher say Minecraft is missing?

Install or update the official Minecraft app from Google Play, then open
LeviLauncher for Android again.

### Why can an imported version fail to launch?

Imported versions often need version isolation. Enable it in the instance
settings and try again.

### Where should developer API questions go?

Use the [Developer](/guide/developer) section. The Preloader API reference is
intentionally grouped there instead of the main user guide.
