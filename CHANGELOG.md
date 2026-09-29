# Changelog

Build numbers use the format `XNNNN`: `X` is the major version and `NNNN`
is the build sequence. The full rationale for every v0.5 build is kept in
the header comment of `audioclock_5.ino`.

## v0.5: BTT beat tracking

### Build 50010 (no functional change)
- `SAMPLE_RATE` now uses `AUDIO_SAMPLE_RATE_EXACT` (44100 Hz on Teensy 4.x)
  instead of a literal.
- Corrected notes that claimed the Teensy 4 codec runs at 44117.647 Hz. That
  figure only applies to Teensy 3.x.
- Stale comments fixed (MIDI library, span-floor expectations, loop step numbering).
- BTT is vendored with all four patches applied. The repo builds straight
  from a clone.

### Build 50009
- Span floor: a hard resync can no longer move pending ticks into the past,
  which used to make them fire as a burst. Added the `spanFloorHits` counter.

### Build 50008
- MIDI clock ticks now come from a 200 µs IntervalTimer ISR. Worst-case
  lateness went from ~4 ms to ≤ 200 µs, which fixes jitter on slaves that
  measure tick-to-tick.
- MIDI library removed. `Serial1` is written only by the ISR, and transport
  messages go through a lock-free queue.
- Gate servicing moved into the ISR.

### Build 50007
- Pin 2 is now a dedicated HOLD input. The CV A output was removed, and
  pin 10 is the only CV output.

### Build 50006
- The CV A jack became a tempo HOLD toggle, replacing phase reset.
- Status row reads TRACK / FREEWHL / HOLD / PAUSED / WAITING.

### Build 50005
- Dropped-block detection using virtual-clock drift. Shown as `!` on the OLED.

### Build 50004
- The debug line reports average `btt_process()` cost alongside the peak.

### Build 50003
- Non-blocking 1.5 s splash screen.
- `RESP_DECAY[]` raised to the 4th power to match tempo decimation.

### Build 50002
- `setTempoBias()` resets the tempo-weight width at every mean change.
- `btt_process()` is timed with `ARM_DWT_CYCCNT`.

### v0.5 initial
- The entire detection and tempo stack was replaced with BTT.
- Removed: PEAK / WAVELET / SEF detectors, the onset-autocorrelation tempo
  engine, and the frequency-correcting PLL.
- Added: the split period/phase clock, the virtual audio clock, and the
  page-diffed OLED driver.
- Controls repurposed: THRESH → BIAS, and the mode switch → responsiveness.

## v0.4 (Build 40014)
- Three switchable onset detectors: amplitude peak (PKD), a Morlet-inspired
  6-band filter bank (CWT), and spectral energy flux (SEF, after Alonso,
  David & Richard, ISMIR 2004).
- Onset autocorrelation tempo engine with 3-peak octave correction and
  quadratic interpolation.
- Adaptive PLL clock.
- Phase reset on the CV A jack.

## v0.3 (Build 30003), v0.2 (Build 20008), v0.1
- Earlier iterations of the peak and SEF detectors, and the mode-aware
  threshold pot.
