# Contributing to GLITCH

GLITCH is a hobby project. Help is welcome, answers may take a while.

## Most useful: hardware test reports

GLITCH has been tested in the browser simulator and with native DSP tests,
not yet on many real FM-1 units. Please use the **Hardware test report**
issue template and tell:

- the firmware build (shown by `python3 tools/flash_fm1.py info`, or the
  release you installed),
- what you played (pattern number, DENSITY, effects, stutter keys),
- the **CPU figure on the HOME page** at its highest,
- anything that crackled, dropped out, hung, or did not do what GLITCH.md says.

## Bug reports and ideas

Open an issue. For sounds, a short recording or a description of a reference
sound helps a lot.

## Code

Pull requests are welcome. Please keep to the rules of the audio code (see the
top of `app/glitch_dsp.c`): it runs from SRAM in the audio interrupt, so no
libc calls, no struct copies, no division or modulo by variables and no 64-bit
shifts by variables; the firmware build checks the SRAM calls. Run the tests
before sending:

```
tests/run_native.sh
node tests/glitch_sim_test.mjs
python3 tests/flash_fm1_test.py
```

Contributions are accepted under the project license (GPL-3.0-only with the
FM1 linking exception, see LICENSE.md). The maintainer decides what goes into
official releases. Forks and own variants are welcome under their own name.
