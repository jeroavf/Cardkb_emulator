#include <Wire.h>
#include <M5Cardputer.h>

// ===================== CardKB emulation =====================
static const uint8_t CARDKB_ADDR = 0x5F;

// ===================== Buzzer settings =====================
static const int BEEP_FREQ = 1800;  // Hz
static const int BEEP_DURATION = 10; // ms

// ===================== Display mode =====================
static bool debugMode = false;
static const uint32_t CONNECTION_TIMEOUT_MS = 2000; // consider disconnected after 2s no requests

// Text preview buffer (last typed characters)
static const int PREVIEW_SIZE = 40;
static char previewBuf[PREVIEW_SIZE + 1] = {0};
static int previewLen = 0;

// CardKB arrow codes (common)
static const uint8_t KB_LEFT  = 0xB4;
static const uint8_t KB_UP    = 0xB5;
static const uint8_t KB_DOWN  = 0xB6;
static const uint8_t KB_RIGHT = 0xB7;

// ===================== Ring buffer =====================
static const int QSIZE = 128;
volatile uint8_t q[QSIZE];
volatile int qh = 0, qt = 0;

static inline int q_count() {
  int c = qt - qh;
  if (c < 0) c += QSIZE;
  return c;
}

static inline void q_push(uint8_t c) {
  int n = (qt + 1) % QSIZE;
  if (n == qh) return; // drop if full
  q[qt] = c;
  qt = n;
}

static inline uint8_t q_pop() {
  if (qh == qt) return 0x00;
  uint8_t c = q[qh];
  qh = (qh + 1) % QSIZE;
  return c;
}

// ===================== UI helpers =====================
static uint8_t last_enq = 0x00;
static uint8_t last_tx  = 0x00;

static uint32_t last_enq_ms = 0;
static uint32_t last_tx_ms  = 0;

// Request status tracking
static volatile uint32_t last_req_ms = 0;
static volatile bool req_served = false;

static const char* decodeSpecial(uint8_t v) {
  switch (v) {
    case 0x00: return "NONE";
    case 0x08: return "BACKSPACE";
    case 0x09: return "TAB";
    case 0x0A: return "LF";
    case 0x0D: return "ENTER (CR)";
    case 0x1B: return "ESC";
    case KB_LEFT:  return "ARROW_LEFT";
    case KB_UP:    return "ARROW_UP";
    case KB_DOWN:  return "ARROW_DOWN";
    case KB_RIGHT: return "ARROW_RIGHT";
    default: return nullptr;
  }
}

// Add character to preview buffer
static void previewAdd(uint8_t c) {
  if (c == 0x08) { // Backspace
    if (previewLen > 0) {
      previewLen--;
      previewBuf[previewLen] = '\0';
    }
  } else if (c == 0x0D || c == 0x0A) { // Enter/LF - clear preview
    previewLen = 0;
    previewBuf[0] = '\0';
  } else if (c >= 0x20 && c <= 0x7E) { // Printable ASCII
    if (previewLen >= PREVIEW_SIZE) {
      // Shift buffer left
      memmove(previewBuf, previewBuf + 1, PREVIEW_SIZE - 1);
      previewLen = PREVIEW_SIZE - 1;
    }
    previewBuf[previewLen++] = (char)c;
    previewBuf[previewLen] = '\0';
  }
  // Arrow keys and other special codes don't appear in preview
}

static bool isConnected() {
  return (last_req_ms > 0) && (millis() - last_req_ms < CONNECTION_TIMEOUT_MS);
}

static void drawUserDisplay() {
  auto &d = M5Cardputer.Display;

  // Header
  d.setTextSize(2);
  d.setCursor(0, 0);
  d.println("CardKB Emulator");

  // Connection status
  d.setCursor(0, 30);
  d.setTextSize(2);
  if (isConnected()) {
    d.setTextColor(GREEN, BLACK);
    d.println("  * Connected");
  } else {
    d.setTextColor(DARKGREY, BLACK);
    d.println("  * Waiting...");
  }
  d.setTextColor(WHITE, BLACK);

  // Text preview area
  d.setCursor(0, 70);
  d.setTextSize(2);
  d.println("Text:");

  d.setCursor(0, 95);
  d.setTextSize(2);
  if (previewLen > 0) {
    // Show last portion that fits on screen (about 20 chars at size 2)
    int startIdx = (previewLen > 20) ? previewLen - 20 : 0;
    d.printf("%s_", &previewBuf[startIdx]);
  } else {
    d.setTextColor(DARKGREY, BLACK);
    d.println("_");
    d.setTextColor(WHITE, BLACK);
  }

  // Help hint at bottom
  d.setCursor(0, 125);
  d.setTextSize(1);
  d.setTextColor(DARKGREY, BLACK);
  d.println("Fn+D: debug mode");
  d.setTextColor(WHITE, BLACK);
}

static void drawDebugDisplay() {
  auto &d = M5Cardputer.Display;

  // Request status line at top
  d.setTextSize(2);
  d.setCursor(0, 0);
  if (last_req_ms > 0) {
    if (req_served) {
      d.printf("t=%lu REQ SRVD\n", (unsigned long)last_req_ms);
    } else {
      d.printf("t=%lu REQ RCVD\n", (unsigned long)last_req_ms);
    }
  } else {
    d.println("No request yet");
  }

  d.setTextSize(2);
  d.println("CardKB emu (0x5F)");

  d.setTextSize(2);
  d.printf("Queue: %d/%d\n\n", q_count(), QSIZE - 1);

  // ---- ENQ ----
  d.setTextSize(2);
  d.println("[ENQ] last queued:");

  d.setTextSize(3);
  d.printf("0x%02X\n", last_enq);

  const char* sp1 = decodeSpecial(last_enq);
  d.setTextSize(2);

  if (sp1) {
    d.printf("%s\n", sp1);
  } else if (last_enq >= 0x20 && last_enq <= 0x7E) {
    d.printf("ASCII '%c'\n", (char)last_enq);
  } else {
    d.println("ASCII/CTRL ?");
  }

  d.setTextSize(2);
  if (last_enq_ms) {
    d.printf("t=%lums\n\n", (unsigned long)last_enq_ms);
  } else {
    d.println("t=---\n");
  }

  // ---- TX ----
  d.setTextSize(2);
  d.println("[TX] last I2C sent:");

  d.setTextSize(3);
  d.printf("0x%02X\n", last_tx);

  const char* sp2 = decodeSpecial(last_tx);
  d.setTextSize(2);
  if (sp2) {
    d.printf("%s\n", sp2);
  } else if (last_tx >= 0x20 && last_tx <= 0x7E) {
    d.printf("ASCII '%c'\n", (char)last_tx);
  } else {
    d.println("ASCII/CTRL ?");
  }

  d.setTextSize(2);
  if (last_tx_ms) {
    d.printf("t=%lums\n", (unsigned long)last_tx_ms);
  } else {
    d.println("t=---\n");
  }

  // Help hint
  d.setTextSize(1);
  d.setTextColor(DARKGREY, BLACK);
  d.println("\nFn+D: user mode");
  d.setTextColor(WHITE, BLACK);
}

static void uiUpdate(bool forceClear = false) {
  static uint8_t prev_enq = 0xFF;
  static uint8_t prev_tx  = 0xFF;
  static int prev_qc = -1;
  static uint32_t prev_req_ms = 0;
  static int prev_previewLen = -1;
  static bool prev_connected = false;
  static bool prev_debugMode = false;

  int qc = q_count();
  bool connected = isConnected();

  bool needsRedraw = forceClear || prev_debugMode != debugMode;

  if (debugMode) {
    needsRedraw = needsRedraw || prev_enq != last_enq || prev_tx != last_tx ||
                  prev_qc != qc || prev_req_ms != last_req_ms;
  } else {
    needsRedraw = needsRedraw || prev_previewLen != previewLen ||
                  prev_connected != connected || prev_enq != last_enq;
  }

  if (needsRedraw) {
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setTextColor(WHITE, BLACK);

    if (debugMode) {
      drawDebugDisplay();
    } else {
      drawUserDisplay();
    }

    prev_enq = last_enq;
    prev_tx  = last_tx;
    prev_qc  = qc;
    prev_req_ms = last_req_ms;
    prev_previewLen = previewLen;
    prev_connected = connected;
    prev_debugMode = debugMode;
  }
}

// ===================== I2C onRequest =====================
void onRequest() {
  last_req_ms = millis();
  req_served = false;

  uint8_t b = q_pop();
  Wire.write(b);

  // UI: confirm what was really sent to Heltec
  last_tx = b;
  last_tx_ms = millis();
  req_served = true;
}

// ===================== Key mapping =====================
static inline uint8_t arrowFromFnChar(char c) {
  if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');

  switch (c) {
    case 'i': return KB_UP;
    case 'k': return KB_DOWN;
    case 'j': return KB_LEFT;
    case 'l': return KB_RIGHT;

    // optional fallback
    case 'w': return KB_UP;
    case 's': return KB_DOWN;
    case 'a': return KB_LEFT;
    case 'd': return KB_RIGHT;
  }
  return 0;
}

static inline void enqueueWithUi(uint8_t b) {
  q_push(b);
  last_enq = b;
  last_enq_ms = millis();
  previewAdd(b);
  M5Cardputer.Speaker.tone(BEEP_FREQ, BEEP_DURATION);
}

// ===================== Setup/Loop =====================
void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true); // enableKeyboard

  // Display init
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  uiUpdate(true);

  // I2C slave @ 0x5F
  Wire.begin((uint8_t)CARDKB_ADDR);
  Wire.onRequest(onRequest);
}

void loop() {
  M5Cardputer.update(); // required to read keyboard

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    bool fn = M5Cardputer.Keyboard.isKeyPressed(KEY_FN);

    auto st = M5Cardputer.Keyboard.keysState();

    // typed chars
    for (auto c : st.word) {
      if (!c) continue;

      if (fn) {
        // Fn+D toggles debug mode
        if (c == 'd' || c == 'D') {
          debugMode = !debugMode;
          uiUpdate(true); // force redraw
          continue;
        }

        uint8_t arrow = arrowFromFnChar((char)c);
        if (arrow) {
          enqueueWithUi(arrow); // enqueue arrow code expected by CardKB/Meshtastic
          continue;             // don't also enqueue the letter
        }
      }
      enqueueWithUi((uint8_t)c);
    }

    // Backspace / Enter
    if (st.del)   enqueueWithUi(0x08);
    if (st.enter) enqueueWithUi(0x0D);
  }

  // Update UI if needed
  uiUpdate(false);

  delay(3);
}
