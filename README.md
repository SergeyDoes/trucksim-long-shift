# Long Shift

A plugin for **American Truck Simulator** and **Euro Truck Simulator 2** that makes a long shift behind the
wheel feel long. As your driver's mandatory rest approaches:

- **Yawns.** Every now and then the steering pulls hard to one side for a few seconds. Steer against it and
  the driver "wakes up": the pull fades at once.
- **Head sway.** The cab view starts to sway like a drowsy head, along a lazy figure eight that is smeared
  at random, so the pattern never quite repeats.

Both effects fade in gradually as the rest gets closer and are fully on once the rest is due. Take your
rest and they are gone.

Long Shift uses only the official SCS SDK (telemetry and input API). It does not patch the game or touch
its memory, and there is nothing to bind in the controls.

## Requirements

- American Truck Simulator or Euro Truck Simulator 2, 64-bit, a recent version. Developed and tested on
  ATS 1.61.
- **Fatigue simulation** turned on in the game's gameplay options. Long Shift reads the time left until the
  mandatory rest; without fatigue simulation the game may not report it, and the mod stays idle.

## Installation

1. Download `long-shift-<version>.zip` from the
   [Releases](https://github.com/SergeyDoes/trucksim-long-shift/releases) page.
2. Open the game folder: in Steam, right-click the game → *Manage* → *Browse local files*.
3. Go to `bin\win_x64` and create a folder named `plugins` there if there is none.
4. Copy `long_shift.dll` into `bin\win_x64\plugins`.
5. Start the game. It shows a notice that SDK plugins are loaded; that is expected, confirm it.

On the first start Long Shift creates `long_shift.ini` (the settings) and `long_shift.log` next to the DLL.

To uninstall, delete `long_shift.dll`, `long_shift.ini` and `long_shift.log` from `bin\win_x64\plugins`.

## Settings

Everything lives in `long_shift.ini` in the plugins folder. Save the file while the game is running and the
changes apply within about a second. Delete it to get the defaults back. Mistakes in the file are reported
in `long_shift.log`, and a broken line keeps its default.

Each effect is scaled by two factors between 0 and 1:

- **Speed:** 0 when standing, growing linearly to 1 at `full_speed_mps` and above
  (m/s; 25 m/s = 90 km/h = 56 mph). The head sway keeps `standing_strength` of itself while standing still.
- **Fatigue:** 0 until `start_minutes_before_rest` in-game minutes before the mandatory rest, growing
  linearly to 1 at `full_minutes_before_rest` (0 = when the rest is due; an overdue rest stays at 1). Equal
  values switch the effect on at once: both 0 means only once the rest is due.

### `[general]`

| Setting | Default | Meaning |
|---|---|---|
| `enabled` | `1` | 1 = on, 0 = off. |
| `status_log` | `0` | 1 = write a status line to `long_shift.log` every second, for troubleshooting. |

### `[yawn]`

| Setting | Default | Meaning |
|---|---|---|
| `strength` | `0.25` | How hard a yawn pulls, as a fraction of full steering lock; each yawn takes 60-100 % of it. 0 = no yawns. |
| `average_interval_s` | `30` | Average seconds from the start of one yawn to the start of the next. |
| `min_pause_s` | `10` | Minimum pause after a yawn, seconds. |
| `full_speed_mps` | `25` | Speed at which yawns reach full strength, m/s. |
| `start_minutes_before_rest` | `30` | Yawns start this many in-game minutes before the rest... |
| `full_minutes_before_rest` | `0` | ...and are at full strength from here on. |

A yawn eases in over 2-4 seconds to a random side, holds for 2-5 seconds and eases out over 1-2 seconds.
Once your steering against it outweighs the pull, the driver wakes up: the yawn fades within about half a
second, and the next one comes no sooner than `min_pause_s` later. Letting the truck drift does not count.

### `[camera]`

| Setting | Default | Meaning |
|---|---|---|
| `left_right` | `10` | Reach of the sway left-right, in the game's mouse-look units (10-20 is a clear head movement). |
| `up_down` | `6` | Reach up-down. Both reaches 0 = no head sway. |
| `loop_time_s` | `12` | Average seconds for one figure eight. |
| `randomness` | `0.5` | 0 = a clean figure eight; up to 1 the speed, reach and shape wander more and more. |
| `standing_strength` | `0.6` | Share of the sway while standing still (0 = none, 1 = as at full speed). |
| `full_speed_mps` | `15` | Speed at which the sway reaches full strength, m/s (15 m/s = 54 km/h = 34 mph). |
| `start_minutes_before_rest` | `120` | The sway starts this many in-game minutes before the rest... |
| `full_minutes_before_rest` | `0` | ...and is at full strength from here on. |

## Compatibility and known limitations

- Tested on ATS 1.61 with a gamepad. Steering wheels (including force feedback), keyboard steering and
  ETS2 use the same SDK and are expected to work, but have not been tested yet. Reports are welcome.
- Steering and camera go through the game's *semantical* inputs, which current control profiles include.
  Very old profiles (from before about game version 1.38) may lack them in the steering setup; resetting the
  controls to defaults fixes that.
- The head sway moves the view the way mouse look does: around wherever you are looking, and back there
  when it fades out. Other plugins that move the camera may interfere with it.
- The game's own fatigue effects stay as they are. Long Shift cannot sync with them, because the SDK does
  not report them.
- Meant for single player and Convoy. Check the rules before using it on TruckersMP.

## Troubleshooting

- Look into `long_shift.log` next to the DLL. On start it shows the version and the settings in use, and it
  lists any problems in `long_shift.ini`.
- With `status_log = 1` there is a line every second: speed, in-game minutes to the rest (`rest.stop`), the
  yawn and camera factors, and how much time the game spends in the plugin. `rest.stop none` means the game
  does not report the time to the rest: check that fatigue simulation is on.
- Nothing happens? The effects only start within `start_minutes_before_rest` of the mandatory rest (by
  default 30 minutes for yawns and 120 for the head sway), and yawns need speed.

## Building from source

Requirements: Windows, Visual Studio 2022 with C++ (MSVC, x64), CMake 3.22 or newer.

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

The plugin is built to `build/src/plugin/Release/long_shift.dll`. `scripts/package.ps1` packs a release zip;
GitHub Actions builds, tests and publishes a release for every `v*` tag.

- `src/core` - game-independent logic, unit-tested: yawns (`FatigueModel`), head sway (`CameraSway`),
  settings (`Config`).
- `src/plugin` - the DLL: SDK entry points, the log and the settings file.
- `tests` - doctest unit tests and a smoke test that loads the DLL against a fake of the game's SDK.
- `third_party/scs_sdk` - the official SCS SDK 1.14 headers.
- `docs` - development notes (in Russian) and the first experiment with the input API.

## License

MIT, see [LICENSE](LICENSE). The SCS SDK headers in `third_party/scs_sdk` are distributed under their own
license, see `third_party/scs_sdk/sdk_license.txt`.

Long Shift is a fan-made mod and is not affiliated with SCS Software.
