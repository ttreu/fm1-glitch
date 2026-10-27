# GLITCH – FM-1 Custom Firmware

Fork of M-VAVE FM1 Open V14. The FM-1 becomes a microsound drum machine: fine
glitch sounds, random and living loops. All sounds are synthesized; there are
no samples. The FM1 sequencer, synth engines and project pages are gone.

## Sounds

27 slots, one per key. **A** is a classic hi-hat; every other slot can play:

| Source | Sound | COLOR knob |
|---|---|---|
| IMPULSE | a single 0.1-1.5 ms click | single click → doublet |
| TRAIN | 3-20 clicks in a row: a short rasp | speeds up ← → slows down |
| CRUNCH | sample-and-hold digital noise (PITCH = hold rate) | smooth steps → harsh, jittery |
| TICK | a short burst of noise | dark → bright |
| CHIRP | a fast sine sweep | down ← → up |
| ZAP | a falling laser sweep | low → high start |
| SNAP | a dry, gated band of noise with a click | dark → bright |
| DUST | a crackle of random clicks | sparse → dense |
| BITS | 8-bit LFSR noise | tonal → noisy |
| GRAIN | a resonant noise grain | wide → resonant |
| THUMP | a micro kick: one to a few cycles of a low, falling sine (PITCH 18-200 Hz) | pure → click and drive |
| POP | a speaker pop: a jump that slides back | soft → sharp edge |
| LOOP | a snippet looped too short (like the kuuluu loop): a round low wave plus bright noise, repeated exactly, jumping to a slightly different snippet every 3-6 passes (PITCH = loop length, 7.2 ms at 0; DECAY 20-300 ms) | round body → bright sizzle |
| HIGH | a very high, short sine beep (PITCH: 7.7 kHz at 0, 4-18 kHz; DECAY 5-200 ms) | pure → a beating second tone |
| DROP | silence: a dropout of the whole mix, effects included | darkens the dip |

PITCH, DECAY and RANDOM (per-hit variation) work on every source. There are
no tonal sounds: nothing plays notes in a key.

**REVERSE** (slot page 5) plays a slot's hits backwards: the sound swells up
instead of fading and stops with a cut; sweeps run the other way, click
trains get louder and faster. The value is the chance per hit (100 % = every
hit); DECAY sets the length of the swell.

## Playing

| Control | Function |
|---|---|
| Keys | play and select a slot |
| SEQ held + white keys | the selected slot's steps, 16 per page; press a step again for a ratchet: on → X2 → X3 → X4 → off |
| OCT- / OCT+ | step page 1-4 (64 steps) |
| PLAY | start / stop |
| SEL held + PLAY | tap tempo |
| ARP | a completely new random beat (at the next bar while running) |
| ARP held + key | a new sound and line for that one slot |
| REC | undo the mutations: back to the stored pattern |
| GLO held + key | mute / unmute a slot; GLO alone: unmute all |
| FX held + white key | momentary effect (see below) |
| FX held + black key | punch-in effect (see below) |
| HOME | tempo, swing, mutate, level |
| HOME held + white key | load pattern 1-16 |
| SAVE | store what is playing in the current pattern |
| SAVE held + white key | store it in pattern 1-16 |
| EDIT (repeat) | slot pages: 1 SOUND (source, pitch, decay, color), 2 RHYTHM (probability, ratchet, micro timing, scatter), 3 LOOP (length, rate, random, level), 4 SPACE (pan mode, pan, tape send, shimmer send), 5 REVERSE |
| ENV / LFO | slot sound page / filter page |
| SEL | KIT: accent, flam, hiss, density of random beats |
| FX (repeat) | 1 filter, 2 tape echo, 3 delay + drive, 4 shimmer reverb, 5 sync + tape tone |
| SELECT knob | slot |
| PRESETS knob | pattern (changes at the next bar while running) |

**Panning** (slot page 4, PAN MODE): FIXED, RANDOM (every hit somewhere
else), PINGPONG, LFO, and JUMP: the hits stay in the middle, but now and then
one jumps all the way to one side – 100 % there, 0 % on the other side, dry
and past echo and reverb. PAN sets how often (up to every second hit).

**Loudness:** every source is matched in loudness, so the fine, short sounds
(clicks, ticks, bits, dust, pops) are not lost next to snaps and chirps.

**Ratchets:** a step's own ratchet (X2-X4, set with SEQ + key, shown as a
split step) plays 2-4 hits in the length of the step. It wins over the
slot's RATCHET (RHYTHM page), which applies to all other steps. Random beats
and MUTATE also put ratchets on single steps now and then.

**Groove:** ACCENT (KIT page) makes the offbeats softer than the beats
(quarters full, eighths and sixteenths quieter). FLAM lets some hits come
twice: the second one softer and between the steps (1/2, 1/3, 1/4 ... of a
step later). Which hits get a flam, and the MICRO timing of each step, repeat
with the loop, so the irregularities stay in the beat; MUTATE moves them.

**Random beats** (ARP) are built like the Keinseier references: a frame of
micro kick (THUMP), a snap or tick on the backbeat (sometimes with a roll
into it) and the hi-hat, then two to five quieter glitch layers – clicks,
pops, short loops, very high beeps, a line in another length, sparse
crunches, dust. About half of these layers sit hard left or right (fixed,
random per hit or ping-pong); kick and snare stay in the middle. The beats
are dry: little reverb and echo, silence between the hits.

**Polymeter:** every slot has its own LENGTH (1-64 steps) and RATE (1/16,
1/32, 1/64), so loops of different lengths drift against each other.

**Living loops:** MUTATE (HOME page) lets the loop change a little every
bar – steps flip, lines turn by a step, ratchets and pitch move – while the
density stays near the stored pattern. Low values drift back towards it.
REC returns to the stored pattern at once; SAVE keeps the mutated version.

**Momentary effects** (hold FX, then a white key; release to end):

| Key | Effect | Key | Effect |
|---|---|---|---|
| 1 | repeat 1/4 | 9 | tape stop |
| 2 | repeat 1/8 | 10 | freeze |
| 3 | repeat 1/16 | 11 | bitcrush |
| 4 | repeat 1/32 | 12 | downsample |
| 5 | repeat 1/64 | 13 | lowpass sweep |
| 6 | reverse 1/4 | 14 | highpass sweep |
| 7 | reverse 1/8 | 15 | gate 1/32 |
| 8 | pitch down | 16 | dropout |

Repeats loop the last slice of the mix (up to 0.37 s; longer values halve).

**Punch-in effects** (hold FX, then a black key; release to end). The black
keys still play their sounds when FX is not held. Ratchets, roll, reverse all
and chaos change the hits the sequencer plays, so they can run together with
a stutter from a white key.

| Black key (left to right) | Effect |
|---|---|
| F# | ratchet X2: every hit twice |
| G# | ratchet X3 |
| A# | ratchet X4 (steps with a bigger ratchet keep theirs) |
| C# | roll: every hit as 5 accelerating, rising hits |
| D# | reverse all: every hit backwards |
| F# | reverse 1/16: the last sixteenth backwards, looped |
| G# | scratch: the last eighth back and forth |
| A# | shuffle: random sixteenths of the last beat |
| C# | octave up: the last sixteenth at double speed |
| D# | stutter: repeats that jump between 1/8 and 1/64 |
| F# | chaos: random pitch on every hit (±1 octave), half of them backwards |

**Patterns:** 16 patterns (stored format 2: step ratchets and REVERSE are
kept; patterns saved by an earlier GLITCH build fall back to the factory
pattern). Pattern 1 is a calm microsound groove; 2-16 are
seeded random beats with different densities, swing and effect colours. SAVE
stores a pattern in Flash: the live loop, mutes, all settings and the scatter
seed. Tempo is kept when a pattern is loaded while running or under MIDI clock.

**MIDI in** (USB, BLE, DIN): notes from C1 (any channel) play the slots,
Start / Continue / Stop drive the sequencer, MIDI clock sets the tempo.

## Building on macOS

Toolchains next to this folder (`../toolchains`, a link to the shared `Developer/FM1 toolchains`):
JieLi 2.5.3 (`jl/C$/JL/pi32`), Wine (`wine/Wine Stable.app`), WASI SDK 27.

```
python3 tools/build_web_mac.py      # browser simulator  -> build/preview/index.html
python3 tools/build_mac.py          # firmware + update page
tests/run_native.sh                 # DSP tests
node tests/glitch_sim_test.mjs      # simulator: all functions, writes a demo WAV
```

Outputs:
- `build/dx7/generated/FM1-GLITCH.fwsc` – firmware package
- `build/official-web/FM1-GLITCH.html` – workbench page with this firmware
  and the original V15 recovery image (Chrome/Edge)

## Flashing

1. Back up projects, voices and **samples** you want to keep, **with the
   firmware that is on the device now** (stock or GLIDER). GLITCH stores its
   16 patterns in the last 64 KB of the sample area; the first SAVE
   overwrites whatever is there. GLITCH itself cannot read or write FM1
   projects, DX7 voices or samples (see below).
2. Open the workbench page in Chrome or Edge, *Device update*, connect the
   FM-1 by USB, start the update. Keep power and USB connected.
3. Back to stock: same page, *Recovery & force update*, original V15.

**From the command line** (macOS, the FM-1 by USB, FM1 Open firmware already
on it – GLIDER, GLITCH, Official Open): `tools/flash_fm1.py` speaks the same
USB-MIDI update protocol as the Workbench page and checks the package the
same way. Needs `python3 -m pip install python-rtmidi`; close the Workbench
page and other MIDI software first.

```
python3 tools/flash_fm1.py info     # read only: which firmware and build is on the device
python3 tools/flash_fm1.py check    # check build/dx7/generated/FM1-GLITCH.fwsc
python3 tools/flash_fm1.py flash    # flash it (asks: type FLASH)
```

Stock V15, third-party firmware, recovery and going back to stock stay with
the Workbench page. `tests/flash_fm1_test.py` runs the tool against a
simulated FM-1.

## What GLITCH leaves out

The FM1 parts GLITCH never uses are compiled out (`#if FM1_GLITCH` in
`app/fm_factory.c`, `fm_library.c`, `presets.c`, `samples.c` and the
project slots in `project.c`): the 128-voice DX7 factory bank, the DX7
voice library, the 16 FM1 demo songs, FM1 project slots, project
import/export and the sample bank. This frees about 17 KB of program Flash
and 16 KB of RAM. Device settings (Bluetooth, MIDI, clock), firmware updates
and the Workbench connection are unchanged. Workbench functions for
projects, voices and samples answer with an error under GLITCH.

## Status

Built and checked in the simulator and with native DSP tests. **Not yet
tested on hardware.** Check the CPU figure on the HOME page: with dense
beats, ratchets, all effects and a stutter running it must stay well below
100 %.
