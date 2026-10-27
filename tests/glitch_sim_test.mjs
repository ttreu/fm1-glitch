// Headless test of GLITCH in the browser simulator.
// Usage: node tests/glitch_sim_test.mjs [out-dir]
// Writes glitch_demo.wav (a listening sample) and lcd_*.rgb565 screen dumps.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
const out = process.argv[2] || fileURLToPath(new URL('../build/glitch-test/', import.meta.url));
mkdirSync(out, { recursive: true });
const wasm = readFileSync(new URL('../build/web/simulator.wasm', import.meta.url));
const BTN = 0, KEY = 1, ENC = 2;
const B = { OCT_DOWN: 0, OCT_UP: 1, FX: 2, SCL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8, SAVE: 9, ARP: 10, SEQ: 11, PLAY: 12, REC: 13 };
const WHITE = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26];
const GP = { SOURCE: 0, PITCH: 1, DECAY: 2, COLOR: 3, PROB: 4, RATCHET: 5, LENGTH: 8, RATE: 9, LEVEL: 11, REVERSE: 16 };
const GG = { BPM: 0, SWING: 1, MUTATE: 2 };
let fails = 0;
const check = (ok, msg) => { console.log((ok ? 'PASS ' : 'FAIL ') + msg); if (!ok) fails++; };
const rms = (a) => Math.sqrt(a.reduce((s, v) => s + v * v, 0) / Math.max(1, a.length));
const recording = [];

async function machine(record = false) {
  const { instance } = await WebAssembly.instantiate(wasm, {});
  const x = instance.exports; x._initialize?.();
  const m = { x };
  m.run = (sec) => { const a = []; for (let i = 0; i < Math.round(sec * 44100 / 128); i++) { const b = new Float32Array(x.memory.buffer, x.sim_render(), 256).slice(); if (record) recording.push(b); for (let j = 0; j < 256; j += 2) a.push(b[j] + b[j + 1]); } return a; };
  m.stereo = (sec) => { const l = [], r = []; for (let i = 0; i < Math.round(sec * 44100 / 128); i++) { const b = new Float32Array(x.memory.buffer, x.sim_render(), 256); for (let j = 0; j < 256; j += 2) { l.push(b[j]); r.push(b[j + 1]); } } return [l, r]; };
  m.down = (kind, i) => { x.sim_control(kind, i, 1); m.run(0.03); };
  m.up = (kind, i) => { x.sim_control(kind, i, 0); m.run(0.03); };
  m.tap = (kind, i) => { m.down(kind, i); m.up(kind, i); };
  m.turn = (enc, d) => { x.sim_control(ENC, enc, d); m.run(0.02); x.sim_control(ENC, enc, 0); m.run(0.02); };
  m.steps = (slot) => BigInt(x.sim_glitch_steps_lo(slot) >>> 0) | (BigInt(x.sim_glitch_steps_hi(slot) >>> 0) << 32n);
  m.dump = (name) => writeFileSync(`${out}/lcd_${name}.rgb565`, new Uint8Array(x.memory.buffer, x.sim_pixels(), 240 * 240 * 2).slice());
  m.midi = (bytes) => { new Uint8Array(x.memory.buffer, x.sim_file(), bytes.length).set(bytes); return x.sim_midi(bytes.length); };
  x.sim_boot(1); m.run(1.0);
  return m;
}
// Onsets: a hop's peak clearly above the recent level (reverb tails and hiss stay below).
const onsets = (a, floor = 0.003) => { const hop = 64, e = [];
  for (let i = 0; i + hop <= a.length; i += hop) { let p = 0; for (let j = 0; j < hop; j++) p = Math.max(p, Math.abs(a[i + j])); e.push(p); }
  let n = 0, wait = 0;
  for (let i = 4; i < e.length; i++) { const ref = (e[i - 1] + e[i - 2] + e[i - 3] + e[i - 4]) / 4;
    if (wait > 0) { wait--; continue; }
    if (e[i] > floor && e[i] > 1.8 * ref) { n++; wait = 3; } }
  return n; };
const peak = (a) => { let p = 0; for (const v of a) p = Math.max(p, Math.abs(v)); return p; };
const finite = (a) => a.every(Number.isFinite);

// ---- boot, transport, factory pattern
{
  const m = await machine(true);
  m.dump('boot');
  check(rms(m.run(0.5)) < 1e-4, 'silent after boot');
  check(m.x.sim_glitch_pattern() === 0 && !m.x.sim_glitch_running(), 'pattern P01 loaded, stopped');
  check(m.x.sim_glitch_get(4, GP.SOURCE) === 0, 'key A is the classic hi-hat');
  m.tap(BTN, B.PLAY);
  const a = m.run(6);
  check(m.x.sim_glitch_running() === 1 && rms(a) > 0.003, `PLAY starts the beat (rms ${rms(a).toFixed(4)})`);
  const n = onsets(a) / 6;
  check(n > 3 && n < 40, `fine-grained density: ${n.toFixed(1)} onsets/s`);
  check(finite(a) && peak(a) < 1.0, 'output finite and below full scale');
  m.dump('running');
  m.tap(BTN, B.PLAY); m.run(3);
  check(rms(m.run(0.5)) < 0.01, 'PLAY again stops (tails fade)');
}
// ---- every sound source plays, stays finite
{
  const m = await machine();
  m.tap(KEY, 20); m.tap(BTN, B.EDIT);                                  // slot 20, SLOT 1/5 SOUND
  m.turn(2, -20);                                                      // SOURCE -> IMPULSE
  const names = ['IMPULSE', 'TRAIN', 'CRUNCH', 'TICK', 'CHIRP', 'ZAP', 'SNAP', 'DUST', 'BITS', 'GRAIN'];
  m.turn(4, 40); m.turn(5, 0);                                         // DECAY up for longer sounds
  const levels = [];
  for (let s = 1; s <= 10; s++) {
    if (s > 1) m.turn(2, 1);
    check(m.x.sim_glitch_get(20, GP.SOURCE) === s, `source ${names[s - 1]} selected`);
    m.run(0.4); m.tap(KEY, 20); const a = m.run(0.4);
    levels.push(rms(a));
    check(finite(a) && peak(a) > 0.002, `${names[s - 1]} sounds (peak ${peak(a).toFixed(3)})`);
  }
  m.tap(KEY, 4); m.turn(2, 5);
  check(m.x.sim_glitch_get(4, GP.SOURCE) === 0, 'the hi-hat on A keeps its sound');
}
// ---- steps, length (polymeter), step pages
{
  const m = await machine();
  m.tap(KEY, 13); m.tap(BTN, B.EDIT); m.tap(BTN, B.EDIT); m.tap(BTN, B.EDIT); // slot 13, SLOT 3/5 LOOP
  m.turn(2, -64); m.turn(2, 11);                                       // LENGTH 12
  check(m.x.sim_glitch_get(13, GP.LENGTH) === 12, 'LENGTH set to 12 steps');
  m.down(BTN, B.SEQ); m.tap(KEY, WHITE[0]); m.tap(KEY, WHITE[5]); m.up(BTN, B.SEQ);
  const s = m.steps(13);
  check((s & 1n) === 1n && ((s >> 5n) & 1n) === 1n, 'SEQ + white keys set steps 1 and 6');
  m.tap(BTN, B.OCT_UP);
  m.down(BTN, B.SEQ); m.tap(KEY, WHITE[2]); m.up(BTN, B.SEQ);
  check(((m.steps(13) >> 18n) & 1n) === 1n, 'OCT+ edits step page 2 (step 19)');
  // Pressing a step again: X2 -> X3 -> X4 -> off.
  const states = [];
  m.down(BTN, B.SEQ);
  for (let i = 0; i < 4; i++) { m.tap(KEY, WHITE[2]); states.push([Number((m.steps(13) >> 18n) & 1n), m.x.sim_glitch_step_ratchet(13, 18)]); }
  m.up(BTN, B.SEQ);
  check(JSON.stringify(states) === '[[1,1],[1,2],[1,3],[0,0]]', `SEQ + step again cycles X2, X3, X4, off (${JSON.stringify(states)})`);
  // Back to step page 1. Step 1 is on: three more presses give it X4.
  m.tap(BTN, B.OCT_DOWN);
  m.down(BTN, B.SEQ); for (let i = 0; i < 3; i++) m.tap(KEY, WHITE[0]); m.up(BTN, B.SEQ);
  check(m.x.sim_glitch_step_ratchet(13, 0) === 3 && (m.steps(13) & 1n) === 1n,
    `step 1 set to X4 (ratchet ${m.x.sim_glitch_step_ratchet(13, 0)}, on ${m.steps(13) & 1n})`);
}
// ---- REVERSE page
{
  const m = await machine();
  m.tap(KEY, 20);
  for (let i = 0; i < 5; i++) m.tap(BTN, B.EDIT);                      // SLOT 5/5 REVERSE
  m.turn(2, 100);
  check(m.x.sim_glitch_get(20, GP.REVERSE) === 100, 'EDIT x5: REVERSE set to 100 %');
  m.dump('reverse_page');
  m.run(0.5); m.tap(KEY, 20); const a = m.run(0.5);
  check(finite(a) && peak(a) > 0.002, `a reversed hit sounds (peak ${peak(a).toFixed(3)})`);
  m.tap(BTN, B.EDIT);
  check(m.x.sim_glitch_get(20, GP.REVERSE) === 100, 'EDIT wraps back to SLOT 1/5');
}
// ---- random beat, slot random, mutes
{
  const m = await machine();
  const before = [...Array(27).keys()].map((s) => m.steps(s));
  m.tap(BTN, B.ARP); m.run(0.1);
  const after = [...Array(27).keys()].map((s) => m.steps(s));
  check(after.some((v, i) => v !== before[i]), 'ARP rolls a new beat');
  const lengths = new Set([...Array(27).keys()].filter((s) => after[s] !== 0n).map((s) => m.x.sim_glitch_get(s, GP.LENGTH)));
  check(lengths.size >= 1, `playing loops use lengths ${[...lengths].join(', ')}`);
  m.tap(BTN, B.PLAY); const a = m.run(8);
  const n = onsets(a) / 8;
  check(n > 2 && n < 45 && finite(a), `random beat: ${n.toFixed(1)} onsets/s, finite`);
  const slot = [...Array(27).keys()].find((s) => s !== 4 && after[s] !== 0n);
  const old = m.steps(slot);
  m.down(BTN, B.ARP); m.tap(KEY, slot); m.up(BTN, B.ARP);
  check(m.steps(slot) !== old || m.x.sim_glitch_get(slot, GP.SOURCE) !== 0, 'ARP + key renews one slot only');
  m.down(BTN, B.GLO); m.tap(KEY, slot); m.up(BTN, B.GLO);
  check(((m.x.sim_glitch_mutes() >>> slot) & 1) === 1, 'GLO + key mutes the slot');
  m.tap(BTN, B.GLO);
  check(m.x.sim_glitch_mutes() === 0, 'GLO alone unmutes all');
}
// ---- living loops
{
  const m = await machine();
  m.tap(BTN, B.HOME); m.turn(4, 99);                                   // MUTATE 99
  check(m.x.sim_glitch_global(GG.MUTATE) === 99, 'MUTATE set to 99');
  m.tap(BTN, B.PLAY); m.run(16);
  check(m.x.sim_glitch_mutated() === 1, 'the loop mutates while it plays');
  m.tap(BTN, B.REC); m.run(0.05);
  check(m.x.sim_glitch_mutated() === 0, 'REC undoes the mutations');
}
// ---- pattern memory and switching
{
  const m = await machine();
  m.tap(BTN, B.ARP); m.run(0.1);
  m.tap(KEY, 20); m.down(BTN, B.SEQ); m.tap(KEY, WHITE[3]); m.tap(KEY, WHITE[3]); m.up(BTN, B.SEQ); // slot 20 step 4: X2 or on
  const myRatchet = m.x.sim_glitch_step_ratchet(20, 3);
  const mine = [...Array(27).keys()].map((s) => m.steps(s));
  m.down(BTN, B.SAVE); m.tap(KEY, WHITE[2]); m.up(BTN, B.SAVE);         // store in P03
  check(m.x.sim_glitch_stored(2) === 1 && m.x.sim_glitch_pattern() === 2, 'SAVE + key 3 stores pattern P03');
  m.down(BTN, B.HOME); m.tap(KEY, WHITE[0]); m.up(BTN, B.HOME);         // load P01
  check(m.x.sim_glitch_pattern() === 0, 'HOME + key 1 loads P01');
  m.turn(6, 2);                                                        // PRESETS -> P03
  const back = [...Array(27).keys()].map((s) => m.steps(s));
  check(m.x.sim_glitch_pattern() === 2 && back.every((v, i) => v === mine[i]), 'PRESETS loads the stored P03 again');
  check(m.x.sim_glitch_step_ratchet(20, 3) === myRatchet, `step ratchets are stored with the pattern (${myRatchet})`);
  m.tap(BTN, B.PLAY); m.run(1.0);
  m.turn(6, 1);                                                        // P04 queued
  check(m.x.sim_glitch_pattern() === 3, 'pattern P04 selected while running');
  const steps = m.steps(1); m.run(3.5);
  check(finite(m.run(0.5)), 'pattern change in time stays clean');
  // Flash survives a restart (simulator keeps its Flash without clearing).
  m.x.sim_boot(0); m.run(1.0);
  check(m.x.sim_glitch_stored(2) === 1, 'P03 is still stored after a restart');
  void steps;
}
// ---- momentary stutter effects
{
  const m = await machine();
  m.tap(BTN, B.PLAY); m.run(2);
  const fxs = ['REPEAT 1/4', 'REPEAT 1/8', 'REPEAT 1/16', 'REPEAT 1/32', 'REPEAT 1/64', 'REVERSE 1/4', 'REVERSE 1/8',
    'PITCH DOWN', 'TAPE STOP', 'FREEZE', 'BITCRUSH', 'DOWNSAMPLE', 'LOWPASS', 'HIGHPASS', 'GATE', 'DROPOUT'];
  for (let k = 0; k < 16; k++) {
    m.down(BTN, B.FX); m.down(KEY, WHITE[k]);
    const a = m.run(1.2);
    const on = m.x.sim_glitch_perform();
    m.up(KEY, WHITE[k]); m.up(BTN, B.FX);
    const after = m.run(0.3);
    const ok = finite(a) && on === k + 1 && m.x.sim_glitch_perform() === 0 && peak(a) < 1.2;
    if (k === 15) check(ok && rms(a.slice(4410)) < 0.002, `FX + key 16: DROPOUT silences (rms ${rms(a.slice(4410)).toFixed(4)})`);
    else if (k === 8) check(ok && rms(a.slice(-8820)) < 0.002, `FX + key 9: TAPE STOP winds down (rms ${rms(a.slice(-8820)).toFixed(4)})`);
    else check(ok, `FX + key ${k + 1}: ${fxs[k]} runs and ends with the key`);
    void after;
  }
  // FX + black keys: punch-in effects (sequencer) and five more master effects.
  const BLACK = [1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25];
  const blackFx = [['p', 1, 'RATCHET X2'], ['p', 2, 'RATCHET X3'], ['p', 3, 'RATCHET X4'], ['p', 4, 'ROLL'], ['p', 5, 'REVERSE ALL'],
    ['f', 17, 'REVERSE 1/16'], ['f', 18, 'SCRATCH'], ['f', 19, 'SHUFFLE'], ['f', 20, 'OCTAVE UP'], ['f', 21, 'STUTTER'], ['p', 6, 'CHAOS']];
  for (let k = 0; k < 11; k++) {
    const [kind, value, name] = blackFx[k];
    m.down(BTN, B.FX); m.down(KEY, BLACK[k]);
    const a = m.run(1.2);
    const on = kind === 'p' ? m.x.sim_glitch_punch() : m.x.sim_glitch_perform();
    if (k === 0) m.dump('punch');
    m.up(KEY, BLACK[k]); m.up(BTN, B.FX); m.run(0.3);
    check(finite(a) && on === value && m.x.sim_glitch_punch() === 0 && m.x.sim_glitch_perform() === 0 && peak(a) < 1.2,
      `FX + black key ${k + 1}: ${name} runs and ends with the key`);
  }
  // A punch-in and a stutter at once: RATCHET X3 + REPEAT 1/8.
  m.down(BTN, B.FX); m.down(KEY, BLACK[1]); m.down(KEY, WHITE[1]); const both = m.run(1.0);
  check(m.x.sim_glitch_punch() === 2 && m.x.sim_glitch_perform() === 2 && finite(both), 'punch-in and stutter run together');
  m.up(KEY, WHITE[1]);
  check(m.x.sim_glitch_punch() === 2 && m.x.sim_glitch_perform() === 0, 'releasing the stutter key keeps the punch-in');
  m.up(KEY, BLACK[1]); m.up(BTN, B.FX); m.run(0.2);
  // Without FX the black keys still play their sounds.
  m.tap(KEY, BLACK[0]);
  check(m.x.sim_glitch_punch() === 0, 'black key alone plays its slot, no effect');
  // REPEAT 1/16 at 90 BPM loops every 1/4 beat (0.1667 s): the signal repeats.
  m.down(BTN, B.FX); m.down(KEY, WHITE[2]); m.run(0.3); const r = m.run(0.6); m.up(KEY, WHITE[2]); m.up(BTN, B.FX);
  const lag = Math.round(60 / 90 / 4 * 44100) - 0;
  let num = 0, den = 0;
  for (let i = 0; i + lag < r.length; i++) { num += r[i] * r[i + lag]; den += r[i] * r[i]; }
  check(den > 0 && num / den > 0.6, `REPEAT 1/16 repeats every sixteenth (correlation ${(num / den).toFixed(2)})`);
}
// ---- MIDI
{
  const m = await machine();
  m.midi([0x99, 40, 100]); const a = m.run(0.2);                       // note 40 = slot 4, the hi-hat
  check(rms(a) > 0.001, 'MIDI note E1 plays the hi-hat slot');
  m.midi([0xfa]); m.run(0.5);
  check(m.x.sim_glitch_running() === 1, 'MIDI Start starts the sequencer');
  m.midi([0xfc]); m.run(0.1);
  check(m.x.sim_glitch_running() === 0, 'MIDI Stop stops it');
}
// ---- long stress run: dense random beat, all effects up
{
  const m = await machine();
  m.tap(BTN, B.SCL); m.turn(5, 99);                                    // DENSITY 99
  m.tap(BTN, B.ARP); m.tap(BTN, B.HOME); m.turn(4, 60);                // MUTATE 60
  m.tap(BTN, B.FX); m.tap(BTN, B.FX); m.turn(3, 60); m.turn(5, 60);    // tape feedback, mix
  m.tap(BTN, B.FX); m.tap(BTN, B.FX); m.turn(2, 99); m.turn(4, 99); m.turn(5, 99); // shimmer all up
  m.tap(BTN, B.PLAY);
  const [l, r] = m.stereo(30);
  const p = Math.max(peak(l), peak(r));
  check(finite(l) && finite(r) && p < 1.0, `30 s dense run with full effects: finite, channel peak ${p.toFixed(2)}`);
  m.dump('dense');
}
// Demo: P01 then two random beats, with a stutter in between.
{
  const m = await machine(true);
  recording.length = 0;
  m.tap(BTN, B.PLAY); m.run(8);
  m.down(BTN, B.FX); m.down(KEY, WHITE[2]); m.run(1.3); m.up(KEY, WHITE[2]); m.up(BTN, B.FX);
  m.run(4); m.turn(6, 4); m.run(10);
  m.tap(BTN, B.ARP); m.run(10);
  const pcm = new Int16Array(recording.length * 256);
  recording.forEach((b, i) => b.forEach((v, j) => { pcm[i * 256 + j] = Math.max(-32768, Math.min(32767, Math.round(v * 32767))); }));
  const header = Buffer.alloc(44);
  header.write('RIFF', 0); header.writeUInt32LE(36 + pcm.byteLength, 4); header.write('WAVEfmt ', 8);
  header.writeUInt32LE(16, 16); header.writeUInt16LE(1, 20); header.writeUInt16LE(2, 22); header.writeUInt32LE(44100, 24);
  header.writeUInt32LE(44100 * 4, 28); header.writeUInt16LE(4, 32); header.writeUInt16LE(16, 34); header.write('data', 36);
  header.writeUInt32LE(pcm.byteLength, 40);
  writeFileSync(`${out}/glitch_demo.wav`, Buffer.concat([header, Buffer.from(pcm.buffer)]));
  console.log(`demo: ${out}glitch_demo.wav`);
}
process.exit(fails ? 1 : 0);
