# F1 25 Mods

Career-immersion addons for F1 25, made for [Swofty Launcher](https://f1.swofty.net).

| Addon | What it does | Status |
| --- | --- | --- |
| [Season Development](albamd.season-development/) | AI teams develop their cars through a career season: upgrade packages that work, half work or fail, upgrade windows, teams that stop to focus on next year, and a paddock report at Team HQ. | 0.1.0, in testing |

## Install

Download the `.swoftyaddon` from [Releases](https://github.com/AlbaMD07/mods.swofty/releases) and drop it on the launcher, or install it from the Addon Browser once it's listed there.

## Build

Each addon's `native/build.ps1` (Windows) or `native/build.sh` builds its engine plugin with `zig c++`, following Swofty's addon template.

## Licence

MIT. The build scripts download Swofty's MIT-licensed addon SDK headers into each addon's `native/include`; they aren't stored in this repo.
