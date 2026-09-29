// ============================================================================
//  audioclock_5.ino
//  Audio-to-MIDI/CV Clock v0.5 — Teensy 4.0 + Audio Board Rev D
//  https://github.com/<your-user>/audioclock   — MIT licence, see LICENSE
// ============================================================================
//
//  Build 50010 changes — DOCUMENTATION / PACKAGING, no functional change
//    - SAMPLE_RATE now comes from AUDIO_SAMPLE_RATE_EXACT rather than a
//      literal, so it follows the Audio library's configured rate. On
//      Teensy 4.x that is exactly 44100 Hz: the audio PLL is programmed to
//      hit 44100 precisely. The 44117.647 Hz figure seen in older notes is
//      a Teensy 3.x artefact and does not apply here.
//    - Header comments brought up to date (MIDI library, span floor
//      expectations, sketch folder now vendored in the repo).
//
//  Build 50009 changes — TICK BURST ON RESYNC
//    Observed once as late=92304 in the debug line, during a tempo jump.
//    The ISR cannot be blocked that long, so the DUE TIMES moved backwards.
//
//    A hard resync sets beatEndUs = beatUs, and the span clamp only floored
//    it at 0.5 * period. Halving the span halves tickInterval, which
//    retroactively recomputes the due time of every pending tick in the
//    current beat. Worked example at 129 BPM, period 464 ms, tickInBeat 20:
//    tick 20 was due at beatStart + 387 ms; after the clamp tickInterval
//    halves and it is due at beatStart + 193 ms, already in the past. Ticks
//    20-23 then fire back to back — a burst on the wire, which is exactly
//    the stutter the ISR was meant to remove.
//
//    Fix: the span may never shrink so far that an already-pending tick
//    becomes overdue. This only ever RAISES a span that has already been
//    clamped short, so it is inert during normal correction: PHASE_ALPHA is
//    0.20 and PHASE_SLIP_LIMIT 0.30, so a routine correction moves beatEndUs
//    by at most ~6% of a period, nowhere near the 50% floor. spanFloorHits
//    counts the times it actually did something, so that claim is testable
//    rather than assumed. Validated: frozen during steady locked playing,
//    +1 on a genuine tempo change, ~1 per beat when the source tempo never
//    settles (continuous resyncs are exactly when the floor should engage).
//
//    Cost: a resync now completes on the following beat instead of
//    compressing the remainder of the current one. A beat of slightly-off
//    phase is inaudible; a five-tick pile-up is not.
//
//  Build 50008 changes — MIDI CLOCK JITTER FIX
//    Ticks were emitted from loop(), so they could only leave at loop
//    granularity. A btt_process() frame that runs the generalized
//    autocorrelation takes ~2900 us, plus up to ~1300 us for an OLED page
//    push, so a tick coming due mid-frame waited up to ~4 ms. At 125 BPM a
//    tick is 20 ms, so instantaneous spacing swung roughly 16-24 ms. The
//    average was exactly right — which is why it never drifted — but slaves
//    that measure tick-to-tick rather than averaging heard ~20% wobble.
//
//    Ticks now come from an IntervalTimer ISR polling every 200 us, so
//    worst-case lateness is <=200 us: 1% of a tick instead of 20%.
//
//    Consequences, all handled below:
//    - The ISR is now the ONLY writer to Serial1. loop() cannot write to it
//      concurrently without racing on the TX buffer head pointer, so
//      Start/Stop/Continue/SPP are pushed to a lock-free byte queue that the
//      ISR drains. Single producer (loop), single consumer (ISR), so no
//      interrupt masking is needed for the queue itself.
//    - The MIDI library is gone. It was only ever emitting single bytes and
//      mixing it with raw writes would reintroduce the same race. Serial1 is
//      opened directly at 31250 baud. Byte values are spelled out below.
//    - Gate servicing moved into the ISR too, so gate-off timing no longer
//      inherits loop() jitter.
//    - Clock state is volatile, and loop()'s read-modify-write sequences on
//      it are wrapped in noInterrupts()/interrupts(). Those sections are
//      pure arithmetic with no I/O, so they cannot deadlock against a
//      blocking Serial1 write.
//    - tickLateMax reports worst-case tick lateness in the debug line, so
//      the fix is verifiable rather than assumed.
//
//  Build 50007 changes:
//    - Pin 2 is now a DEDICATED INPUT. The CV clock A output is removed
//      entirely; the jack is a tempo lock button and nothing else.
//      This drops the OUTPUT/INPUT_PULLUP flipping, the two-pass 20 ms
//      read, and the gateA.active guard that blocked the button whenever
//      gate A happened to be high. The pin is simply INPUT_PULLUP for the
//      life of the program and read directly.
//    - gateA removed. CV clock B (pin 10) is now the only CV output.
//      Quarter notes are the "x2" position on the div/mult pot, 24 ticks,
//      which is also the power-on default.
//    - The beat LED is unaffected — it was always a separate gate.
//
//  Build 50006 changes:
//    - CV A jack tip-to-GND repurposed from phase reset to TEMPO LOCK.
//      Toggling lock freezes the period AND stops phase correction: a
//      locked clock is a pure freewheel. If phase correction kept running,
//      BTT would drag the clock back onto whatever audio is playing, which
//      is exactly what lock is meant to prevent. The silence timeout is
//      also suspended — a locked clock should not stop when the music does.
//      BTT keeps processing underneath, so unlocking resumes tracking
//      immediately with no count-in.
//      Pressing lock while stopped starts the clock freewheeling at the
//      last known tempo, which is a useful way to start it in silence.
//    - Status row now reads TRACK / FREEWHL / HOLD / PAUSED / WAITING.
//      HOLD is drawn inverted so the locked state is unmistakable.
//
//  Build 50005 changes:
//    - Dropped-block detection. AudioRecordQueue silently discards incoming
//      blocks when its queue is full and exposes no counter, so the loss is
//      invisible. But it is detectable from our own virtual clock: whenever
//      the queue has just drained, audioTimeUs must agree with micros(),
//      because both should have advanced by the same amount. Each dropped
//      block leaves audioTimeUs one block period behind, permanently.
//      A large drift at that point therefore means blocks were lost — the
//      firmware hard re-anchors, counts the event, and shows "!" on the
//      status row so a silent timing failure becomes a visible one.
//
//  Build 50004 changes:
//    - Debug line now reports the AVERAGE btt_process() cost as well as the
//      peak. With tempo tracking decimated, the peak is unchanged (the GAC
//      frame still costs what it always did, it just happens a quarter as
//      often) so the peak alone cannot tell you whether the patch is live.
//      The average can: ~2500 us means decimation is NOT running, ~900-1400
//      means it is. The average is also what determines whether the audio
//      queue drains, so it is the number to judge headroom by.
//
//  Build 50003 changes:
//    - Splash screen now holds for 1.5 s without blocking loop(). Build
//      50002 drew it and then overwrote it on the first loop pass, because
//      lastRenderUs started at zero.
//    - RESP_DECAY coefficients raised to the 4th power to compensate for
//      decimating btt_tempo_tracking() by 4 in src/BTT.c. Apply that patch
//      or the tempo histogram will be four times more sluggish than the
//      labels claim.
//
//  Build 50002 changes:
//    - Tempo-bias width is now re-pinned after every mean change. BTT's
//      set_width() stores a RATIO, (bpm/mean)^2, and set_mean() preserves
//      the width in BPM at the OLD mean — so turning the bias knob down
//      silently widened the harmonic-suppression window. setTempoBias()
//      re-pins it to the stock 75/120 ratio at every setting.
//    - btt_process() is now timed with ARM_DWT_CYCCNT and reported in the
//      debug line as btt=last/max microseconds against a 2902 us budget.
//
//  v0.5 — Detection front end replaced entirely by BTT
//         (Beat-and-Tempo-Tracking, Michael Krzyzaniak, MIT licence)
//         https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking
//
//    REMOVED: MODE_PEAK / MODE_WAVELET / MODE_SEF detectors,
//             onsetBuf, pktBuf, acGrid, autocorrelateOnsets(), findBestLag(),
//             wavelet_onset.h, AudioAnalyzePeak, AudioAnalyzeFFT256.
//
//    ADDED:   BTT spectral-flux onset detection, generalized-autocorrelation
//             tempo tracking and cbss beat prediction.
//
//  CLOCK ARCHITECTURE — "split" (option C)
//    Period : taken directly from btt_get_beat_period_audio_samples().
//             BTT already smooths tempo through a decaying Gaussian
//             histogram, so no second frequency integrator is used.
//             The old PLL beta term is GONE.
//    Phase  : corrected from BTT beat callbacks with a proportional term
//             only (alpha). Corrections are applied to the END of the
//             current beat, so the 24 ticks inside it stretch or squeeze
//             smoothly and never move backwards.
//    Between beats the clock freewheels on beatPeriodUs.
//
//  TIMING
//    BTT reports beats in the AUDIO SAMPLE domain, roughly 26 ms ahead of
//    real time. A virtual audio clock (audioTimeUs) advances one block
//    period per block fed and is slowly re-anchored to micros() only when
//    the queue has drained. This survives OLED stalls and absorbs the
//    sub-microsecond rounding of US_PER_BLOCK.
//
//  OLED
//    Audio has absolute priority. The frame is rendered into RAM, diffed
//    against a shadow copy, and only changed 128-byte pages are pushed —
//    one page per loop pass, and only when the audio queue is empty.
//    A failsafe pushes one page anyway if nothing has gone out for 400 ms,
//    so the display degrades to sluggish but never freezes.
//
//  Pin map (unchanged except where noted):
//    1  (TX1)   MIDI DIN out
//    2          TEMPO LOCK button in ← BSS138 LV2 ← 3.5mm jack (INPUT only)
//    10         CV clock  → BSS138 LV3 → 3.5mm jack (the only CV output)
//    22 (A8)    3-state switch — REPURPOSED: tempo responsiveness
//                 <200 = FST, 200–800 = MED, >800 = SLW
//    5          Input source switch: GND=LINE, 3.3V=MIC (INPUT_PULLDOWN)
//    14 (A0)    REPURPOSED: tempo bias (log-Gaussian weight mean, 60–180 BPM)
//    15 (A1)    Divide/multiply pot wiper
//    16 (A2)    Gate length pot wiper
//    9          Beat LED
//    18 (SDA)   OLED + Audio Board I2C (shared bus)
//    19 (SCL)   OLED + Audio Board I2C
//
//  AVOID: pin 3 (dead on prototype), pin 4 (not analog), pin 17 (Serial4 TX)
//
//  Sketch folder (complete in the repo — open this .ino and build):
//    audioclock_5.ino
//    BTT.h, BTT_LICENSE    ← BTT, vendored (MIT, Michael Krzyzaniak)
//    src/                  ← BTT sources; BTT.c and DFT.c carry the four
//                            TEENSY PORT patches (see docs/btt-patches.md)
//
//  Libraries: Teensyduino Audio, Adafruit SSD1306 + GFX.
//  (The MIDI library is NOT used — removed in Build 50008.)
// ============================================================================

const int BUILD = 50010;

#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "BTT.h"          // already carries its own extern "C" guards

// ── MIDI byte constants ───────────────────────────────────────────────────
// Emitted raw. The MIDI library only ever wrapped single-byte writes, and
// mixing library calls with the ISR's raw writes would race on the Serial1
// TX buffer. DIN MIDI is 31250 baud, 8N1.
const uint8_t MIDI_CLOCK    = 0xF8;
const uint8_t MIDI_START    = 0xFA;
const uint8_t MIDI_CONTINUE = 0xFB;
const uint8_t MIDI_STOP     = 0xFC;
const uint8_t MIDI_SPP      = 0xF2;
const uint32_t MIDI_BAUD    = 31250;

// ── OLED ──────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_ADDRESS  0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// I2C speeds. The SSD1306 runs happily at 1 MHz on short wires; the
// SGTL5000 is only specified to 400 kHz, so codec writes drop the clock.
const uint32_t I2C_FAST  = 1000000UL;
const uint32_t I2C_CODEC =  400000UL;

// ── Audio ─────────────────────────────────────────────────────────────────
AudioInputI2S        audioIn;
AudioAnalyzeRMS      rms;              // signal meter + silence detection
AudioRecordQueue     audioQueue;       // raw blocks → BTT
AudioConnection      patchCord1(audioIn, 0, rms, 0);
AudioConnection      patchCord2(audioIn, 0, audioQueue, 0);
AudioControlSGTL5000 codec;

// ── BTT ───────────────────────────────────────────────────────────────────
BTT* btt = NULL;

// Follows the Audio library's configured rate. Exactly 44100 Hz on Teensy
// 4.x (the audio PLL is programmed for it) — the 44117.647 Hz figure is a
// Teensy 3.x artefact. Beat periods arrive in samples and are converted here.
const float SAMPLE_RATE   = AUDIO_SAMPLE_RATE_EXACT;
const float US_PER_SAMPLE = 1000000.0f / SAMPLE_RATE;
const unsigned long US_PER_BLOCK =
  (unsigned long)(AUDIO_BLOCK_SAMPLES * US_PER_SAMPLE + 0.5f);   // ~2902 us

// Virtual audio clock. audioTimeUs is the wall-clock time at which the NEXT
// block to be fed was captured. It advances deterministically and is only
// re-anchored to micros() when we are demonstrably caught up.
unsigned long      audioTimeUs      = 0;
unsigned long long bttSamplesFed    = 0;
unsigned long      blockStartUs     = 0;   // valid during btt_process()
unsigned long long blockStartSample = 0;

// ── Pins ──────────────────────────────────────────────────────────────────
const int LOCK_PIN     = 2;    // input only — tempo lock button
const int CV_B_PIN     = 10;   // the only CV clock output
const int RESP_PIN     = A8;   // pin 22 — 3-state responsiveness switch
const int INPUT_PIN    = 5;
const int BIAS_PIN     = A0;   // pin 14 — tempo bias
const int MULT_PIN     = A1;   // pin 15
const int LED_PIN      = 9;
const int GATE_LEN_PIN = A2;   // pin 16

// ── Input source ──────────────────────────────────────────────────────────
enum InputSource { SRC_LINE, SRC_MIC };
InputSource   inputSource = SRC_LINE;
const uint8_t MIC_GAIN_DB = 40;
const uint8_t LINE_LEVEL  = 5;

// ── Tempo responsiveness (pin 22) ─────────────────────────────────────────
// Gaussian tempo histogram decay. Lower = forgets old estimates faster =
// follows tempo changes sooner, at the cost of stability.
enum Resp { RESP_FAST, RESP_MED, RESP_SLOW };
Resp        respMode = RESP_MED;
const char* RESP_LABEL[3] = { "FST", "MED", "SLW" };
// Decay is applied once per call to btt_tempo_tracking(). With the
// decimate-by-4 patch in src/BTT.c that is 86 Hz, not 344 Hz, so the
// coefficients are the 4th powers of the per-frame values we actually want
// (0.9950, 0.9990, 0.9997). Remove the exponent if you drop the patch.
const double RESP_DECAY[3] = { 0.980149, 0.996006, 0.998801 };

// ── Beat event queue (filled from the BTT callback) ───────────────────────
// The callback runs inside btt_process(), which runs inside the drain loop.
// Keep it to a store — all real work happens afterwards.
const int   BEATQ_LEN = 8;
unsigned long beatQ[BEATQ_LEN];
volatile int  beatQHead = 0;
volatile int  beatQTail = 0;

extern "C" void bttBeatCallback(void* self, unsigned long long sampleTime)
{
  // sampleTime is in audio samples and is typically ~26 ms in the future.
  long long delta = (long long)sampleTime - (long long)blockStartSample;
  unsigned long beatUs = blockStartUs + (long)(delta * (double)US_PER_SAMPLE);

  int next = (beatQHead + 1) % BEATQ_LEN;
  if (next != beatQTail) { beatQ[beatQHead] = beatUs; beatQHead = next; }
}

// btt_process() cost, microseconds. Budget is US_PER_BLOCK (~2902 us).
uint32_t bttUsLast  = 0;
uint32_t bttUsMax   = 0;
uint32_t bttUsAccum = 0;   // summed over the reporting interval
uint32_t bttBlocks  = 0;

// Dropped-block detector. See serviceAudio(). Three block periods is well
// beyond normal jitter but under the cost of a single lost block, so this
// catches the first drop rather than an accumulation of them.
const long DRIFT_LIMIT_US = 3 * (long)US_PER_BLOCK;   // ~8700 us
uint32_t   dropEvents     = 0;
long       lastDriftUs    = 0;

unsigned long lastOnsetUs = 0;
extern "C" void bttOnsetCallback(void* self, unsigned long long sampleTime)
{
  lastOnsetUs = micros();   // activity indicator only — not used for clocking
}

// ════════════════════════════════════════════════════════════════════════════
//  CLOCK ENGINE — period from BTT, phase from a proportional-only corrector
// ════════════════════════════════════════════════════════════════════════════

const int PPQN = 24;

// Shared with clockISR(). 32-bit aligned scalars, so individual reads and
// writes are atomic on Cortex-M — volatile is enough to stop the compiler
// caching them. What is NOT safe without masking is a read-modify-write in
// loop() that the ISR could interleave with (see handleBeat), because the
// ISR may roll the beat over in between and have its write clobbered.
volatile unsigned long beatPeriodUs = 0;  // authoritative, straight from BTT
volatile unsigned long beatStartUs  = 0;  // start of the beat now playing
volatile unsigned long beatEndUs    = 0;  // where the next beat is expected
volatile int           tickInBeat   = 0;  // 0..23
volatile unsigned long tickCounter  = 0;  // free-running, drives div/mult
volatile int           songPosBeats = 0;
volatile bool          clockRunning = false;

// Worst-case tick lateness, microseconds, reset each debug print. With the
// ISR this should sit at or below CLOCK_ISR_US. Before the fix it ran
// 3000-4500 us.
volatile uint32_t tickLateMax = 0;

// Counts how often the span floor in handleBeat() actually raised the span.
// Cumulative, never reset. Expected to stay frozen while the tempo is locked
// and steady; it legitimately climbs across resyncs (tempo changes, or
// material whose tempo never settles). If it climbs during steady, locked
// playing, the span-floor reasoning is wrong — restore Build 50008.
volatile uint32_t spanFloorHits = 0;

IntervalTimer clockTimer;
const unsigned long CLOCK_ISR_US = 200;
bool          wasPaused    = false;
unsigned long lastBeatUs   = 0;

// Phase corrector. Proportional only — no frequency term, BTT owns tempo.
const float PHASE_ALPHA      = 0.20f;  // fraction of error applied per beat
const float PHASE_SLIP_LIMIT = 0.30f;  // |err| beyond this = suspected slip
const int   PHASE_SLIP_COUNT = 3;      // consecutive slips before hard resync
int         phaseSlips       = 0;

// Span clamp: the current beat may never be stretched or squeezed past these
// bounds, which is what guarantees the tick stream stays monotonic.
const float SPAN_MIN_FRAC = 0.50f;
const float SPAN_MAX_FRAC = 1.50f;

// ── Gates ─────────────────────────────────────────────────────────────────
// Fired and serviced from clockISR(). The one exception is the lock-button
// confirmation flash, which loop() raises under a brief critical section.
struct Gate {
  volatile bool          active = false;
  volatile unsigned long offAt  = 0;
  void fire(int pin, unsigned long now, unsigned long us) {
    digitalWrite(pin, HIGH); active = true; offAt = now + us;
  }
  void service(int pin, unsigned long now) {
    if (active && (long)(now - offAt) >= 0) { digitalWrite(pin, LOW); active = false; }
  }
};
Gate gateB, gateLed;

unsigned long gateLength   = 10000UL;
const unsigned long LED_MS = 30000UL;

// ── Div/mult table ────────────────────────────────────────────────────────
struct MultEntry { int ticks; const char* label; };
const MultEntry MULT_TABLE[] = {
  { 96, "/2"  }, { 48, "/1"  }, { 24, "x2"  },
  { 12, "x4"  }, {  6, "x8"  }, { 16, "T8"  }, {  8, "T16" },
};
const int MULT_COUNT = sizeof(MULT_TABLE) / sizeof(MULT_TABLE[0]);
int multIdx = 2;

// ── Silence detection (drives Stop — beats alone cannot, BTT freewheels) ──
const float         SILENCE_RMS = 0.004f;
const unsigned long SILENCE_US  = 3000000UL;
unsigned long lastLoudUs = 0;
float         sigLevel   = 0.0f;

// ── Tempo bias pot ────────────────────────────────────────────────────────
float tempoBias = 120.0f;   // BPM centre of the log-Gaussian tempo weight

// ── Pot smoothing ─────────────────────────────────────────────────────────
float potSmooth[3] = {512.0f, 512.0f, 512.0f};   // BIAS, MULT, GATE
const float POT_EMA = 0.15f;

int readPotSmoothed(int pin, int idx) {
  float raw = (float)analogRead(pin);
  potSmooth[idx] = POT_EMA * raw + (1.0f - POT_EMA) * potSmooth[idx];
  return (int)(potSmooth[idx] + 0.5f);
}

// ── Tempo lock button (pin 2, dedicated input) ────────────────────────────
// Pin 2 is INPUT_PULLUP for the life of the program, so the button is just
// read: LOW = pressed. Sampled at 50 Hz with an armed flag, which is all the
// debouncing a mechanical contact needs at this rate.
bool          tempoLocked     = false;
bool          lockArmed       = true;
unsigned long lastLockCheck   = 0;
const unsigned long LOCK_CHECK_INTERVAL = 20000UL;

// ════════════════════════════════════════════════════════════════════════════
//  OLED — offscreen render, per-page diff, one page per pass
// ════════════════════════════════════════════════════════════════════════════

uint8_t oledShadow[1024];
bool    pageDirty[8] = {false};
int     nextPageScan = 0;
unsigned long lastPagePushUs   = 0;
unsigned long splashUntilUs    = 0;
const unsigned long SPLASH_US  = 1500000UL;
unsigned long lastRenderUs     = 0;
const unsigned long RENDER_INTERVAL   = 100000UL;   // rebuild RAM frame
const unsigned long PAGE_FAILSAFE_US  = 400000UL;   // never freeze entirely
bool    oledReady = false;

void oledCommand(uint8_t c) {
  Wire.beginTransmission(OLED_ADDRESS);
  Wire.write(0x00); Wire.write(c);
  Wire.endTransmission();
}

// Push one 128-byte page. ~1.3 ms at 1 MHz including transaction overhead.
void oledPushPage(int page) {
  uint8_t* buf = display.getBuffer() + page * 128;
  oledCommand(0xB0 | (page & 0x07));   // set page
  oledCommand(0x00);                   // column low nibble  = 0
  oledCommand(0x10);                   // column high nibble = 0
  for (int i = 0; i < 128; i += 16) {
    Wire.beginTransmission(OLED_ADDRESS);
    Wire.write(0x40);
    Wire.write(buf + i, 16);
    Wire.endTransmission();
  }
  memcpy(oledShadow + page * 128, buf, 128);
  pageDirty[page] = false;
}

void renderFrame();   // fwd

void markDirtyPages() {
  uint8_t* buf = display.getBuffer();
  for (int p = 0; p < 8; p++)
    if (memcmp(buf + p * 128, oledShadow + p * 128, 128) != 0)
      pageDirty[p] = true;
}

// Called every loop pass. Pushes at most one page, and only when the audio
// queue is empty — unless the failsafe timer has expired.
void serviceOled(unsigned long now) {
  if (!oledReady) return;

  // Hold the splash without blocking — loop() keeps draining audio, the
  // display simply declines to redraw until the timer expires.
  if ((long)(now - splashUntilUs) < 0) return;

  if ((long)(now - lastRenderUs) >= (long)RENDER_INTERVAL) {
    lastRenderUs = now;
    renderFrame();
    markDirtyPages();
  }

  bool caughtUp = (audioQueue.available() == 0);
  bool overdue  = ((long)(now - lastPagePushUs) > (long)PAGE_FAILSAFE_US);
  if (!caughtUp && !overdue) return;

  for (int i = 0; i < 8; i++) {
    int p = (nextPageScan + i) % 8;
    if (pageDirty[p]) {
      oledPushPage(p);
      nextPageScan   = (p + 1) % 8;
      lastPagePushUs = now;
      return;
    }
  }
  lastPagePushUs = now;   // nothing dirty — keep the failsafe from arming
}

// ════════════════════════════════════════════════════════════════════════════
//  MIDI OUTPUT QUEUE
//
//  clockISR() is the only writer to Serial1. Teensy's HardwareSerial::write
//  does a read-modify-write on the TX buffer head, so if loop() wrote while
//  the ISR preempted it mid-write, one of the two bytes would be lost.
//
//  Non-realtime messages from loop() therefore go through this queue.
//  loop() is the only producer and the ISR the only consumer, so it needs
//  no masking: the producer fills the slot before advancing head, and the
//  consumer reads the slot before advancing tail.
// ════════════════════════════════════════════════════════════════════════════

const uint16_t MIDIQ_LEN = 32;          // power of two — mask instead of %
volatile uint8_t  midiQ[MIDIQ_LEN];
volatile uint16_t midiQHead = 0;
volatile uint16_t midiQTail = 0;

// Producer — loop() only.
void midiQPush(uint8_t b) {
  uint16_t next = (uint16_t)((midiQHead + 1) & (MIDIQ_LEN - 1));
  if (next == midiQTail) return;        // full: drop rather than block
  midiQ[midiQHead] = b;                 // fill BEFORE advancing head
  midiQHead = next;
}

// ════════════════════════════════════════════════════════════════════════════
//  HELPERS
// ════════════════════════════════════════════════════════════════════════════

void applyInputSource(InputSource src) {
  Wire.setClock(I2C_CODEC);
  if (src == SRC_LINE) {
    codec.inputSelect(AUDIO_INPUT_LINEIN);
    codec.lineInLevel(LINE_LEVEL);
  } else {
    codec.inputSelect(AUDIO_INPUT_MIC);
    codec.micGain(MIC_GAIN_DB);
  }
  Wire.setClock(I2C_FAST);
}

void sendSPP() {
  int mb = constrain(songPosBeats * 4, 0, 16383);
  midiQPush(MIDI_SPP);
  midiQPush(mb & 0x7F);
  midiQPush((mb >> 7) & 0x7F);
}

void startClock(unsigned long firstBeatUs) {
  noInterrupts();
  songPosBeats = 0; tickCounter = 0; tickInBeat = 0;
  beatStartUs  = firstBeatUs;
  beatEndUs    = firstBeatUs + beatPeriodUs;
  clockRunning = true;
  interrupts();
  wasPaused = false; phaseSlips = 0;
  sendSPP(); midiQPush(MIDI_START);      // queued — never written directly
}

void continueClock(unsigned long firstBeatUs) {
  noInterrupts();
  tickInBeat  = 0;
  beatStartUs = firstBeatUs;
  beatEndUs   = firstBeatUs + beatPeriodUs;
  clockRunning = true;
  interrupts();
  phaseSlips = 0;
  sendSPP(); midiQPush(MIDI_CONTINUE);
}

void stopClock() {
  noInterrupts();
  clockRunning = false;
  gateB.active = gateLed.active = false;
  interrupts();
  wasPaused = true;
  digitalWrite(CV_B_PIN, LOW);
  digitalWrite(LED_PIN,  LOW);
  midiQPush(MIDI_STOP);
}

// Full reset of BTT and clock state — mode or input change.
void resetAll() {
  if (clockRunning) stopClock();
  wasPaused = false;
  beatQHead = beatQTail = 0;
  beatPeriodUs = 0; tickInBeat = 0; tickCounter = 0;
  phaseSlips = 0; lastBeatUs = 0;
  dropEvents = 0;
  tempoLocked = false;
  audioQueue.clear();
  if (btt) btt_clear(btt);
  bttSamplesFed = 0;
  audioTimeUs   = micros();
}

// Set the tempo-weight centre AND re-pin its width.
//
// btt_set_log_gaussian_tempo_weight_width() stores (bpm / mean)^2 — a ratio,
// not an absolute. btt_set_log_gaussian_tempo_weight_mean() reads the width
// back in BPM at the OLD mean and re-applies it against the NEW one, so the
// stored ratio drifts as the knob moves: mean 120 gives (75/120)^2 = 0.39,
// mean 69 gives (75/69)^2 = 1.18. Three times wider in log terms, which
// weakens octave suppression exactly where it is needed most. Re-pinning to
// the stock 0.625 ratio keeps suppression constant across the knob's travel.
const float TEMPO_WEIGHT_RATIO = 75.0f / 120.0f;   // 0.625, BTT default

void setTempoBias(float bpm) {
  if (!btt) return;
  btt_set_log_gaussian_tempo_weight_mean (btt, bpm);
  btt_set_log_gaussian_tempo_weight_width(btt, bpm * TEMPO_WEIGHT_RATIO);
}

void readPots() {
  int bRaw = readPotSmoothed(BIAS_PIN,     0);
  int mRaw = readPotSmoothed(MULT_PIN,     1);
  int gRaw = readPotSmoothed(GATE_LEN_PIN, 2);

  // Tempo bias — centre of the log-Gaussian tempo weight window.
  // This is the control that decides half-time vs double-time.
  float newBias = 60.0f + (bRaw / 1023.0f) * 120.0f;   // 60–180 BPM
  if (fabsf(newBias - tempoBias) > 1.0f) {
    tempoBias = newBias;
    setTempoBias(tempoBias);
  }

  multIdx = constrain(map(mRaw, 0, 1023, 0, MULT_COUNT - 1), 0, MULT_COUNT - 1);

  unsigned long newGate = (unsigned long)map(gRaw, 0, 1023, 2000, 50000);
  if (labs((long)newGate - (long)gateLength) > 1000) gateLength = newGate;
}

// ════════════════════════════════════════════════════════════════════════════
//  AUDIO → BTT
// ════════════════════════════════════════════════════════════════════════════

void serviceAudio() {
  float fbuf[AUDIO_BLOCK_SAMPLES];

  while (audioQueue.available() > 0) {
    int16_t* buf = audioQueue.readBuffer();
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
      fbuf[i] = (float)buf[i] / 32768.0f;
    audioQueue.freeBuffer();

    // Anchor this block on the virtual clock before processing, so the
    // beat callback can convert its sample-domain timestamp.
    blockStartUs     = audioTimeUs;
    blockStartSample = bttSamplesFed;

    uint32_t c0 = ARM_DWT_CYCCNT;
    btt_process(btt, fbuf, AUDIO_BLOCK_SAMPLES);
    uint32_t cycles = ARM_DWT_CYCCNT - c0;        // unsigned, wrap-safe
    bttUsLast = cycles / (F_CPU / 1000000);
    if (bttUsLast > bttUsMax) bttUsMax = bttUsLast;
    bttUsAccum += bttUsLast;
    bttBlocks++;

    bttSamplesFed += AUDIO_BLOCK_SAMPLES;
    audioTimeUs   += US_PER_BLOCK;
  }

  // The queue is now empty, so audioTimeUs and micros() should agree: both
  // have advanced by one block period for every block captured. They only
  // diverge if blocks were captured but never fed to BTT — i.e. dropped
  // inside AudioRecordQueue because loop() stalled longer than the queue
  // could cover. Each loss puts audioTimeUs one block period behind for
  // good, which would silently offset every future beat timestamp.
  long drift = (long)(micros() - audioTimeUs);
  lastDriftUs = drift;

  if (labs(drift) > DRIFT_LIMIT_US) {
    dropEvents++;
    audioTimeUs = micros();     // hard re-anchor — restores beat timing
  } else {
    // Normal path: slow pull towards real time. Also absorbs the ~0.5 us
    // per block lost by rounding US_PER_BLOCK to whole microseconds.
    audioTimeUs += (long)(0.02f * drift);
  }
}

// ════════════════════════════════════════════════════════════════════════════
//  BEAT HANDLING
// ════════════════════════════════════════════════════════════════════════════

// nowUs is passed in rather than read inside the critical section — micros()
// is cheap but there is no reason to do anything avoidable with interrupts
// masked, and the value must be consistent with the span arithmetic.
void handleBeat(unsigned long beatUs, unsigned long nowUs) {
  lastBeatUs = beatUs;

  if (beatPeriodUs == 0) return;   // no tempo yet — nothing to phase against

  if (!clockRunning) {
    wasPaused ? continueClock(beatUs) : startClock(beatUs);
    return;
  }

  // Read-modify-write on beatEndUs. The ISR may roll the beat over between
  // our read and our write, which would silently discard its rollover and
  // either skip or double a beat. Pure arithmetic, no I/O — cannot deadlock.
  noInterrupts();

  long err = (long)(beatUs - beatEndUs);
  long half = (long)(beatPeriodUs / 2);
  while (err >  half) err -= (long)beatPeriodUs;
  while (err < -half) err += (long)beatPeriodUs;

  if (labs(err) > (long)(PHASE_SLIP_LIMIT * beatPeriodUs)) {
    // Big error. One of these is usually a syncopation artefact; several in
    // a row means BTT has genuinely re-phased and we should follow it.
    if (++phaseSlips >= PHASE_SLIP_COUNT) {
      beatEndUs  = beatUs;
      phaseSlips = 0;
    }
  } else {
    phaseSlips = 0;
    beatEndUs  = (unsigned long)((long)beatEndUs + (long)(PHASE_ALPHA * err));
  }

  // Clamp the span so the remaining ticks of this beat stay monotonic.
  long span    = (long)(beatEndUs - beatStartUs);
  long spanMin = (long)(SPAN_MIN_FRAC * beatPeriodUs);
  long spanMax = (long)(SPAN_MAX_FRAC * beatPeriodUs);
  if (span < spanMin) beatEndUs = beatStartUs + spanMin;
  if (span > spanMax) beatEndUs = beatStartUs + spanMax;

  // Span floor: a shrunk span recomputes tickInterval, which can move
  // pending ticks into the past and make them fire as a burst. Require the
  // span to be long enough that the next pending tick is still ahead of us.
  //
  //   tick k is due at beatStart + k * (span / PPQN)
  //   we need that > nowUs for k = tickInBeat
  //   => span > (nowUs - beatStart) * PPQN / tickInBeat
  //
  // The + PPQN term adds a 1 us margin per tick so the comparison in the
  // ISR resolves strictly in the future rather than exactly on the boundary.
  if (tickInBeat > 0 && tickInBeat < PPQN) {
    long elapsed = (long)(nowUs - beatStartUs);
    if (elapsed > 0) {
      long minSpan = (elapsed * PPQN) / tickInBeat + PPQN;
      if ((long)(beatEndUs - beatStartUs) < minSpan) {
        beatEndUs = beatStartUs + (unsigned long)minSpan;
        spanFloorHits++;
      }
    }
  }

  interrupts();
}

// ════════════════════════════════════════════════════════════════════════════
//  CLOCK ISR — the only writer to Serial1
//
//  Polls every CLOCK_ISR_US (200 us). Everything here must stay short: it
//  preempts the audio library's update ISR. Measured cost is a few
//  microseconds, and it only does real work on the ~1 pass in 100 where a
//  tick is actually due.
// ════════════════════════════════════════════════════════════════════════════

void clockISR() {
  unsigned long now = micros();

  // Gate off-timing. Here rather than in loop() so gate length does not
  // inherit loop jitter.
  gateB.service(CV_B_PIN, now);
  gateLed.service(LED_PIN, now);

  // Drain transport messages queued by loop().
  while (midiQTail != midiQHead) {
    Serial1.write(midiQ[midiQTail]);
    midiQTail = (uint16_t)((midiQTail + 1) & (MIDIQ_LEN - 1));
  }

  if (!clockRunning || beatPeriodUs == 0) return;

  unsigned long span         = beatEndUs - beatStartUs;
  unsigned long tickInterval = span / PPQN;
  if (tickInterval == 0) return;

  // Emit any ticks that are due. Catching up rather than dropping keeps
  // downstream song position correct after a stall.
  int guard = 0;
  while (tickInBeat < PPQN &&
         (long)(now - (beatStartUs + (unsigned long)tickInBeat * tickInterval)) >= 0 &&
         guard++ < PPQN) {

    unsigned long due  = beatStartUs + (unsigned long)tickInBeat * tickInterval;
    uint32_t      late = (uint32_t)(now - due);
    if (late > tickLateMax) tickLateMax = late;

    Serial1.write(MIDI_CLOCK);

    if (tickInBeat == 0) {
      gateLed.fire(LED_PIN, now, LED_MS);
      songPosBeats++;
    }
    if (tickCounter % MULT_TABLE[multIdx].ticks == 0)
      gateB.fire(CV_B_PIN, now, gateLength);

    tickCounter++;
    tickInBeat++;
  }

  if (tickInBeat >= PPQN) {
    beatStartUs = beatEndUs;
    beatEndUs   = beatStartUs + beatPeriodUs;
    tickInBeat  = 0;
  }
}

// ════════════════════════════════════════════════════════════════════════════
//  DISPLAY RENDER (RAM only — no I2C traffic here)
// ════════════════════════════════════════════════════════════════════════════

bool isLocked(unsigned long now) {
  return beatPeriodUs > 0 && lastBeatUs > 0 &&
         (long)(now - lastBeatUs) < (long)(2 * beatPeriodUs);
}

void renderFrame() {
  unsigned long now = micros();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Row 0 — input source | responsiveness
  if (inputSource == SRC_LINE) {
    display.fillRect(0, 0, 30, 9, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(3, 1);  display.print("LINE");
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(34, 1); display.print("MIC");
  } else {
    display.fillRect(31, 0, 27, 9, SSD1306_WHITE);
    display.setCursor(3, 1);  display.print("LINE");
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(34, 1); display.print("MIC");
    display.setTextColor(SSD1306_WHITE);
  }
  display.drawPixel(62, 4, SSD1306_WHITE);
  {
    const int modeX[3] = {64, 85, 106};
    for (int m = 0; m < 3; m++) {
      if (m == (int)respMode) {
        display.fillRect(modeX[m], 0, 21, 9, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.setTextColor(SSD1306_WHITE);
      }
      display.setCursor(modeX[m] + 2, 1);
      display.print(RESP_LABEL[m]);
    }
    display.setTextColor(SSD1306_WHITE);
  }

  // Row 1 — BPM
  float bpm = (beatPeriodUs > 0) ? 60000000.0f / (float)beatPeriodUs : 0.0f;
  if (clockRunning && bpm > 0.0f) {
    display.setTextSize(3);
    display.setCursor(0, 12);
    char s[8]; dtostrf(bpm, 6, 1, s); display.print(s);
  } else {
    display.setTextSize(2);
    display.setCursor(0, 14);
    display.print(wasPaused ? "  PAUSE" : "   ---");
  }

  // Row 2 — status, div/mult, gate
  display.setTextSize(1);
  display.setCursor(0, 38);
  if (tempoLocked) {
    // Inverted badge — the held state must be unmistakable at a glance.
    display.fillRect(0, 37, 29, 9, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(2, 38); display.print("HOLD");
    display.setTextColor(SSD1306_WHITE);
  } else if (clockRunning) {
    display.print(isLocked(now) ? "TRACK  " : "FREEWHL");
  } else {
    display.print(wasPaused ? "PAUSED " : "WAITING");
  }
  if (dropEvents) { display.setCursor(46, 38); display.print("!"); }
  display.setCursor(56, 38);
  display.print("B:"); display.print(MULT_TABLE[multIdx].label);
  display.setCursor(90, 38);
  display.print("G:"); display.print((int)(gateLength / 1000)); display.print("m");

  // Row 3 — tempo bias, 60–180 BPM
  display.setCursor(0, 49);
  display.print("BIA");
  const int BAR_W = 80;
  display.drawRect(22, 50, BAR_W, 5, SSD1306_WHITE);
  int biasX = 22 + constrain((int)((tempoBias - 60.0f) / 120.0f * BAR_W), 0, BAR_W - 1);
  display.fillRect(biasX - 1, 50, 3, 5, SSD1306_WHITE);
  display.setCursor(106, 49);
  display.print((int)tempoBias);

  // Row 4 — signal. Quantised to 4-pixel steps so a drifting RMS reading
  // does not dirty this page on every single render pass.
  display.setCursor(0, 57);
  display.print("SIG");
  int sw = constrain((int)(sigLevel * 80.0f), 0, 80);
  sw = (sw / 4) * 4;
  display.fillRect(22, 58, sw, 5, SSD1306_WHITE);
  display.drawRect(22, 58, 80, 5, SSD1306_WHITE);
}

// ════════════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
  pinMode(LOCK_PIN,  INPUT_PULLUP);   // dedicated input — never driven
  pinMode(CV_B_PIN,  OUTPUT);
  pinMode(LED_PIN,   OUTPUT);
  pinMode(INPUT_PIN, INPUT_PULLDOWN);

  Serial.begin(115200);

  // 60 blocks. An OLED page push is ~1.3 ms; a full frame spread over eight
  // loop passes never stalls audio, but headroom costs nothing here.
  AudioMemory(60);

  Wire.begin();
  Wire.setClock(I2C_FAST);

  codec.enable();
  inputSource = digitalRead(INPUT_PIN) ? SRC_MIC : SRC_LINE;
  applyInputSource(inputSource);
  codec.volume(0.0);

  Serial1.begin(MIDI_BAUD);   // DIN MIDI out, 31250 8N1

  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  Wire.setClock(I2C_FAST);   // begin() forces 400 kHz — put it back
  if (oledReady) {
    oledCommand(0x20); oledCommand(0x02);   // page addressing mode
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(14, 10); display.print("AudioClock  v0.5");
    display.setCursor(10, 24); display.print("BTT beat track");
    display.setCursor(10, 36); display.print("Phase-lock clock");
    display.setCursor(10, 48); display.print("Build "); display.print(BUILD);
    memset(oledShadow, 0xFF, sizeof(oledShadow));   // force full first push
    for (int p = 0; p < 8; p++) oledPushPage(p);
    splashUntilUs = micros() + SPLASH_US;
    lastRenderUs  = micros();
  }

  btt = btt_new_default();
  if (btt == NULL) {
    Serial.println("BTT allocation failed");
    while (1) { digitalWrite(LED_PIN, HIGH); delay(100);
                digitalWrite(LED_PIN, LOW);  delay(100); }
  }
  btt_set_beat_tracking_callback (btt, bttBeatCallback,  NULL);
  btt_set_onset_tracking_callback(btt, bttOnsetCallback, NULL);
  btt_set_tracking_mode(btt, BTT_ONSET_AND_TEMPO_AND_BEAT_TRACKING);
  btt_set_min_tempo(btt, 60.0);
  btt_set_max_tempo(btt, 200.0);
  btt_set_gaussian_tempo_histogram_decay(btt, RESP_DECAY[respMode]);
  setTempoBias(tempoBias);

  audioTimeUs = micros();
  lastLoudUs  = micros();
  audioQueue.begin();

  // Start the tick ISR last, once all clock state is initialised.
  // Priority 64 is above the default 128 so ticks stay punctual; the ISR is
  // short enough that preempting the audio update is harmless.
  clockTimer.begin(clockISR, CLOCK_ISR_US);
  clockTimer.priority(64);
}

// ════════════════════════════════════════════════════════════════════════════
//  MAIN LOOP
// ════════════════════════════════════════════════════════════════════════════

void loop() {
  unsigned long now = micros();

  // 1. Audio → BTT. Gates and ticks are no longer serviced here — they run
  //    in clockISR(), which is exactly the point of Build 50008.
  serviceAudio();

  // 2. Tempo straight from BTT — unless held.
  if (!tempoLocked) {
    int periodSamples = btt_get_beat_period_audio_samples(btt);
    if (periodSamples > 0)
      beatPeriodUs = (unsigned long)(periodSamples * US_PER_SAMPLE);
  }

  // 3. Drain the beat queue. While held the queue is still drained — it must
  //    not be allowed to fill — but the beats are discarded, so no phase
  //    correction is applied and the clock freewheels on the frozen period.
  while (beatQTail != beatQHead) {
    unsigned long beatUs = beatQ[beatQTail];
    beatQTail = (beatQTail + 1) % BEATQ_LEN;
    if (!tempoLocked) handleBeat(beatUs, micros());
    else              lastBeatUs = beatUs;   // keep the status row honest
  }

  now = micros();

  // 4. Signal level and silence timeout. BTT freewheels through silence,
  //    so beats can never be the stop condition — RMS is.
  if (rms.available()) {
    sigLevel = rms.read();
    if (sigLevel > SILENCE_RMS) lastLoudUs = now;
  }
  if (!tempoLocked && clockRunning && (long)(now - lastLoudUs) > (long)SILENCE_US) {
    stopClock();   // BTT keeps its tempo — a return of signal resumes fast
  }

  // 5. Controls, every 50 ms.
  static unsigned long lastPotRead = 0;
  if ((long)(now - lastPotRead) > 50000L) {
    lastPotRead = now;
    readPots();

    InputSource newSrc = digitalRead(INPUT_PIN) ? SRC_MIC : SRC_LINE;
    if (newSrc != inputSource) {
      inputSource = newSrc;
      applyInputSource(inputSource);
      resetAll();
    }

    int raw = analogRead(RESP_PIN);
    Resp newResp = (raw < 200) ? RESP_FAST : (raw < 800) ? RESP_MED : RESP_SLOW;
    if (newResp != respMode) {
      respMode = newResp;
      btt_set_gaussian_tempo_histogram_decay(btt, RESP_DECAY[respMode]);
      // Deliberately no resetAll() here — responsiveness is a live tweak.
    }
  }

  // 6. Tempo lock button — pin 2, dedicated input, LOW = pressed.
  //    Runs whether or not the clock is running: pressing while stopped
  //    with a retained tempo starts it freewheeling at that tempo.
  if ((long)(now - lastLockCheck) >= (long)LOCK_CHECK_INTERVAL) {
    lastLockCheck = now;
    bool pressed = (digitalRead(LOCK_PIN) == LOW);

    if (pressed && lockArmed) {
      lockArmed = false;
      if (beatPeriodUs > 0) {            // nothing to hold without a tempo
        tempoLocked = !tempoLocked;
        if (tempoLocked && !clockRunning)
          wasPaused ? continueClock(now) : startClock(now);
        noInterrupts();                       // gateLed is serviced by the ISR
        gateLed.fire(LED_PIN, now, LED_MS);
        interrupts();
      }
    } else if (!pressed) {
      lockArmed = true;                  // rearm on release
    }
  }

  // 7. OLED — lowest priority, yields to audio, degrades to sluggish.
  serviceOled(now);

  // 8. Debug. Remove for production.
  static unsigned long lastDebug = 0;
  if ((long)(now - lastDebug) >= 1000000L) {
    lastDebug = now;
    Serial.print("bpm=");    Serial.print(btt_get_tempo_bpm(btt), 2);
    Serial.print(" period="); Serial.print(beatPeriodUs);
    Serial.print(" bias=");  Serial.print((int)tempoBias);
    Serial.print(" run=");   Serial.print(clockRunning);
    Serial.print(" qmax=");  Serial.print(AudioMemoryUsageMax());
    Serial.print(" btt=");   Serial.print(bttBlocks ? bttUsAccum / bttBlocks : 0);
    Serial.print("avg/");    Serial.print(bttUsMax);
    Serial.print("pk of ");  Serial.print(US_PER_BLOCK);
    Serial.print("us drift="); Serial.print(lastDriftUs);
    Serial.print(" drops=");   Serial.print(dropEvents);
    Serial.print(" hold=");    Serial.print(tempoLocked);
    Serial.print(" late=");    Serial.print(tickLateMax);
    Serial.print(" floor=");   Serial.print(spanFloorHits);
    tickLateMax = 0;
    Serial.print(" rms=");     Serial.println(sigLevel, 3);
    bttUsMax = 0; bttUsAccum = 0; bttBlocks = 0;   // per-second, not since-boot

  }
}
