# Third-party notices

## Beat-and-Tempo-Tracking (BTT)

- Author: Michael Krzyzaniak
- Source: https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking
- Upstream commit vendored: `c039090f1af771092d95c3ffc402e557940f7384`
- Licence: MIT. The full text is in [`firmware/audioclock_5/BTT_LICENSE`](firmware/audioclock_5/BTT_LICENSE).
- Location in this repo: `firmware/audioclock_5/BTT.h` and `firmware/audioclock_5/src/`

Modified files: `src/BTT.c` and `src/DFT.c`. Every change is marked with a
`TEENSY PORT:` comment, and full diffs against upstream are in
[`patches/`](patches). All other BTT files are unmodified.

## Arduino libraries (not vendored, installed separately)

- Teensy Audio Library, PJRC (MIT)
- Adafruit SSD1306 and Adafruit GFX (BSD)
