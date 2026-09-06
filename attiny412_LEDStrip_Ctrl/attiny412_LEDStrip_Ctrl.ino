/*
 * ATtiny412 WS2812B Multi-Function Controller (Bit-Packed & 10s Deferred EEPROM)
 * -----------------------------------------------------------------------------
 * Features:
 * - Single Click: Cycle Color
 * - Double Click: Cycle Effects (0: Solid, 1: Chase, 2: Breathe, 3: Alt Blink, 4: Full Blink)
 * - Long Press:   Adjust Brightness (21 steps: 1% - 100%)
 * 
 * EEPROM Management:
 * - Bit-Packing: Compresses Color (3 bits), Effect (3 bits), and Brightness (2 bits/scaled) 
 *   into a single 8-bit byte at EEPROM address 0.
 * - 10s Deferred Save: Settings update in RAM immediately and save to EEPROM 
 *   10 seconds after the last interaction.
 */

#include <tinyNeoPixel_Static.h>
#include <EEPROM.h>

// =============================================================================
// COMPILER FLAGS & CONFIGURATION
// =============================================================================
#define ENABLE_SERIAL // Active Serial debugging on PA6

#define NUM_LEDS 10
#define NUM_EFFECTS 5 // Total number of available animation effects

// EEPROM Config
#define EEPROM_ADDR_DATA      0
#define EEPROM_SAVE_DELAY_MS  10000 // 10 seconds delay after last change

// Hardware Pin Definitions
#define LED_PIN       PIN_PA1  // Physical Pin 4 (Status LED)
#define BUTTON_PIN    PIN_PA2  // Physical Pin 5 (Button)
#define LEDSTRIP_PIN  PIN_PA3  // Physical Pin 7 (WS2812B Data Line)

// Button Timing Constraints (ms)
#define DEBOUNCE_MS     50
#define DBL_CLICK_MS   350
#define LONG_PRESS_MS  600

// Memory buffer for static pixel allocation
uint8_t pixels[NUM_LEDS * 3];

// Initialize tinyNeoPixel Static
tinyNeoPixel strip = tinyNeoPixel(NUM_LEDS, LEDSTRIP_PIN, NEO_GRB + NEO_KHZ800, pixels);

uint8_t colorIndex = 0;          // Range: 0 to 7 (3 bits)
uint8_t currentEffect = 0;       // Range: 0 to 4 (3 bits)
uint8_t brightnessStep = 20;      // Range: 0 to 20 (mapped into 2 bits for EEPROM)
int8_t brightnessDirection = -1; // Ramps DOWN on first long press

// EEPROM Deferral State Variables
bool eepromDirty = false;
unsigned long lastSettingChangeTime = 0;

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
bool waitingForSecondClick = false;
bool longPressExecuted = false;

// Effect animation states
unsigned long lastEffectUpdate = 0;
uint8_t effectStep = 0;
int8_t breatheDirection = 1;
uint8_t breatheStep = 100;
bool blinkToggleState = false;

// -----------------------------------------------------------------------------
// BIT-PACKING & EEPROM HELPERS (Single 8-bit Byte)
// -----------------------------------------------------------------------------
// Color (3 bits: 0-7) | Effect (3 bits: 0-7) | Brightness (2 bits: 0-3 mapped)
uint8_t packSettings() {
  uint8_t bScale = brightnessStep / 6; // Map 0-20 step down to 0-3 range (2 bits)
  return (colorIndex & 0x07) | ((currentEffect & 0x07) << 3) | ((bScale & 0x03) << 6);
}

void unpackSettings(uint8_t packedVal) {
  colorIndex = packedVal & 0x07;
  currentEffect = (packedVal >> 3) & 0x07;
  brightnessStep = ((packedVal >> 6) & 0x03) * 6; // Map 0-3 back to 0-20 scale
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
  uint8_t r = pgm_read_byte(&(colors[colorIndex][0]));
  uint8_t g = pgm_read_byte(&(colors[colorIndex][1]));
  uint8_t b = pgm_read_byte(&(colors[colorIndex][2]));

  uint8_t scaleFactor = (brightnessStep == 0) ? 3 : (uint8_t)((uint16_t)brightnessStep * 255 / 20);

  if (currentEffect == 2) {
    scaleFactor = (scaleFactor * breatheStep) / 100;
  }

  r = (r * scaleFactor) >> 8;
  g = (g * scaleFactor) >> 8;
  b = (b * scaleFactor) >> 8;

  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    bool active = true;
    if (currentEffect == 1) {
      uint8_t chaseSpacing = (NUM_LEDS < 10) ? NUM_LEDS : 10;
      active = ((i % chaseSpacing) == (effectStep % chaseSpacing));
    } else if (currentEffect >= 3) {
      active = (currentEffect == 4) ? blinkToggleState : ((i % 2 == 0) ? blinkToggleState : !blinkToggleState);
    }
    
    strip.setPixelColor(i, active ? r : 0, active ? g : 0, active ? b : 0);
  }

  strip.show();
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
    waitingForSecondClick = false;
    onLongPressHold();
    delay(100); 
    longPressExecuted = true;
  }

  if (currentButtonState == LOW && lastButtonState == HIGH) {
    buttonReleaseTime = now;

    if (longPressExecuted) {
      markSettingChanged();
      brightnessDirection = -brightnessDirection;
    } else if (now - buttonPressTime >= DEBOUNCE_MS) {
      if (waitingForSecondClick) {
        onDoubleClick();
        waitingForSecondClick = false;
      } else {
        waitingForSecondClick = true;
      }
    }
  }

  if (waitingForSecondClick && (now - buttonReleaseTime > DBL_CLICK_MS)) {
    onSingleClick();
    waitingForSecondClick = false;
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

  strip.begin();
  applyStripOutput();
}

void loop() {
  handleButton();
  processEEPROMSave();

  if (currentEffect > 0) {
    unsigned long now = millis();
    uint16_t interval = 80;

    if (currentEffect == 2) interval = 25;
    else if (currentEffect == 3) interval = 300;
    else if (currentEffect == 4) interval = 250;

    if (now - lastEffectUpdate > interval) {
      lastEffectUpdate = now;

      if (currentEffect == 1) {
        effectStep = (effectStep + 1) % 10;
      } else if (currentEffect == 2) {
        if (breatheStep >= 100) breatheDirection = -2;
        if (breatheStep <= 10) breatheDirection = 2;
        breatheStep += breatheDirection;
      } else if (currentEffect >= 3) {
        blinkToggleState = !blinkToggleState;
      }

      applyStripOutput();
    }
  }
}