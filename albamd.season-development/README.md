# Season Development

AI teams develop their cars through a Driver Career or My Team season, so the order of the teams changes as the year goes on.

## What it does

- **Upgrade packages.** Between rounds each AI team may bring a package: a front wing, bodywork, a new floor, a major aero package or, more rarely, a power-unit update. A team's resources set how often it brings one and how big it is.
- **Not every package works.** A package can work, work less than the team hoped, or fail. A failed aero package makes the car slower until the team goes back to the old specification a round or two later.
- **Upgrade windows.** Big packages cluster in the early-season and summer windows. Late in the season, teams out of the fight may stop developing to focus on next year.
- **Season to season.** 40% of a team's development carries into the next season. Teams that stopped early start the new year stronger, and the slowest teams get more development resources, like the real wind-tunnel allocation.
- **Paddock report.** Back at Team HQ before each round, a popup lists who brought what and whether it worked.
- **Your car is never touched.** By default your team-mate isn't either, so your team develops only through the game's own R&D.

Development is applied to every AI car in every career session through the Swofty core's car layers: `ai.grip_scale` for chassis and aero (it affects only AI-driven cars) and `engine.power_scale` for power. The layers stack with other addons' layers. Development is capped at ±1.2% pace for chassis and ±0.6% for power.

Each career keeps its own development state, stored with the career through the core's career saves.

## Settings

| Setting | Default | What it does |
| --- | --- | --- |
| AI season development | On | Turns the effect on cars on or off. |
| Strength | 100% | Scales how much a development step changes the car. |
| Upgrade frequency | Normal | How often packages arrive. |
| Packages that fail | Normal | How often a package fails. |
| Develop your team-mate's car | Off | Off: your team develops only through the game's own R&D. |
| Paddock report | Summary | Off, Summary, or Detailed with the numbers and each team's season total. |

## Works with

- Driver Career and My Team, 2025 and 2026 cars, Steam build 26566308.
- Offline only.
- Other AI addons, such as Driver Form or AI Driver Behaviour: their layers on the same fields stack.

## Build

`native/build.ps1` (Windows) or `native/build.sh` builds `native/season_development.dll` with `zig c++`, as in Swofty's addon template. The first build downloads Swofty's addon SDK headers into `native/include`, pinned to one commit of [SwoftyLauncher](https://github.com/Swofty-Developments/SwoftyLauncher).

## Credits

- Built on Swofty's MIT-licensed addon SDK (`templates/addon/native/include` in the SwoftyLauncher repository).
