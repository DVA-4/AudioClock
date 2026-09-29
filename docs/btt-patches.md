# BTT on Teensy 4.0: patches and measurements

[Beat-and-Tempo-Tracking](https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking)
by Michael Krzyzaniak (MIT licence) is vendored in `firmware/audioclock_5/`,
based on upstream commit `c039090`. Four patches are applied: two for
performance and two for correctness. Every changed spot carries a
`TEENSY PORT:` comment, and the full diffs are in [`../patches/`](../patches).

| # | File | Kind | Summary |
|---|---|---|---|
| 1 | `src/DFT.c` | performance | Rect-to-polar computes magnitude only |
| 2 | `src/DFT.c` | performance | `sqrtf` fast path in the generalized autocorrelation |
| 3 | `src/BTT.c` | performance | Tempo tracking decimated by 4 |
| 4 | `src/BTT.c` | correctness | Sub-bin tempo interpolation |

All four are required. The firmware's `RESP_DECAY[]` coefficients assume
that patch 3 is applied.

Files are otherwise unchanged from upstream. The upstream `.old` / `.old.txt`
files are left out, because Arduino compiles `src/` recursively and they
cause duplicate-symbol errors.

---

## Patch 1: magnitude-only rect-to-polar

`rdft_rect_to_polar()` computed `atan2()` for every bin. It has two callers,
`btt_onset_tracking()` and `rdft_real_generalized_autocorrelation()`, and
**neither one reads the phase**. Onset tracking only uses the magnitudes in
the lower half of the buffer, and the autocorrelation sets the phase to zero
two lines later. That made the `atan2()` pure wasted work: 1024
double-precision calls per oss frame, at 344 frames per second. It's replaced
with a single-precision magnitude calculation, and the phase slot is set to
zero.

## Patch 2: `sqrtf` fast path

`rdft_real_generalized_autocorrelation()` raises each magnitude to
`exponent`, which defaults to 0.5. So every double-precision `pow()` call was
really just a square root. The patch adds an `exponent == 0.5` path that uses
`sqrtf`, and uses `powf` for any other exponent.

## Patch 3: decimated tempo tracking

`btt_tempo_tracking()` runs a 2048-point generalized autocorrelation, and it
was being called on every oss frame. Tempo doesn't change at 344 Hz. The
patch calls it only every `BTT_TEMPO_DECIMATION` frames (default 4, and it
must be a power of two), at the call site in
`btt_spectral_flux_stft_callback()`. Setting it to 1 restores stock
behaviour.

**The histogram decay is applied once per call, so decimating stretches its
time constant by the same factor.** The firmware compensates by raising
`RESP_DECAY[]` to the 4th power. If you change the decimation factor, change
those values too.

## Patch 4: sub-bin tempo interpolation

Candidate tempo lags are integer indices into `autocorrelation_real`, so the
reported tempo was rounded to one oss bin. That's 0.75 BPM at 125 BPM, and
worse at higher tempos. On a steady source, the reading would walk between
neighbouring bins (125.28 / 124.53 / 123.78).

The patch fits a parabola to the autocorrelation peak at the winning
candidate lag. The result is EMA-smoothed and stored in a new
`beat_period_oss_interp` float, which is **used only by the getters**. The
integer lag still drives all internal indexing (cbss, beat phase,
histogram). The refined value is only trusted when it's within 1.5 bins of
the smoothed histogram peak. Otherwise it snaps back to the integer value.

**Don't interpolate the tempo histogram instead.** The histogram places a
symmetric Gaussian centred on the integer candidate lag, so its peak always
lands exactly on a bin, and the parabola's vertex always comes back with
zero offset. This approach was tried first and made no measurable
difference. The sub-bin information only exists earlier in the chain, in
the autocorrelation.

---

## Measurements

### Host: accuracy and cost

Test setup: 60 s of synthetic click track fed through `btt_new_default()`,
gcc -O2 on a 2.1 GHz Xeon.

| Build | CPU time | Realtime factor | Tracking result |
|---|---|---|---|
| stock | 3.735 s | 0.0623 | 120.19 BPM, 119 onsets, 114 beats |
| + patches 1, 2 | 2.340 s | 0.0390 | identical |
| + patch 3 | 1.172 s | 0.0195 | identical |
| + patch 4 | 1.139 s | 0.0190 | identical counts, more accurate BPM |

| Source BPM | 118 | 125 | 131 | 137 | 152 | 168 | Mean abs error |
|---|---|---|---|---|---|---|---|
| quantised | 118.12 | 125.28 | 130.83 | 136.90 | 152.00 | 168.06 | 0.122 |
| interpolated | 118.04 | 125.06 | 130.98 | 137.00 | 151.97 | 168.03 | 0.030 |

These numbers were reproduced against the vendored sources in this repo,
with identical onset and beat counts to stock at every tempo tested.

### Teensy 4.0 @ 600 MHz, all four patches

```
onset tracking only        232 µs / block   (first ~3 s, before tempo tracking starts)
steady-state average      1230 µs / block   = 42% of the 2902 µs budget
peak (autocorrelation)    2730–2894 µs      = 94–99% of budget
AudioMemoryUsageMax          4 of 60 blocks
```

**Decimation doesn't change the peak.** The autocorrelation frame costs the
same as before; it just runs a quarter as often. Judge headroom by the
average and by `qmax`, never by the peak. Three cheap frames follow every
expensive one, so the queue always drains.

### Headroom

Tempo and beat tracking account for about 91% of the total cost. If you
ever need more headroom, that's where it has to come from: raise
`BTT_TEMPO_DECIMATION` to 8 (and update `RESP_DECAY[]` to 8th powers), or
lower `btt_set_num_tempo_candidates()` from its default of 10. Overclocking
to 720 MHz would bring the peak down to about 2290 µs, but it isn't
recommended in a sealed enclosure.

### A warning about extrapolating from desktop benchmarks

An early estimate assumed the Cortex-M7 would be about 8× slower than the
Xeon on this workload. It turned out to be closer to **22×**. The first run
on hardware used 94% of the budget, where about 30% had been predicted.
Measure on the target early, using `ARM_DWT_CYCCNT`.

---

## Upstream API notes

- The upstream README's `btt_new()` signature is out of date. The real one
  takes 9 arguments, including two analysis-latency adjustments that have to
  be measured empirically (see `demos/analysis_latency/` upstream). That's
  why the firmware uses `btt_new_default()`.
- `btt_new()`'s declaration in `BTT.h` and its definition in `BTT.c` list
  `cbss_length` and `onset_threshold_len` in opposite orders. This is
  harmless with the defaults (both are 1024), but it matters if you pass
  different values.
- `btt_new_default()` assumes 44100 Hz, which matches Teensy 4.x exactly.
  (Teensy 3.x runs at 44117.647 Hz. Porting to one would mean using
  `btt_new()` with the true rate and re-measuring the latency constants.)

## Bring-up checks

1. `qmax` in the debug line should stay well below 60. If it reaches 60,
   blocks are being dropped. The firmware also detects this independently
   (`drops=`, and `!` on the OLED).
2. The `btt=` average should be around 900–1400 µs. If it's near 2500 µs,
   patch 3 isn't active.
3. `late=` should stay at or below 200 µs.
