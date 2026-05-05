# PROJECT_MEMORY

Last updated: 2026-05-05

## Repo and build target
- Active desktop repo: `D:\DesktopProjects\bearpi-pico_h2821e-master\bearpi-pico_h2821e-master`
- Main feature area: `application/samples/products/sle_measure_dis`
- Common output target used in this work: `output/bs21e/acore/standard-bs21e-1100e`

## What this project does
- SLE ranging server/client sample.
- Current work focused on the server side distance alarm flow.
- Server measures distance from IQ data, compares it with a threshold, and drives local LED and buzzer.
- Threshold can be updated at runtime through UART2.

## Important source files
- `application/samples/products/sle_measure_dis/sle_measure_dis_server/sle_measure_dis_server.c`
- `application/samples/products/sle_measure_dis/sle_measure_dis_server/sle_measure_dis_server_alg.c`
- `application/samples/products/sle_measure_dis/sle_measure_dis_server/sle_measure_dis_server_alg.h`
- `application/samples/products/sle_measure_dis/sle_measure_dis_server/sle_measure_dis_server_adv.c`
- `application/samples/products/sle_measure_dis/Kconfig`

## Backup created
- Backup zip: `D:\DesktopProjects\bearpi-pico_h2821e-master\bearpi-pico_h2821e-master\backup_sleuart_current_20260428_164808.zip`

## Current runtime behavior confirmed from logs
- Distance measurement is working.
- Threshold updates through UART are working.
- Alarm decision is working.
- Example confirmed behavior:
  - threshold updated to `2.00m` -> distances around `2.4m~3.3m` gave `far:1 buzzer:1`
  - threshold updated back to `4.00m` -> distances around `2.3m~2.5m` gave `far:0 buzzer:0`

## Current alarm logic
- Local buzzer is not controlled by a fixed beep-duration timer.
- Buzzer state follows the latest valid distance result and threshold comparison.
- Response speed is limited by the next valid ranging result, not by a separate buzzer delay.
- There is hysteresis in the alarm decision.

## Current parameter state in source
From `sle_measure_dis_server.c`:
- default threshold macro: `250 cm`
- hysteresis: `15 cm`
- max jump filter: `300 cm`

This means if threshold center is `2.50m`:
- enter alarm when distance is `>= 2.65m`
- leave alarm when distance is `<= 2.35m`

## Important config mismatch
At the time of this note:
- source file default threshold was changed to `250 cm`
- `application/samples/products/sle_measure_dis/Kconfig` default threshold was changed to `250`
- but `build/config/target_config/bs21e/menuconfig/acore/standard_bs21e_1100e.config` still showed `CONFIG_MEASURE_DIS_DEFAULT_THRESHOLD_CM=100`

Implication:
- A rebuild may still use `100 cm` if the generated/menuconfig config overrides the source default.
- Do not assume the source default alone changes the final firmware behavior.
- If default threshold still behaves as `1.00m` after rebuild, check and sync the menuconfig-generated config.

## Stability fixes that should remain
Applied in the current desktop repo:
1. Remote IQ processing uses a snapshot of local IQ data before algorithm calculation.
2. Added busy protection with `g_measure_dis_alg_busy`.
3. While algorithm is busy, new remote IQ is dropped instead of queueing stale work.
4. Timeout fallback in indicator task only triggers when algorithm is not busy.

Reason:
- This reduced stale timestamp pairing, false timeout fallback, and queue backlog effects.

## Changes previously tried and then reverted
- Extra application-layer smoothing was added once and later reverted because the user reported worse distance behavior.
- Do not reintroduce averaging/IIR smoothing without explicit request.

## Known runtime issues still seen
- Occasional algorithm failure:
  - `slem_alg_calc_smoothed_dis failed. ret:0x8000a453`
- This was observed as a frame-level failure, not a guaranteed crash.
- Some logs showed reboot banners, but one captured reboot cause looked like power-on/reset style rather than a confirmed hardfault.

## Build notes
- User IDE build path works.
- One observed failure was post-build file locking, not source compile failure:
  - `patch_riscv.py` failed moving `application.bin` to `unpatch.bin`
  - Windows error `WinError 32` means the file was in use.
- This is likely a tooling/file-lock issue, not a C code logic issue.

## Flashing / deliverables
Output directory contained:
- `application.bin`
- `application.hex`
- `application_std.hex`
- `application_sign.bin`
- `fota.fwpkg`

Default direct flashing choice:
- Use `output/bs21e/acore/standard-bs21e-1100e/application_sign.bin`

Other artifact meanings:
- `application_std.hex`: only when the flashing tool requires hex
- `fota.fwpkg`: OTA/FOTA package, not the normal direct-flash default
- `application.bin`: raw unsigned app image, usually not the first choice here

## Tooling observations
- A direct shell launch of the cross compiler once returned `-1073741515`, suggesting missing runtime dependency in that shell environment.
- User-side IDE/build system still produced firmware artifacts successfully.

## Guidance for future Codex sessions
- Read this file before making assumptions.
- Treat the desktop repo as the source of truth for actual builds.
- Do not silently overwrite user changes.
- If behavior does not match source defaults, inspect generated build config under `build/config/target_config/bs21e/menuconfig/...`.
- Before changing filtering or alarm behavior, separate these concepts clearly:
  - algorithm smoothing inside vendor ranging stack
  - application jump filter (`MEASURE_DIS_MAX_JUMP_CM`)
  - hysteresis around threshold
  - measurement update period versus buzzer reaction time
