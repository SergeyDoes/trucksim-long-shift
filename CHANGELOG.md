# Changelog

## 1.0.0

First public release.

- **Yawns:** once the driver gets tired, the steering pulls hard to one side for a few seconds, about every
  30 seconds. Steer against it to "wake up" and the pull fades at once.
- **Head sway:** the cab view sways like a drowsy head along a figure eight that is smeared at random, a little
  even while standing still.
- Both effects fade in as the mandatory rest approaches and grow with speed; each has its own settings.
- Everything is tunable in `long_shift.ini`, and changes apply while the game runs.
- Uses only the official SCS SDK (telemetry and input API): no memory hacks, nothing to bind in the controls.
