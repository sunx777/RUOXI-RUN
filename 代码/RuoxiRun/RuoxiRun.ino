#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <esp32-hal-psram.h>
#include <arduinoFFT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "wanwu_assets_v2.h"
#include "ruoxi_skin_assets_v52.h"
#include "ruoxi_pink_v54.h"
#include "ruoxi_ending_audio.h"

// RUOXI RUN 
// 目标：
// 1. 320x240 大屏，顶部 40px UI，下面 320x200 跑酷画面
// 2. 背景/云朵/多障碍物/同源完整角色皮肤动画
// 3. FFT 音频识别放到独立 FreeRTOS 任务，减少画面卡顿
// 4. 正确 -> 跳跃 + 星星；错误 -> 受击；超时 -> 摔倒；结束 -> 庆祝
// 5. UNKNOWN 不扣分、不换音符

// -------------------- TFT --------------------
#define TFT_CS    10
#define TFT_DC    4
#define TFT_RST   5
#define TFT_MOSI  11
#define TFT_SCLK  12
#define TFT_MISO  13

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// -------------------- Secondary rhythm TFT (ST7735S 1.8") --------------------

#define RHYTHM_TFT_CS   9
#define RHYTHM_TFT_DC   16
#define RHYTHM_TFT_RST  15
#define RHYTHM_W        128
#define RHYTHM_H        160
#define RHYTHM_FRAME_MS 70

Adafruit_ST7735 rhythmTft(RHYTHM_TFT_CS, RHYTHM_TFT_DC, RHYTHM_TFT_RST);
GFXcanvas16 rhythmCanvas(RHYTHM_W, RHYTHM_H);
bool rhythmTftReady = false;
uint32_t rhythmLastFrameMs = 0;
  
// -------------------- 12V WS2811 LED strip --------------------
// This strip has 3 physical LEDs per addressable pixel.
// 15 physical LEDs => 5 addressable pixels.
#define LED_PIN    14
#define LED_COUNT  5
#define LED_BRIGHTNESS 90

// Your strip was verified experimentally as RBG byte order.
Adafruit_NeoPixel ledStrip(LED_COUNT, LED_PIN, NEO_RBG + NEO_KHZ800);

enum LedFxType : uint8_t {
  LEDFX_IDLE,
  LEDFX_START,
  LEDFX_PERFECT,
  LEDFX_GREAT,
  LEDFX_GOOD,
  LEDFX_BAD,
  LEDFX_MISS,
  LEDFX_WIN,
  LEDFX_FAIL
};

LedFxType ledFx = LEDFX_IDLE;
uint32_t ledFxStartMs = 0;
uint32_t ledFxLastStepMs = 0;

void ledAll(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t c = ledStrip.Color(r, g, b);
  for (int i = 0; i < LED_COUNT; i++) ledStrip.setPixelColor(i, c);
  ledStrip.show();
}

void ledOff() {
  ledStrip.clear();
  ledStrip.show();
}

void startLedFx(LedFxType fx) {
  ledFx = fx;
  ledFxStartMs = millis();
  ledFxLastStepMs = 0;
}

void updateLedFx() {
  if (ledFx == LEDFX_IDLE) return;

  uint32_t now = millis();
  uint32_t e = now - ledFxStartMs;

  switch (ledFx) {
    case LEDFX_START: {
      // 5-pixel countdown/start chase: bright head + fading tail.
      if (e >= 1000) {
        ledOff();
        ledFx = LEDFX_IDLE;
        break;
      }
      int step = (e / 110) % LED_COUNT;
      ledStrip.clear();
      for (int i = 0; i < LED_COUNT; i++) {
        int d = step - i;
        if (d == 0) ledStrip.setPixelColor(i, ledStrip.Color(220, 255, 255));
        else if (d == 1) ledStrip.setPixelColor(i, ledStrip.Color(40, 120, 180));
        else if (d == 2) ledStrip.setPixelColor(i, ledStrip.Color(8, 30, 55));
      }
      ledStrip.show();
      break;
    }

    case LEDFX_PERFECT: {
      // Center-out green/white burst:  ..#.. -> .###. -> ##### -> white -> off
      if (e >= 360) {
        ledOff(); ledFx = LEDFX_IDLE; break;
      }
      ledStrip.clear();
      if (e < 80) {
        ledStrip.setPixelColor(2, ledStrip.Color(0, 255, 50));
      } else if (e < 160) {
        for (int i=1;i<=3;i++) ledStrip.setPixelColor(i, ledStrip.Color(0, 255, 50));
      } else if (e < 250) {
        for (int i=0;i<LED_COUNT;i++) ledStrip.setPixelColor(i, ledStrip.Color(0, 255, 50));
      } else {
        for (int i=0;i<LED_COUNT;i++) ledStrip.setPixelColor(i, ledStrip.Color(220, 255, 220));
      }
      ledStrip.show();
      break;
    }

    case LEDFX_GREAT: {
      // Blue sweep left -> right with a short tail.
      if (e >= 420) { ledOff(); ledFx = LEDFX_IDLE; break; }
      int step = min((int)(e / 75), LED_COUNT - 1);
      ledStrip.clear();
      ledStrip.setPixelColor(step, ledStrip.Color(20, 100, 255));
      if (step > 0) ledStrip.setPixelColor(step-1, ledStrip.Color(0, 25, 90));
      ledStrip.show();
      break;
    }

    case LEDFX_GOOD: {
      // Warm yellow breathing pulse.
      if (e >= 420) { ledOff(); ledFx = LEDFX_IDLE; break; }
      uint8_t v = (e < 210) ? map(e, 0, 209, 35, 220) : map(e, 210, 419, 220, 20);
      ledAll(v, (uint8_t)(v * 0.55f), 0);
      break;
    }

    case LEDFX_BAD: {
      // Red alternating sides.
      if (e >= 500) { ledOff(); ledFx = LEDFX_IDLE; break; }
      bool phase = ((e / 110) % 2) == 0;
      ledStrip.clear();
      if (phase) {
        ledStrip.setPixelColor(0, ledStrip.Color(255,0,0));
        ledStrip.setPixelColor(1, ledStrip.Color(120,0,0));
      } else {
        ledStrip.setPixelColor(3, ledStrip.Color(120,0,0));
        ledStrip.setPixelColor(4, ledStrip.Color(255,0,0));
      }
      ledStrip.show();
      break;
    }

    case LEDFX_MISS: {
      // Two unmistakable full red flashes.
      if (e >= 620) { ledOff(); ledFx = LEDFX_IDLE; break; }
      bool on = (e < 140) || (e >= 280 && e < 420);
      if (on) ledAll(255, 0, 0); else ledOff();
      break;
    }

    case LEDFX_WIN: {
      // 5-second celebration designed for only five addressable WS2811 groups.
      if (e >= 5000) {
        ledOff();
        ledFx = LEDFX_IDLE;
        break;
      }

      // Phase 1 (0-900 ms): reveal a clear five-color rainbow from left to right.
      if (e < 900) {
        static const uint8_t rainbow[5][3] = {
          {255, 0, 0}, {255, 120, 0}, {0, 255, 30}, {0, 90, 255}, {170, 0, 255}
        };
        int lit = min(LED_COUNT, 1 + (int)(e / 170));
        ledStrip.clear();
        for (int i=0; i<lit; i++) {
          ledStrip.setPixelColor(i, ledStrip.Color(rainbow[i][0], rainbow[i][1], rainbow[i][2]));
        }
        ledStrip.show();
        break;
      }

      // Phase 2 (900-3000 ms): rotate the five distinct colors slowly so movement is obvious.
      if (e < 3000) {
        static const uint8_t rainbow[5][3] = {
          {255, 0, 0}, {255, 120, 0}, {0, 255, 30}, {0, 90, 255}, {170, 0, 255}
        };
        int shift = ((e - 900) / 180) % LED_COUNT;
        for (int i=0; i<LED_COUNT; i++) {
          int cidx = (i - shift + LED_COUNT) % LED_COUNT;
          ledStrip.setPixelColor(i, ledStrip.Color(rainbow[cidx][0], rainbow[cidx][1], rainbow[cidx][2]));
        }
        ledStrip.show();
        break;
      }

      // Phase 3 (3000-4200 ms): colorful breathing, slow enough to read as celebration.
      if (e < 4200) {
        uint32_t q = e - 3000;
        uint8_t b = (q < 600) ? map(q,0,599,45,230) : map(q,600,1199,230,45);
        static const uint8_t rainbow[5][3] = {
          {255, 0, 0}, {255, 120, 0}, {0, 255, 30}, {0, 90, 255}, {170, 0, 255}
        };
        for (int i=0; i<LED_COUNT; i++) {
          ledStrip.setPixelColor(i, ledStrip.Color(
            (uint8_t)(rainbow[i][0] * b / 255),
            (uint8_t)(rainbow[i][1] * b / 255),
            (uint8_t)(rainbow[i][2] * b / 255)));
        }
        ledStrip.show();
        break;
      }

      // Phase 4 (4200-5000 ms): white-color-white finale.
      bool white = (((e - 4200) / 160) % 2) == 0;
      if (white) ledAll(230,230,230);
      else {
        static const uint8_t rainbow[5][3] = {
          {255, 0, 0}, {255, 120, 0}, {0, 255, 30}, {0, 90, 255}, {170, 0, 255}
        };
        for (int i=0; i<LED_COUNT; i++)
          ledStrip.setPixelColor(i, ledStrip.Color(rainbow[i][0], rainbow[i][1], rainbow[i][2]));
        ledStrip.show();
      }
      break;
    }

    case LEDFX_FAIL: {
      // Red closes in from the edges, then fades.
      if (e >= 1500) { ledOff(); ledFx = LEDFX_IDLE; break; }
      ledStrip.clear();
      if (e < 300) {
        ledStrip.setPixelColor(0, ledStrip.Color(220,0,0));
        ledStrip.setPixelColor(4, ledStrip.Color(220,0,0));
      } else if (e < 600) {
        for (int i : {0,1,3,4}) ledStrip.setPixelColor(i, ledStrip.Color(180,0,0));
      } else if (e < 900) {
        ledStrip.setPixelColor(2, ledStrip.Color(255,0,0));
      } else {
        uint8_t v = map(e,900,1499,180,0);
        ledAll(v,0,0);
        break;
      }
      ledStrip.show();
      break;
    }

    default:
      ledOff();
      ledFx = LEDFX_IDLE;
      break;
  }
}

// -------------------- FT6336U --------------------
#define TOUCH_SDA 18
#define TOUCH_SCL 17
#define TOUCH_RST 21
#define FT6336_ADDR 0x38

// -------------------- INMP441 --------------------
#define I2S_PORT I2S_NUM_0
#define I2S_SCK  6
#define I2S_WS   7
#define I2S_SD   8
#define I2S_SPK_DIN 3   // MAX98357A DIN

// Speaker SFX volume. 16-bit-equivalent amplitude; start conservative.
#define SFX_VOLUME 6500
#define SAMPLE_RATE 16000

// Result audio clips are 16 kHz mono PCM in ruoxi_ending_audio.h.
// Scale them here so prerecorded audio is not dramatically louder than the countdown tones.
#define ENDING_AUDIO_GAIN 0.55f

volatile bool endingAudioPlaying = false;
volatile bool endingAudioStopRequested = false;
TaskHandle_t endingAudioTaskHandle = nullptr;

// -------------------- FFT --------------------
#define SAMPLES 2048

float *vReal = nullptr;
float *vImag = nullptr;
ArduinoFFT<float> *FFT = nullptr;
int32_t audioBuffer[256];

// -------------------- Audio recognition thresholds --------------------
const float HIT_THRESHOLD = 1400.0f;
const float RELEASE_THRESHOLD = 750.0f;
const int RELEASE_COUNT_REQUIRED = 4;
const uint32_t MIN_HIT_INTERVAL = 280;
const uint32_t FORCE_REARM_MS = 380;
const uint32_t NEW_NOTE_GUARD_MS = 110;
const uint32_t NEW_NOTE_FORCE_ARM_MS = 230;
const float ADAPTIVE_RELEASE_RATIO = 0.30f;
const float ATTACK_OVER_ENVELOPE = 1.28f;
const float OCTAVE_CORRECTION_RATIO = 0.18f;

const int SKIP_MS = 18;
const float MIN_FREQ = 700.0f;
const float MAX_FREQ = 3400.0f;
const float MAX_FREQ_ERROR = 0.055f;
const float MIN_PEAK_AMPLITUDE = 70000.0f;

// RMS of the hit currently being analysed. Used by adaptive FFT peak gating.
float currentHitRmsForFFT = 0.0f;

// -------------------- Screen --------------------
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr int UI_H = 40;
constexpr int GAME_H = 200;

GFXcanvas16 gameCanvas(SCREEN_W, GAME_H);

// -------------------- Notes --------------------
constexpr int NOTE_COUNT = 15;

const float noteFreq[NOTE_COUNT] = {
  781.81, 880.92, 988.28,
  1040.81, 1168.09, 1313.85, 1390.02, 1554.14,
  1756.43, 1981.19,
  2093.07, 2339.25, 2641.83, 2788.41, 3129.63
};

const char *noteName[NOTE_COUNT] = {
  "L5","L6","L7",
  "1","2","3","4","5","6","7",
  "H1","H2","H3","H4","H5"
};

enum NoteId {
  NOTE_LOW5=0, NOTE_LOW6, NOTE_LOW7,
  NOTE_1, NOTE_2, NOTE_3, NOTE_4, NOTE_5, NOTE_6, NOTE_7,
  NOTE_H1, NOTE_H2, NOTE_H3, NOTE_H4, NOTE_H5
};

// -------------------- Songs / Difficulty / Unlocks --------------------
struct SongDef {
  const char *name;
  const uint8_t *notes;
  const uint8_t *units;
  uint16_t len;
  uint16_t unlockCost;   // 0 = free
};

// Two Tigers
const uint8_t SONG_TIGERS_NOTES[] = {
  NOTE_1,NOTE_2,NOTE_3,NOTE_1, NOTE_1,NOTE_2,NOTE_3,NOTE_1,
  NOTE_3,NOTE_4,NOTE_5, NOTE_3,NOTE_4,NOTE_5,
  NOTE_5,NOTE_6,NOTE_5,NOTE_4,NOTE_3,NOTE_1,
  NOTE_5,NOTE_6,NOTE_5,NOTE_4,NOTE_3,NOTE_1,
  NOTE_1,NOTE_LOW5,NOTE_1, NOTE_1,NOTE_LOW5,NOTE_1
};
const uint8_t SONG_TIGERS_UNITS[] = {
  2,2,2,2, 2,2,2,2,
  2,2,4, 2,2,4,
  1,1,1,1,2,2,
  1,1,1,1,2,2,
  2,2,4, 2,2,4
};

// Twinkle Twinkle Little Star
const uint8_t SONG_STAR_NOTES[] = {
  NOTE_1,NOTE_1,NOTE_5,NOTE_5,NOTE_6,NOTE_6,NOTE_5,
  NOTE_4,NOTE_4,NOTE_3,NOTE_3,NOTE_2,NOTE_2,NOTE_1,
  NOTE_5,NOTE_5,NOTE_4,NOTE_4,NOTE_3,NOTE_3,NOTE_2,
  NOTE_5,NOTE_5,NOTE_4,NOTE_4,NOTE_3,NOTE_3,NOTE_2,
  NOTE_1,NOTE_1,NOTE_5,NOTE_5,NOTE_6,NOTE_6,NOTE_5,
  NOTE_4,NOTE_4,NOTE_3,NOTE_3,NOTE_2,NOTE_2,NOTE_1
};
const uint8_t SONG_STAR_UNITS[] = {
  2,2,2,2,2,2,4,
  2,2,2,2,2,2,4,
  2,2,2,2,2,2,4,
  2,2,2,2,2,2,4,
  2,2,2,2,2,2,4,
  2,2,2,2,2,2,4
};

// Ode to Joy (simple opening)
const uint8_t SONG_ODE_NOTES[] = {
  NOTE_3,NOTE_3,NOTE_4,NOTE_5, NOTE_5,NOTE_4,NOTE_3,NOTE_2,
  NOTE_1,NOTE_1,NOTE_2,NOTE_3, NOTE_3,NOTE_2,NOTE_2,
  NOTE_3,NOTE_3,NOTE_4,NOTE_5, NOTE_5,NOTE_4,NOTE_3,NOTE_2,
  NOTE_1,NOTE_1,NOTE_2,NOTE_3, NOTE_2,NOTE_1,NOTE_1
};
const uint8_t SONG_ODE_UNITS[] = {
  2,2,2,2, 2,2,2,2,
  2,2,2,2, 3,1,4,
  2,2,2,2, 2,2,2,2,
  2,2,2,2, 3,1,4
};

// Jingle Bells - public-domain melody excerpt
const uint8_t SONG_JINGLE_NOTES[] = {
  NOTE_3,NOTE_3,NOTE_3,
  NOTE_3,NOTE_3,NOTE_3,
  NOTE_3,NOTE_5,NOTE_1,NOTE_2,NOTE_3,
  NOTE_4,NOTE_4,NOTE_4,NOTE_4,
  NOTE_4,NOTE_3,NOTE_3,NOTE_3,NOTE_3,
  NOTE_3,NOTE_2,NOTE_2,NOTE_3,NOTE_2,NOTE_5
};
const uint8_t SONG_JINGLE_UNITS[] = {
  2,2,4, 2,2,4,
  2,2,3,1,4,
  2,2,3,1,
  2,2,2,1,1,
  2,2,2,2,4
};

// Happy Birthday - public-domain melody excerpt
const uint8_t SONG_BDAY_NOTES[] = {
  NOTE_LOW5,NOTE_LOW5,NOTE_1,NOTE_LOW5,NOTE_3,NOTE_2,
  NOTE_LOW5,NOTE_LOW5,NOTE_1,NOTE_LOW5,NOTE_4,NOTE_3,
  NOTE_LOW5,NOTE_LOW5,NOTE_5,NOTE_3,NOTE_2,NOTE_1,
  NOTE_4,NOTE_4,NOTE_3,NOTE_1,NOTE_2,NOTE_1
};
const uint8_t SONG_BDAY_UNITS[] = {
  1,1,2,2,2,4,
  1,1,2,2,2,4,
  1,1,2,2,2,2,
  1,1,2,2,2,4
};

// Qi Feng Le / 起风了 - excerpt transcribed from the user-provided numbered score.
// H1/H2 correspond to the dotted high-octave 1/2 in the score.
const uint8_t SONG_QIFENG_NOTES[] = {
  NOTE_1,NOTE_2,NOTE_3,NOTE_1,NOTE_6,NOTE_5,NOTE_6,
  NOTE_1,NOTE_7,NOTE_6,NOTE_7,
  NOTE_7,NOTE_6,NOTE_7,NOTE_3,NOTE_H1,NOTE_H2,NOTE_H1,NOTE_7,NOTE_6,
  NOTE_5,NOTE_6,NOTE_5,NOTE_6,NOTE_5,NOTE_6,NOTE_5,NOTE_6,NOTE_5,NOTE_2,NOTE_5,NOTE_3,
  NOTE_1,NOTE_2,NOTE_3,NOTE_1,NOTE_6,NOTE_5,NOTE_6,
  NOTE_1,NOTE_7,NOTE_6,NOTE_7,
  NOTE_7,NOTE_6,NOTE_7,NOTE_3,NOTE_H1,NOTE_H2,NOTE_H1,NOTE_7,NOTE_6,
  NOTE_5,NOTE_6,NOTE_3,NOTE_3,NOTE_5,NOTE_6,NOTE_3,NOTE_3,NOTE_5,NOTE_6
};
const uint8_t SONG_QIFENG_UNITS[] = {
  1,1,1,1,1,1,2,
  1,1,1,2,
  1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,2,
  1,1,1,2,
  1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1,1,2
};

// Qing Hua Ci / 青花瓷 - excerpt transcribed from the user-provided numbered score.
const uint8_t SONG_QINGHUA_NOTES[] = {
  NOTE_5,NOTE_5,NOTE_3,NOTE_2,NOTE_3,NOTE_6,NOTE_2,NOTE_3,NOTE_5,NOTE_3,NOTE_2,
  NOTE_5,NOTE_5,NOTE_3,NOTE_2,NOTE_3,NOTE_5,NOTE_2,NOTE_3,NOTE_5,NOTE_2,NOTE_1,
  NOTE_1,NOTE_2,NOTE_3,NOTE_5,NOTE_6,NOTE_5,NOTE_3,NOTE_5,NOTE_3,NOTE_3,NOTE_2,NOTE_2,
  NOTE_1,NOTE_2,NOTE_1,NOTE_2,NOTE_1,NOTE_2,NOTE_3,NOTE_5,NOTE_3,
  NOTE_5,NOTE_5,NOTE_3,NOTE_2,NOTE_3,NOTE_6,NOTE_2,NOTE_3,NOTE_5,NOTE_3,NOTE_2,
  NOTE_5,NOTE_5,NOTE_3,NOTE_2,NOTE_3,NOTE_5,NOTE_2,NOTE_3,NOTE_5,NOTE_2,NOTE_1,
  NOTE_1,NOTE_2,NOTE_3,NOTE_5,NOTE_6,NOTE_5,NOTE_3,NOTE_5,NOTE_3,NOTE_3,NOTE_2,NOTE_2,
  NOTE_5,NOTE_3,NOTE_2,NOTE_2,NOTE_1
};
const uint8_t SONG_QINGHUA_UNITS[] = {
  1,1,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1,1,1,1,2,
  1,1,1,1,3
};

// Qing Tian / 晴天 - excerpt transcribed from the user-provided numbered score.
// LOW5/LOW7 follow the under-dotted notes visible in the score.
const uint8_t SONG_QINGTIAN_NOTES[] = {
  NOTE_5,NOTE_5,NOTE_1,NOTE_1,NOTE_2,NOTE_3, NOTE_5,NOTE_5,NOTE_1,NOTE_1,NOTE_2,NOTE_3,NOTE_2,NOTE_1,NOTE_LOW5,
  NOTE_LOW5,NOTE_5,NOTE_1,NOTE_1,NOTE_2,NOTE_3, NOTE_3,NOTE_5,NOTE_3,NOTE_4,NOTE_3,NOTE_2,NOTE_4,NOTE_3,NOTE_2,NOTE_1,
  NOTE_LOW5,NOTE_1,NOTE_1,NOTE_3,NOTE_4,NOTE_3,NOTE_2, NOTE_1,NOTE_2,NOTE_3,NOTE_3,NOTE_3,NOTE_3,NOTE_2,NOTE_3,NOTE_2,NOTE_1,
  NOTE_LOW5,NOTE_1,NOTE_1,NOTE_3,NOTE_4,NOTE_3,NOTE_2,NOTE_1, NOTE_2,NOTE_3,NOTE_3,NOTE_3,NOTE_3,NOTE_2,NOTE_3,NOTE_2,NOTE_1,NOTE_1,
  NOTE_LOW7,NOTE_1,NOTE_1,NOTE_1,NOTE_1,NOTE_LOW7,NOTE_1,NOTE_1, NOTE_1,NOTE_1,NOTE_1,NOTE_LOW7,NOTE_1,NOTE_1,
  NOTE_1,NOTE_1,NOTE_1,NOTE_LOW7,NOTE_1,NOTE_1, NOTE_1,NOTE_1,NOTE_1,NOTE_LOW7,NOTE_LOW5,NOTE_LOW5,NOTE_LOW5
};
const uint8_t SONG_QINGTIAN_UNITS[] = {
  1,1,1,1,1,1, 1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1, 1,1,1,1,1,2,
  1,1,1,1,1,1, 1,1,1,1,1,1,3
};

// -------------------- CREATE MODE / user songs --------------------
constexpr uint16_t CUSTOM_MAX_NOTES = 64;
constexpr uint8_t CUSTOM_SONG_SLOTS = 3;
uint8_t customSongNotes[CUSTOM_SONG_SLOTS][CUSTOM_MAX_NOTES] = {{0}};
uint8_t customSongUnits[CUSTOM_SONG_SLOTS][CUSTOM_MAX_NOTES] = {{1}};
uint16_t customSongIntervalsMs[CUSTOM_SONG_SLOTS][CUSTOM_MAX_NOTES] = {{500}};
uint16_t customSongLen[CUSTOM_SONG_SLOTS] = {0,0,0};

// Recording uses a temporary buffer, so BACK/UNDO never damages a saved song.
uint8_t recordNotes[CUSTOM_MAX_NOTES] = {0};
uint32_t recordTimesMs[CUSTOM_MAX_NOTES] = {0};
uint16_t recordCount = 0;
uint32_t recordFirstHitMs = 0;
uint32_t recordingGeneration = 0;
bool customSongDirty = false;
bool deleteConfirmArmed = false;
uint32_t deleteConfirmUntil = 0;
uint8_t selectedCreateSlot = 0;

SongDef SONGS[] = {
  {"Two Tigers",     SONG_TIGERS_NOTES,  SONG_TIGERS_UNITS,  (uint16_t)(sizeof(SONG_TIGERS_NOTES)/sizeof(uint8_t)),  0},
  {"Twinkle Star",   SONG_STAR_NOTES,    SONG_STAR_UNITS,    (uint16_t)(sizeof(SONG_STAR_NOTES)/sizeof(uint8_t)),    0},
  {"Ode to Joy",     SONG_ODE_NOTES,     SONG_ODE_UNITS,     (uint16_t)(sizeof(SONG_ODE_NOTES)/sizeof(uint8_t)),     0},
  {"Jingle Bells",   SONG_JINGLE_NOTES,  SONG_JINGLE_UNITS,  (uint16_t)(sizeof(SONG_JINGLE_NOTES)/sizeof(uint8_t)),  500},
  {"Happy Birthday", SONG_BDAY_NOTES,    SONG_BDAY_UNITS,    (uint16_t)(sizeof(SONG_BDAY_NOTES)/sizeof(uint8_t)),    800},
  {"Qi Feng Le",     SONG_QIFENG_NOTES,  SONG_QIFENG_UNITS,  (uint16_t)(sizeof(SONG_QIFENG_NOTES)/sizeof(uint8_t)),  1000},
  {"Qing Hua Ci",    SONG_QINGHUA_NOTES, SONG_QINGHUA_UNITS, (uint16_t)(sizeof(SONG_QINGHUA_NOTES)/sizeof(uint8_t)), 1200},
  {"Qing Tian",      SONG_QINGTIAN_NOTES,SONG_QINGTIAN_UNITS,(uint16_t)(sizeof(SONG_QINGTIAN_NOTES)/sizeof(uint8_t)),1500},
  {"My Song 1",      customSongNotes[0], customSongUnits[0], 0, 0},
  {"My Song 2",      customSongNotes[1], customSongUnits[1], 0, 0},
  {"My Song 3",      customSongNotes[2], customSongUnits[2], 0, 0}
};
constexpr int SONG_COUNT = sizeof(SONGS) / sizeof(SONGS[0]);
constexpr int CUSTOM_SONG_FIRST_INDEX = SONG_COUNT - CUSTOM_SONG_SLOTS;
inline bool isCustomSongIndex(int i) { return i >= CUSTOM_SONG_FIRST_INDEX && i < SONG_COUNT; }
inline int customSlotFromSongIndex(int i) { return i - CUSTOM_SONG_FIRST_INDEX; }
inline int customSongIndexFromSlot(int slot) { return CUSTOM_SONG_FIRST_INDEX + slot; }

enum Difficulty { DIFF_EASY, DIFF_NORMAL, DIFF_HARD };
const char *DIFF_NAME[] = {"EASY","NORMAL","HARD"};
// 单位节拍时长：难度只改变节奏，不降低音高识别精度
const uint32_t DIFF_UNIT_MS[] = {750, 500, 360};
const uint32_t DIFF_GRACE_MS[] = {500, 340, 250};

int selectedSong = 0;
constexpr int SONGS_PER_PAGE = 4;
int songPage = 0;
Difficulty difficulty = DIFF_NORMAL;
const SongDef *activeSong = &SONGS[0];
uint16_t songIndex = 0;

inline uint8_t targetNote() { return activeSong->notes[songIndex]; }
inline uint16_t songLength() { return activeSong->len; }

// -------------------- Game / animation --------------------
enum AppState {
  APP_HOME,
  APP_SONG_SELECT,
  APP_DIFFICULTY,
  APP_PLAYING,
  APP_RESULT,
  APP_CALIBRATION,
  APP_WARDROBE,
  APP_CREATE,
  APP_RECORDING
};

enum AnimState : uint8_t {
  ANIM_RUN,
  ANIM_JUMP,
  ANIM_HIT,
  ANIM_FALL,
  ANIM_CHEER
};

enum JudgeResult : uint8_t {
  JUDGE_NONE,
  JUDGE_PERFECT,
  JUDGE_GREAT,
  JUDGE_GOOD,
  JUDGE_BAD,
  JUDGE_MISS
};

// Explicit prototypes prevent Arduino's auto-prototyper from emitting
// prototypes before the enum type declarations.
const Sprite565 *getFrames(uint8_t state, uint8_t &count);
void setAnimState(uint8_t state);
uint8_t judgeCorrectHit(uint32_t hitTime);

AppState appState = APP_HOME;
AnimState animState = ANIM_RUN;
JudgeResult judgeResult = JUDGE_NONE;

constexpr int PLAYER_X = 32;
constexpr int PLAYER_GROUND_Y = 178;
constexpr int JUDGE_X = 126;

uint8_t animFrame = 0;
uint32_t lastAnimFrameTime = 0;
uint32_t animStateStartTime = 0;

constexpr uint32_t RUN_FRAME_MS = 105;
constexpr uint32_t JUMP_FRAME_MS = 125;
constexpr uint32_t HIT_FRAME_MS = 150;
constexpr uint32_t FALL_FRAME_MS = 180;
constexpr uint32_t CHEER_FRAME_MS = 170;

constexpr uint32_t JUMP_TOTAL_MS = 650;
constexpr uint32_t HIT_TOTAL_MS = 520;
constexpr uint32_t FALL_TOTAL_MS = 760;
constexpr uint32_t RESULT_HOLD_MS = 420;

int score = 0;
int maxPossibleScore = 0;
int combo = 0;
int maxCombo = 0;
int lifeHalfUnits = 6;  // 3颗心，每次 BAD/MISS 扣半颗
bool gameFailed = false;
uint32_t hitIdealTime = 0;
int lastTimingError = 0;

// 长期成长数据（NVS）
Preferences prefs;
uint32_t coins = 0;
uint32_t totalPlays = 0;
String finalRank = "C";

// Song unlocks: first 3 are free by default.
uint32_t songUnlockedMask = 0x07;
uint16_t bestScores[SONG_COUNT] = {0};

// Wardrobe / cosmetic categories
// Each category is independent, so the player can equip one cap + one clothes + one shoes + one accessory.
enum OutfitCategory : uint8_t {
  CAT_CAP = 0,
  CAT_CLOTHES,
  CAT_SHOES,
  CAT_ACCESSORIES,
  CAT_COUNT
};

struct OutfitDef {
  const char *name;
  uint16_t cost;
  const char *desc;
  uint8_t category;
};

const char *CATEGORY_NAMES[CAT_COUNT] = {"CAP", "CLOTHES", "SHOES", "ACCESSORIES"};

const OutfitDef OUTFITS[] = {
  // CAP - 6 items
  {"SUNNY CAP",    300, "Rabbit day cap",        CAT_CAP},
  {"BLACK RX",     420, "Street rhythm cap",     CAT_CAP},
  {"PINK HEART",   380, "Sweet heart cap",       CAT_CAP},
  {"CAT EARS",     520, "Music cat ears",        CAT_CAP},
  {"CREAM BERET",  450, "Cafe style beret",     CAT_CAP},
  {"STAR CAT",     900, "Secret galaxy headwear",CAT_CAP},

  // CLOTHES - 6 items
  {"VARSITY",      550, "Red-white jacket",      CAT_CLOTHES},
  {"HEART TEE",    360, "Pixel heart shirt",     CAT_CLOTHES},
  {"MUSIC HOODIE", 620, "Black-pink hoodie",     CAT_CLOTHES},
  {"SAILOR TOP",   680, "Blue ribbon top",       CAT_CLOTHES},
  {"MINT CARDI",   720, "Soft mint cardigan",    CAT_CLOTHES},
  {"NEON HOODIE",  980, "Galaxy neon hoodie",    CAT_CLOTHES},

  // SHOES - 6 items
  {"RED HIGH",     420, "Rabbit high-tops",      CAT_SHOES},
  {"LILAC RUN",    450, "Pastel sneakers",       CAT_SHOES},
  {"BLACK BOOTS",  520, "Heart strap boots",     CAT_SHOES},
  {"CLOUD WALK",   580, "Cloud-white runners",   CAT_SHOES},
  {"PINK SKATE",   640, "Pink chunky sneakers",  CAT_SHOES},
  {"MOON STEP",    920, "Galaxy light shoes",    CAT_SHOES},

  // ACCESSORIES - 6 items
  {"PINK BOW",     240, "Ribbon accessory",      CAT_ACCESSORIES},
  {"HEART GLASS",  320, "Heart sunglasses",      CAT_ACCESSORIES},
  {"PEARL PIN",    280, "Pearl hair pin",        CAT_ACCESSORIES},
  {"MUSIC BAG",    460, "Tiny note shoulder bag",CAT_ACCESSORIES},
  {"BUNNY BADGE",  400, "Lucky rabbit badge",    CAT_ACCESSORIES},
  {"STAR CHARM",   860, "Galaxy lucky charm",    CAT_ACCESSORIES}
};
constexpr int OUTFIT_COUNT = sizeof(OUTFITS) / sizeof(OUTFITS[0]);

// Ownership bitmask for all cosmetic items. Default starts with nothing purchased.
uint32_t outfitOwnedMask = 0x00000000UL;
// One equipped item index per category; -1 means none.
int8_t equippedByCategory[CAT_COUNT] = {-1,-1,-1,-1};
uint8_t wardrobeCategory = CAT_CAP;
uint8_t wardrobePage = 0;
int wardrobeSelection = 0;
bool wardrobeModalVisible = false;
int wardrobePendingIndex = -1;
String uiMessage = "";

inline bool outfitOwned(int i) {
  return i >= 0 && i < OUTFIT_COUNT && ((outfitOwnedMask >> i) & 1U);
}

int firstItemInCategory(uint8_t cat) {
  for (int i=0;i<OUTFIT_COUNT;i++) if (OUTFITS[i].category == cat) return i;
  return 0;
}

int nthItemInCategory(uint8_t cat, int n) {
  int seen = 0;
  for (int i=0;i<OUTFIT_COUNT;i++) {
    if (OUTFITS[i].category != cat) continue;
    if (seen == n) return i;
    seen++;
  }
  return -1;
}

int countItemsInCategory(uint8_t cat) {
  int c=0;
  for (int i=0;i<OUTFIT_COUNT;i++) if (OUTFITS[i].category == cat) c++;
  return c;
}

bool outfitEquipped(int i) {
  if (i < 0 || i >= OUTFIT_COUNT) return false;
  uint8_t cat = OUTFITS[i].category;
  return equippedByCategory[cat] == i;
}

// Surprise full-set bonus: last item of every category forms the GALAXY set.
bool galaxySetEquipped() {
  return equippedByCategory[CAT_CAP] == nthItemInCategory(CAT_CAP,5) &&
         equippedByCategory[CAT_CLOTHES] == nthItemInCategory(CAT_CLOTHES,5) &&
         equippedByCategory[CAT_SHOES] == nthItemInCategory(CAT_SHOES,5) &&
         equippedByCategory[CAT_ACCESSORIES] == nthItemInCategory(CAT_ACCESSORIES,5);
}

inline bool songUnlocked(int i) {
  if (isCustomSongIndex(i)) {
    int slot = customSlotFromSongIndex(i);
    return slot >= 0 && slot < CUSTOM_SONG_SLOTS && customSongLen[slot] > 0;
  }
  return i >= 0 && i < CUSTOM_SONG_FIRST_INDEX && ((songUnlockedMask >> i) & 1U);
}

// ============================================================
// V5 complete character skin system
// One complete outfit is bought/equipped as a unit.
// ============================================================
struct CharacterSkinDef {
  const char *name;
  uint16_t cost;
  const char *desc;
};

const CharacterSkinDef CHARACTER_SKINS[] = {
  {"SCHOOL GIRL",      0,   "Classic navy school uniform"},
  {"BLUE WORKER",      220, "Bright blue work-day uniform"},
  {"TEACHER",          520, "Smart brown teacher suit"},
  {"FLIGHT ATTENDANT", 800, "Navy cabin-crew uniform"},
  {"DOCTOR",           680, "White coat with stethoscope"},
  {"PINK MUSIC GIRL",  650, "Pink outfit with music bag"}
};
constexpr int CHARACTER_SKIN_COUNT = sizeof(CHARACTER_SKINS)/sizeof(CHARACTER_SKINS[0]);

uint32_t skinOwnedMask = 0x01UL; // School Girl is free
uint8_t equippedSkin = 0;
uint8_t wardrobeSkin = 0;
bool skinBuyConfirm = false;

inline bool skinOwned(int i) {
  return i >= 0 && i < CHARACTER_SKIN_COUNT && ((skinOwnedMask >> i) & 1U);
}

void saveSkinProgress() {
  prefs.putUInt("skinMask", skinOwnedMask);
  prefs.putUChar("skinEq", equippedSkin);
}

// 校准页数据
int calibrationNote = -1;
float calibrationFreq = 0.0f;
float calibrationRms = 0.0f;
bool calibrationValueDirty = true;
uint32_t calibrationLastHit = 0;

bool noteResolved = false;
uint32_t noteStartTime = 0;
uint32_t noteWindowMs = 0;
uint32_t resolveUntil = 0;

uint32_t lastRenderTime = 0;
bool resultRendered = false;
constexpr uint32_t FRAME_MS = 40;  // ~25fps

// -------------------- Audio task queue --------------------
struct DetectionEvent {
  int note;
  float freq;
  float rms;
  uint32_t hitMs;  // true strike trigger time; FFT latency is excluded from timing judge
  uint32_t generation; // which note/calibration gate produced this event
};

QueueHandle_t detectionQueue = nullptr;
volatile bool audioEnabled = false;
volatile bool audioReadyForHit = true;
volatile bool audioProcessing = false;
// 每个音符拥有独立的“接收窗口”。判定完成后立即关门，避免上一击余振/二次触发串到下一拍。
volatile bool audioAcceptHits = false;
volatile bool audioNewNoteRequest = false;
volatile uint32_t audioGeneration = 0;
uint32_t currentNoteGeneration = 0;
uint32_t calibrationGeneration = 0;

// V5.6 stability: game tells the audio recognizer which pitch is currently expected.
// This is used only as secondary evidence so a ringing previous bar does not dominate the next hit.
volatile int expectedTargetNote = -1;
int wrongStrikeCount = 0;
int lastWrongNote = -1;

// -------------------- Clouds --------------------
struct Cloud {
  const Sprite565 *sp;
  float x;
  int y;
  float speed;
};

Cloud clouds[3];

// -------------------- Obstacle --------------------
struct Obstacle {
  const Sprite565 *sp;
  float x;
  int y;
};

Obstacle obstacle;

const Sprite565 *obstaclePool[] = {
  &OBS_CONE,
  &OBS_CRATE,
  &OBS_BARRIER,
  &OBS_BLOCK,
  &OBS_SPRING
};

constexpr int OBSTACLE_COUNT =
  sizeof(obstaclePool) / sizeof(obstaclePool[0]);

// -------------------- Particles --------------------
struct Particle {
  bool active;
  float x, y;
  float vx, vy;
  uint16_t color;
  uint8_t life;
  uint8_t radius;
};

constexpr int MAX_PARTICLES = 20;
Particle particles[MAX_PARTICLES];

// ============================================================
// Memory
// ============================================================
void printMemory(const char *tag) {
  Serial.println();
  Serial.print("[MEM] ");
  Serial.println(tag);
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());
  Serial.print("Free PSRAM: ");
  Serial.println(ESP.getFreePsram());
}

// ============================================================
// Touch
// ============================================================
uint8_t readTouchReg(uint8_t reg) {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return 0xFF;
  }

  Wire.requestFrom(FT6336_ADDR, (uint8_t)1);

  if (Wire.available()) {
    return Wire.read();
  }

  return 0xFF;
}

bool isTouched() {
  uint8_t v = readTouchReg(0x02);
  if (v == 0xFF) return false;

  uint8_t n = v & 0x0F;
  return n >= 1 && n <= 2;
}

// FT6336U 第一个触点。当前屏幕 setRotation(1)，默认采用常见的 240x320 触摸坐标映射。
// 如果你发现左右/上下反了，只需要改下面 3 个常量，不动菜单逻辑。
constexpr bool TOUCH_SWAP_XY = true;
constexpr bool TOUCH_FLIP_X = true;
constexpr bool TOUCH_FLIP_Y = false;

bool readTouchPoint(int &sx, int &sy) {
  uint8_t n = readTouchReg(0x02);
  if (n == 0xFF || (n & 0x0F) == 0) return false;

  uint8_t xh = readTouchReg(0x03);
  uint8_t xl = readTouchReg(0x04);
  uint8_t yh = readTouchReg(0x05);
  uint8_t yl = readTouchReg(0x06);
  if (xh == 0xFF || xl == 0xFF || yh == 0xFF || yl == 0xFF) return false;

  int rx = ((xh & 0x0F) << 8) | xl;
  int ry = ((yh & 0x0F) << 8) | yl;

  int x = TOUCH_SWAP_XY ? ry : rx;
  int y = TOUCH_SWAP_XY ? rx : ry;
  int maxX = TOUCH_SWAP_XY ? 320 : 240;
  int maxY = TOUCH_SWAP_XY ? 240 : 320;

  if (TOUCH_FLIP_X) x = maxX - 1 - x;
  if (TOUCH_FLIP_Y) y = maxY - 1 - y;

  sx = constrain(x, 0, 319);
  sy = constrain(y, 0, 239);
  return true;
}

bool touchReleased() {
  static bool wasDown = false;
  bool down = isTouched();
  bool released = wasDown && !down;
  wasDown = down;
  return released;
}

void initTouch() {
  pinMode(TOUCH_RST, OUTPUT);

  digitalWrite(TOUCH_RST, LOW);
  delay(10);

  digitalWrite(TOUCH_RST, HIGH);
  delay(300);

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  Wire.beginTransmission(FT6336_ADDR);

  if (Wire.endTransmission() == 0) {
    Serial.println("FT6336U OK");
  } else {
    Serial.println("FT6336U NOT FOUND");
  }
}

// ============================================================
// FFT / I2S
// ============================================================
bool initFFT() {
  Serial.println("Checking PSRAM...");

  if (!psramFound()) {
    Serial.println("ERROR: PSRAM NOT FOUND");
    return false;
  }

  Serial.print("PSRAM size: ");
  Serial.println(ESP.getPsramSize());

  vReal = (float*)ps_malloc(SAMPLES * sizeof(float));
  vImag = (float*)ps_malloc(SAMPLES * sizeof(float));

  if (!vReal || !vImag) {
    Serial.println("ERROR: FFT PSRAM allocation failed");
    return false;
  }

  FFT = new ArduinoFFT<float>(
    vReal,
    vImag,
    SAMPLES,
    SAMPLE_RATE
  );

  if (!FFT) {
    Serial.println("ERROR: ArduinoFFT object allocation failed");
    return false;
  }

  Serial.println("FFT buffers allocated in PSRAM.");
  return true;
}

bool initI2S() {
  Serial.println("Initializing INMP441...");

  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pins = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num = I2S_SD
  };

  if (i2s_driver_install(I2S_PORT, &config, 0, nullptr) != ESP_OK) {
    Serial.println("I2S driver install failed");
    return false;
  }

  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    Serial.println("I2S pin setup failed");
    return false;
  }

  i2s_zero_dma_buffer(I2S_PORT);

  Serial.println("INMP441 + MAX98357A I2S OK");
  return true;
}

// ============================================================
// MAX98357A short game SFX
// Uses the SAME I2S peripheral/clock as the INMP441.
// I2S is configured as 32-bit because the microphone needs it;
// the 16-bit sine sample is shifted into the high 16 bits for TX.
// ============================================================
void speakerSilence(uint16_t ms) {
  i2s_zero_dma_buffer(I2S_PORT);
  delay(ms);
}

void speakerTone(float freqHz, uint16_t durationMs, int volume = SFX_VOLUME) {
  const int N = 128;
  int32_t buf[N];
  float phase = 0.0f;
  const float step = 2.0f * PI * freqHz / (float)SAMPLE_RATE;
  const uint32_t endAt = millis() + durationMs;

  while ((int32_t)(endAt - millis()) > 0) {
    for (int i = 0; i < N; ++i) {
      int16_t s16 = (int16_t)(sinf(phase) * volume);
      buf[i] = ((int32_t)s16) << 16;
      phase += step;
      if (phase >= 2.0f * PI) phase -= 2.0f * PI;
    }
    size_t written = 0;
    i2s_write(I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);
  }
  i2s_zero_dma_buffer(I2S_PORT);
}

void playCountdownSfx(int k) {
  // 3, 2, 1, GO! -> rising notes
  static const float f[4] = {523.25f, 659.25f, 783.99f, 1046.50f};
  speakerTone(f[k], k == 3 ? 230 : 140, k == 3 ? 7800 : SFX_VOLUME);
}

// ------------------------------------------------------------
// Prerecorded random result audio
// - Failure: random one of FAIL_01 / FAIL_02
// - Victory: random one of WIN_01 / WIN_02
// Runs in its own task so the result screen/LED animation stays responsive.
// ------------------------------------------------------------
void stopEndingAudio() {
  if (!endingAudioPlaying) return;

  endingAudioStopRequested = true;
  uint32_t t0 = millis();
  while (endingAudioPlaying && millis() - t0 < 120) {
    delay(1);
  }
  i2s_zero_dma_buffer(I2S_PORT);
}

void playPcmClip(const RuoxiPcmClip &clip) {
  const int N = 128;
  int32_t out[N];
  uint32_t pos = 0;

  while (pos < clip.samples && !endingAudioStopRequested) {
    int count = min((uint32_t)N, clip.samples - pos);

    for (int i = 0; i < count; ++i) {
      int16_t raw = (int16_t)pgm_read_word(&clip.data[pos + i]);
      int32_t scaled = (int32_t)((float)raw * ENDING_AUDIO_GAIN);
      scaled = constrain(scaled, -32768, 32767);
      out[i] = scaled << 16;
    }

    size_t written = 0;
    i2s_write(I2S_PORT, out, count * sizeof(int32_t), &written, portMAX_DELAY);
    pos += count;
  }

  i2s_zero_dma_buffer(I2S_PORT);
}

struct EndingAudioJob {
  bool failed;
  uint8_t index;
};

void endingAudioTask(void *arg) {
  EndingAudioJob job = *(EndingAudioJob*)arg;
  delete (EndingAudioJob*)arg;

  Serial.printf("[ENDING AUDIO] %s clip %u\n",
                job.failed ? "FAIL" : "WIN",
                (unsigned)(job.index + 1));

  if (job.failed) {
    playPcmClip(RUOXI_FAIL_CLIPS[job.index]);
  } else {
    playPcmClip(RUOXI_WIN_CLIPS[job.index]);
  }

  endingAudioPlaying = false;
  endingAudioTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void startRandomEndingAudio(bool failed) {
  stopEndingAudio();

  EndingAudioJob *job = new EndingAudioJob;
  if (!job) {
    // Low-memory fallback: keep a short synthesized jingle.
    if (failed) {
      speakerTone(659.25f, 110, 6000);
      speakerSilence(35);
      speakerTone(523.25f, 110, 6000);
      speakerSilence(35);
      speakerTone(392.00f, 220, 6200);
    } else {
      speakerTone(659.25f, 90, 6500);
      speakerSilence(35);
      speakerTone(783.99f, 90, 6800);
      speakerSilence(35);
      speakerTone(1046.50f, 210, 8000);
    }
    return;
  }

  job->failed = failed;
  job->index = (uint8_t)random(0, 2);   // true random 2-way ending choice

  endingAudioStopRequested = false;
  endingAudioPlaying = true;

  BaseType_t ok = xTaskCreatePinnedToCore(
    endingAudioTask,
    "ending_audio",
    4096,
    job,
    1,
    &endingAudioTaskHandle,
    0
  );

  if (ok != pdPASS) {
    delete job;
    endingAudioTaskHandle = nullptr;
    endingAudioPlaying = false;
  }
}

// Keep the old call names so the game-flow code stays simple.
void playWinSfx()  { startRandomEndingAudio(false); }
void playFailSfx() { startRandomEndingAudio(true); }

float getRMS() {
  size_t bytesRead = 0;

  esp_err_t err = i2s_read(
    I2S_PORT,
    audioBuffer,
    sizeof(audioBuffer),
    &bytesRead,
    portMAX_DELAY
  );

  if (err != ESP_OK || bytesRead == 0) {
    return 0;
  }

  int count = bytesRead / sizeof(int32_t);
  if (count <= 0) return 0;

  double sum = 0;

  for (int i = 0; i < count; i++) {
    int32_t x = audioBuffer[i] >> 14;
    sum += (double)x * (double)x;
  }

  return sqrt(sum / count);
}

void discardSamples(int milliseconds) {
  int remaining = SAMPLE_RATE * milliseconds / 1000;

  while (remaining > 0) {
    int requested = min(remaining, 256);

    size_t bytesRead = 0;

    i2s_read(
      I2S_PORT,
      audioBuffer,
      requested * sizeof(int32_t),
      &bytesRead,
      portMAX_DELAY
    );

    int received = bytesRead / sizeof(int32_t);

    if (received <= 0) {
      break;
    }

    remaining -= received;
  }
}

bool captureFFT() {
  if (!vReal || !vImag) return false;

  int index = 0;

  while (index < SAMPLES) {
    size_t bytesRead = 0;

    if (
      i2s_read(
        I2S_PORT,
        audioBuffer,
        sizeof(audioBuffer),
        &bytesRead,
        portMAX_DELAY
      ) != ESP_OK
    ) {
      return false;
    }

    int count = bytesRead / sizeof(int32_t);

    for (int i = 0; i < count && index < SAMPLES; i++) {
      vReal[index] = (float)(audioBuffer[i] >> 14);
      vImag[index] = 0.0f;
      index++;
    }
  }

  return true;
}

float localSpectralPeak(float freq, int radiusBins, int &peakBinOut) {
  int center = (int)lroundf(freq * SAMPLES / SAMPLE_RATE);
  int lo = max(1, center - radiusBins);
  int hi = min(SAMPLES / 2 - 2, center + radiusBins);

  float best = 0.0f;
  peakBinOut = center;
  for (int i = lo; i <= hi; i++) {
    if (vReal[i] > best) {
      best = vReal[i];
      peakBinOut = i;
    }
  }
  return best;
}

int recognizeNote(float &detectedFreq) {
  detectedFreq = 0.0f;

  if (!FFT || !captureFFT()) {
    return -1;
  }

  // 去直流。强敲时 INMP441 的瞬态偏置会更明显。
  double mean = 0.0;
  for (int i = 0; i < SAMPLES; i++) mean += vReal[i];
  mean /= SAMPLES;
  for (int i = 0; i < SAMPLES; i++) vReal[i] -= (float)mean;

  FFT->windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT->compute(FFT_FORWARD);
  FFT->complexToMagnitude();

  int minBin = max(1, (int)(MIN_FREQ * SAMPLES / SAMPLE_RATE));
  int maxBin = min(SAMPLES / 2 - 2, (int)(MAX_FREQ * SAMPLES / SAMPLE_RATE));

  // 先恢复到原版最稳定的逻辑：寻找全频段最强的“局部谱峰”。
  // 上一版的“基频+谐波模板”在本项目里容易把相邻八度互相拉偏，现已移除。
  float peak1 = 0.0f;
  float peak2 = 0.0f;
  int peakBin = -1;

  for (int i = minBin; i <= maxBin; i++) {
    float current = vReal[i];
    if (current > vReal[i - 1] && current >= vReal[i + 1]) {
      if (current > peak1) {
        peak2 = peak1;
        peak1 = current;
        peakBin = i;
      } else if (current > peak2) {
        peak2 = current;
      }
    }
  }

  float adaptiveMinPeak = constrain(currentHitRmsForFFT * 7.0f, 30000.0f, MIN_PEAK_AMPLITUDE);
  float prominence = peak2 > 1.0f ? peak1 / peak2 : 999.0f;
  if (peakBin < 1 || peak1 < adaptiveMinPeak || prominence < 1.12f) {
    Serial.print("UNKNOWN: weak/flat peak, amp="); Serial.print(peak1,0);
    Serial.print(" min="); Serial.print(adaptiveMinPeak,0);
    Serial.print(" ratio="); Serial.println(prominence,2);
    return -1;
  }

  auto interpFreq = [&](int bin) -> float {
    float a = vReal[bin - 1];
    float b = vReal[bin];
    float c = vReal[bin + 1];
    float den = a - 2.0f * b + c;
    float corr = 0.0f;
    if (fabsf(den) > 0.000001f) {
      corr = 0.5f * (a - c) / den;
    }
    corr = constrain(corr, -0.5f, 0.5f);
    return (bin + corr) * SAMPLE_RATE / SAMPLES;
  };

  float peakFreq = interpFreq(peakBin);

  int best = -1;
  float bestError = 1000.0f;
  for (int i = 0; i < NOTE_COUNT; i++) {
    float error = fabsf(peakFreq - noteFreq[i]) / noteFreq[i];
    if (error < bestError) {
      bestError = error;
      best = i;
    }
  }

  if (best < 0 || bestError > MAX_FREQ_ERROR) {
    Serial.print("UNKNOWN: strongest peak ");
    Serial.print(peakFreq, 2);
    Serial.print(" Hz, nearest=");
    if (best >= 0) Serial.print(noteName[best]); else Serial.print("none");
    Serial.print(", error=");
    Serial.print(bestError * 100.0f, 2);
    Serial.println("%");
    return -1;
  }

  // 强敲时最常见的问题：二次谐波超过基频。
  // 这套15音琴里，高八度的若干音恰好接近低音的2倍频，
  // 所以只有在“半频位置确实存在明显、窄的局部峰”时才纠正到低八度。
  int finalNote = best;
  float finalFreq = peakFreq;
  float octaveRatio = 0.0f;

  if (best >= 7) {
    int lower = best - 7;
    int lowerBin = -1;
    float lowerAmp = localSpectralPeak(noteFreq[lower], 2, lowerBin);

    if (lowerBin > 1 && lowerBin < SAMPLES / 2 - 1) {
      bool narrowPeak =
        vReal[lowerBin] > vReal[lowerBin - 1] &&
        vReal[lowerBin] >= vReal[lowerBin + 1];

      float lowerFreq = interpFreq(lowerBin);
      float lowerError = fabsf(lowerFreq - noteFreq[lower]) / noteFreq[lower];
      float harmonicError = fabsf(peakFreq - 2.0f * lowerFreq) / peakFreq;
      octaveRatio = lowerAmp / peak1;

      if (
        narrowPeak &&
        octaveRatio >= OCTAVE_CORRECTION_RATIO &&
        lowerAmp >= MIN_PEAK_AMPLITUDE * 0.35f &&
        lowerError <= MAX_FREQ_ERROR &&
        harmonicError <= 0.025f
      ) {
        finalNote = lower;
        finalFreq = lowerFreq;
      }
    }
  }

  // V5.6 target-band rescue:
  // metallophone bars ring for a long time, so the previous note can remain the global maximum.
  // If the CURRENT target band contains a clear, narrow peak, prefer it even when an old ringing
  // bar is still slightly stronger elsewhere in the spectrum.
  int expected = expectedTargetNote;
  if (audioAcceptHits && expected >= 0 && expected < NOTE_COUNT) {
    int targetBin = -1;
    float targetAmp = localSpectralPeak(noteFreq[expected], 2, targetBin);
    if (targetBin > 1 && targetBin < SAMPLES / 2 - 1) {
      bool targetNarrow =
        vReal[targetBin] > vReal[targetBin - 1] &&
        vReal[targetBin] >= vReal[targetBin + 1];
      float targetFreq = interpFreq(targetBin);
      float targetError = fabsf(targetFreq - noteFreq[expected]) / noteFreq[expected];
      float targetVsGlobal = peak1 > 1.0f ? targetAmp / peak1 : 0.0f;
      float targetMinAmp = max(30000.0f, adaptiveMinPeak * 0.55f);

      if (targetNarrow &&
          targetError <= 0.030f &&
          targetAmp >= targetMinAmp &&
          targetVsGlobal >= 0.10f) {
        if (finalNote != expected) {
          Serial.print("TARGET-BAND RESCUE: global=");
          Serial.print(noteName[finalNote]);
          Serial.print(" target=");
          Serial.print(noteName[expected]);
          Serial.print(" target/global=");
          Serial.println(targetVsGlobal, 3);
        }
        finalNote = expected;
        finalFreq = targetFreq;
      }
    }
  }

  detectedFreq = finalFreq;

  Serial.println();
  Serial.println("========== HIT ==========");
  Serial.print("Strongest peak = ");
  Serial.print(peakFreq, 2);
  Serial.print(" Hz  Amp=");
  Serial.println(peak1, 0);
  Serial.print("Peak ratio = ");
  Serial.println(peak2 > 1.0f ? peak1 / peak2 : 999.0f, 2);
  if (best >= 7) {
    Serial.print("Half-frequency evidence = ");
    Serial.println(octaveRatio, 3);
  }
  Serial.print("Detected = ");
  Serial.print(noteName[finalNote]);
  Serial.print(" @ ");
  Serial.print(detectedFreq, 2);
  Serial.println(" Hz");

  return finalNote;
}

// ============================================================
// Audio FreeRTOS task
// ============================================================
void audioTask(void *parameter) {
  bool hitArmed = true;
  int releaseCount = 0;
  uint32_t lastHitTime = 0;
  float lastHitRms = 0.0f;
  float envelopeRms = 0.0f;

  bool newNoteArming = false;
  uint32_t newNoteGateStart = 0;

  while (true) {
    float rms = getRMS();
    uint32_t now = millis();

    if (!audioEnabled) {
      // 每局之外都彻底恢复检测器，防止上一局的大余振/状态带进下一局。
      hitArmed = false;
      newNoteArming = false;
      releaseCount = 0;
      lastHitRms = 0.0f;
      envelopeRms = rms;
      audioReadyForHit = false;
      audioProcessing = false;
      vTaskDelay(1);
      continue;
    }

    // 主循环在每个新音符开始时发出一次同步请求。
    // 这里重新建立包络，并短暂等待上一击余振衰减；等待期间游戏计时会暂停。
    if (audioNewNoteRequest) {
      audioNewNoteRequest = false;
      hitArmed = false;
      newNoteArming = true;
      newNoteGateStart = now;
      releaseCount = 0;
      lastHitRms = 0.0f;
      envelopeRms = rms;
      audioReadyForHit = false;
      Serial.print(">>> NEW NOTE AUDIO GATE, rms=");
      Serial.println(rms, 1);
    }

    // note 已经判定时继续不停读取 I2S（这样硬件缓冲不会积旧声音），
    // 但完全禁止生成新的 HIT。此前日志里 GOOD 后又出现一次 Detected=1，根因就在这里。
    if (!audioAcceptHits) {
      hitArmed = false;
      newNoteArming = false;
      audioReadyForHit = false;
      if (rms > envelopeRms) envelopeRms = 0.90f * envelopeRms + 0.10f * rms;
      else envelopeRms = 0.75f * envelopeRms + 0.25f * rms;
      vTaskDelay(1);
      continue;
    }

    if (newNoteArming) {
      uint32_t gateAge = now - newNoteGateStart;
      bool quietEnough = rms < 1200.0f;
      if ((gateAge >= NEW_NOTE_GUARD_MS && quietEnough) ||
          gateAge >= NEW_NOTE_FORCE_ARM_MS) {
        newNoteArming = false;
        hitArmed = true;
        audioReadyForHit = true;
        lastHitTime = now - MIN_HIT_INTERVAL;
        envelopeRms = rms;
        Serial.print(">>> AUDIO READY FOR NEW NOTE, rms=");
        Serial.println(rms, 1);
      } else {
        if (rms > envelopeRms) envelopeRms = 0.92f * envelopeRms + 0.08f * rms;
        else envelopeRms = 0.78f * envelopeRms + 0.22f * rms;
        vTaskDelay(1);
        continue;
      }
    }

    // 用慢包络追踪当前余振/环境响度。新敲击必须明显高于这个包络，
    // 因此即使强敲后被强制重新上膛，也不会把正在衰减的余振再次识别成新敲击。
    float dynamicThreshold = max(
      HIT_THRESHOLD,
      envelopeRms * ATTACK_OVER_ENVELOPE + 120.0f
    );

    if (hitArmed) {
      if (
        rms > dynamicThreshold &&
        now - lastHitTime > MIN_HIT_INTERVAL
      ) {
        hitArmed = false;
        audioReadyForHit = false;
        releaseCount = 0;
        lastHitTime = now;
        lastHitRms = rms;
        uint32_t hitGeneration = audioGeneration;

        Serial.print("NEW HIT RMS = ");
        Serial.print(rms, 1);
        Serial.print("  trigger=");
        Serial.println(dynamicThreshold, 1);

        // 强敲的最初几十毫秒宽带冲击/削顶最严重，稍晚一点截取共鸣段。
        int adaptiveSkip = SKIP_MS;
        if (rms > 8000.0f) adaptiveSkip = 55;
        else if (rms > 5000.0f) adaptiveSkip = 42;
        else if (rms > 2500.0f) adaptiveSkip = 28;

        audioReadyForHit = false;
        audioProcessing = true;
        currentHitRmsForFFT = rms;
        discardSamples(adaptiveSkip);

        float freq = 0.0f;
        int detected = recognizeNote(freq);
        audioProcessing = false;

        if (detected >= 0) {
          DetectionEvent ev;
          ev.note = detected;
          ev.freq = freq;
          ev.rms = rms;
          ev.hitMs = lastHitTime;
          ev.generation = hitGeneration;
          xQueueSend(detectionQueue, &ev, 0);
        }
      }
    } else {
      float adaptiveRelease = max(
        RELEASE_THRESHOLD,
        lastHitRms * ADAPTIVE_RELEASE_RATIO
      );

      if (rms < adaptiveRelease) {
        releaseCount++;
        if (releaseCount >= RELEASE_COUNT_REQUIRED) {
          hitArmed = true;
          audioReadyForHit = true;
          releaseCount = 0;
          Serial.println(">>> AUDIO READY (released)");
        }
      } else {
        releaseCount = 0;
      }

      // 当前音符内若 UNKNOWN，最迟约0.38s恢复；有效判定后则由 audioAcceptHits 直接锁住到下一拍，
      // 因此不会像简单“强制上膛”那样把余振误触发。
      if (!hitArmed && now - lastHitTime >= FORCE_REARM_MS) {
        hitArmed = true;
        audioReadyForHit = true;
        releaseCount = 0;
        Serial.println(">>> AUDIO READY (forced + envelope guarded)");
      }
    }

    // 非对称包络：上升不要跟得太快（否则会吞掉攻击沿），下降则平滑追踪余振。
    if (rms > envelopeRms) {
      envelopeRms = 0.92f * envelopeRms + 0.08f * rms;
    } else {
      envelopeRms = 0.82f * envelopeRms + 0.18f * rms;
    }

    vTaskDelay(1);
  }
}

// ============================================================
// Animation frames
// ============================================================
const Sprite565 *getFrames(
  uint8_t state,
  uint8_t &count
) {
  uint8_t s = equippedSkin;
  if (s >= CHARACTER_SKIN_COUNT) s = 0;
  uint8_t a = state;
  if (a > ANIM_CHEER) a = ANIM_RUN;
  // Skin 5 = Pink Music Girl.
  // Its action count and frame canvas sizes match School Girl.
  if (s == 5) {
    switch (a) {
      case ANIM_RUN:   count = 4; return V54_PINK_RUN_FRAMES;
      case ANIM_JUMP:  count = 2; return V54_PINK_JUMP_FRAMES;
      case ANIM_HIT:   count = 2; return V54_PINK_HIT_FRAMES;
      case ANIM_FALL:  count = 2; return V54_PINK_FALL_FRAMES;
      case ANIM_CHEER: count = 3; return V54_PINK_CHEER_FRAMES;
      default:         count = 4; return V54_PINK_RUN_FRAMES;
    }
  }

  count = SKIN_COUNT_TABLE[s][a];
  return SKIN_FRAME_TABLE[s][a];
}

void setAnimState(uint8_t state) {
  animState = static_cast<AnimState>(state);
  animFrame = 0;
  lastAnimFrameTime = millis();
  animStateStartTime = millis();
}

void updateAnimation() {
  uint32_t now = millis();

  uint8_t count = 0;
  getFrames(animState, count);

  if (count == 0) return;

  uint32_t frameMs = RUN_FRAME_MS;

  switch (animState) {
    case ANIM_RUN:   frameMs = RUN_FRAME_MS; break;
    case ANIM_JUMP:  frameMs = JUMP_FRAME_MS; break;
    case ANIM_HIT:   frameMs = HIT_FRAME_MS; break;
    case ANIM_FALL:  frameMs = FALL_FRAME_MS; break;
    case ANIM_CHEER: frameMs = CHEER_FRAME_MS; break;
  }

  if (now - lastAnimFrameTime >= frameMs) {
    lastAnimFrameTime = now;

    if (
      animState == ANIM_RUN ||
      animState == ANIM_CHEER
    ) {
      animFrame =
        (animFrame + 1) % count;
    } else {
      if (animFrame + 1 < count) {
        animFrame++;
      }
    }
  }

  if (
    animState == ANIM_JUMP &&
    now - animStateStartTime >= JUMP_TOTAL_MS
  ) {
    setAnimState(ANIM_RUN);
  }

  if (
    animState == ANIM_HIT &&
    now - animStateStartTime >= HIT_TOTAL_MS
  ) {
    setAnimState(ANIM_RUN);
  }

  if (
    animState == ANIM_FALL &&
    now - animStateStartTime >= FALL_TOTAL_MS
  ) {
    setAnimState(ANIM_RUN);
  }
}

// ============================================================
// Particles
// ============================================================
void clearParticles() {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    particles[i].active = false;
  }
}

void spawnParticle(
  float x,
  float y,
  float vx,
  float vy,
  uint16_t color,
  uint8_t life,
  uint8_t radius
) {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!particles[i].active) {
      particles[i].active = true;
      particles[i].x = x;
      particles[i].y = y;
      particles[i].vx = vx;
      particles[i].vy = vy;
      particles[i].color = color;
      particles[i].life = life;
      particles[i].radius = radius;
      return;
    }
  }
}

void spawnGoodParticles() {
  for (int i = 0; i < 12; i++) {
    spawnParticle(
      PLAYER_X + 68,
      PLAYER_GROUND_Y - 45,
      random(-24, 25) / 10.0f,
      random(-36, -8) / 10.0f,
      ST77XX_YELLOW,
      random(16, 25),
      random(1, 3)
    );
  }
}

void spawnBadParticles() {
  for (int i = 0; i < 10; i++) {
    spawnParticle(
      PLAYER_X + 68,
      PLAYER_GROUND_Y - 40,
      random(-20, 21) / 10.0f,
      random(-26, -4) / 10.0f,
      ST77XX_RED,
      random(14, 22),
      2
    );
  }
}

void spawnMissParticles() {
  for (int i = 0; i < 9; i++) {
    spawnParticle(
      PLAYER_X + 65,
      PLAYER_GROUND_Y - 20,
      random(-12, 13) / 10.0f,
      random(4, 20) / 10.0f,
      ST77XX_CYAN,
      random(14, 22),
      2
    );
  }
}

void updateParticles() {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!particles[i].active) continue;

    particles[i].x += particles[i].vx;
    particles[i].y += particles[i].vy;
    particles[i].vy += 0.10f;

    if (particles[i].life > 0) {
      particles[i].life--;
    }

    if (particles[i].life == 0) {
      particles[i].active = false;
    }
  }
}

// ============================================================
// Secondary 1.8" TFT rhythm display (V5.7 stage 1)
// This screen is visual-only for now: it DOES NOT change scoring/timing logic.
// ============================================================
uint16_t rhythmNoteColor(uint8_t note) {
  static const uint16_t pal[NOTE_COUNT] = {
    0x4D7F, 0x55FF, 0x7DFF,   // L5 L6 L7
    0xF81F, 0xFD20, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0x781F, // 1..7
    0xF97F, 0xFC9F, 0xFE5F, 0xB7FF, 0xFFFF  // H1..H5
  };
  if (note >= NOTE_COUNT) return ST77XX_WHITE;
  return pal[note];
}

void rhythmPushCanvas() {
  if (!rhythmTftReady || !rhythmCanvas.getBuffer()) return;
  rhythmTft.drawRGBBitmap(0, 0, rhythmCanvas.getBuffer(), RHYTHM_W, RHYTHM_H);
}

void rhythmDrawCentered(const char *txt, int16_t y, uint16_t color, uint8_t size) {
  rhythmCanvas.setTextWrap(false);
  rhythmCanvas.setTextColor(color);
  rhythmCanvas.setTextSize(size);
  int16_t x1, y1; uint16_t w, h;
  rhythmCanvas.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  rhythmCanvas.setCursor((RHYTHM_W - (int)w) / 2, y);
  rhythmCanvas.print(txt);
}

void rhythmShowIdle() {
  if (!rhythmTftReady) return;
  rhythmCanvas.fillScreen(0x0842);
  rhythmCanvas.fillRoundRect(8, 10, 112, 34, 8, 0x780F);
  rhythmCanvas.drawRoundRect(8, 10, 112, 34, 8, ST77XX_WHITE);
  rhythmDrawCentered("RUOXI RUN", 20, ST77XX_WHITE, 2);

  rhythmCanvas.drawCircle(64, 82, 24, 0x07FF);
  rhythmCanvas.drawCircle(64, 82, 18, 0x07FF);
  rhythmCanvas.fillCircle(58, 77, 3, ST77XX_YELLOW);
  rhythmCanvas.fillCircle(70, 77, 3, ST77XX_YELLOW);
  rhythmCanvas.drawFastHLine(58, 90, 13, ST77XX_WHITE);

  rhythmDrawCentered("RHYTHM TRACK", 115, ST77XX_CYAN, 1);
  rhythmDrawCentered("READY", 135, ST77XX_GREEN, 2);
  rhythmPushCanvas();
}

void rhythmShowCountdown(const char *label) {
  if (!rhythmTftReady) return;
  rhythmCanvas.fillScreen(0x0842);
  rhythmDrawCentered("GET READY", 20, ST77XX_CYAN, 2);
  rhythmCanvas.drawRoundRect(19, 52, 90, 78, 12, ST77XX_WHITE);
  rhythmDrawCentered(label, 72, ST77XX_YELLOW, (label[1] == '\0') ? 6 : 4);
  rhythmDrawCentered("HIT ON THE CUE", 141, ST77XX_WHITE, 1);
  rhythmPushCanvas();
}

const char *rhythmJudgeText() {
  switch (judgeResult) {
    case JUDGE_PERFECT: return "PERFECT";
    case JUDGE_GREAT:   return "GREAT";
    case JUDGE_GOOD:    return "GOOD";
    case JUDGE_BAD:     return "WRONG";
    case JUDGE_MISS:    return "MISS";
    default:            return "";
  }
}

uint16_t rhythmJudgeColor() {
  switch (judgeResult) {
    case JUDGE_PERFECT: return ST77XX_YELLOW;
    case JUDGE_GREAT:   return ST77XX_GREEN;
    case JUDGE_GOOD:    return ST77XX_CYAN;
    case JUDGE_BAD:     return ST77XX_RED;
    case JUDGE_MISS:    return ST77XX_MAGENTA;
    default:            return ST77XX_WHITE;
  }
}

void rhythmRenderPlaying() {
  if (!rhythmTftReady || !activeSong || songIndex >= songLength()) return;

  const uint16_t bg = 0x0842;
  const int judgeY = 121;
  const int blockW = 58;
  const int blockH = 28;
  const int startCenterY = 42;
  const uint32_t visualFallMs = 330; // aligned near the current PERFECT window

  rhythmCanvas.fillScreen(bg);

  // Header + song progress
  rhythmCanvas.fillRect(0, 0, RHYTHM_W, 18, 0x18C3);
  rhythmCanvas.setTextColor(ST77XX_WHITE);
  rhythmCanvas.setTextSize(1);
  rhythmCanvas.setCursor(4, 5);
  rhythmCanvas.print("NOTE ");
  rhythmCanvas.print(songIndex + 1);
  rhythmCanvas.print("/");
  rhythmCanvas.print(songLength());

  // Show the next note as a small preview.
  if (songIndex + 1 < songLength()) {
    rhythmCanvas.setCursor(78, 5);
    rhythmCanvas.print("NEXT:");
    rhythmCanvas.print(noteName[activeSong->notes[songIndex + 1]]);
  }

  // Lane rails
  rhythmCanvas.drawFastVLine(23, 22, 107, 0x39E7);
  rhythmCanvas.drawFastVLine(104, 22, 107, 0x39E7);

  // Judge zone
  rhythmCanvas.fillRect(18, judgeY - 3, 92, 7, 0x4208);
  rhythmCanvas.drawFastHLine(15, judgeY, 98, ST77XX_WHITE);
  rhythmCanvas.drawFastHLine(22, judgeY + 4, 84, 0x07E0);

  uint8_t target = targetNote();
  uint32_t now = millis();
  uint32_t elapsed = now >= noteStartTime ? (now - noteStartTime) : 0;
  float p = min(1.0f, elapsed / (float)visualFallMs);
  // Ease-out so the block is readable at the top but settles crisply on the line.
  float pe = 1.0f - (1.0f - p) * (1.0f - p);
  int centerY = startCenterY + (int)((judgeY - startCenterY) * pe);

  uint16_t c = rhythmNoteColor(target);
  int bx = (RHYTHM_W - blockW) / 2;
  int by = centerY - blockH / 2;

  // Glow/shadow
  rhythmCanvas.fillRoundRect(bx + 3, by + 3, blockW, blockH, 7, 0x2104);
  rhythmCanvas.fillRoundRect(bx, by, blockW, blockH, 7, c);
  rhythmCanvas.drawRoundRect(bx, by, blockW, blockH, 7, ST77XX_WHITE);

  const char *nn = noteName[target];
  rhythmCanvas.setTextSize((strlen(nn) == 1) ? 3 : 2);
  rhythmCanvas.setTextColor(ST77XX_BLACK);
  int16_t x1, y1; uint16_t tw, th;
  rhythmCanvas.getTextBounds(nn, 0, 0, &x1, &y1, &tw, &th);
  rhythmCanvas.setCursor((RHYTHM_W - (int)tw) / 2, by + ((blockH - (int)th) / 2));
  rhythmCanvas.print(nn);

  // Timing cue. After reaching the line, hold the block there until this note resolves.
  if (!noteResolved && p >= 1.0f) {
    rhythmDrawCentered("HIT!", 136, ST77XX_GREEN, 2);
  }

  if (noteResolved && judgeResult != JUDGE_NONE) {
    // Strong result banner so the small display feels responsive.
    rhythmCanvas.fillRoundRect(14, 63, 100, 32, 7, 0x0000);
    rhythmCanvas.drawRoundRect(14, 63, 100, 32, 7, rhythmJudgeColor());
    rhythmDrawCentered(rhythmJudgeText(), 73, rhythmJudgeColor(), 2);
  }

  rhythmPushCanvas();
}

void rhythmShowResult() {
  if (!rhythmTftReady) return;
  rhythmCanvas.fillScreen(0x0842);
  if (gameFailed) {
    rhythmDrawCentered("GAME OVER", 28, ST77XX_RED, 2);
  } else {
    rhythmDrawCentered("SONG CLEAR!", 28, ST77XX_GREEN, 2);
  }
  rhythmDrawCentered("RANK", 65, ST77XX_WHITE, 1);
  rhythmDrawCentered(finalRank.c_str(), 80, ST77XX_YELLOW, 4);
  char buf[32];
  snprintf(buf, sizeof(buf), "SCORE %ld", (long)score);
  rhythmDrawCentered(buf, 132, ST77XX_CYAN, 1);
  rhythmPushCanvas();
}

void rhythmUpdate() {
  if (!rhythmTftReady) return;
  static int lastState = -1;
  uint32_t now = millis();

  if (appState == APP_PLAYING) {
    if (now - rhythmLastFrameMs >= RHYTHM_FRAME_MS) {
      rhythmLastFrameMs = now;
      rhythmRenderPlaying();
    }
    lastState = APP_PLAYING;
    return;
  }

  // Non-playing pages are static on the secondary TFT; redraw only on state change.
  if ((int)appState == lastState) return;
  lastState = (int)appState;
  if (appState == APP_RESULT) rhythmShowResult();
  else rhythmShowIdle();
}

// ============================================================
// CREATE MODE - capture detected pitch + onset time and save to NVS
// ============================================================
void saveCustomSongSlot(uint8_t slot) {
  if (slot >= CUSTOM_SONG_SLOTS) return;
  int songIdx = customSongIndexFromSlot(slot);
  SONGS[songIdx].len = customSongLen[slot];

  char kLen[12], kNotes[12], kInt[12];
  snprintf(kLen, sizeof(kLen), "usrLen%u", slot);
  snprintf(kNotes, sizeof(kNotes), "usrNotes%u", slot);
  snprintf(kInt, sizeof(kInt), "usrInt%u", slot);
  prefs.putUShort(kLen, customSongLen[slot]);
  if (customSongLen[slot] > 0) {
    prefs.putBytes(kNotes, customSongNotes[slot], customSongLen[slot] * sizeof(uint8_t));
    prefs.putBytes(kInt, customSongIntervalsMs[slot], customSongLen[slot] * sizeof(uint16_t));
  }
  customSongDirty = false;
  Serial.print("[CREATE] saved My Song "); Serial.print(slot + 1);
  Serial.print(", notes="); Serial.println(customSongLen[slot]);
}

void loadCustomSongs() {
  for (uint8_t slot = 0; slot < CUSTOM_SONG_SLOTS; ++slot) {
    char kLen[12], kNotes[12], kInt[12];
    snprintf(kLen, sizeof(kLen), "usrLen%u", slot);
    snprintf(kNotes, sizeof(kNotes), "usrNotes%u", slot);
    snprintf(kInt, sizeof(kInt), "usrInt%u", slot);

    customSongLen[slot] = prefs.getUShort(kLen, 0);
    if (slot == 0 && customSongLen[slot] == 0 && prefs.isKey("usrLen")) {
      customSongLen[slot] = prefs.getUShort("usrLen", 0);
      if (customSongLen[slot] > CUSTOM_MAX_NOTES) customSongLen[slot] = CUSTOM_MAX_NOTES;
      size_t wantNotes = customSongLen[slot] * sizeof(uint8_t);
      size_t wantInts = customSongLen[slot] * sizeof(uint16_t);
      if (prefs.getBytesLength("usrNotes") >= wantNotes) prefs.getBytes("usrNotes", customSongNotes[slot], wantNotes);
      if (prefs.getBytesLength("usrInt") >= wantInts) prefs.getBytes("usrInt", customSongIntervalsMs[slot], wantInts);
      saveCustomSongSlot(0);
      prefs.remove("usrLen");
      prefs.remove("usrNotes");
      prefs.remove("usrInt");
    }

    if (customSongLen[slot] > CUSTOM_MAX_NOTES) customSongLen[slot] = CUSTOM_MAX_NOTES;
    if (customSongLen[slot] > 0 && !prefs.isKey("usrLen") ) {
      size_t wantNotes = customSongLen[slot] * sizeof(uint8_t);
      size_t wantInts = customSongLen[slot] * sizeof(uint16_t);
      if (prefs.getBytesLength(kNotes) >= wantNotes) prefs.getBytes(kNotes, customSongNotes[slot], wantNotes);
      if (prefs.getBytesLength(kInt) >= wantInts) prefs.getBytes(kInt, customSongIntervalsMs[slot], wantInts);
    } else if (customSongLen[slot] > 0 && slot > 0) {
      size_t wantNotes = customSongLen[slot] * sizeof(uint8_t);
      size_t wantInts = customSongLen[slot] * sizeof(uint16_t);
      if (prefs.getBytesLength(kNotes) >= wantNotes) prefs.getBytes(kNotes, customSongNotes[slot], wantNotes);
      if (prefs.getBytesLength(kInt) >= wantInts) prefs.getBytes(kInt, customSongIntervalsMs[slot], wantInts);
    } else if (customSongLen[slot] > 0 && slot == 0 && prefs.isKey(kLen)) {
      size_t wantNotes = customSongLen[slot] * sizeof(uint8_t);
      size_t wantInts = customSongLen[slot] * sizeof(uint16_t);
      if (prefs.getBytesLength(kNotes) >= wantNotes) prefs.getBytes(kNotes, customSongNotes[slot], wantNotes);
      if (prefs.getBytesLength(kInt) >= wantInts) prefs.getBytes(kInt, customSongIntervalsMs[slot], wantInts);
    }

    for (uint16_t i = 0; i < customSongLen[slot]; ++i) {
      if (customSongNotes[slot][i] >= NOTE_COUNT) customSongNotes[slot][i] = NOTE_1;
      customSongIntervalsMs[slot][i] = constrain((int)customSongIntervalsMs[slot][i], 260, 3000);
      customSongUnits[slot][i] = (uint8_t)constrain((int)lroundf(customSongIntervalsMs[slot][i] / 500.0f), 1, 8);
    }
    SONGS[customSongIndexFromSlot(slot)].len = customSongLen[slot];
  }
}

void beginCreateRecording() {
  stopEndingAudio();
  recordCount = 0;
  recordFirstHitMs = 0;
  memset(recordNotes, 0, sizeof(recordNotes));
  memset(recordTimesMs, 0, sizeof(recordTimesMs));
  customSongDirty = false;
  if (detectionQueue) {
    DetectionEvent trash;
    while (xQueueReceive(detectionQueue, &trash, 0) == pdTRUE) {}
  }
  expectedTargetNote = -1; // no target-band rescue while composing
  recordingGeneration = ++audioGeneration;
  audioEnabled = true;
  audioAcceptHits = true;
  audioReadyForHit = false;
  audioNewNoteRequest = true;
  appState = APP_RECORDING;
  Serial.print("[CREATE] recording started for slot "); Serial.println(selectedCreateSlot + 1);
}

void undoCreateNote() {
  if (recordCount == 0) {
    uiMessage = "NOTHING TO UNDO";
    return;
  }
  recordCount--;
  recordNotes[recordCount] = 0;
  recordTimesMs[recordCount] = 0;
  if (recordCount == 0) recordFirstHitMs = 0;
  customSongDirty = recordCount > 0;
  uiMessage = "LAST NOTE UNDONE";
  Serial.print("[CREATE] undo, notes="); Serial.println(recordCount);
}

bool finishCreateRecording() {
  audioAcceptHits = false;
  audioReadyForHit = false;
  audioEnabled = false;
  expectedTargetNote = -1;

  if (recordCount == 0) {
    uiMessage = "NO NOTES RECORDED";
    appState = APP_CREATE;
    return false;
  }

  uint8_t slot = selectedCreateSlot;
  customSongLen[slot] = recordCount;
  for (uint16_t i = 0; i < customSongLen[slot]; ++i) {
    customSongNotes[slot][i] = recordNotes[i];
    uint32_t interval = 500;
    if (i + 1 < customSongLen[slot]) interval = recordTimesMs[i+1] - recordTimesMs[i];
    else if (customSongLen[slot] >= 2) interval = recordTimesMs[customSongLen[slot]-1] - recordTimesMs[customSongLen[slot]-2];
    interval = constrain((int)interval, 260, 3000);
    customSongIntervalsMs[slot][i] = (uint16_t)interval;
    customSongUnits[slot][i] = (uint8_t)constrain((int)lroundf(interval / 500.0f), 1, 8);
  }
  SONGS[customSongIndexFromSlot(slot)].len = customSongLen[slot];
  saveCustomSongSlot(slot);
  selectedSong = customSongIndexFromSlot(slot);
  songPage = selectedSong / SONGS_PER_PAGE;
  uiMessage = "SAVED TO SONG LIBRARY";
  appState = APP_CREATE;
  Serial.println("[CREATE] recording finished");
  return true;
}

void cancelCreateRecording() {
  audioAcceptHits = false;
  audioReadyForHit = false;
  audioEnabled = false;
  expectedTargetNote = -1;
  recordCount = 0;
  recordFirstHitMs = 0;
  customSongDirty = false;
  if (detectionQueue) {
    DetectionEvent trash;
    while (xQueueReceive(detectionQueue, &trash, 0) == pdTRUE) {}
  }
  uiMessage = "RECORDING CANCELLED";
  appState = APP_CREATE;
  Serial.println("[CREATE] recording cancelled");
}

void deleteCustomSong(uint8_t slot) {
  if (slot >= CUSTOM_SONG_SLOTS) return;
  audioAcceptHits = false;
  audioReadyForHit = false;
  audioEnabled = false;
  expectedTargetNote = -1;

  customSongLen[slot] = 0;
  recordCount = 0;
  customSongDirty = false;
  memset(customSongNotes[slot], 0, sizeof(customSongNotes[slot]));
  memset(customSongUnits[slot], 1, sizeof(customSongUnits[slot]));
  for (uint16_t i = 0; i < CUSTOM_MAX_NOTES; ++i) customSongIntervalsMs[slot][i] = 500;
  int songIdx = customSongIndexFromSlot(slot);
  SONGS[songIdx].len = 0;

  char kLen[12], kNotes[12], kInt[12];
  snprintf(kLen, sizeof(kLen), "usrLen%u", slot);
  snprintf(kNotes, sizeof(kNotes), "usrNotes%u", slot);
  snprintf(kInt, sizeof(kInt), "usrInt%u", slot);
  prefs.remove(kLen); prefs.remove(kNotes); prefs.remove(kInt);
  if (slot == 0) { prefs.remove("usrLen"); prefs.remove("usrNotes"); prefs.remove("usrInt"); }
  bestScores[songIdx] = 0;
  saveProgress();

  if (selectedSong == songIdx) selectedSong = 0;
  deleteConfirmArmed = false;
  deleteConfirmUntil = 0;
  uiMessage = "CUSTOM SONG DELETED";
  Serial.print("[CREATE] My Song "); Serial.print(slot + 1); Serial.println(" deleted");
}

// ============================================================
// Game flow
// ============================================================
uint32_t currentNoteWindow() {
  uint32_t rhythmMs;
  if (isCustomSongIndex(selectedSong)) {
    int slot = customSlotFromSongIndex(selectedSong);
    if (slot >= 0 && slot < CUSTOM_SONG_SLOTS && songIndex < customSongLen[slot]) {
      // Preserve the player's captured spacing; difficulty scales it rather than replacing it.
      float scale = (difficulty == DIFF_EASY ? 1.35f : (difficulty == DIFF_HARD ? 0.78f : 1.0f));
      rhythmMs = (uint32_t)(customSongIntervalsMs[slot][songIndex] * scale);
    } else {
      rhythmMs = 500;
    }
  } else {
    rhythmMs = (uint32_t)activeSong->units[songIndex] * DIFF_UNIT_MS[difficulty];
  }
  uint32_t window = rhythmMs + DIFF_GRACE_MS[difficulty];
  uint32_t minWindow = (difficulty == DIFF_EASY ? 900 : (difficulty == DIFF_NORMAL ? 620 : 480));
  return max(window, minWindow);
}

void chooseObstacle() {
  obstacle.sp = obstaclePool[random(OBSTACLE_COUNT)];
  obstacle.x = 300;
  obstacle.y = PLAYER_GROUND_Y - obstacle.sp->h + 3;
}

void prepareCurrentNote() {
  noteResolved = false;
  judgeResult = JUDGE_NONE;
  wrongStrikeCount = 0;
  lastWrongNote = -1;
  expectedTargetNote = targetNote();
  noteStartTime = millis();
  noteWindowMs = currentNoteWindow();
  hitIdealTime = noteStartTime; // compatibility only; judging uses response time from note start
  chooseObstacle();

  Serial.print("Target ");
  Serial.print(songIndex + 1);
  Serial.print("/");
  Serial.print(songLength());
  Serial.print(": ");
  Serial.println(noteName[targetNote()]);

  if (detectionQueue) {
    DetectionEvent trash;
    while (xQueueReceive(detectionQueue, &trash, 0) == pdTRUE) {}
  }
  currentNoteGeneration = ++audioGeneration;
  audioReadyForHit = false;
  audioAcceptHits = true;
  audioNewNoteRequest = true;
}

void saveProgress() {
  prefs.putUInt("coins", coins);
  prefs.putUInt("plays", totalPlays);
  prefs.putUInt("songMask", songUnlockedMask);
  prefs.putUInt("outfitMask", outfitOwnedMask);
  prefs.putChar("eqCap", equippedByCategory[CAT_CAP]);
  prefs.putChar("eqClothes", equippedByCategory[CAT_CLOTHES]);
  prefs.putChar("eqShoes", equippedByCategory[CAT_SHOES]);
  prefs.putChar("eqAcc", equippedByCategory[CAT_ACCESSORIES]);
  prefs.putUInt("skinMask", skinOwnedMask);
  prefs.putUChar("skinEq", equippedSkin);
  for (int i = 0; i < SONG_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "best%d", i);
    prefs.putUShort(key, bestScores[i]);
  }
}

String rankFromPercent(float p) {
  if (p >= 0.97f) return "SSS";
  if (p >= 0.93f) return "SS";
  if (p >= 0.88f) return "S";
  if (p >= 0.80f) return "A";
  if (p >= 0.70f) return "B";
  return "C";
}

void showStartCountdown() {
  // No microphone scoring during countdown. MP3 voice can replace this later.
  audioAcceptHits = false;
  audioReadyForHit = false;
  audioEnabled = false;
  expectedTargetNote = -1;

  const char *labels[] = {"3", "2", "1", "GO!"};
  const uint16_t colors[] = {ST77XX_WHITE, ST77XX_WHITE, ST77XX_WHITE, ST77XX_YELLOW};

  startLedFx(LEDFX_START);
  for (int k = 0; k < 4; ++k) {
    rhythmShowCountdown(labels[k]);
    tft.fillScreen(0x0861);
    tft.setTextColor(ST77XX_CYAN);
    tft.setTextSize(2);
    tft.setCursor(80, 42);
    tft.print("GET READY");

    tft.setTextColor(colors[k]);
    tft.setTextSize(k == 3 ? 6 : 8);
    int16_t x = (k == 3 ? 103 : 137);
    int16_t y = (k == 3 ? 102 : 88);
    tft.setCursor(x, y);
    tft.print(labels[k]);

    // Keep each countdown step 700 ms total, including its sound.
    uint32_t until = millis() + 700;
    playCountdownSfx(k);
    while ((int32_t)(until - millis()) > 0) {
      updateLedFx();
      delay(5);
    }
  }

  // A short silent buffer after GO prevents the first hit from being rushed and
  // gives future speaker/MP3 audio time to decay before the microphone is armed.
  rhythmShowCountdown("GO!");
  tft.fillScreen(0x0861);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(77, 105);
  tft.print("READY...");
  uint32_t until = millis() + 500;
  while ((int32_t)(until - millis()) > 0) {
    updateLedFx();
    delay(5);
  }
}

void startGame() {
  stopEndingAudio();
  if (isCustomSongIndex(selectedSong)) {
    int slot = customSlotFromSongIndex(selectedSong);
    if (slot < 0 || slot >= CUSTOM_SONG_SLOTS || customSongLen[slot] == 0) {
      selectedCreateSlot = constrain(slot, 0, (int)CUSTOM_SONG_SLOTS - 1);
      appState = APP_CREATE;
      uiMessage = "RECORD THIS SLOT FIRST";
      drawCreateMode();
      return;
    }
  }
  activeSong = &SONGS[selectedSong];
  score = 0;
  maxPossibleScore = activeSong->len * 100;
  combo = 0;
  maxCombo = 0;
  lifeHalfUnits = 6;
  gameFailed = false;
  songIndex = 0;
  resultRendered = false;
  lastTimingError = 0;

  if (detectionQueue) {
    DetectionEvent trash;
    while (xQueueReceive(detectionQueue, &trash, 0) == pdTRUE) {}
  }

  clearParticles();
  setAnimState(ANIM_RUN);

  // V5.6: show 3-2-1-GO before entering gameplay.
  showStartCountdown();

  appState = APP_PLAYING;
  audioAcceptHits = false;
  audioReadyForHit = false;
  audioEnabled = true;
  prepareCurrentNote();
}

void finishGame(bool failed) {
  audioAcceptHits = false;
  audioReadyForHit = false;
  audioEnabled = false;
  expectedTargetNote = -1;
  appState = APP_RESULT;
  gameFailed = failed;
  resultRendered = false;
  lastRenderTime = 0;

  if (failed) setAnimState(ANIM_FALL);
  else setAnimState(ANIM_CHEER);
  startLedFx(failed ? LEDFX_FAIL : LEDFX_WIN);

  float pct = maxPossibleScore > 0 ? (float)score / maxPossibleScore : 0.0f;
  finalRank = rankFromPercent(pct);
  uint32_t earned = (uint32_t)(score / 20);
  if (!failed) earned += 30;
  coins += earned;
  totalPlays++;
  if (!failed && selectedSong >= 0 && selectedSong < SONG_COUNT && score > bestScores[selectedSong]) {
    bestScores[selectedSong] = (uint16_t)min(score, 65535);
  }
  saveProgress();    

  // Random prerecorded result audio starts after result data is ready.
  // Microphone scoring is disabled; audio runs in a background task.
  if (failed) playFailSfx();
  else playWinSfx();

  Serial.println();
  Serial.println("===== GAME FINISHED =====");
  Serial.print("Score: "); Serial.println(score);
  Serial.print("Rank: "); Serial.println(finalRank);
  Serial.print("Coins: "); Serial.println(coins);
}

void advanceNote() {
  songIndex++;
  if (songIndex >= songLength()) {
    finishGame(false);
    return;
  }
  prepareCurrentNote();
}

void resolveNote(uint8_t result, int timingErrorMs = 0) {
  if (noteResolved) return;
  noteResolved = true;
  judgeResult = static_cast<JudgeResult>(result);
  lastTimingError = timingErrorMs;

  if (result == JUDGE_PERFECT) startLedFx(LEDFX_PERFECT);
  else if (result == JUDGE_GREAT) startLedFx(LEDFX_GREAT);
  else if (result == JUDGE_GOOD) startLedFx(LEDFX_GOOD);
  else if (result == JUDGE_BAD) startLedFx(LEDFX_BAD);
  else if (result == JUDGE_MISS) startLedFx(LEDFX_MISS);
  audioAcceptHits = false;
  audioReadyForHit = false;
  resolveUntil = millis() + RESULT_HOLD_MS;

  int add = 0;
  if (result == JUDGE_PERFECT) add = 100;
  else if (result == JUDGE_GREAT) add = 85;
  else if (result == JUDGE_GOOD) add = 65;
  else if (result == JUDGE_BAD) add = 20;

  if (result == JUDGE_PERFECT || result == JUDGE_GREAT || result == JUDGE_GOOD) {
    score += add;
    combo++;
    if (combo > maxCombo) maxCombo = combo;
    setAnimState(ANIM_JUMP);
    spawnGoodParticles();
  } else {
    combo = 0;
    lifeHalfUnits = max(0, lifeHalfUnits - 1);
    if (result == JUDGE_BAD) { setAnimState(ANIM_HIT); spawnBadParticles(); }
    else { setAnimState(ANIM_FALL); spawnMissParticles(); }
  }
}

uint8_t judgeCorrectHit(uint32_t hitTime) {
  uint32_t elapsed = hitTime >= noteStartTime ? (hitTime - noteStartTime) : 0;
  if (elapsed <= 350) return JUDGE_PERFECT;
  if (elapsed <= 600) return JUDGE_GREAT;
  return JUDGE_GOOD;
}

void processDetectionQueue() {
  if (appState == APP_RECORDING) {
    DetectionEvent ev;
    bool added = false;
    while (detectionQueue && xQueueReceive(detectionQueue, &ev, 0) == pdTRUE) {
      if (ev.generation != recordingGeneration) continue;
      if (recordCount >= CUSTOM_MAX_NOTES) continue;
      if (recordFirstHitMs == 0) recordFirstHitMs = ev.hitMs;
      recordNotes[recordCount] = (uint8_t)ev.note;
      recordTimesMs[recordCount] = ev.hitMs - recordFirstHitMs;
      Serial.print("[CREATE] #"); Serial.print(recordCount + 1);
      Serial.print(" "); Serial.print(noteName[ev.note]);
      Serial.print(" @ "); Serial.print(recordTimesMs[recordCount]); Serial.println(" ms");
      recordCount++;
      added = true;
    }
    if (added) customSongDirty = true;
    return;
  }

  if (appState == APP_CALIBRATION) {
    DetectionEvent ev;
    while (detectionQueue && xQueueReceive(detectionQueue, &ev, 0) == pdTRUE) {
      if (ev.generation != calibrationGeneration) continue;
      calibrationNote = ev.note;
      calibrationFreq = ev.freq;
      calibrationRms = ev.rms;
      calibrationLastHit = millis();
      calibrationValueDirty = true;
    }
    return;
  }

  if (appState != APP_PLAYING || noteResolved) {
    DetectionEvent trash;
    while (detectionQueue && xQueueReceive(detectionQueue, &trash, 0) == pdTRUE) {}
    return;
  }

  DetectionEvent ev;
  while (detectionQueue && xQueueReceive(detectionQueue, &ev, 0) == pdTRUE) {
    // A completed FFT from the previous note may arrive after the next note has begun.
    // Generation tagging makes that event impossible to score on the new target.
    if (ev.generation != currentNoteGeneration) {
      Serial.print("STALE HIT IGNORED generation=");
      Serial.print(ev.generation);
      Serial.print(" current=");
      Serial.println(currentNoteGeneration);
      continue;
    }

    uint32_t hitTime = ev.hitMs;
    int target = targetNote();
    Serial.println();
    Serial.println("------ GAME RESULT ------");
    Serial.print("Target: "); Serial.println(noteName[target]);
    Serial.print("Detected: "); Serial.println(noteName[ev.note]);
    Serial.print("Frequency: "); Serial.println(ev.freq, 2);

    if (ev.note == target) {
      JudgeResult jr = static_cast<JudgeResult>(judgeCorrectHit(hitTime));
      int err = (int)hitTime - (int)noteStartTime;
      Serial.print("Timing from note start: "); Serial.print(err); Serial.print(" ms -> ");
      if (jr == JUDGE_PERFECT) Serial.println("PERFECT");
      else if (jr == JUDGE_GREAT) Serial.println("GREAT");
      else Serial.println("GOOD");
      resolveNote(jr, err);
    } else {
      // V5.6: one wrong spectral decision is not enough to punish the player.
      // Long aluminium-bar sustain can leak into the next note. Require the same wrong
      // note twice during the current window before resolving BAD.
      if (ev.note == lastWrongNote) wrongStrikeCount++;
      else {
        lastWrongNote = ev.note;
        wrongStrikeCount = 1;
      }

      if (wrongStrikeCount >= 2) {
        Serial.print("WRONG CONFIRMED x");
        Serial.print(wrongStrikeCount);
        Serial.println(" -> BAD");
        resolveNote(JUDGE_BAD, 0);
      } else {
        Serial.println("WRONG CANDIDATE x1 -> ignored, try again");
      }
    }
    if (noteResolved) break; // unresolved first wrong candidate keeps the note alive
  }
}

void updateNoteFlow() {
  static uint32_t lastTick = 0;
  uint32_t now = millis();
  if (appState != APP_PLAYING) { lastTick = now; return; }
  if (lastTick == 0) lastTick = now;
  uint32_t dt = now - lastTick;
  lastTick = now;

  if (!noteResolved) {
    if (!audioReadyForHit || audioProcessing) {
      noteStartTime += dt;
      hitIdealTime += dt;
      return;
    }
    if (now - noteStartTime >= noteWindowMs) {
      Serial.println("MISS -> FALL");
      resolveNote(JUDGE_MISS, 0);
    }
  } else if (now >= resolveUntil) {
    if (lifeHalfUnits <= 0) finishGame(true);
    else advanceNote();
  }
}

void enterCalibration() {
  appState = APP_CALIBRATION;
  calibrationNote = -1;
  calibrationFreq = 0;
  calibrationRms = 0;
  calibrationValueDirty = true;
  resultRendered = false;
  if (detectionQueue) {
    DetectionEvent trash;
    while (xQueueReceive(detectionQueue, &trash, 0) == pdTRUE) {}
  }
  calibrationGeneration = ++audioGeneration;
  audioEnabled = true;
  audioAcceptHits = true;
  audioReadyForHit = false;
  audioNewNoteRequest = true;
}

void leaveToHome() {
  stopEndingAudio();
  audioAcceptHits = false;
  audioEnabled = false;
  appState = APP_HOME;
  resultRendered = false;
}

// ============================================================
// World movement
// ============================================================
void updateClouds() {
  for (int i = 0; i < 3; i++) {
    clouds[i].x -= clouds[i].speed;

    if (
      clouds[i].x <
      -clouds[i].sp->w
    ) {
      clouds[i].x =
        SCREEN_W + random(10, 80);

      clouds[i].y =
        random(8, 58);
    }
  }
}

void updateObstacle() {
  if (
    appState != APP_PLAYING ||
    noteResolved
  ) {
    return;
  }

  float p =
    (float)(
      millis() -
      noteStartTime
    ) /
    (float)noteWindowMs;

  p =
    constrain(
      p,
      0.0f,
      1.0f
    );

  const float startX = 302.0f;
  const float endX =
    JUDGE_X - 8.0f;

  // smoothstep，让移动更自然
  float s =
    p * p *
    (3.0f - 2.0f * p);

  obstacle.x =
    startX +
    (endX - startX) *
    s;
}

// ============================================================
// Rendering
// ============================================================
void drawSprite(
  const Sprite565 &sp,
  int x,
  int y
) {
  gameCanvas.drawRGBBitmap(
    x,
    y,
    sp.bitmap,
    sp.mask,
    sp.w,
    sp.h
  );
}

void drawLifeHeart(int x, int y, int activeHalfUnits) {
  // 16x14 像素心形。activeHalfUnits: 0=灰，1=左半红，2=全红。
  static const char *heart[14] = {
    "  ####  ####    ",
    " ###### ######  ",
    "##############  ",
    "##############  ",
    "##############  ",
    " ############   ",
    "  ##########    ",
    "   ########     ",
    "    ######      ",
    "     ####       ",
    "      ##        ",
    "                ",
    "                ",
    "                "
  };

  for (int yy = 0; yy < 14; yy++) {
    for (int xx = 0; xx < 16; xx++) {
      if (heart[yy][xx] != '#') continue;
      bool active = (activeHalfUnits >= 2) || (activeHalfUnits == 1 && xx < 8);
      tft.drawPixel(x + xx, y + yy, active ? ST77XX_RED : 0x4208);
    }
  }
}

void drawTopUI() {
  tft.fillRect(
    0,
    0,
    SCREEN_W,
    UI_H,
    ST77XX_BLACK
  );

  tft.drawFastHLine(
    0,
    UI_H - 1,
    SCREEN_W,
    ST77XX_WHITE
  );

  tft.setTextWrap(false);
  tft.setTextSize(1);

  if (appState == APP_PLAYING) {
    tft.setTextColor(ST77XX_WHITE);

    tft.setCursor(5, 5);
    tft.print("SCORE ");
    tft.print(score);

    tft.setCursor(78, 5);
    tft.print("COMBO ");
    tft.print(combo);

    tft.setCursor(158, 5);
    tft.print("STEP ");
    tft.print(songIndex + 1);
    tft.print("/");
    tft.print(songLength());

    tft.setTextColor(ST77XX_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(260, 4);
    tft.print(
      noteName[
        targetNote()
      ]
    );

    tft.setTextSize(1);
    tft.setTextColor(ST77XX_RED);

    tft.setCursor(5, 23);
    tft.print("LIFE");

    for (int i = 0; i < 3; i++) {
      int remain = lifeHalfUnits - i * 2;
      int activeHalf = constrain(remain, 0, 2);
      drawLifeHeart(40 + i * 22, 20, activeHalf);
    }

    // 小型节奏进度条
    float p =
      (float)(
        millis() -
        noteStartTime
      ) /
      (float)noteWindowMs;

    p =
      constrain(
        p,
        0.0f,
        1.0f
      );

    tft.drawRect(
      112,
      23,
      138,
      9,
      ST77XX_WHITE
    );

    tft.fillRect(
      114,
      25,
      (int)(134 * p),
      5,
      ST77XX_CYAN
    );
  }

  else if (appState == APP_RESULT) {
    if (gameFailed) {
      // 失败页使用整屏图片，不再绘制顶部FINISH栏。
      return;
    }
    tft.setTextColor(ST77XX_GREEN);
    tft.setTextSize(2);
    tft.setCursor(8, 12);
    tft.print("FINISH!");
  }
}


void drawOutfitOverlay(int x, int y, int w, int h) {
  int cx = x + w / 2;
  int headY = y + max(2, h / 8);
  int bodyY = y + h / 2;
  int footY = y + h - 4;

  // CAP layer
  int cap = equippedByCategory[CAT_CAP];
  if (cap >= 0) {
    int local=0; for(int i=0;i<cap;i++) if(OUTFITS[i].category==CAT_CAP) local++;
    if (local == 0) { // sunny cap
      gameCanvas.fillRoundRect(cx-11,headY-4,22,8,3,0xD8A7);
      gameCanvas.fillRect(cx-8,headY-6,16,4,ST77XX_WHITE);
      gameCanvas.fillRect(cx+6,headY+3,8,2,0x8000);
      gameCanvas.fillCircle(cx-2,headY-1,1,ST77XX_RED);
    } else if (local == 1) { // black rx
      gameCanvas.fillRoundRect(cx-11,headY-4,22,8,3,0x2104);
      gameCanvas.drawFastHLine(cx-7,headY,14,ST77XX_WHITE);
      gameCanvas.fillRect(cx+6,headY+3,8,2,ST77XX_WHITE);
    } else if (local == 2) { // pink heart
      gameCanvas.fillRoundRect(cx-11,headY-4,22,8,3,0xF97F);
      gameCanvas.fillCircle(cx-2,headY-1,2,ST77XX_RED);
      gameCanvas.fillCircle(cx+1,headY-1,2,ST77XX_RED);
    } else if (local == 3) { // cat ears
      gameCanvas.fillRoundRect(cx-10,headY-3,20,7,3,0x2104);
      gameCanvas.fillTriangle(cx-9,headY-2,cx-6,headY-10,cx-2,headY-2,0x2104);
      gameCanvas.fillTriangle(cx+2,headY-2,cx+6,headY-10,cx+9,headY-2,0x2104);
      gameCanvas.fillTriangle(cx-7,headY-3,cx-6,headY-7,cx-4,headY-3,ST77XX_MAGENTA);
      gameCanvas.fillTriangle(cx+4,headY-3,cx+6,headY-7,cx+7,headY-3,ST77XX_MAGENTA);
    } else if (local == 4) { // cream beret
      gameCanvas.fillEllipse(cx,headY,12,7,0xF6F0);
      gameCanvas.fillCircle(cx+7,headY+2,2,0x780F);
      gameCanvas.fillCircle(cx-4,headY-2,1,ST77XX_MAGENTA);
    } else { // STAR CAT
      gameCanvas.fillRoundRect(cx-10,headY-3,20,7,3,0x39FF);
      gameCanvas.fillTriangle(cx-9,headY-2,cx-6,headY-10,cx-2,headY-2,0x39FF);
      gameCanvas.fillTriangle(cx+2,headY-2,cx+6,headY-10,cx+9,headY-2,0x39FF);
      gameCanvas.fillCircle(cx,headY-1,2,ST77XX_YELLOW);
      gameCanvas.drawPixel(cx-1,headY-2,ST77XX_WHITE);
    }
  }

  // CLOTHES layer
  int cloth = equippedByCategory[CAT_CLOTHES];
  if (cloth >= 0) {
    int local=0; for(int i=0;i<cloth;i++) if(OUTFITS[i].category==CAT_CLOTHES) local++;
    if (local == 0) { // varsity
      gameCanvas.fillRoundRect(cx-13,bodyY-6,26,18,4,0xD8A7);
      gameCanvas.drawFastVLine(cx,bodyY-5,15,ST77XX_WHITE);
      gameCanvas.drawFastHLine(cx-11,bodyY+1,22,ST77XX_WHITE);
    } else if (local == 1) { // tee
      gameCanvas.fillRoundRect(cx-12,bodyY-5,24,17,3,ST77XX_WHITE);
      gameCanvas.fillCircle(cx-2,bodyY+2,3,ST77XX_RED);
      gameCanvas.fillCircle(cx+2,bodyY+2,3,ST77XX_RED);
    } else if (local == 2) { // music hoodie
      gameCanvas.fillRoundRect(cx-13,bodyY-6,26,18,4,0x2104);
      gameCanvas.drawFastHLine(cx-11,bodyY+7,22,ST77XX_MAGENTA);
      gameCanvas.drawLine(cx-10,bodyY+8,cx+10,bodyY-2,ST77XX_MAGENTA);
      gameCanvas.fillCircle(cx+5,bodyY,2,ST77XX_YELLOW);
    } else if (local == 3) { // sailor
      gameCanvas.fillRoundRect(cx-12,bodyY-5,24,17,3,0xC65F);
      gameCanvas.fillTriangle(cx-4,bodyY-3,cx,bodyY+3,cx+4,bodyY-3,ST77XX_WHITE);
      gameCanvas.fillCircle(cx,bodyY+4,2,ST77XX_RED);
    } else if (local == 4) { // mint cardigan
      gameCanvas.fillRoundRect(cx-13,bodyY-6,26,18,4,0x87F0);
      gameCanvas.drawFastVLine(cx,bodyY-5,15,ST77XX_WHITE);
      gameCanvas.fillCircle(cx,bodyY,1,ST77XX_YELLOW);
      gameCanvas.fillCircle(cx,bodyY+5,1,ST77XX_YELLOW);
    } else { // neon hoodie
      gameCanvas.fillRoundRect(cx-13,bodyY-6,26,18,4,0x2128);
      gameCanvas.drawRect(cx-11,bodyY-4,22,14,0x39FF);
      gameCanvas.drawLine(cx-10,bodyY+8,cx+9,bodyY-3,ST77XX_MAGENTA);
      gameCanvas.fillCircle(cx+5,bodyY+2,2,ST77XX_YELLOW);
    }
  }

  // SHOES layer
  int shoes = equippedByCategory[CAT_SHOES];
  if (shoes >= 0) {
    int local=0; for(int i=0;i<shoes;i++) if(OUTFITS[i].category==CAT_SHOES) local++;
    uint16_t c = 0xD8A7;
    if(local==1)c=0xA55F; else if(local==2)c=0x2104; else if(local==3)c=ST77XX_WHITE;
    else if(local==4)c=0xF97F; else if(local==5)c=0x39FF;
    gameCanvas.fillRoundRect(cx-15,footY-4,12,6,2,c);
    gameCanvas.fillRoundRect(cx+3,footY-4,12,6,2,c);
    gameCanvas.drawFastHLine(cx-14,footY+1,10,ST77XX_WHITE);
    gameCanvas.drawFastHLine(cx+4,footY+1,10,ST77XX_WHITE);
    if(local==5){ gameCanvas.drawPixel(cx-16,footY-3,ST77XX_YELLOW); gameCanvas.drawPixel(cx+16,footY-3,ST77XX_YELLOW); }
  }

  // ACCESSORY layer
  int acc = equippedByCategory[CAT_ACCESSORIES];
  if (acc >= 0) {
    int local=0; for(int i=0;i<acc;i++) if(OUTFITS[i].category==CAT_ACCESSORIES) local++;
    if (local == 0) { // bow
      gameCanvas.fillTriangle(cx-14,bodyY-12,cx-8,bodyY-8,cx-14,bodyY-4,ST77XX_MAGENTA);
      gameCanvas.fillTriangle(cx-2,bodyY-12,cx-8,bodyY-8,cx-2,bodyY-4,ST77XX_MAGENTA);
      gameCanvas.fillCircle(cx-8,bodyY-8,2,ST77XX_YELLOW);
    } else if (local == 1) { // heart glasses
      gameCanvas.drawCircle(cx-5,headY+5,4,ST77XX_MAGENTA);
      gameCanvas.drawCircle(cx+5,headY+5,4,ST77XX_MAGENTA);
      gameCanvas.drawFastHLine(cx-1,headY+5,3,ST77XX_MAGENTA);
    } else if (local == 2) { // pearl pin
      gameCanvas.fillCircle(cx+10,headY-4,2,ST77XX_WHITE);
      gameCanvas.fillCircle(cx+13,headY-2,2,ST77XX_WHITE);
      gameCanvas.fillCircle(cx+10,headY+1,2,ST77XX_WHITE);
    } else if (local == 3) { // music bag
      gameCanvas.drawLine(cx+7,bodyY-10,cx+14,bodyY+8,ST77XX_YELLOW);
      gameCanvas.fillRoundRect(cx+9,bodyY+2,9,8,2,0xF97F);
      gameCanvas.fillCircle(cx+13,bodyY+6,1,ST77XX_YELLOW);
    } else if (local == 4) { // bunny badge
      gameCanvas.fillCircle(cx-9,bodyY-3,4,ST77XX_WHITE);
      gameCanvas.fillCircle(cx-11,bodyY-8,2,ST77XX_WHITE);
      gameCanvas.fillCircle(cx-7,bodyY-8,2,ST77XX_WHITE);
      gameCanvas.drawPixel(cx-10,bodyY-3,ST77XX_BLACK);
      gameCanvas.drawPixel(cx-8,bodyY-3,ST77XX_BLACK);
    } else { // star charm
      gameCanvas.fillTriangle(cx+11,bodyY-10,cx+13,bodyY-5,cx+18,bodyY-5,ST77XX_YELLOW);
      gameCanvas.fillTriangle(cx+18,bodyY-5,cx+14,bodyY-1,cx+16,bodyY+4,ST77XX_YELLOW);
      gameCanvas.drawFastVLine(cx+13,bodyY-13,4,ST77XX_CYAN);
    }
  }

  // Surprise: complete the four galaxy pieces to get a sparkling trail in gameplay.
  if (galaxySetEquipped()) {
    uint32_t t = millis()/90;
    for(int k=0;k<4;k++) {
      int sx = x - 5 - ((t + k*7) % 18);
      int sy = y + 8 + ((t*3 + k*11) % max(12,h-12));
      uint16_t col = (k%2==0) ? ST77XX_YELLOW : ST77XX_CYAN;
      gameCanvas.drawPixel(sx,sy,col);
      gameCanvas.drawFastHLine(sx-1,sy,3,col);
      gameCanvas.drawFastVLine(sx,sy-1,3,col);
    }
  }
}

void drawPlayer() {
  uint8_t count = 0;
  const Sprite565 *frames = getFrames(animState, count);
  if (count == 0) return;
  const Sprite565 &sp = frames[animFrame % count];

  int x = PLAYER_X + 6;
  int y = PLAYER_GROUND_Y - sp.h;
  uint32_t now = millis();

  if (animState == ANIM_RUN) {
    const int bob[4] = {0,-2,0,1};
    y += bob[animFrame % 4];
  } else if (animState == ANIM_JUMP) {
    float u=(float)(now-animStateStartTime)/(float)JUMP_TOTAL_MS;
    u=constrain(u,0.0f,1.0f);
    float jump=44.0f*(1.0f-(2*u-1)*(2*u-1));
    y -= (int)jump;
    x += (int)(10.0f*sinf(u*PI));
  } else if (animState == ANIM_HIT) {
    x -= 6;
    if (((now/45)%2)==0) x -= 2;
  } else if (animState == ANIM_FALL) {
    float u=(float)(now-animStateStartTime)/(float)FALL_TOTAL_MS;
    u=constrain(u,0.0f,1.0f);
    x -= (int)(u*10);
    y += (int)(u*12);
  } else if (animState == ANIM_CHEER) {
    const int bounce[3]={0,-6,-2};
    y += bounce[animFrame%3];
  }
  drawSprite(sp,x,y);
}

void drawObstacle() {
  if (appState != APP_PLAYING) {
    return;
  }

  // 判定线
  gameCanvas.drawFastVLine(
    JUDGE_X,
    102,
    75,
    ST77XX_WHITE
  );

  drawSprite(
    *obstacle.sp,
    (int)obstacle.x,
    obstacle.y
  );

  // 目标音符气泡
  int bubbleX =
    (int)obstacle.x +
    obstacle.sp->w / 2 -
    18;

  bubbleX =
    constrain(
      bubbleX,
      5,
      278
    );

  int bubbleY =
    max(
      8,
      obstacle.y - 28
    );

  gameCanvas.fillRoundRect(
    bubbleX,
    bubbleY,
    38,
    23,
    6,
    ST77XX_WHITE
  );

  gameCanvas.drawRoundRect(
    bubbleX,
    bubbleY,
    38,
    23,
    6,
    ST77XX_BLUE
  );

  gameCanvas.setTextSize(2);
  gameCanvas.setTextColor(ST77XX_BLUE);

  gameCanvas.setCursor(
    bubbleX + 12,
    bubbleY + 4
  );

  gameCanvas.print(
    noteName[
      targetNote()
    ]
  );
}

void drawParticles() {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!particles[i].active) continue;

    gameCanvas.fillCircle(
      (int)particles[i].x,
      (int)particles[i].y,
      particles[i].radius,
      particles[i].color
    );
  }
}

void drawJudgeText() {
  if (appState != APP_PLAYING || judgeResult == JUDGE_NONE) return;
  gameCanvas.setTextSize(2);
  const char *txt = "";
  uint16_t col = ST77XX_WHITE;
  if (judgeResult == JUDGE_PERFECT) { txt="PERFECT!"; col=ST77XX_YELLOW; }
  else if (judgeResult == JUDGE_GREAT) { txt="GREAT!"; col=ST77XX_GREEN; }
  else if (judgeResult == JUDGE_GOOD) { txt="GOOD!"; col=ST77XX_CYAN; }
  else if (judgeResult == JUDGE_BAD) { txt="BAD!"; col=ST77XX_RED; }
  else if (judgeResult == JUDGE_MISS) { txt="MISS!"; col=ST77XX_MAGENTA; }
  gameCanvas.setTextColor(col);
  gameCanvas.setCursor(178, 18);
  gameCanvas.print(txt);
}

void renderPlayingFrame() {
  // 直接复制静态背景，明显快于逐像素重画
  memcpy(
    gameCanvas.getBuffer(),
    BG_MAIN_320x200,
    SCREEN_W *
    GAME_H *
    sizeof(uint16_t)
  );

  // 云层漂移
  for (int i = 0; i < 3; i++) {
    drawSprite(
      *clouds[i].sp,
      (int)clouds[i].x,
      clouds[i].y
    );
  }

  drawObstacle();
  drawPlayer();
  drawParticles();
  drawJudgeText();

  // 歌曲总进度
  int totalBarW = 294;

  gameCanvas.drawRoundRect(
    13,
    188,
    totalBarW,
    7,
    3,
    ST77XX_WHITE
  );

  int fill =
    (
      songIndex *
      (totalBarW - 4)
    ) /
    songLength();

  gameCanvas.fillRect(
    15,
    190,
    fill,
    3,
    ST77XX_GREEN
  );

  tft.drawRGBBitmap(
    0,
    UI_H,
    gameCanvas.getBuffer(),
    SCREEN_W,
    GAME_H
  );
}

void drawButton(int x, int y, int w, int h, uint16_t color, const char *label, uint16_t textColor=ST77XX_WHITE, uint8_t textSize=2) {
  tft.fillRoundRect(x,y,w,h,10,color);
  tft.drawRoundRect(x,y,w,h,10,ST77XX_WHITE);
  tft.setTextColor(textColor);
  tft.setTextSize(textSize);
  int16_t x1,y1; uint16_t tw,th;
  tft.getTextBounds(label,0,0,&x1,&y1,&tw,&th);
  tft.setCursor(x + (w-tw)/2, y + (h-th)/2);
  tft.print(label);
}

void drawHome() {
  audioEnabled = false;
  tft.setTextWrap(false);

  memcpy(
    gameCanvas.getBuffer(),
    BG_MAIN_320x200,
    SCREEN_W * GAME_H * sizeof(uint16_t)
  );

  for (int i = 0; i < 3; i++) {
    drawSprite(*clouds[i].sp, (int)clouds[i].x, clouds[i].y);
  }

  uint8_t homeFrameCount = 0;
  const Sprite565 *homeFrames = getFrames(ANIM_CHEER, homeFrameCount);
  if (!homeFrames || homeFrameCount == 0) {
    homeFrames = getFrames(ANIM_RUN, homeFrameCount);
  }
  if (homeFrames && homeFrameCount > 0) {
    const Sprite565 &sp = homeFrames[0];
    int hx = 10;
    int hy = PLAYER_GROUND_Y - sp.h + 4;
    gameCanvas.fillCircle(hx + sp.w / 2, hy + sp.h / 2, 34, 0xEF7D);
    drawSprite(sp, hx, hy);
  }

  tft.drawRGBBitmap(0, 0, gameCanvas.getBuffer(), SCREEN_W, GAME_H);
  tft.fillRect(0, 200, SCREEN_W, 40, 0x10A3);

  tft.setTextSize(3);
  tft.setTextColor(0x780F);
  tft.setCursor(74, 13);
  tft.print("RUOXI RUN");
  tft.setTextColor(0xFFE0);
  tft.setCursor(72, 11);
  tft.print("RUOXI RUN");

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(194, 39);
  tft.print("MUSIC RUNNER");

  drawButton(108, 58, 198, 32, 0xF94E, "PLAY", ST77XX_WHITE, 2);
  drawButton(108, 96, 198, 32, 0x801F, "CREATE", ST77XX_WHITE, 2);
  drawButton(108, 134, 198, 32, 0x39FF, "WARDROBE", ST77XX_WHITE, 1);
  drawButton(108, 172, 198, 28, 0x04FF, "CALIBRATE", ST77XX_WHITE, 1);

  tft.fillCircle(92, 45, 3, ST77XX_CYAN);
  tft.drawFastVLine(95, 31, 14, ST77XX_CYAN);
  tft.drawFastHLine(95, 31, 8, ST77XX_CYAN);
  tft.fillCircle(287, 48, 3, 0xF81F);
  tft.drawFastVLine(290, 34, 14, 0xF81F);

  tft.setTextSize(1);
  tft.setTextColor(0xBDF7);
  tft.setCursor(10, 216);
  tft.print("Xylophone Rhythm Game");

  tft.fillRoundRect(214, 207, 98, 25, 10, 0x3186);
  tft.drawRoundRect(214, 207, 98, 25, 10, ST77XX_YELLOW);
  tft.fillCircle(226, 219, 8, ST77XX_YELLOW);
  tft.setTextColor(0x2104);
  tft.setTextSize(1);
  tft.setCursor(223, 216);
  tft.print("$");
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(238, 216);
  tft.print("Coins:");
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(274, 216);
  tft.print(coins);
}

void drawCreateMode() {
  tft.fillScreen(0x10A3);
  tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(2);
  tft.setCursor(86, 8); tft.print("CREATE");
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  tft.setCursor(42, 30); tft.print("Choose one of 3 custom song slots");

  for (uint8_t slot = 0; slot < CUSTOM_SONG_SLOTS; ++slot) {
    int y = 48 + slot * 36;
    bool selected = slot == selectedCreateSlot;
    uint16_t c = selected ? ST77XX_BLUE : 0x2945;
    char label[40];
    if (customSongLen[slot] > 0)
      snprintf(label, sizeof(label), "MY SONG %u   %u notes", slot + 1, customSongLen[slot]);
    else
      snprintf(label, sizeof(label), "MY SONG %u   EMPTY", slot + 1);
    drawButton(28, y, 264, 30, c, label, ST77XX_WHITE, 1);
  }

  bool hasSong = customSongLen[selectedCreateSlot] > 0;
  drawButton(18, 160, 142, 34, 0xF94E, hasSong ? "RE-RECORD" : "START RECORD", ST77XX_WHITE, 1);
  uint16_t delColor = hasSong ? (deleteConfirmArmed ? ST77XX_RED : 0xB104) : 0x2104;
  drawButton(170, 160, 132, 34, delColor, deleteConfirmArmed ? "CONFIRM DELETE" : "DELETE", hasSong ? ST77XX_WHITE : 0x7BEF, 1);

  drawButton(8, 204, 70, 29, 0x4208, "BACK", ST77XX_WHITE, 1);
  drawButton(90, 204, 132, 29, ST77XX_GREEN, "OPEN LIBRARY", ST77XX_BLACK, 1);

  if (uiMessage.length()) {
    tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
    tft.setCursor(228, 214); tft.print(uiMessage.substring(0, 14));
  }
}

void drawRecordingMode() {
  tft.fillScreen(0x080F);
  tft.fillCircle(26, 24, 8, ST77XX_RED);
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2);
  tft.setCursor(44, 17); tft.print("RECORDING #"); tft.print(selectedCreateSlot + 1);
  tft.setTextSize(1); tft.setTextColor(0xBDF7);
  tft.setCursor(44, 42); tft.print("Hit one bar at a time");

  tft.fillRoundRect(20, 62, 280, 92, 10, 0x18E3);
  tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2);
  tft.setCursor(34, 74); tft.print("NOTES: "); tft.print(recordCount); tft.print("/64");

  tft.setTextSize(2); tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(34, 108);
  int start = recordCount > 7 ? recordCount - 7 : 0;
  for (int i=start; i<recordCount; ++i) {
    tft.print(noteName[recordNotes[i]]);
    if (i + 1 < recordCount) tft.print(" ");
  }

  drawButton(8, 178, 72, 40, 0x4208, "BACK", ST77XX_WHITE, 1);
  drawButton(88, 178, 72, 40, 0xFD20, "UNDO", ST77XX_BLACK, 1);
  drawButton(168, 178, 144, 40, ST77XX_RED, "FINISH + SAVE", ST77XX_WHITE, 1);
  tft.setTextColor(0x7BEF); tft.setTextSize(1);
  tft.setCursor(14, 224); tft.print("UNDO removes the last captured note");
}

void drawSongSelect() {
  tft.fillScreen(0x10A3);
  tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(2);
  tft.setCursor(74,8); tft.print("SELECT SONG");

  const int pageCount = (SONG_COUNT + SONGS_PER_PAGE - 1) / SONGS_PER_PAGE;
  songPage = constrain(songPage, 0, pageCount - 1);
  int first = songPage * SONGS_PER_PAGE;

  for (int row=0; row<SONGS_PER_PAGE; row++) {
    int i = first + row;
    if (i >= SONG_COUNT) break;
    int y = 38 + row * 36;
    uint16_t c = (i==selectedSong) ? ST77XX_BLUE : 0x4208;
    char label[44];
    if (isCustomSongIndex(i) && !songUnlocked(i)) {
      int slot = customSlotFromSongIndex(i);
      snprintf(label,sizeof(label),"My Song %d  (EMPTY - CREATE)", slot + 1);
      if (i==selectedSong) c = 0x801F;
    } else if (songUnlocked(i)) {
      if (bestScores[i] > 0) snprintf(label,sizeof(label),"%s BEST:%u",SONGS[i].name,bestScores[i]);
      else snprintf(label,sizeof(label),"%s",SONGS[i].name);
    } else {
      snprintf(label,sizeof(label),"%s LOCK %u",SONGS[i].name,SONGS[i].unlockCost);
      if (i==selectedSong) c = 0xFD20;
    }
    drawButton(20,y,280,30,c,label,ST77XX_WHITE,1);
  }

  tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
  tft.setCursor(116,187);
  if (uiMessage.length()) tft.print(uiMessage);
  else { tft.print("Coins: "); tft.print(coins); }

  char pg[16];
  snprintf(pg,sizeof(pg),"%d/%d",songPage+1,pageCount);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(149,216); tft.print(pg);

  drawButton(8,206,62,28,0x4208,"BACK",ST77XX_WHITE,1);
  drawButton(78,206,50,28,(songPage>0?0x39E7:0x2104),"<",ST77XX_WHITE,2);
  drawButton(186,206,50,28,(songPage<pageCount-1?0x39E7:0x2104),">",ST77XX_WHITE,2);
  if (isCustomSongIndex(selectedSong) && !songUnlocked(selectedSong))
    drawButton(240,206,76,28,0x801F,"CREATE",ST77XX_WHITE,1);
  else if (songUnlocked(selectedSong))
    drawButton(244,206,68,28,ST77XX_GREEN,"PLAY",ST77XX_BLACK,1);
  else
    drawButton(240,206,76,28,0xFD20,"UNLOCK",ST77XX_BLACK,1);
}

uint16_t outfitAccentColor(int idx) {
  if (idx < 0 || idx >= OUTFIT_COUNT) return 0x7BEF;
  uint8_t cat = OUTFITS[idx].category;
  if (cat == CAT_CAP) return 0xF97F;
  if (cat == CAT_CLOTHES) return 0x6D7F;
  if (cat == CAT_SHOES) return 0xA55F;
  return ST77XX_MAGENTA;
}

void drawCapIcon(int x,int y,int variant,bool selected) {
  uint16_t c[] = {0xD8A7,0x2104,0xF97F,0x2104,0xF6F0,0x39FF};
  uint16_t cc = c[variant % 6];
  if (variant == 4) {
    tft.fillEllipse(x+16,y+12,12,7,cc);
    tft.fillCircle(x+23,y+13,2,0x780F);
    tft.fillCircle(x+11,y+9,1,ST77XX_MAGENTA);
  } else if (variant == 3 || variant == 5) {
    tft.fillRoundRect(x+5,y+9,22,9,4,cc);
    tft.fillTriangle(x+7,y+10,x+11,y+2,x+15,y+10,cc);
    tft.fillTriangle(x+18,y+10,x+22,y+2,x+26,y+10,cc);
    if(variant==3){ tft.fillTriangle(x+9,y+9,x+11,y+5,x+13,y+9,ST77XX_MAGENTA); tft.fillTriangle(x+20,y+9,x+22,y+5,x+24,y+9,ST77XX_MAGENTA); }
    else { tft.fillCircle(x+16,y+12,2,ST77XX_YELLOW); tft.drawPixel(x+16,y+11,ST77XX_WHITE); }
  } else {
    tft.fillRoundRect(x+5,y+8,22,9,4,cc);
    tft.fillRect(x+20,y+15,9,3,cc);
    if (variant==0) { tft.fillCircle(x+14,y+11,2,ST77XX_WHITE); tft.drawPixel(x+14,y+11,ST77XX_RED); }
    if (variant==1) { tft.drawFastHLine(x+10,y+11,10,ST77XX_WHITE); }
    if (variant==2) { tft.fillCircle(x+14,y+11,2,ST77XX_RED); tft.fillCircle(x+17,y+11,2,ST77XX_RED); }
  }
  if (selected) tft.drawRoundRect(x,y,34,26,5,ST77XX_YELLOW);
}

void drawClothesIcon(int x,int y,int variant,bool selected) {
  uint16_t c[] = {0xD8A7,ST77XX_WHITE,0x2104,0xC65F,0x87F0,0x2128};
  uint16_t cc=c[variant%6];
  tft.drawLine(x+6,y+5,x+17,y+1,ST77XX_WHITE);
  tft.drawLine(x+17,y+1,x+28,y+5,ST77XX_WHITE);
  tft.fillRoundRect(x+8,y+7,18,15,4,cc);
  tft.fillTriangle(x+8,y+8,x+3,y+13,x+8,y+15,cc);
  tft.fillTriangle(x+26,y+8,x+31,y+13,x+26,y+15,cc);
  if (variant==0) { tft.drawFastVLine(x+17,y+8,13,ST77XX_WHITE); tft.drawFastHLine(x+9,y+13,16,ST77XX_WHITE); }
  else if (variant==1) { tft.fillCircle(x+15,y+13,2,ST77XX_RED); tft.fillCircle(x+18,y+13,2,ST77XX_RED); }
  else if (variant==2) { tft.drawLine(x+10,y+19,x+24,y+9,ST77XX_MAGENTA); tft.fillCircle(x+21,y+11,2,ST77XX_YELLOW); }
  else if (variant==3) { tft.fillTriangle(x+13,y+9,x+17,y+14,x+21,y+9,ST77XX_WHITE); tft.fillCircle(x+17,y+15,2,ST77XX_RED); }
  else if (variant==4) { tft.drawFastVLine(x+17,y+8,13,ST77XX_WHITE); tft.fillCircle(x+17,y+13,1,ST77XX_YELLOW); }
  else { tft.drawRect(x+10,y+9,14,11,0x39FF); tft.drawLine(x+10,y+19,x+24,y+9,ST77XX_MAGENTA); }
  if (selected) tft.drawRoundRect(x,y,34,26,5,ST77XX_YELLOW);
}

void drawShoesIcon(int x,int y,int variant,bool selected) {
  uint16_t c[] = {0xD8A7,0xA55F,0x2104,ST77XX_WHITE,0xF97F,0x39FF};
  uint16_t cc=c[variant%6];
  tft.fillRoundRect(x+4,y+12,12,8,3,cc);
  tft.fillRoundRect(x+17,y+12,12,8,3,cc);
  tft.drawFastHLine(x+5,y+19,10,ST77XX_WHITE);
  tft.drawFastHLine(x+18,y+19,10,ST77XX_WHITE);
  if(variant==0){ tft.drawLine(x+7,y+14,x+12,y+18,ST77XX_WHITE); tft.drawLine(x+20,y+14,x+25,y+18,ST77XX_WHITE); }
  else if(variant==2){ tft.drawFastVLine(x+9,y+10,7,ST77XX_MAGENTA); tft.drawFastVLine(x+22,y+10,7,ST77XX_MAGENTA); }
  else if(variant==3){ tft.fillCircle(x+8,y+14,1,ST77XX_CYAN); tft.fillCircle(x+21,y+14,1,ST77XX_CYAN); }
  else if(variant==4){ tft.drawFastHLine(x+6,y+15,8,ST77XX_WHITE); tft.drawFastHLine(x+19,y+15,8,ST77XX_WHITE); }
  else if(variant==5){ tft.drawPixel(x+3,y+11,ST77XX_YELLOW); tft.drawPixel(x+30,y+11,ST77XX_YELLOW); }
  if(selected) tft.drawRoundRect(x,y,34,26,5,ST77XX_YELLOW);
}

void drawAccessoryIcon(int x,int y,int variant,bool selected) {
  if (variant==0) {
    tft.fillTriangle(x+6,y+9,x+14,y+13,x+6,y+17,ST77XX_MAGENTA);
    tft.fillTriangle(x+26,y+9,x+18,y+13,x+26,y+17,ST77XX_MAGENTA);
    tft.fillCircle(x+16,y+13,3,ST77XX_YELLOW);
  } else if (variant==1) {
    tft.drawCircle(x+11,y+13,6,ST77XX_MAGENTA); tft.drawCircle(x+23,y+13,6,ST77XX_MAGENTA); tft.drawFastHLine(x+16,y+13,3,ST77XX_MAGENTA);
  } else if (variant==2) {
    tft.fillCircle(x+14,y+11,3,ST77XX_WHITE); tft.fillCircle(x+20,y+11,3,ST77XX_WHITE); tft.fillCircle(x+17,y+16,3,ST77XX_WHITE);
  } else if (variant==3) {
    tft.drawLine(x+10,y+5,x+22,y+21,ST77XX_YELLOW); tft.fillRoundRect(x+16,y+12,10,9,2,0xF97F); tft.fillCircle(x+21,y+16,2,ST77XX_YELLOW);
  } else if (variant==4) {
    tft.fillCircle(x+17,y+13,7,ST77XX_WHITE); tft.fillCircle(x+13,y+5,3,ST77XX_WHITE); tft.fillCircle(x+21,y+5,3,ST77XX_WHITE); tft.drawPixel(x+15,y+12,ST77XX_BLACK); tft.drawPixel(x+19,y+12,ST77XX_BLACK);
  } else {
    tft.fillTriangle(x+17,y+3,x+20,y+10,x+28,y+10,ST77XX_YELLOW); tft.fillTriangle(x+28,y+10,x+22,y+15,x+24,y+23,ST77XX_YELLOW); tft.fillTriangle(x+24,y+23,x+17,y+18,x+10,y+23,ST77XX_YELLOW); tft.fillTriangle(x+10,y+23,x+12,y+15,x+6,y+10,ST77XX_YELLOW); tft.fillTriangle(x+6,y+10,x+14,y+10,x+17,y+3,ST77XX_YELLOW); tft.fillCircle(x+17,y+13,3,0x39FF);
  }
  if(selected) tft.drawRoundRect(x,y,34,26,5,ST77XX_YELLOW);
}

void drawWardrobeItemIcon(int x,int y,int idx,bool selected) {
  int local=0;
  for(int i=0;i<idx;i++) if(OUTFITS[i].category==OUTFITS[idx].category) local++;
  if (OUTFITS[idx].category==CAT_CAP) drawCapIcon(x,y,local,selected);
  else if (OUTFITS[idx].category==CAT_CLOTHES) drawClothesIcon(x,y,local,selected);
  else if (OUTFITS[idx].category==CAT_SHOES) drawShoesIcon(x,y,local,selected);
  else drawAccessoryIcon(x,y,local,selected);
}

void drawCabinetBackground() {
  // Warm wood cabinet
  uint16_t wood = 0x9B85;
  uint16_t darkWood = 0x6243;
  uint16_t shelf = 0xBCA7;
  tft.fillRect(6,34,308,164,wood);
  tft.fillRect(10,38,300,156,darkWood);
  tft.drawRect(6,34,308,164,ST77XX_WHITE);
  tft.fillRect(10,75,300,5,shelf);
  tft.fillRect(10,118,300,5,shelf);
  tft.fillRect(10,161,300,5,shelf);
  for(int x=18;x<310;x+=58) tft.fillCircle(x,43,2,ST77XX_YELLOW);
  // drawers
  tft.fillRoundRect(18,169,82,20,4,wood);
  tft.fillRoundRect(119,169,82,20,4,wood);
  tft.fillRoundRect(220,169,82,20,4,wood);
  tft.drawFastHLine(52,179,14,0xFEA0);
  tft.drawFastHLine(153,179,14,0xFEA0);
  tft.drawFastHLine(254,179,14,0xFEA0);
}

void drawWardrobeGrid() {
  drawCabinetBackground();
  int count = countItemsInCategory(wardrobeCategory);
  const int perPage = 6;
  int pages = max(1, (count + perPage - 1)/perPage);
  if (wardrobePage >= pages) wardrobePage = pages-1;
  int first = wardrobePage * perPage;
  for(int slot=0;slot<perPage;slot++) {
    int idx = nthItemInCategory(wardrobeCategory, first + slot);
    if (idx < 0) continue;
    int col = slot % 3;
    int row = slot / 3;
    int bx = 24 + col*96;
    int by = 47 + row*43;
    bool sel = idx == wardrobeSelection;
    if(sel) {
      tft.fillRoundRect(bx-4,by-4,84,34,6,0x2945);
      tft.drawRoundRect(bx-4,by-4,84,34,6,ST77XX_YELLOW);
    }
    drawWardrobeItemIcon(bx,by,idx,sel);
    tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
    tft.setCursor(bx+38,by+5); tft.print(OUTFITS[idx].name);
    tft.setCursor(bx+38,by+16);
    if (outfitEquipped(idx)) tft.print("EQUIPPED");
    else if (outfitOwned(idx)) tft.print("OWNED");
    else { tft.print(OUTFITS[idx].cost); tft.print(" C"); }
  }

  if (pages > 1) {
    tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
    tft.setCursor(142,145); tft.print(wardrobePage+1); tft.print("/"); tft.print(pages);
    drawButton(10,140,42,18,0x4208,"<",ST77XX_WHITE,1);
    drawButton(268,140,42,18,0x4208,">",ST77XX_WHITE,1);
  }
}

void drawCategoryTabs() {
  const int x[4]={4,83,162,241};
  const int w[4]={75,75,75,75};
  for(int c=0;c<CAT_COUNT;c++) {
    uint16_t fill = (c==wardrobeCategory) ? 0xF97F : 0x4208;
    drawButton(x[c],202,w[c],30,fill,CATEGORY_NAMES[c],ST77XX_WHITE,1);
  }
}

void drawWardrobeModal() {
  if (!wardrobeModalVisible || wardrobePendingIndex < 0 || wardrobePendingIndex >= OUTFIT_COUNT) return;
  const OutfitDef &of = OUTFITS[wardrobePendingIndex];
  bool owned = outfitOwned(wardrobePendingIndex);
  bool equipped = outfitEquipped(wardrobePendingIndex);

  tft.fillRoundRect(28,54,264,126,12,0x1082);
  tft.drawRoundRect(28,54,264,126,12,ST77XX_WHITE);
  tft.drawRoundRect(32,58,256,118,10,outfitAccentColor(wardrobePendingIndex));
  drawWardrobeItemIcon(42,72,wardrobePendingIndex,true);
  tft.setTextSize(2); tft.setTextColor(outfitAccentColor(wardrobePendingIndex));
  tft.setCursor(84,70); tft.print(of.name);
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(84,94); tft.print(of.desc);
  int localIndex=0; for(int i=0;i<wardrobePendingIndex;i++) if(OUTFITS[i].category==of.category) localIndex++;
  if(localIndex==5){ tft.setTextColor(ST77XX_CYAN); tft.setCursor(84,104); tft.print("SURPRISE SET PIECE"); tft.setTextColor(ST77XX_WHITE); }
  tft.setCursor(84,118);
  if(equipped) tft.print("Already equipped");
  else if(owned) tft.print("Equip this item?");
  else { tft.print("Spend "); tft.print(of.cost); tft.print(" coins?"); }
  tft.setCursor(84,131); tft.print("Coins: "); tft.print(coins);
  if(!owned && coins>=of.cost){ tft.print(" -> "); tft.print(coins-of.cost); }
  if(equipped) drawButton(58,148,90,24,0x4208,"CLOSE",ST77XX_WHITE,1);
  else if(owned) drawButton(58,148,90,24,ST77XX_GREEN,"EQUIP",ST77XX_BLACK,1);
  else drawButton(58,148,90,24,ST77XX_GREEN,"BUY",ST77XX_BLACK,1);
  drawButton(172,148,90,24,0x4208,"EXIT",ST77XX_WHITE,1);
}



void drawSpriteToTFT(const Sprite565 &sp, int x, int y) {
  tft.drawRGBBitmap(x, y, sp.bitmap, sp.mask, sp.w, sp.h);
}

void drawWardrobeArrow(int cx, int cy, bool right) {
  tft.fillCircle(cx, cy, 19, 0xFFDF);
  tft.drawCircle(cx, cy, 19, 0x79E0);
  tft.drawCircle(cx, cy, 18, 0xBC88);
  uint16_t c = 0x5125;
  if (right) {
    tft.drawLine(cx-5,cy-8,cx+5,cy,c);
    tft.drawLine(cx+5,cy,cx-5,cy+8,c);
    tft.drawLine(cx-4,cy-8,cx+6,cy,c);
    tft.drawLine(cx+6,cy,cx-4,cy+8,c);
  } else {
    tft.drawLine(cx+5,cy-8,cx-5,cy,c);
    tft.drawLine(cx-5,cy,cx+5,cy+8,c);
    tft.drawLine(cx+4,cy-8,cx-6,cy,c);
    tft.drawLine(cx-6,cy,cx+4,cy+8,c);
  }
}

void drawWardrobeBase() {
  // One shared UI for every outfit. This replaces six 320x240 full-screen bitmaps.
  // Result: much smaller APP size and the coin value is always the real saved value.
  const uint16_t woodDark = 0x6243;
  const uint16_t wood = 0x9B85;
  const uint16_t cream = 0xFFDF;
  const uint16_t panel = 0xF6D2;
  const uint16_t brown = 0x5125;

  tft.fillScreen(woodDark);

  // top wooden sign
  tft.fillRoundRect(77,4,166,31,9,wood);
  tft.drawRoundRect(77,4,166,31,9,0xBC88);
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2);
  tft.setCursor(108,10); tft.print("RUOXI RUN");
  tft.setTextSize(1);
  tft.setCursor(126,27); tft.print("WARDROBE");

  // real coin counter -- no hard-coded 1280 in artwork
  tft.fillRoundRect(250,7,64,24,9,cream);
  tft.drawRoundRect(250,7,64,24,9,0xFEA0);
  tft.fillCircle(260,19,7,0xFEA0);
  tft.setTextColor(brown); tft.setTextSize(1);
  tft.setCursor(271,15); tft.print(coins);

  // wardrobe / stage
  tft.fillRoundRect(6,40,188,158,8,wood);
  tft.fillRoundRect(12,46,176,146,7,0xFDD2);
  tft.drawRoundRect(12,46,176,146,7,0xFEA0);
  // spotlight
  tft.fillTriangle(83,47,117,47,145,185,0xFEF5);
  tft.fillRoundRect(46,174,108,12,6,0xFBAE);

  // selected girl: wardrobe and game use assets generated from this same source image.
  const Sprite565 *preview =
      (wardrobeSkin == 5) ? &V54_PINK_PREVIEW : SKIN_PREVIEW_TABLE[wardrobeSkin];

  // Same bottom alignment used by every other character.
  int px = 100 - preview->w/2;
  int py = 184 - preview->h;
  drawSpriteToTFT(*preview, px, py);

  drawWardrobeArrow(32,119,false);
  drawWardrobeArrow(168,119,true);

  // right information card
  tft.fillRoundRect(198,43,116,154,8,panel);
  tft.drawRoundRect(198,43,116,154,8,0xBC88);
  tft.setTextColor(brown); tft.setTextSize(1);
  tft.setCursor(205,53);
  tft.print(CHARACTER_SKINS[wardrobeSkin].name);
  tft.drawFastHLine(205,67,100,0xDDB0);

  tft.setCursor(205,75);
  // wrap short description manually
  String d=CHARACTER_SKINS[wardrobeSkin].desc;
  int split=d.indexOf(' ',18);
  if(split<0) split=d.length();
  tft.print(d.substring(0,split));
  if(split<d.length()) { tft.setCursor(205,87); tft.print(d.substring(split+1)); }

  tft.fillRoundRect(209,103,94,22,7,0xFF5A);
  tft.setTextColor(brown); tft.setTextSize(1);
  tft.setCursor(220,111);
  if (CHARACTER_SKINS[wardrobeSkin].cost==0) tft.print("FREE");
  else { tft.print(CHARACTER_SKINS[wardrobeSkin].cost); tft.print(" COINS"); }

  bool owned=skinOwned(wardrobeSkin);
  if (!owned) {
    drawButton(207,134,98,26,0xEACD,"BUY",ST77XX_WHITE,1);
    drawButton(207,166,98,26,0x4208,"LOCKED",ST77XX_WHITE,1);
  } else if (wardrobeSkin==equippedSkin) {
    drawButton(207,134,98,26,0x4208,"OWNED",ST77XX_WHITE,1);
    drawButton(207,166,98,26,ST77XX_GREEN,"EQUIPPED",ST77XX_BLACK,1);
  } else {
    drawButton(207,134,98,26,0x4208,"OWNED",ST77XX_WHITE,1);
    drawButton(207,166,98,26,ST77XX_GREEN,"EQUIP",ST77XX_BLACK,1);
  }

  drawButton(8,207,72,27,0xFFDF,"BACK",brown,1);

  tft.setTextColor(0xFF9A); tft.setTextSize(1);
  tft.setCursor(102,214); tft.print(wardrobeSkin+1); tft.print("/"); tft.print(CHARACTER_SKIN_COUNT);
}

void drawSkinBuyDialog() {
  uint16_t cost=CHARACTER_SKINS[wardrobeSkin].cost;
  const uint16_t cream=0xFFDF, brown=0x5125;
  tft.fillRoundRect(54,66,212,108,12,cream);
  tft.drawRoundRect(54,66,212,108,12,0xEACD);
  tft.setTextColor(brown); tft.setTextSize(2);
  tft.setCursor(72,76); tft.print("BUY THIS OUTFIT?");
  tft.setTextSize(1);
  tft.setCursor(73,103); tft.print(CHARACTER_SKINS[wardrobeSkin].name);
  tft.setCursor(73,118); tft.print("Current: "); tft.print(coins);
  tft.setCursor(73,132); tft.print("Cost:    "); tft.print(cost);
  tft.setCursor(73,146); tft.print("After:   ");
  if (coins>=cost) tft.print(coins-cost); else tft.print("NOT ENOUGH");
  drawButton(68,151,82,18,ST77XX_GREEN,"BUY",ST77XX_BLACK,1);
  drawButton(170,151,82,18,0x4208,"CANCEL",ST77XX_WHITE,1);
}

void drawWardrobe() {
  audioEnabled=false;
  if (wardrobeSkin >= CHARACTER_SKIN_COUNT) wardrobeSkin=0;
  drawWardrobeBase();
  if (uiMessage.length()) {
    tft.fillRoundRect(88,184,102,13,5,0xFFDF);
    tft.setTextColor(0x5125); tft.setTextSize(1);
    tft.setCursor(93,187); tft.print(uiMessage);
  }
  if (skinBuyConfirm) drawSkinBuyDialog();
}

void drawDifficulty() {
  tft.fillScreen(0x10A3);
  tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(2);
  tft.setCursor(43,18); tft.print("SELECT DIFFICULTY");
  const uint16_t cols[3] = {0x07E0,0xFD20,0xF800};
  for (int i=0;i<3;i++) {
    int y=64+i*50;
    drawButton(70,y,180,38,cols[i],DIFF_NAME[i],ST77XX_WHITE,2);
  }
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  tft.setCursor(72,218); tft.print("Easy = slower   Hard = faster");
}

void drawCalibrationValue() {
  // Only refresh the inner data panel; never clear the full screen here.
  // This removes the visible flash caused by a full 320x240 fillScreen per hit.
  const uint16_t bg = 0x0861;
  tft.fillRect(36,72,248,101,bg);

  if (calibrationNote >= 0) {
    tft.setTextColor(ST77XX_CYAN, bg); tft.setTextSize(4);
    tft.setCursor(112,78); tft.print(noteName[calibrationNote]);

    tft.setTextColor(ST77XX_WHITE, bg); tft.setTextSize(2);
    tft.setCursor(72,128); tft.print(calibrationFreq,1); tft.print(" Hz   ");

    tft.setTextSize(1);
    tft.setCursor(80,157); tft.print("RMS: "); tft.print(calibrationRms,0); tft.print("      ");
  } else {
    tft.setTextColor(ST77XX_WHITE, bg); tft.setTextSize(2);
    tft.setCursor(82,108); tft.print("WAITING...");
  }

  // Restore the border because the local fill slightly overlaps its inside area.
  tft.drawRoundRect(28,65,264,116,12,ST77XX_WHITE);
  calibrationValueDirty = false;
}

void drawCalibration() {
  // Static page is drawn only once on entry.
  tft.fillScreen(0x0861);
  tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(2);
  tft.setCursor(62,12); tft.print("CALIBRATION");
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  tft.setCursor(50,43); tft.print("Strike any key and watch detection");
  tft.drawRoundRect(28,65,264,116,12,ST77XX_WHITE);
  drawButton(95,198,130,32,0x4208,"BACK",ST77XX_WHITE,1);
  calibrationValueDirty = true;
  drawCalibrationValue();
}

void drawResult() {
  if (gameFailed) {
    tft.drawRGBBitmap(0,0,FAIL_SCREEN_320x240,SCREEN_W,SCREEN_H);
    // 在失败图底部覆一块可点击提示，不会闪烁，因为只绘制一次
    tft.fillRoundRect(88,202,144,28,8,0x4208);
    tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
    tft.setCursor(117,212); tft.print("TOUCH TO MENU");
    return;
  }

  tft.fillScreen(0x0861);
  tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(2);
  tft.setCursor(106,12); tft.print("RESULT");
  tft.setTextColor(ST77XX_CYAN); tft.setTextSize(5);
  tft.setCursor(104,50); tft.print(finalRank);
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2);
  tft.setCursor(60,112); tft.print("Score: "); tft.print(score);
  tft.setTextSize(1);
  tft.setCursor(72,145); tft.print("Max Combo: "); tft.print(maxCombo);
  tft.setCursor(72,163); tft.print("Coins Total: "); tft.print(coins);
  drawButton(48,195,104,34,ST77XX_BLUE,"RETRY",ST77XX_WHITE,1);
  drawButton(168,195,104,34,0x4208,"MENU",ST77XX_WHITE,1);
}

void updateRender() {
  uint32_t now = millis();
  if (appState == APP_RESULT) {
    if (!resultRendered) { drawResult(); resultRendered=true; }
    return;
  }
  if (appState == APP_CALIBRATION) {
    if (!resultRendered) {
      drawCalibration();
      resultRendered = true;
    } else if (calibrationValueDirty) {
      drawCalibrationValue();
    }
    return;
  }
  if (appState != APP_PLAYING) return;
  if (now-lastRenderTime < FRAME_MS) return;
  lastRenderTime=now;
  drawTopUI();
  renderPlayingFrame();
}

bool tapIn(int x,int y,int bx,int by,int bw,int bh) {
  return x>=bx && x<bx+bw && y>=by && y<by+bh;
}

bool getTap(int &x,int &y) {
  // V3.0.6: press-down fires immediately; corrected Y-axis orientation.  Do not wait for FT6336U release,
  // because some modules keep TD_STATUS=1 for too long after a tap.
  // A latch + watchdog prevents one physical press from blocking all later pages.
  static bool latched = false;
  static uint32_t lastEmit = 0;
  static int lastEmitX = -1000, lastEmitY = -1000;

  int tx, ty;
  bool down = readTouchPoint(tx, ty);
  uint32_t now = millis();

  if (!down) {
    latched = false;
    return false;
  }

  bool moved = (abs(tx - lastEmitX) + abs(ty - lastEmitY)) >= 18;
  bool watchdog = (now - lastEmit) >= 700;

  if (!latched || (watchdog && moved)) {
    latched = true;
    lastEmit = now;
    lastEmitX = tx;
    lastEmitY = ty;
    x = tx;
    y = ty;
    Serial.print("TOUCH press x="); Serial.print(x);
    Serial.print(" y="); Serial.println(y);
    return true;
  }

  return false;
}

// ============================================================
// Setup / Loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(900);
  Serial.println();
  Serial.println("=================================");
  Serial.println("RUOXI RUN V6.2 CREATE MODE");
  Serial.println("=================================");

  prefs.begin("wanwu", false);
  coins = prefs.getUInt("coins", 0);
  totalPlays = prefs.getUInt("plays", 0);
  songUnlockedMask = prefs.getUInt("songMask", 0x07);
  songUnlockedMask |= 0x07; // first three songs are always free
  outfitOwnedMask = prefs.getUInt("outfitMask", 0x00000000UL);
  equippedByCategory[CAT_CAP] = prefs.getChar("eqCap", -1);
  equippedByCategory[CAT_CLOTHES] = prefs.getChar("eqClothes", -1);
  equippedByCategory[CAT_SHOES] = prefs.getChar("eqShoes", -1);
  equippedByCategory[CAT_ACCESSORIES] = prefs.getChar("eqAcc", -1);
  for(int c=0;c<CAT_COUNT;c++) {
    int idx = equippedByCategory[c];
    if(idx < 0 || idx >= OUTFIT_COUNT || OUTFITS[idx].category != c || !outfitOwned(idx)) equippedByCategory[c] = -1;
  }
  wardrobeCategory = CAT_CAP;
  wardrobeSelection = firstItemInCategory(wardrobeCategory);

  skinOwnedMask = prefs.getUInt("skinMask", 0x01UL) | 0x01UL;
  equippedSkin = prefs.getUChar("skinEq", 0);
  if (equippedSkin >= CHARACTER_SKIN_COUNT || !skinOwned(equippedSkin)) equippedSkin = 0;
  wardrobeSkin = equippedSkin;
  skinBuyConfirm = false;
  loadCustomSongs();
  for (int i = 0; i < SONG_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "best%d", i);
    bestScores[i] = prefs.getUShort(key, 0);
  }

  ledStrip.begin();
  ledStrip.setBrightness(LED_BRIGHTNESS);
  ledOff();

  if (!initFFT()) while (true) delay(1000);
  SPI.begin(TFT_SCLK,TFT_MISO,TFT_MOSI,TFT_CS);
  tft.init(240,320);
  tft.setRotation(1);
  tft.invertDisplay(false);
  tft.setTextWrap(false);

  // Secondary 1.8" ST7735S on the same SPI bus.
  rhythmTft.initR(INITR_BLACKTAB);
  rhythmTft.setRotation(0);          // portrait 128x160
  rhythmTft.invertDisplay(false);
  rhythmTft.setTextWrap(false);
  rhythmTftReady = true;
  rhythmShowIdle();
  if (!gameCanvas.getBuffer()) while (true) delay(1000);
  initTouch();
  if (!initI2S()) while (true) delay(1000);

  detectionQueue=xQueueCreate(4,sizeof(DetectionEvent));
  if (!detectionQueue) while (true) delay(1000);
  xTaskCreatePinnedToCore(audioTask,"audioTask",8192,nullptr,2,nullptr,0);

  randomSeed(micros());
  clouds[0]={&CLOUD_BIG,18,12,0.20f};
  clouds[1]={&CLOUD_MID,150,35,0.13f};
  clouds[2]={&CLOUD_SMALL,258,18,0.08f};
  clearParticles();
  printMemory("V6.2 initialized");
  drawHome();
  Serial.println("System ready.");
}

void loop() {
  updateLedFx();
  rhythmUpdate();

  int tx,ty;
  bool tapped=getTap(tx,ty);

  if (appState == APP_HOME) {
    if (tapped) {
      if (tapIn(tx,ty,100,54,212,40)) {
        uiMessage="";
        appState=APP_SONG_SELECT;
        songPage = selectedSong / SONGS_PER_PAGE;
        drawSongSelect();
      }
      else if (tapIn(tx,ty,100,92,212,40)) {
        uiMessage="";
        appState=APP_CREATE;
        drawCreateMode();
      }
      else if (tapIn(tx,ty,100,130,212,40)) {
        uiMessage="";
        wardrobeSkin=equippedSkin;
        skinBuyConfirm=false;
        appState=APP_WARDROBE;
        drawWardrobe();
      }
      else if (tapIn(tx,ty,100,168,212,36)) {
        enterCalibration();
        drawCalibration();
        resultRendered=true;
      }
    }
    delay(2); return;
  }

  if (appState == APP_CREATE) {
    if (deleteConfirmArmed && millis() > deleteConfirmUntil) {
      deleteConfirmArmed = false;
      uiMessage = "";
      drawCreateMode();
    }
    if (tapped) {
      // Select one of the three custom slots.
      bool slotChanged = false;
      for (uint8_t slot = 0; slot < CUSTOM_SONG_SLOTS; ++slot) {
        int y = 48 + slot * 36;
        if (tapIn(tx, ty, 24, y - 3, 272, 36)) {
          selectedCreateSlot = slot;
          deleteConfirmArmed = false;
          uiMessage = "";
          drawCreateMode();
          slotChanged = true;
          break;
        }
      }
      if (slotChanged) { delay(2); return; }

      if (tapIn(tx,ty,14,156,150,42)) {
        deleteConfirmArmed = false;
        uiMessage="";
        beginCreateRecording();
        drawRecordingMode();
      } else if (tapIn(tx,ty,166,156,140,42) && customSongLen[selectedCreateSlot] > 0) {
        if (deleteConfirmArmed && millis() <= deleteConfirmUntil) {
          deleteCustomSong(selectedCreateSlot);
        } else {
          deleteConfirmArmed = true;
          deleteConfirmUntil = millis() + 3000;
          uiMessage = "CONFIRM DELETE";
        }
        drawCreateMode();
      } else if (tapIn(tx,ty,0,200,82,40)) {
        deleteConfirmArmed = false;
        appState = APP_HOME;
        uiMessage="";
        drawHome();
      } else if (tapIn(tx,ty,86,200,140,40)) {
        deleteConfirmArmed = false;
        selectedSong = customSongIndexFromSlot(selectedCreateSlot);
        songPage = selectedSong / SONGS_PER_PAGE;
        uiMessage="";
        appState = APP_SONG_SELECT;
        drawSongSelect();
      }
    }
    delay(2); return;
  }

  if (appState == APP_RECORDING) {
    uint16_t before = recordCount;
    processDetectionQueue();
    if (recordCount != before) drawRecordingMode();
    if (recordCount >= CUSTOM_MAX_NOTES) {
      finishCreateRecording();
      drawCreateMode();
      delay(2); return;
    }
    if (tapped) {
      if (tapIn(tx,ty,0,172,84,52)) {
        cancelCreateRecording();
        drawCreateMode();
      } else if (tapIn(tx,ty,84,172,80,52)) {
        undoCreateNote();
        drawRecordingMode();
      } else if (tapIn(tx,ty,164,172,156,52)) {
        finishCreateRecording();
        drawCreateMode();
      }
    }
    delay(1); return;
  }

  if (appState == APP_SONG_SELECT) {
    if (tapped) {
      bool changed=false;
      int first = songPage * SONGS_PER_PAGE;
      for (int row=0; row<SONGS_PER_PAGE; row++) {
        int i = first + row;
        if (i >= SONG_COUNT) break;
        int y = 38 + row * 36;
        if (tapIn(tx,ty,16,y-3,288,36)) {
          selectedSong=i;
          uiMessage="";
          changed=true;
          break;
        }
      }

      const int pageCount = (SONG_COUNT + SONGS_PER_PAGE - 1) / SONGS_PER_PAGE;
      if (tapIn(tx,ty,0,200,72,40)) {
        appState=APP_HOME;
        uiMessage="";
        drawHome();
      }
      else if (tapIn(tx,ty,74,200,58,40)) {
        if (songPage > 0) { songPage--; uiMessage=""; drawSongSelect(); }
      }
      else if (tapIn(tx,ty,180,200,60,40)) {
        if (songPage < pageCount-1) { songPage++; uiMessage=""; drawSongSelect(); }
      }
      else if (tapIn(tx,ty,238,200,82,40)) {
        if (isCustomSongIndex(selectedSong) && !songUnlocked(selectedSong)) {
          selectedCreateSlot = (uint8_t)customSlotFromSongIndex(selectedSong);
          uiMessage="";
          appState=APP_CREATE;
          drawCreateMode();
        } else if (songUnlocked(selectedSong)) {
          uiMessage="";
          appState=APP_DIFFICULTY;
          drawDifficulty();
        } else {
          uint16_t cost = SONGS[selectedSong].unlockCost;
          if (coins >= cost) {
            coins -= cost;
            songUnlockedMask |= (1UL << selectedSong);
            uiMessage="SONG UNLOCKED!";
            saveProgress();
          } else {
            uiMessage="NOT ENOUGH COINS";
          }
          drawSongSelect();
        }
      }
      else if (changed) {
        drawSongSelect();
      }
    }
    delay(2); return;
  }

  if (appState == APP_WARDROBE) {
    if (tapped) {
      if (skinBuyConfirm) {
        // confirmation dialog buttons
        if (tapIn(tx,ty,62,145,94,30)) {
          uint16_t cost=CHARACTER_SKINS[wardrobeSkin].cost;
          if (!skinOwned(wardrobeSkin) && coins >= cost) {
            coins -= cost;
            skinOwnedMask |= (1UL << wardrobeSkin);
            equippedSkin = wardrobeSkin;
            uiMessage = "BOUGHT + EQUIPPED";
            saveProgress();
          } else if (skinOwned(wardrobeSkin)) {
            equippedSkin = wardrobeSkin;
            uiMessage = "EQUIPPED";
            saveProgress();
          } else {
            uiMessage = "NOT ENOUGH COINS";
          }
          skinBuyConfirm=false;
          drawWardrobe();
        } else if (tapIn(tx,ty,164,145,94,30)) {
          skinBuyConfirm=false;
          uiMessage="";
          drawWardrobe();
        }
      } else {
        // BACK
        if (tapIn(tx,ty,0,198,90,42)) {
          uiMessage="";
          appState=APP_HOME;
          drawHome();
        }
        // previous outfit
        else if (tapIn(tx,ty,5,88,55,65)) {
          wardrobeSkin=(wardrobeSkin+CHARACTER_SKIN_COUNT-1)%CHARACTER_SKIN_COUNT;
          uiMessage="";
          drawWardrobe();
        }
        // next outfit
        else if (tapIn(tx,ty,140,88,60,65)) {
          wardrobeSkin=(wardrobeSkin+1)%CHARACTER_SKIN_COUNT;
          uiMessage="";
          drawWardrobe();
        }
        // BUY
        else if (tapIn(tx,ty,198,126,118,40)) {
          if (skinOwned(wardrobeSkin)) {
            uiMessage="ALREADY OWNED";
            drawWardrobe();
          } else {
            skinBuyConfirm=true;
            uiMessage="";
            drawWardrobe();
          }
        }
        // EQUIP
        else if (tapIn(tx,ty,198,160,118,42)) {
          if (skinOwned(wardrobeSkin)) {
            equippedSkin=wardrobeSkin;
            uiMessage="READY TO RUN!";
            saveProgress();
          } else {
            uiMessage="BUY FIRST";
          }
          drawWardrobe();
        }
      }
    }
    delay(2); return;
  }

  if (appState == APP_DIFFICULTY) {
    if (tapped) {
      for (int i=0;i<3;i++) {
        if (tapIn(tx,ty,50,56+i*50,220,48)) {
          difficulty=(Difficulty)i;
          if (songUnlocked(selectedSong)) startGame();
          else { appState=APP_SONG_SELECT; uiMessage="LOCKED"; drawSongSelect(); }
          break;
        }
      }
    }
    delay(2); return;
  }

  if (appState == APP_CALIBRATION) {
    processDetectionQueue();
    updateRender();
    if (tapped && tapIn(tx,ty,72,188,176,52)) { leaveToHome(); drawHome(); }
    delay(1); return;
  }

  if (appState == APP_RESULT) {
    updateRender();
    if (tapped) {
      if (gameFailed) { leaveToHome(); drawHome(); }
      else if (tapIn(tx,ty,48,195,104,34)) startGame();
      else if (tapIn(tx,ty,168,195,104,34)) { leaveToHome(); drawHome(); }
    }
    delay(2); return;
  }

  // PLAYING
  updateClouds();
  updateParticles();
  updateAnimation();
  processDetectionQueue();
  updateNoteFlow();
  updateObstacle();
  updateRender();
  delay(1);
}
