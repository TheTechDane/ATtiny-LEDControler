/*
 * ATtiny412 WS2812B Multi-Function Controller (Smooth White Fade Sparkle)
 * -----------------------------------------------------------------------------
 * LED CAPACITY CONSTRAINTS:
 * - RAM Limit: ATtiny412 has 256 bytes of SRAM total.
 * - Strip Size: Drives up to 180 physical LEDs.
 * - Pattern Buffer: Allocates RAM for 20 pixels (60 bytes SRAM) and streams 
 *   it 9 times continuously using low-level bit-banging.
 * 
 * INTERACTION:
 * - Single Click: Cycle Color (8 Presets including Warm White)
 * - Double Click: Cycle Effects (0: Solid, 1: 20px Chase, 2: 10px Chase, 
 *                 3: Breathe, 4: Alt Blink, 5: Full Blink, 6: Rainbow, 7: Sparkle Fade)
 * - Long Press:   Adjust Brightness (5% steps, 1% to 100%, alternating direction)
 * - EEPROM:       Bit-packed (1 byte) with 10-second deferred write delay.
 */

#include <EEPROM.h>

// =============================================================================
// COMPILER FLAGS & CONFIGURATION
// =============================================================================
#define ENABLE_SERIAL // Active Serial debugging on PA6

#define VIRTUAL_LEDS  20   // Pattern resolution in RAM (60 bytes SRAM)
#define ACTUAL_LEDS   180  // Total physical LEDs on strip (180 / 20 = 9 repeats)
#define NUM_REPEAT    (ACTUAL_LEDS / VIRTUAL_LEDS)
#define NUM_EFFECTS   8    // Total animation modes (0 to 7)

// EEPROM Config
#define EEPROM_ADDR_DATA      0
#define EEPROM_SAVE_DELAY_MS  10000 // 10-second delay after last button interaction

// Hardware Pin Definitions
#define LED_PIN       PIN_PA1  // Physical Pin 4 (Status LED)
#define BUTTON_PIN    PIN_PA2  // Physical Pin 5 (Button)
#define LEDSTRIP_PIN  PIN_PA3  // Physical Pin 7 (WS2812B Data Line - Bit 3 on PORTA)
#define DATA_BIT      3        // PA3

// Button Timing Constraints (ms)
#define DEBOUNCE_MS     40
#define DBL_CLICK_MS   280
#define LONG_PRESS_MS  500

// Memory buffer allocated strictly for 20 virtual pixels (60 bytes SRAM, GRB layout)
uint8_t pixels[VIRTUAL_LEDS * 3];

uint8_t colorIndex = 0;          // Range: 0 to 7
uint8_t currentEffect = 0;       // Range: 0 to 7
uint8_t brightnessStep = 10;     // Range: 0 to 20 (Set to 10 = 50% default brightness)
int8_t brightnessDirection = -1; // Ramps DOWN on first hold

// EEPROM Deferral State Variables
bool eepromDirty = false;
unsigned long lastSettingChangeTime = 0;

// Fast Lightweight PRNG state
uint16_t rngState = 0xACE1;

// Colors stored in FLASH (PROGMEM)
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

// Button tracking variables
bool lastButtonState = LOW;
unsigned long buttonPressTime = 0;
unsigned long buttonReleaseTime = 0;
uint8_t clickCount = 0;
bool longPressExecuted = false;

// Effect animation states
unsigned long lastEffectUpdate = 0;
uint8_t effectStep = 0;
int8_t breatheDirection = 1;
uint8_t breatheStep = 100;
bool blinkToggleState = false;
uint8_t rainbowWheelPos = 0;

// Sparkle Fade state variables
uint8_t sparkleFadeProgress = 0;  // 0 to 100 (fade position inside cycle)
int8_t sparkleFadeDirection = 5;  // Step speed for fading in/out
uint8_t sparkleActivePixel = 0;   // Currently sparkling pixel index

// Lightweight 16-bit PRNG
uint16_t fastRandom() {
  rngState = (rngState >> 1) ^ (-(rngState & 1u) & 0xB400u);
  return rngState;
}

// -----------------------------------------------------------------------------
// HARDWARE WS2812B LOW-LEVEL BITBANG STREAMER
// -----------------------------------------------------------------------------
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
  delayMicroseconds(60); // Force WS2812 Reset pulse
}

void setPixelGRB(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index >= VIRTUAL_LEDS) return;
  pixels[index * 3 + 0] = g; // Green
  pixels[index * 3 + 1] = r; // Red
  pixels[index * 3 + 2] = b; // Blue
}

// Compact Hue-to-RGB conversion
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

// -----------------------------------------------------------------------------
// BIT-PACKING & EEPROM HELPERS
// -----------------------------------------------------------------------------
uint8_t packSettings() {
  uint8_t bScale = brightnessStep / 6; // Map 0-20 down to 0-3 (2 bits)
  return (colorIndex & 0x07) | ((currentEffect & 0x07) << 3) | ((bScale & 0x03) << 6);
}

void unpackSettings(uint8_t packedVal) {
  colorIndex = packedVal & 0x07;
  currentEffect = (packedVal >> 3) & 0x07;
  brightnessStep = ((packedVal >> 6) & 0x03) * 6;
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
    Serial.println(F("Saved to EEPROM!"));
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

// -----------------------------------------------------------------------------
// LED OUTPUT CONTROLLER
// -----------------------------------------------------------------------------
void applyStripOutput() {
  uint8_t baseR = pgm_read_byte(&(colors[colorIndex][0]));
  uint8_t baseG = pgm_read_byte(&(colors[colorIndex][1]));
  uint8_t baseB = pgm_read_byte(&(colors[colorIndex][2]));

  uint8_t scaleFactor = (brightnessStep == 0) ? 3 : (uint8_t)((uint16_t)brightnessStep * 255 / 20);

  if (currentEffect == 3) { // Breathing Pulse
    scaleFactor = (scaleFactor * breatheStep) / 100;
  }

  uint8_t r = (baseR * scaleFactor) >> 8;
  uint8_t g = (baseG * scaleFactor) >> 8;
  uint8_t b = (baseB * scaleFactor) >> 8;

  // Populate 20-pixel Virtual Pattern Buffer
  for (uint8_t i = 0; i < VIRTUAL_LEDS; i++) {
    uint8_t pr = r, pg = g, pb = b;

    if (currentEffect == 1) { // 20-pixel Chase
      bool active = (i == (effectStep % VIRTUAL_LEDS));
      pr = active ? r : 0; pg = active ? g : 0; pb = active ? b : 0;
    } 
    else if (currentEffect == 2) { // 10-pixel Spaced Chase
      bool active = ((i % 10) == (effectStep % 10));
      pr = active ? r : 0; pg = active ? g : 0; pb = active ? b : 0;
    }
    else if (currentEffect >= 4 && currentEffect <= 5) { // Blink / Alt Blink
      bool active = (currentEffect == 5) ? blinkToggleState : ((i % 2 == 0) ? blinkToggleState : !blinkToggleState);
      pr = active ? r : 0; pg = active ? g : 0; pb = active ? b : 0;
    } 
    else if (currentEffect == 6) { // Rainbow Spectrum
      getRainbowColor((uint8_t)((i * 256 / VIRTUAL_LEDS) + rainbowWheelPos), &pr, &pg, &pb);
      pr = (pr * scaleFactor) >> 8;
      pg = (pg * scaleFactor) >> 8;
      pb = (pb * scaleFactor) >> 8;
    } 
    else if (currentEffect == 7) { // Smooth White Fade Sparkle
      if (i == sparkleActivePixel) {
        // Calculate cross-fade between base color and pure white based on fade progress
        uint8_t targetWhite = scaleFactor; // Matches exact current strip brightness
        pr = r + (((int16_t)targetWhite - r) * sparkleFadeProgress / 100);
        pg = g + (((int16_t)targetWhite - g) * sparkleFadeProgress / 100);
        pb = b + (((int16_t)targetWhite - b) * sparkleFadeProgress / 100);
      }
    }

    setPixelGRB(i, pr, pg, pb);
  }

  // Stream raw buffer 9 times (180 LEDs)
  sendRawBytesTiled(pixels, VIRTUAL_LEDS * 3, NUM_REPEAT);
}

// -----------------------------------------------------------------------------
// BUTTON INTERACTION HANDLERS
// -----------------------------------------------------------------------------
void onSingleClick() {
  digitalWrite(LED_PIN, HIGH);
  colorIndex = (colorIndex + 1) % 8;
  markSettingChanged();
  applyStripOutput();

#ifdef ENABLE_SERIAL
  Serial.print(F("Color: "));
  Serial.println(colorIndex);
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
  Serial.print(F("Effect: "));
  Serial.println(currentEffect);
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
  Serial.print(F("Bright: "));
  Serial.print(brightnessStep == 0 ? 1 : brightnessStep * 5);
  Serial.println(F("%"));
#endif

  delay(30);
  digitalWrite(LED_PIN, LOW);
}

void handleButton() {
  bool currentButtonState = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  if (currentButtonState == HIGH && lastButtonState == LOW) {
    buttonPressTime = now;
    longPressExecuted = false;
  }

  if (currentButtonState == HIGH && (now - buttonPressTime >= LONG_PRESS_MS)) {
    clickCount = 0;
    onLongPressHold();
    delay(80); 
    longPressExecuted = true;
  }

  if (currentButtonState == LOW && lastButtonState == HIGH) {
    buttonReleaseTime = now;

    if (longPressExecuted) {
      markSettingChanged();
      brightnessDirection = -brightnessDirection;
    } else if (now - buttonPressTime >= DEBOUNCE_MS) {
      clickCount++;
    }
  }

  if (clickCount > 0 && (now - buttonReleaseTime > DBL_CLICK_MS) && currentButtonState == LOW) {
    if (clickCount == 1) {
      onSingleClick();
    } else if (clickCount >= 2) {
      onDoubleClick();
    }
    clickCount = 0;
  }

  lastButtonState = currentButtonState;
}

// -----------------------------------------------------------------------------
// SETUP & MAIN LOOP
// -----------------------------------------------------------------------------
void setup() {
#ifdef ENABLE_SERIAL
  Serial.begin(115200, SERIAL_8N1 | SERIAL_TX_ONLY);
  Serial.println(F("Ready"));
#endif

  pinMode(LED_PIN, OUTPUT);
  pinMode(LEDSTRIP_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT);

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
    else if (currentEffect == 6) interval = 30; // Rainbow speed
    else if (currentEffect == 7) interval = 25; // Smooth 25ms step update for Sparkle Fade

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
        // Smoothly ramp sparkle white level up to 100% and back down to 0%
        if (sparkleFadeDirection > 0) {
          if (sparkleFadeProgress >= 100) {
            sparkleFadeDirection = -5; // Peak reached, fade out
          } else {
            sparkleFadeProgress += sparkleFadeDirection;
          }
        } else {
          if (sparkleFadeProgress <= 0) {
            // Fade out finished, select a new random LED pixel to sparkle next
            sparkleActivePixel = fastRandom() % VIRTUAL_LEDS;
            sparkleFadeDirection = 5; // Start fading in new pixel
          } else {
            sparkleFadeProgress += sparkleFadeDirection;
          }
        }
      }

      applyStripOutput();
    }
  }
}