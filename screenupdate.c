#include <SPI.h>
#include <Adafruit_PN532.h>


#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// I2C LCD
#define LCD_ADDR 0x27     // may not this address
#define LCD_COLS 16       // 16 or 20. looks like 16
#define LCD_ROWS 2        // 2 or 4. looks like 2

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);


static char lcd_buf[LCD_ROWS][LCD_COLS + 1];
static uint8_t lcd_row = 0, lcd_col = 0;

void lcdClearAll() {
  for (uint8_t r = 0; r < LCD_ROWS; r++) {
    for (uint8_t c = 0; c < LCD_COLS; c++) lcd_buf[r][c] = ' ';
    lcd_buf[r][LCD_COLS] = '\0';
  }
  lcd.clear();
  lcd_row = 0; lcd_col = 0;
  for (uint8_t r = 0; r < LCD_ROWS; r++) {
    lcd.setCursor(0, r); lcd.print(lcd_buf[r]);
  }
}

void lcdRefreshRow(uint8_t r) {
  lcd.setCursor(0, r);
 
  for (uint8_t c = 0; c < LCD_COLS; c++) lcd.write(lcd_buf[r][c]);
}

void lcdScrollUp() {
  for (uint8_t r = 0; r < LCD_ROWS - 1; r++) {
    for (uint8_t c = 0; c < LCD_COLS; c++) lcd_buf[r][c] = lcd_buf[r + 1][c];
    lcd_buf[r][LCD_COLS] = '\0';
    lcdRefreshRow(r);
  }

  for (uint8_t c = 0; c < LCD_COLS; c++) lcd_buf[LCD_ROWS - 1][c] = ' ';
  lcd_buf[LCD_ROWS - 1][LCD_COLS] = '\0';
  lcdRefreshRow(LCD_ROWS - 1);
  lcd_row = LCD_ROWS - 1;
  lcd_col = 0;
}

void lcdNewline() {
  lcd_col = 0;
  if (lcd_row + 1 < LCD_ROWS) {
    lcd_row++;
  } else {
    lcdScrollUp();
  }
}

void lcdPutChar(char ch) {
  if (ch == '\r') return;
  if (ch == '\n') { lcdNewline(); return; }

  lcd_buf[lcd_row][lcd_col] = ch;
  lcd.setCursor(lcd_col, lcd_row); lcd.write(ch);
  lcd_col++;
  if (lcd_col >= LCD_COLS) {
    lcdNewline();
  }
}

void lcdPrint(const String &s) {
  for (size_t i = 0; i < s.length(); i++) lcdPutChar(s[i]);
}
void lcdPrintln(const String &s) { lcdPrint(s); lcdPutChar('\n'); }
void lcdPrint(const __FlashStringHelper *fs) { String s(fs); lcdPrint(s); }
void lcdPrintln(const __FlashStringHelper *fs) { String s(fs); lcdPrintln(s); }
template<typename T>
void lcdPrintT(const T &v) { lcdPrint(String(v)); }
template<typename T>
void lcdPrintlnT(const T &v) { lcdPrintln(String(v)); }

//mirroring from terminal to LCD
//change all  Serial.print(...) / Serial.println(...) to  LOG(...) / LOGLN(...)
#define LOG(x)        do { Serial.print(x);      lcdPrint(x);      } while(0)
#define LOGLN(x)      do { Serial.println(x);    lcdPrintln(x);    } while(0)



// ---------- PN532 wiring (hardware SPI on Uno) ----------
#define PN532_SS 10
Adafruit_PN532 nfc(PN532_SS);

// ---------- IR sensor ----------
#define IR_PIN 2

// ---------- Keypad (3x4) wiring (SparkFun 12-button) ----------
#define KP_C1 5   // Column 1
#define KP_C2 4   // Column 2
#define KP_C3 3   // Column 3
#define KP_R1 9   // Row 1
#define KP_R2 8   // Row 2
#define KP_R3 7   // Row 3
#define KP_R4 6   // Row 4

const char KEYS[4][3] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};

// ---------- Game tunables ----------
const unsigned long INITIAL_TIME_LIMIT_MS = 30000;
const unsigned long MIN_TIME_LIMIT_MS     = 3000;
const unsigned long DECREMENT_MS          = 1000;
const char TARGET_TEXT[] = "BANK CARD";

// ---------- NTAG213 memory window ----------
const uint8_t NTAG_START_PAGE = 4;
const uint8_t NTAG_END_PAGE   = 39;

// ---------- Game state ----------
enum GameState { READY, ROUND_PICK, ROUND_START, WAITING_FOR_ACTION, ROUND_SUCCESS, GAME_OVER };
enum ActionType { ACTION_NFC = 0, ACTION_IR = 1, ACTION_KEYPAD = 2 };

GameState state = READY;
ActionType currentAction = ACTION_NFC;

unsigned long roundDeadline = 0;
unsigned long roundNumber = 0;
unsigned long score = 0;
unsigned long currentTimeLimitMs = INITIAL_TIME_LIMIT_MS;

// ---------- Keypad action state ----------
char pinTarget[5] = {0};   // 4 digits + null
char pinEntered[5] = {0};
uint8_t pinPos = 0;
bool pinShownThisRound = false;

// ---------- Forward decls ----------
bool doNfcAction(String &observedText);
bool findNdefTextFromBuffer(uint8_t *buf, uint16_t len, String &outText);
void waitForTagRemoval();
bool doIrAction();
bool isAnyNfcPresent(uint16_t timeout_ms = 20);

// Keypad helpers
void keypadInit();
char keypadGetKey(bool waitRelease = false);
bool keypadAnyKeyPressed();
void keypadStartNewPin();
bool doKeypadAction();  // returns true only if correct 4-digit PIN was entered

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcdClearAll();

  pinMode(IR_PIN, INPUT);
  keypadInit();

  LOGLN(F("=== BANK IT! (NFC / IR / KEYPAD) ==="));
  LOGLN(F("NFC: present NTAG213 with NDEF Text == \"BANK CARD\"."));
  LOGLN(F("IR: cover IR sensor (D2)."));
  LOGLN(F("KEYPAD: enter the printed 4-digit PIN."));
  LOG(F("Initial time limit (s): ")); LOGLN(INITIAL_TIME_LIMIT_MS / 1000);
  LOG(F("Minimum time limit (s): ")); LOGLN(MIN_TIME_LIMIT_MS / 1000);

  nfc.begin();
  uint32_t ver = nfc.getFirmwareVersion();
  if (!ver) { LOGLN(F("PN532 not found")); while (1) {} }
  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(0xFF);

  randomSeed(analogRead(A0) ^ micros());
  state = ROUND_PICK;
}

void loop() {
  switch (state) {
    case ROUND_PICK: {
      currentAction = (ActionType)random(0, 3); // 0..2
      state = ROUND_START;
    } break;

    case ROUND_START: {
      roundNumber++;
      LOGLN("");

      LOG(F("-- Round ")); LOG(roundNumber); LOG(F(" -- "));

      if (currentAction == ACTION_NFC) {
        LOG(F("Action: TAP the BANK CARD (NFC). Time limit "));
        if (digitalRead(IR_PIN) == LOW) {
          LOGLN(F("\nIR is currently covered. Uncover it before starting."));
        }
      } else if (currentAction == ACTION_IR) {
        LOG(F("Action: COVER the IR sensor (D2). Time limit "));
        if (isAnyNfcPresent(10)) {
          LOGLN(F("\nAn NFC tag is present. Remove it before starting."));
        }
      } else {
        LOG(F("Action: ENTER the 4-digit PIN on the keypad. Time limit "));
        keypadStartNewPin();
      }

      LOG(currentTimeLimitMs / 1000);
      LOGLN(F(" seconds"));

      roundDeadline = millis() + currentTimeLimitMs;
      state = WAITING_FOR_ACTION;
    } break;

    case WAITING_FOR_ACTION: {
      bool success = false;

      if (currentAction == ACTION_NFC) {
        delay(500);
        if (digitalRead(IR_PIN) == LOW) {
          LOGLN(F("Wrong action: IR covered during NFC round. Game over."));
          state = GAME_OVER; break;
        }
        if (keypadAnyKeyPressed()) {
          LOGLN(F("Wrong action: Keypad pressed during NFC round. Game over."));
          state = GAME_OVER; break;
        }

        String text;
        success = doNfcAction(text);
        if (success) {
          LOG(F("NDEF Text read: \"")); LOG(text); LOGLN('"');
          if (text != TARGET_TEXT) {
            LOGLN(F("Wrong card text. Game over."));
            state = GAME_OVER; break;
          }
        }

      } else if (currentAction == ACTION_IR) {
        if (isAnyNfcPresent(10)) {
          LOGLN(F("Wrong action: NFC tag detected during IR round. Game over."));
          state = GAME_OVER; break;
        }
        if (keypadAnyKeyPressed()) {
          LOGLN(F("Wrong action: Keypad pressed during IR round. Game over."));
          state = GAME_OVER; break;
        }

        success = doIrAction();

      } else { // ACTION_KEYPAD
        delay(500);
        if (digitalRead(IR_PIN) == LOW) {
          LOGLN(F("Wrong action: IR covered during KEYPAD round. Game over."));
          state = GAME_OVER; break;
        }
        if (isAnyNfcPresent(10)) {
          LOGLN(F("Wrong action: NFC tag detected during KEYPAD round. Game over."));
          state = GAME_OVER; break;
        }

        success = doKeypadAction();
      }

      if (success) {
        state = ROUND_SUCCESS;
      } else {
        if (millis() > roundDeadline) {
          LOGLN(F("Time up. Game over."));
          state = GAME_OVER;
        }
      }
    } break;

    case ROUND_SUCCESS: {
      score++;
      LOG(F("Success. Score = ")); LOGLN(score);

      if (currentTimeLimitMs > MIN_TIME_LIMIT_MS + DECREMENT_MS) {
        currentTimeLimitMs -= DECREMENT_MS;
      } else {
        currentTimeLimitMs = MIN_TIME_LIMIT_MS;
      }
      LOG(F("Next round time limit (s): "));
      LOGLN(currentTimeLimitMs / 1000);

      if (currentAction == ACTION_NFC) {
        waitForTagRemoval();
      } else if (currentAction == ACTION_IR) {
        delay(250);
      } else {
        while (keypadAnyKeyPressed()) { delay(50); }
      }

      state = ROUND_PICK;
    } break;

    case GAME_OVER: {
      LOG(F("Final score: ")); LOGLN(score);
      LOGLN(F("Reset board to play again."));
      while (1) { delay(1000); }
    } break;

    case READY:
    default: break;
  }
}

/* NFC action */
bool doNfcAction(String &observedText) {
  uint8_t uid[7], uidLen = 0;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50)) {
    return false;
  }
  uint8_t page3[4];
  if (!nfc.ntag2xx_ReadPage(3, page3)) {
    return false;
  }
  const uint16_t totalBytes = (NTAG_END_PAGE - NTAG_START_PAGE + 1) * 4;
  uint8_t buf[totalBytes];
  uint16_t idx = 0;
  for (uint8_t p = NTAG_START_PAGE; p <= NTAG_END_PAGE; p++) {
    uint8_t pg[4];
    if (!nfc.ntag2xx_ReadPage(p, pg)) return false;
    for (uint8_t k = 0; k < 4; k++) buf[idx++] = pg[k];
  }
  String text;
  if (!findNdefTextFromBuffer(buf, idx, text)) return false;
  observedText = text;
  return true;
}

/* IR action */
bool doIrAction() {
  int s = digitalRead(IR_PIN);
  if (s == LOW) {
    LOGLN(F("IR covered."));
    return true;
  }
  delay(25);
  return false;
}

/* Check if any ISO14443A NFC target is present (short timeout) */
bool isAnyNfcPresent(uint16_t timeout_ms) {
  uint8_t uid[7], uidLen = 0;
  return nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, timeout_ms);
}

/* Minimal NDEF TLV + Text record parser (Short Record, type 'T') */
bool findNdefTextFromBuffer(uint8_t *buf, uint16_t len, String &outText) {
  int tlv = -1;
  for (uint16_t i = 0; i + 1 < len; i++) {
    if (buf[i] == 0x03) { tlv = i; break; }
  }
  if (tlv < 0) return false;

  uint16_t ndefLen = buf[tlv + 1];
  uint16_t nStart  = tlv + 2;
  if (ndefLen == 0xFF || nStart + ndefLen > len) return false;

  uint8_t *n = &buf[nStart];
  if (ndefLen < 4) return false;
  if (n[0] != 0xD1) return false;
  if (n[1] != 0x01) return false;
  if (n[3] != 0x54) return false;

  uint8_t payloadLen = n[2];
  if (payloadLen == 0 || (uint16_t)(3 + 1 + payloadLen) > ndefLen) return false;

  uint16_t p0 = 4;
  uint8_t status = n[p0];
  uint8_t langLen = status & 0x3F;
  if ((uint16_t)(p0 + 1 + langLen) > (uint16_t)(3 + 1 + payloadLen)) return false;

  uint16_t textStart = p0 + 1 + langLen;
  uint16_t textLen   = payloadLen - 1 - langLen;
  if (textLen == 0) return false;

  outText.reserve(textLen);
  outText = "";
  for (uint16_t i = 0; i < textLen; i++) outText += (char)n[textStart + i];
  return true;
}

/* Keypad setup: rows INPUT_PULLUP, columns OUTPUT HIGH */
void keypadInit() {
  pinMode(KP_R1, INPUT_PULLUP);
  pinMode(KP_R2, INPUT_PULLUP);
  pinMode(KP_R3, INPUT_PULLUP);
  pinMode(KP_R4, INPUT_PULLUP);

  pinMode(KP_C1, OUTPUT);
  pinMode(KP_C2, OUTPUT);
  pinMode(KP_C3, OUTPUT);
  digitalWrite(KP_C1, HIGH);
  digitalWrite(KP_C2, HIGH);
  digitalWrite(KP_C3, HIGH);
}

/* Scan keypad; returns pressed key char or 0 if none. */
char keypadGetKey(bool waitRelease) {
  const uint8_t cols[3] = {KP_C1, KP_C2, KP_C3};
  const uint8_t rows[4] = {KP_R1, KP_R2, KP_R3, KP_R4};

  for (uint8_t c = 0; c < 3; c++) {
    for (uint8_t cc = 0; cc < 3; cc++) digitalWrite(cols[cc], (cc == c) ? LOW : HIGH);

    for (uint8_t r = 0; r < 4; r++) {
      if (digitalRead(rows[r]) == LOW) {
        char k = KEYS[r][c];
        if (waitRelease) {
          delay(10);
          while (digitalRead(rows[r]) == LOW) { delay(5); }
          delay(10);
        }
        digitalWrite(KP_C1, HIGH); digitalWrite(KP_C2, HIGH); digitalWrite(KP_C3, HIGH);
        return k;
      }
    }
  }
  digitalWrite(KP_C1, HIGH); digitalWrite(KP_C2, HIGH); digitalWrite(KP_C3, HIGH);
  return 0;
}

/* Any keypad activity? */
bool keypadAnyKeyPressed() {
  return keypadGetKey(false) != 0;
}

/* Begin new keypad round: create random 4-digit PIN and reset entry buffer */
void keypadStartNewPin() {
  for (uint8_t i = 0; i < 4; i++) {
    pinTarget[i] = '0' + (char)random(0, 10);
  }
  pinTarget[4] = 0;
  pinEntered[0] = pinEntered[1] = pinEntered[2] = pinEntered[3] = 0;
  pinEntered[4] = 0;
  pinPos = 0;
  pinShownThisRound = false;
}

/* Handle keypad round */
bool doKeypadAction() {
  if (!pinShownThisRound) {
    LOG(F("PIN: "));
    LOGLN(pinTarget);
    LOGLN(F("Enter the 4 digits on the keypad."));
    pinShownThisRound = true;
  }

  char k = keypadGetKey(true);
  if (k == 0) {
    return false;
  }

  if (k < '0' || k > '9') {
    LOGLN(F("Non-digit key pressed. Game over."));
    state = GAME_OVER;
    return false;
  }

  if (pinPos < 4) {
    pinEntered[pinPos++] = k;
    LOG(F("Key: ")); LOGLN(k);
  }

  if (pinPos == 4) {
    pinEntered[4] = 0;
    LOG(F("You entered: ")); LOGLN(pinEntered);
    if (strncmp(pinEntered, pinTarget, 4) == 0) {
      return true;
    } else {
      LOGLN(F("Incorrect PIN. Game over."));
      state = GAME_OVER;
      return false;
    }
  }
  return false;
}

/* Wait until NFC tag is removed */
void waitForTagRemoval() {
  uint8_t uid[7], uidLen;
  LOGLN(F("Remove tag..."));
  while (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 200)) {
    delay(100);
  }
  LOGLN(F("OK."));
}
