# Lessons learned

Things that weren't obvious, in roughly the order they were learned.

## Hardware

1. **Pin 17 (A3) can't be used for analog input.** Serial4 TX holds it. Use A2 instead.
2. **The 3-state switch needs an analog-capable pin.** Pin 22 (A8) works. Pin 4 doesn't.
3. **A 100 kΩ + 100 kΩ divider** gives the switch's open position a stable mid-rail reading.
4. **MIDI OUT doesn't need an optocoupler.** Only MIDI IN requires isolation.
5. **DIN pin 4 goes to +5 V, and pin 5 goes to TX.** Feed both from TX and no current flows, so MIDI never works.
6. **Use a linear regulator, not a switcher,** so no switching noise gets into the audio.

## Signal processing and tempo

7. **Hand-rolled detectors each only handle some kinds of material.** v0.4 needed
   three switchable detectors: peak, wavelet filter bank, and spectral energy
   flux. BTT scores tempo candidates by cross-correlating against pulse
   patterns, so it finds the tempo even when the onset function is mushy. One
   algorithm replaced all three.
8. **BTT's STFT hop is 128 samples**, the same as `AUDIO_BLOCK_SAMPLES`, so no re-buffering is needed.
9. **BTT keeps predicting beats through silence forever.** Stop has to be triggered by RMS level, never by the absence of beats.
10. **`set_log_gaussian_tempo_weight_width()` stores a ratio, not a BPM.** Always set the mean and width together.
11. **Histogram decay is applied once per call to `btt_tempo_tracking()`.** Decimating the calls stretches the time constant by the same factor.
12. **Interpolating the tempo histogram does nothing.** Its peak always lands exactly on a bin by construction. Interpolate the autocorrelation instead. This was only confirmed by testing, after the obvious-looking fix changed nothing.
13. **Check the sample rate for your exact board.** Teensy 4.x runs at exactly 44100 Hz. The widely quoted 44117.647 Hz is Teensy 3.x only. A slow re-anchor can correct timestamps, but it can't correct a period that was converted at the wrong rate, so getting this wrong on a 3.x port would make the clock run slow.

## Performance

14. **`atan2()` in `rect_to_polar` is thrown away by both of its callers.** Removing it was the cheapest optimisation in the whole library.
15. **Decimation lowers the average cost, not the peak.** Judge headroom by the average and by queue depth.
16. **The Cortex-M7 was about 22× slower than a 2.1 GHz Xeon** on this scalar floating-point code, not the 8× that was assumed. Measure on the target early.

## Timing and concurrency

17. **Never timestamp beats with `micros()` when draining the queue.** A backed-up queue drains many blocks at nearly the same moment. Use a virtual audio clock that advances one step per block.
18. **BTT's beats arrive about 26 ms in the future.** Schedule against them instead of correcting after the fact.
19. **`AudioRecordQueue` drops blocks silently.** You can detect it from virtual-clock drift at the moment the queue empties.
20. **Averaging MIDI slaves hide clock jitter.** Sending ticks from `loop()` gave a correct average but ±20% instantaneous spacing. One receiver was fine and another wasn't, and that pattern is the giveaway. Test with a slave that doesn't average.
21. **Never mix a MIDI library with raw ISR writes to the same UART.** `HardwareSerial::write()` does a read-modify-write on the TX buffer head.
22. **Changing a beat's span retroactively moves the due times of ticks already pending in it.** Any change to `tickInterval` in the middle of a beat changes the past as well as the future.
23. **Don't judge an instrumentation counter from one run.** `floor=` climbing steadily looked like proof that the span-floor logic was wrong. It was actually measuring source material whose tempo never settled. A steady-tempo run showed the counter frozen. Get the controlled case before drawing conclusions.

## Display

24. **Diff the OLED by page.** Render into RAM, `memcmp` each page, and push one changed page per loop pass, only when the audio queue is empty.
25. **`display.begin()` forces I2C to 400 kHz.** Set the clock again after calling it.
26. **Never block `loop()` for a splash screen.** It stalls the audio queue.
