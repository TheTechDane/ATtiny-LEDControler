/*
 * ATtiny412 WS2812B Multi-Function Controller (Active LOW Button + INPUT_PULLUP)
 * -----------------------------------------------------------------------------
 * Dynamic Memory & Flash Optimized Edition (Retains Serial Debugging)
 * 
 * HARDWARE CONNECTIONS:
 * - PA1 (Pin 4) : Status LED Output
 * - PA2 (Pin 5) : Button Input (Active LOW, Internal Pull-Up enabled) -> Connect to GND
 * - PA3 (Pin 7) : WS2812B Data Line
 * 
 * INTERACTION:
 * - Single Click: Cycle Color (8 Presets including Warm White)
 * - Double Click: Cycle Effects (0 to 8: Solid, Chase, Breathe, Blink, Rainbow, Sparkle, Lava)
 * - Long Press:   Adjust Brightness (5% steps, 1% to 100%)
 * - EEPROM:       Bit-packed (1 byte) with 10-second deferred write delay.
 */

#include <EEPROM.h>

#define ENABLE_SERIAL // Active Serial debugging on PA6

#define VIRTUAL_LEDS  20   // Pattern resolution in RAM (60 bytes SRAM)
#define ACTUAL_LEDS   180  // Total physical LEDs on strip
#define NUM_REPEAT    (ACTUAL_LEDS / VIRTUAL_LEDS)
#define NUM_EFFECTS   9    // Total animation modes (0 to 8)

#define EEPROM_ADDR_DATA      0
#define EEPROM_SAVE_DELAY_MS  10000

// Hardware Pin Definitions
#define LED_PIN       PIN_PA1  // Status LED
#define BUTTON_PIN    PIN_PA2  // Button (Active LOW)
#define LEDSTRIP_PIN  PIN_PA3  // WS2812B Data Line
#define DATA_BIT      3        // PA3

// Button Timing Constraints (ms)
#define DEBOUNCE_MS     40
#define DBL_CLICK_MS   280
#define LONG_PRESS_MS  500

uint8_t pixels[VIRTUAL_LEDS * 3];

uint8_t colorIndex = 0;          // Range: 0 to 7
uint8_t currentEffect = 0;       // Range: 0 to 8
uint8_t brightnessStep = 10;     // Range: 0 to 20 (50% default)
int8_t brightnessDirection = -1; 

bool eepromDirty = false;
unsigned long lastSettingChangeTime = 0;

uint16_t rngState = 0xACE1;

const uint8_t colors[8][3] PROGMEM = {
  {255, 0,   0  }, // Red
  {255, 255, 0  }, // Yellow
  {0,   255, 0  }, // Green
  {0,   255, 255}, // Cyan
  {0,   0,   255}, // Blue
  {255, 0,   255}, // Magenta
  {255, 255, 255}, // White
  {255, 197, 143}  // Warm White
};

// Active LOW Button Tracking Variables (Default HIGH when released)
bool lastButtonState = HIGH;
unsigned long buttonPressTime = 0;
unsigned long buttonReleaseTime = 0;
uint8_t clickCount = 0;
bool longPressExecuted = false;

unsigned long lastEffectUpdate = 0;
uint8_t effectStep = 0;
int8_t breatheDirection = 1;
uint8_t breatheStep = 100;
bool blinkToggleState = false;
uint8_t rainbowWheelPos = 0;

uint8_t sparkleFadeProgress = 0;
int8_t sparkleFadeDirection = 5;
uint8_t sparkleActivePixel = 0;

uint16_t lavaOffset = 0;

uint16_t fastRandom() {
  rngState = (rngState >> 1) ^ (-(rngState & 1u) & 0xB400u);
  return rngState;
}

// Low-level bitbang streamer
void sendRawBytesTiled(uint8_t *data, uint16_t length, uint8_t repeats) {
  uint8_t pinMask = (1 << DATA_BIT);
  
  noInterrupts();
  for (uint8_t r = 0; r < repeats; r++) {
    for (uint16_t i = 0; i < length; i++) {
      uint8_t b = data[i];
      for (int8_t bit = 7; bit >= 0; bit--) {
        if (b & (1 << bit)) {
          PORTA.OUTSET = pinMask;
          asm volatile("nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n");
          PORTA.OUTCLR = pinMask;
          asm volatile("nop\n nop\n");
        } else {
          PORTA.OUTSET = pinMask;
          asm volatile("nop\n nop\n");
          PORTA.OUTCLR = pinMask;
          asm volatile("nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n");
        }
      }
    }
  }
  interrupts();
  delayMicroseconds(60);
}

void setPixelGRB(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index >= VIRTUAL_LEDS) return;
  pixels[index * 3 + 0] = g;
  pixels[index * 3 + 1] = r;
  pixels[index * 3 + 2] = b;
}

void getRainbowColor(uint8_t pos, uint8_t *r, uint8_t *g, uint8_t *b) {
  pos = 255 - pos;
  if (pos < 85) {
    *r = 255 - pos * 3; *g = 0; *b = pos * 3;
  } else if (pos < 170) {
    pos -= 85;
    *r = 0; *g = pos * 3; *b = 255 - pos * 3;
  } else {
    pos -= 170;
    *r = pos * 3; *g = 255 - pos * 3; *b = 0;
  }
}

// Compact Integer Lava Palette Calculations
void getLavaColor(uint8_t heat, uint8_t scaleFactor, uint8_t *r, uint8_t *g, uint8_t *b) {
  uint8_t rawR = 0, rawG = 0;

  if (heat > 180) {
    rawR = 255;
    rawG = 85 + (((uint16_t)(heat - 180) * 102) >> 8);
  } else if (heat > 85) {
    rawR = 255;
    rawG = ((uint16_t)(heat - 85) * 189) >> 8;
  } else if (heat > 10) {
    uint16_t val = (uint16_t)(heat - 10) * 3 / 2;
    rawR = (val > 255) ? 255 : val;
  }

  *r = ((uint16_t)rawR * scaleFactor) >> 8;
  *g = ((uint16_t)rawG * scaleFactor) >> 8;
  *b = 0;
}

uint8_t packSettings() {
  uint8_t bScale = brightnessStep / 6;
  return (colorIndex & 0x07) | ((currentEffect & 0x0F) << 3) | ((bScale & 0x01) << 7);
}

void unpackSettings(uint8_t packedVal) {
  colorIndex = packedVal & 0x07;
  currentEffect = (packedVal >> 3) & 0x0F;
  brightnessStep = ((packedVal >> 7) & 0x01) * 20;
  if (brightnessStep > 20) brightnessStep = 20;
}

void markSettingChanged() {
  eepromDirty = true;
  lastSettingChangeTime = millis();
}

void processEEPROMSave() {
  if (eepromDirty && (millis() - lastSettingChangeTime >= EEPROM_SAVE_DELAY_MS)) {
    EEPROM.update(EEPROM_ADDR_DATA, packSettings());
    eepromDirty = false;
#ifdef ENABLE_SERIAL
    Serial.println(F("Saved!"));
#endif
  }
}

void loadSettings() {
  uint8_t packedVal = EEPROM.read(EEPROM_ADDR_DATA);
  if (packedVal != 0xFF) {
    unpackSettings(packedVal);
    if (colorIndex >= 8) colorIndex = 0;
    if (currentEffect >= NUM_EFFECTS) currentEffect = 0;
  }
}

void applyStripOutput() {
  uint8_t baseR = pgm_read_byte(&(colors[colorIndex][0]));
  uint8_t baseG = pgm_read_byte(&(colors[colorIndex][1]));
  uint8_t baseB = pgm_read_byte(&(colors[colorIndex][2]));

  uint8_t scaleFactor = (brightnessStep == 0) ? 3 : (uint8_t)((uint16_t)brightnessStep * 255 / 20);

  if (currentEffect == 3) {
    scaleFactor = (scaleFactor * breatheStep) / 100;
  }

  uint8_t r = (baseR * scaleFactor) >> 8;
  uint8_t g = (baseG * scaleFactor) >> 8;
  uint8_t b = (baseB * scaleFactor) >> 8;

  for (uint8_t i = 0; i < VIRTUAL_LEDS; i++) {
    uint8_t pr = r, pg = g, pb = b;

    if (currentEffect == 1) {
      bool active = (i == (effectStep % VIRTUAL_LEDS));
      pr = active ? r : 0; pg = active ? g : 0; pb = active ? b : 0;
    } 
    else if (currentEffect == 2) {
      bool active = ((i % 10) == (effectStep % 10));
      pr = active ? r : 0; pg = active ? g : 0; pb = active ? b : 0;
    }
    else if (currentEffect >= 4 && currentEffect <= 5) {
      bool active = (currentEffect == 5) ? blinkToggleState : ((i % 2 == 0) ? blinkToggleState : !blinkToggleState);
      pr = active ? r : 0; pg = active ? g : 0; pb = active ? b : 0;
    } 
    else if (currentEffect == 6) {
      getRainbowColor((uint8_t)((i * 256 / VIRTUAL_LEDS) + rainbowWheelPos), &pr, &pg, &pb);
      pr = (pr * scaleFactor) >> 8;
      pg = (pg * scaleFactor) >> 8;
      pb = (pb * scaleFactor) >> 8;
    } 
    else if (currentEffect == 7) {
      if (i == sparkleActivePixel) {
        uint8_t targetWhite = scaleFactor;
        pr = r + (((int16_t)targetWhite - r) * sparkleFadeProgress / 100);
        pg = g + (((int16_t)targetWhite - g) * sparkleFadeProgress / 100);
        pb = b + (((int16_t)targetWhite - b) * sparkleFadeProgress / 100);
      }
    }
    else if (currentEffect == 8) {
      uint16_t wave1 = (lavaOffset - (i * 35)) & 255;
      uint16_t wave2 = ((lavaOffset / 2) - (i * 20)) & 255;

      uint8_t val1 = (wave1 < 128) ? wave1 * 2 : (255 - wave1) * 2;
      uint8_t val2 = (wave2 < 128) ? wave2 * 2 : (255 - wave2) * 2;

      uint8_t heat = (val1 + val2) / 2;

      if (heat > 195) heat = 255;
      else if (heat < 40) heat = 0;

      getLavaColor(heat, scaleFactor, &pr, &pg, &pb);
    }

    setPixelGRB(i, pr, pg, pb);
  }

  sendRawBytesTiled(pixels, VIRTUAL_LEDS * 3, NUM_REPEAT);
}

void onSingleClick() {
  digitalWrite(LED_PIN, HIGH);
  colorIndex = (colorIndex + 1) % 8;
  markSettingChanged();
  applyStripOutput();
#ifdef ENABLE_SERIAL
  Serial.print(F("C:")); Serial.println(colorIndex);
#endif
  delay(20);
  digitalWrite(LED_PIN, LOW);
}

void onDoubleClick() {
  digitalWrite(LED_PIN, HIGH);
  currentEffect = (currentEffect + 1) % NUM_EFFECTS;
  markSettingChanged();
  applyStripOutput();
#ifdef ENABLE_SERIAL
  Serial.print(F("E:")); Serial.println(currentEffect);
#endif
  delay(100);
  digitalWrite(LED_PIN, LOW);
}

void onLongPressHold() {
  digitalWrite(LED_PIN, HIGH);
  int8_t nextStep = brightnessStep + brightnessDirection;

  if (nextStep >= 20) brightnessStep = 20;
  else if (nextStep <= 0) brightnessStep = 0;
  else brightnessStep = nextStep;

  applyStripOutput();
#ifdef ENABLE_SERIAL
  Serial.print(F("B:")); Serial.println(brightnessStep);
#endif
  delay(30);
  digitalWrite(LED_PIN, LOW);
}

// Handle Active LOW Button Logics (Press = LOW, Release = HIGH)
void handleButton() {
  bool currentButtonState = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  // Detect Press (Transition HIGH -> LOW)
  if (currentButtonState == LOW && lastButtonState == HIGH) {
    buttonPressTime = now;
    longPressExecuted = false;
  }

  // Detect Long Press while held LOW
  if (currentButtonState == LOW && (now - buttonPressTime >= LONG_PRESS_MS)) {
    clickCount = 0;
    onLongPressHold();
    delay(80); 
    longPressExecuted = true;
  }

  // Detect Release (Transition LOW -> HIGH)
  if (currentButtonState == HIGH && lastButtonState == LOW) {
    buttonReleaseTime = now;

    if (longPressExecuted) {
      markSettingChanged();
      brightnessDirection = -brightnessDirection;
    } else if (now - buttonPressTime >= DEBOUNCE_MS) {
      clickCount++;
    }
  }

  // Process Click Actions after Double Click window timeout
  if (clickCount > 0 && (now - buttonReleaseTime > DBL_CLICK_MS) && currentButtonState == HIGH) {
    if (clickCount == 1) {
      onSingleClick();
    } else if (clickCount >= 2) {
      onDoubleClick();
    }
    clickCount = 0;
  }

  lastButtonState = currentButtonState;
}

void setup() {
#ifdef ENABLE_SERIAL
  Serial.begin(115200, SERIAL_8N1 | SERIAL_TX_ONLY);
  Serial.println(F("OK"));
#endif

  pinMode(LED_PIN, OUTPUT);
  pinMode(LEDSTRIP_PIN, OUTPUT);
  
  // Active LOW Setup with Internal Pull-Up
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  loadSettings();
  applyStripOutput();
}

void loop() {
  handleButton();
  processEEPROMSave();

  if (currentEffect > 0) {
    unsigned long now = millis();
    uint16_t interval = 80;

    if (currentEffect == 1 || currentEffect == 2) interval = 80;
    else if (currentEffect == 3) interval = 25;
    else if (currentEffect == 4) interval = 300;
    else if (currentEffect == 5) interval = 250;
    else if (currentEffect == 6) interval = 30;
    else if (currentEffect == 7) interval = 25;
    else if (currentEffect == 8) interval = 30;

    if (now - lastEffectUpdate > interval) {
      lastEffectUpdate = now;

      if (currentEffect == 1 || currentEffect == 2) {
        effectStep = (effectStep + 1) % VIRTUAL_LEDS;
      } else if (currentEffect == 3) {
        if (breatheStep >= 100) breatheDirection = -2;
        if (breatheStep <= 10) breatheDirection = 2;
        breatheStep += breatheDirection;
      } else if (currentEffect >= 4 && currentEffect <= 5) {
        blinkToggleState = !blinkToggleState;
      } else if (currentEffect == 6) {
        rainbowWheelPos += 4;
      } else if (currentEffect == 7) {
        if (sparkleFadeDirection > 0) {
          if (sparkleFadeProgress >= 100) {
            sparkleFadeDirection = -5;
          } else {
            sparkleFadeProgress += sparkleFadeDirection;
          }
        } else {
          if (sparkleFadeProgress <= 0) {
            sparkleActivePixel = fastRandom() % VIRTUAL_LEDS;
            sparkleFadeDirection = 5;
          } else {
            sparkleFadeProgress += sparkleFadeDirection;
          }
        }
      } else if (currentEffect == 8) {
        lavaOffset += 1;
      }

      applyStripOutput();
    }
  }
}