# Firmware architecture

Firmware: `firmware/audioclock_5/audioclock_5.ino`, Build 50010.

## Signal path

```
AudioInputI2S ─┬─→ AudioAnalyzeRMS      (signal meter + silence detection)
               └─→ AudioRecordQueue ──→ int16→float ──→ btt_process()
                                                            │
                                        ┌───────────────────┴──────────┐
                                        ↓                              ↓
                              beat callback(sampleTime)      btt_get_beat_period_
                              → beatQ ring buffer            audio_samples()
                                        ↓                              ↓
                                  PHASE corrector              PERIOD (direct)
                                        └──────────┬───────────────────┘
                                                   ↓
                                     clockISR() — 24 ppqn, every 200 µs
                                     → MIDI clock, CV gate, beat LED
```

BTT's STFT hop is 128 samples, which is exactly `AUDIO_BLOCK_SAMPLES`. Audio
blocks go straight into BTT without re-buffering.

## Clock generation: the "split" design

- **Period** comes directly from `btt_get_beat_period_audio_samples()`,
  converted to µs at 44100 Hz. BTT already smooths
  tempo through a decaying Gaussian histogram, so there's no second
  frequency integrator. (v0.4's PLL frequency term was removed.)
- **Phase** is corrected proportionally (`PHASE_ALPHA` = 0.20) from beat
  callbacks. Each correction moves `beatEndUs`, the end of the *current*
  beat, so the 24 ticks inside that beat stretch or squeeze smoothly and
  **never move backwards**.
- The beat span is clamped to 0.5–1.5× the period. This clamp is what keeps
  the tick stream monotonic.
- Three errors in a row larger than 30% of a period trigger a hard resync.
  A single large error is usually syncopation. Three in a row means BTT has
  genuinely changed phase, and the clock should follow.
- Between beats, the clock freewheels on `beatPeriodUs`.

## Tick emission: IntervalTimer ISR

Ticks used to be sent from `loop()`, so they could only go out as often as
the loop ran. A `btt_process()` frame that runs the generalized
autocorrelation takes about 2900 µs, and an OLED page push can add up to
about 1300 µs. So a tick that came due mid-frame could wait up to ~4 ms. At
125 BPM a tick is 20 ms apart, which meant instantaneous tick spacing swung
between roughly 16 and 24 ms.

The *average* was exactly right, which is why the clock never drifted.
Slaves that average over a beat didn't notice anything. Slaves that measure
tick-to-tick heard about 20% wobble. The problem was found by testing against
two different receivers: a DIY drum machine was fine, an Akai MPC wasn't.

`clockISR()` now polls every 200 µs (`CLOCK_ISR_US`), so a tick is never
more than 200 µs late. That's 1% of a tick instead of 20%. `late=` in the
debug line reports the worst case.

### Concurrency rules

Read these before changing any clock state:

- **The ISR is the only writer to `Serial1`.** `HardwareSerial::write()` does
  a read-modify-write on the TX buffer head, so a write from `loop()` at the
  same moment would lose a byte.
- Transport messages from `loop()` (Start, Stop, Continue, SPP) go through
  `midiQ`, a lock-free ring buffer. There's one producer (`loop`) and one
  consumer (the ISR). The producer fills a slot **before** advancing the
  head, and the consumer reads a slot **before** advancing the tail, so no
  interrupt masking is needed.
- Clock state is `volatile`. Single 32-bit reads and writes are atomic on
  Cortex-M, so `volatile` is enough for simple reads.
- **Read-modify-write sequences in `loop()` are not safe on their own.**
  They're wrapped in `noInterrupts()`/`interrupts()`: `handleBeat()`,
  `startClock()`, `continueClock()`, `stopClock()`, and the HOLD LED flash.
  Otherwise the ISR could roll over to the next beat partway through and
  have its write overwritten.
- Those critical sections contain **only arithmetic, no I/O**. Never put a
  `Serial1.write()` inside one. A blocking write with interrupts disabled
  can't be drained by the serial ISR, so it would deadlock.
- Gate timing is handled in the ISR, so gate lengths aren't affected by loop
  jitter.
- Timer priority is 64, higher than the default of 128. The ISR preempts the
  audio update ISR, but it's short enough that this doesn't matter.

## Span floor

This was observed once as `late=92304` during a tempo jump. The ISR can't be
blocked for 92 ms, so what had actually happened was that the tick **due
times moved backwards**.

A hard resync sets `beatEndUs = beatUs`, and the span clamp only kept it
above 0.5 × period. But halving the span halves `tickInterval`, which
retroactively changes the due time of every tick still pending in the
current beat. A worked example at 129 BPM, period 464 ms, `tickInBeat` = 20:

- Tick 20 was due at `beatStart + 387 ms`.
- After the clamp, `tickInterval` halves, so it's now due at `beatStart + 193 ms`.
- That time has already passed, so ticks 20–23 fire back to back.

The fix makes sure the span stays long enough that the next pending tick is
still in the future:

```
tick k is due at beatStart + k * (span / PPQN)
we need that > now for k = tickInBeat
  =>  span > (now - beatStart) * PPQN / tickInBeat
```

This can only **raise** a span that the clamp has already shortened. It
never shortens a beat, and it never moves the phase on its own. The cost is
that a resync finishes on the following beat instead of squeezing the rest
of the current one. A single beat with slightly-off phase is inaudible. Five
ticks piled on top of each other are not.

`spanFloorHits` (`floor=` in the debug line) is cumulative and is never
reset. Validated behaviour:

| Condition | `floor=` |
|---|---|
| Locked, steady tempo | Frozen. 10 s at 133.37 BPM with no increment. |
| Locked, one-bin drift 140.62 → 139.67 | Frozen |
| Genuine tempo change 114.21 → 140.62 | +1 |
| Tempo that never settles (156 / 136 / 159 / 157) | About 1 per beat |

The last row is expected: constant resyncs are exactly when the floor
*should* engage. **If `floor=` climbs while the tempo is genuinely locked and
steady, the reasoning above is wrong.** In that case, go back to Build 50008.
`late=` has stayed between 189 and 199 µs in every run since the floor was
added.

## Virtual audio clock

BTT timestamps beats in the **audio sample domain**, about 26 ms in the
**future** (`num_audio_samples_processed + beat_prediction_adjustment × hop −
analysis_latency_beat_adjustment_fine`). Converting that to `micros()` when
the queue is drained would give wrong answers: if the queue backs up and
eight blocks drain in one burst, all eight would get nearly the same
`micros()` reading.

So instead, `audioTimeUs` advances by exactly one block period for each
block fed to BTT. It's pulled toward `micros()` at 2% per pass, and only when
the queue has just emptied. This also absorbs the ~0.5 µs per block lost by
rounding `US_PER_BLOCK` to whole microseconds.

On Teensy 4.x the codec runs at exactly 44100 Hz, because the audio PLL is
programmed to hit it precisely. You may see a 44117.647 Hz figure in older
Teensy material. That figure is correct for Teensy 3.x, but it doesn't
apply to this board.

## Dropped-block detection

`AudioRecordQueue` silently throws away blocks when it's full, and there's
no counter for it. But right after the queue drains, `audioTimeUs` and
`micros()` should agree, because both advance one block period per block
captured. Every lost block leaves `audioTimeUs` one block period behind
**for good**, which would silently shift every beat timestamp after it.

So if the drift at that point exceeds `DRIFT_LIMIT_US` (3 block periods),
blocks were lost. The firmware then re-anchors hard, increments
`dropEvents`, and shows `!` on the OLED.

## Tempo hold

Pin 2 is `INPUT_PULLUP` for the whole time the program runs. It's sampled at
50 Hz with an armed flag, which is enough debouncing for a mechanical
contact.

Hold freezes **both period and phase**, so a held clock simply freewheels.
If only the period were frozen, BTT would still pull the clock toward
whatever audio is playing, which is exactly what hold is meant to prevent.
Beats are still drained from `beatQ` (it mustn't fill up), but they're
discarded. The silence timeout is suspended while held.

BTT keeps processing underneath the whole time, so releasing hold resumes
tracking immediately with no count-in. If the music changed during the hold,
the three-strike resync rule catches up within a few beats.

The button only works once `beatPeriodUs > 0`. Whether the clock is running
doesn't matter:

- **WAITING** (no tempo found yet): the press is ignored.
- **PAUSED** (tempo remembered after silence): the press starts the clock
  freewheeling at that tempo, using `continueClock()`.
- **Running**: the press freezes the current tempo and phase.

## Silence and Stop

BTT **keeps predicting beats through silence forever**, so the absence of
beats can never be the stop condition. Instead, Stop is triggered when RMS
stays below `SILENCE_RMS` (0.004) for `SILENCE_US` (3 s).

`stopClock()` intentionally does **not** call `btt_clear()`. When signal
returns, the clock resumes with Continue at the existing tempo, without
another count-in.

## Tempo bias and the width trap

The BIAS pot sets the centre of BTT's log-Gaussian tempo weight, which is
what chooses between half-time and double-time.

`btt_set_log_gaussian_tempo_weight_width()` stores a **ratio**,
`(bpm/mean)²`, not an absolute value. `btt_set_log_gaussian_tempo_weight_mean()`
reads the width back in BPM **at the old mean** and applies it again at the
**new** mean, so the stored ratio drifts as the knob turns. At mean 120 it's
`(75/120)² = 0.39`. At mean 69 it's `(75/69)² = 1.18`. That's nearly three
times wider in log terms, which weakens octave suppression exactly where
it's needed most.

`setTempoBias()` therefore sets **both** values, pinning the width back to
the stock 0.625 ratio at every knob position. Never call `set_mean()` on
its own.

## Histogram decay and decimation

Histogram decay is applied once per call to `btt_tempo_tracking()`. With
`BTT_TEMPO_DECIMATION 4`, that call happens at 86 Hz rather than 344 Hz. So
`RESP_DECAY[]` holds the **4th powers** of the per-frame coefficients you
actually want:

```
FST  0.9950⁴ = 0.980149
MED  0.9990⁴ = 0.996006
SLW  0.9997⁴ = 0.998801
```

If you change the decimation factor, change these values too. Otherwise the
FST/MED/SLW labels will be wrong.

## OLED

Audio always has priority. The display is allowed to become sluggish under
load, but it must never freeze.

- `renderFrame()` draws into the GFX buffer every 100 ms. This is **RAM only**.
- `markDirtyPages()` compares each 128-byte page against a shadow copy with `memcmp`.
- `serviceOled()` pushes **at most one changed page per loop pass**, and
  only when `audioQueue.available() == 0`.
- A 400 ms failsafe pushes one page regardless, so the display slows down
  rather than freezing.
- The SSD1306 is put in page addressing mode at init. `display.display()`
  is never called.
- I2C runs at 1 MHz. `applyInputSource()` drops to 400 kHz around codec
  writes, because the SGTL5000 is only specified up to 400 kHz.
  `display.begin()` forces the bus to 400 kHz internally, so
  `Wire.setClock(I2C_FAST)` has to be called again right after it.
- The SIG bar moves in 4-pixel steps, so small RMS fluctuations don't mark
  its page as changed on every render.
- The splash screen is held for 1.5 s by `splashUntilUs`, which is a
  non-blocking early return inside `serviceOled()`. **Never** return early
  from `loop()` for this, because that would stall the audio queue.
