#include <LiquidCrystal.h>
#include <Keypad.h>
#include <SPI.h>
#include <MFRC522.h>

#define buzzer 8
#define trigPin 9
#define echoPin 10

// ---- Area 2: PIR motion sensor ----
#define pirPin 26                      // PIR on D26
// Runtime-togglable polarity with the 'D' key on the idle screen:
bool pirActiveHigh = false;            // default: using INPUT_PULLUP, so LOW=motion (set true if your PIR idles LOW)
const unsigned long PIR_WARMUP_MS = 30000UL;   // 30s warmup
const unsigned long PIR_STABLE_MS  = 200UL;    // minimal stable active time for an "event"

// RFID (MFRC522)
#define RST_PIN 22
#define SS_PIN 53
MFRC522 mfrc(SS_PIN, RST_PIN);

long duration;
int distance, initialDistance, currentDistance;
int screenOffMsg = 0;
String password = "1234";
boolean activateAlarm = false;
boolean alarmActivated = false;
boolean passChangeMode = false;

const byte ROWS = 4;
const byte COLS = 4;
char keypressed;
char keyMap[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {14, 15, 16, 17};
byte colPins[COLS] = {18, 19, 20, 21};

Keypad myKeypad = Keypad(makeKeymap(keyMap), rowPins, colPins, ROWS, COLS);
LiquidCrystal lcd(1, 2, 4, 5, 6, 7);

// ---------------- RGB LED pins ----------------
const int pinRGB_R = 11;
const int pinRGB_G = 12;
const int pinRGB_B = 13;
// If your module is common-anode (HIGH = off), set INVERT_RGB = true
const bool INVERT_RGB = false;

void setRGB(bool r, bool g, bool b) {
  bool outR = r ^ INVERT_RGB;
  bool outG = g ^ INVERT_RGB;
  bool outB = b ^ INVERT_RGB;
  digitalWrite(pinRGB_R, outR ? HIGH : LOW);
  digitalWrite(pinRGB_G, outG ? HIGH : LOW);
  digitalWrite(pinRGB_B, outB ? HIGH : LOW);
}

// ---------------- Authorized UIDs (4 bytes each) ----------------
byte authorizedUID[][4] = {
  {0x47, 0xF3, 0xBA, 0x79},
  {0xE0, 0xD9, 0xC2, 0xA3}
};
const byte authorizedCount = 2;

// ---------------- Time from compile/upload ----------------
const char compileTime[] = __TIME__;
int startHour = 0, startMinute = 0, startSecond = 0;
unsigned long bootMillis = 0;
bool timerRunning = false;

void parseCompileTime() {
  int h = 0, m = 0, s = 0;
  if (sscanf(compileTime, "%d:%d:%d", &h, &m, &s) == 3) {
    startHour = h % 24;
    startMinute = m % 60;
    startSecond = s % 60;
  } else {
    startHour = 0; startMinute = 0; startSecond = 0;
  }
}

String getTimeString() {
  unsigned long elapsedSec = (millis() - bootMillis) / 1000UL;
  unsigned long baseSec = (unsigned long)startHour * 3600UL + (unsigned long)startMinute * 60UL + (unsigned long)startSecond;
  unsigned long totalSec = baseSec + elapsedSec;

  unsigned long seconds = totalSec % 60UL;
  unsigned long minutes = (totalSec / 60UL) % 60UL;
  unsigned long hours = (totalSec / 3600UL) % 24UL;

  char buf[9];
  sprintf(buf, "%02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(buf);
}

// ---------------- Alarm History ----------------
#define HISTORY_SIZE 5
struct AlarmRecord { String triggered; String on; String off; };
AlarmRecord alarmHistory[HISTORY_SIZE];
int historyIndex = 0;
int historyCount = 0;

// ---------------- Intrusion flags ----------------
bool intrusionRecorded = false;
int intrusionArea = 0;   // 0 = none, 1 = Ultrasonic (A1), 2 = PIR (A2)

// ---------------- Idle movement indicator state ----------------
unsigned long motionA1LastMs = 0;
unsigned long motionA2LastMs = 0;
const unsigned long MOTION_DISPLAY_MS = 2000;  // how long to keep the "M.AreaX" hint visible
const int MOTION_THRESHOLD_CM = 20;            // Area 1 "near" threshold

// ---------------- Police light variables ----------------
unsigned long lastPoliceToggle = 0;
const unsigned long POLICE_INTERVAL_MS = 250;
bool policeState = false;

// ---------------- Siren (up/down) ----------------
bool sirenActive = false;
unsigned long lastSirenStep = 0;
int sirenFreq = 0;
int sirenDir = +1;

const int SIREN_MIN_HZ = 600;      // lower end of sweep
const int SIREN_MAX_HZ = 1800;     // upper end of sweep
const int SIREN_STEP_HZ = 20;      // frequency increment per step
const unsigned long SIREN_STEP_MS = 20; // step time; lower = faster sweep

void startSiren() {
  sirenActive = true;
  sirenDir = +1;
  sirenFreq = SIREN_MIN_HZ;
  lastSirenStep = 0;
  tone(buzzer, sirenFreq);
}

void stopSiren() {
  sirenActive = false;
  noTone(buzzer);
}

void updateSiren() {
  if (!sirenActive) return;
  unsigned long now = millis();
  if (now - lastSirenStep >= SIREN_STEP_MS) {
    lastSirenStep = now;
    sirenFreq += sirenDir * SIREN_STEP_HZ;
    if (sirenFreq >= SIREN_MAX_HZ) {
      sirenFreq = SIREN_MAX_HZ;
      sirenDir = -1;
    } else if (sirenFreq <= SIREN_MIN_HZ) {
      sirenFreq = SIREN_MIN_HZ;
      sirenDir = +1;
    }
    tone(buzzer, sirenFreq);
  }
}

// forward
void displayIdleScreen();

// ---------------- Small utils ----------------
String safeStr(const String& s, const char* fallback) {
  return (s.length() > 0) ? s : String(fallback);
}

int idxFromNewestPos(int pos) {
  int base = historyIndex - 1 - pos;
  while (base < 0) base += HISTORY_SIZE;
  return base % HISTORY_SIZE;
}

void buildHistoryLines(String* lines, int& lineCount) {
  lineCount = 0;
  if (historyCount == 0) {
    lines[lineCount++] = "Alarm History:";
    lines[lineCount++] = "No records";
    return;
  }

  for (int p = 0; p < historyCount; p++) {
    int idx = idxFromNewestPos(p);
    AlarmRecord rec = alarmHistory[idx];

    String hdr = "Alarm ";
    hdr += String(p + 1);
    hdr += ":";
    lines[lineCount++] = hdr;

    lines[lineCount++] = String("On:  ") + safeStr(rec.on, "n/a");
    lines[lineCount++] = String("Off: ") + safeStr(rec.off, "n/a");
    lines[lineCount++] = String("Trig:") + safeStr(rec.triggered, "n/a");

    lines[lineCount++] = "-----";
  }
  lines[lineCount++] = "******END******";
}

// ---------------- Show Alarm History ----------------
void showAlarmHistory() {
  const int MAX_LINES = HISTORY_SIZE * 5 + 1;
  String lines[MAX_LINES];
  int lineCount = 0;
  buildHistoryLines(lines, lineCount);

  int topIdx = 0;
  auto render = [&]() {
    lcd.clear();
    lcd.setCursor(0,0);
    if (topIdx < lineCount) {
      String s0 = lines[topIdx];
      if (s0.length() > 16) s0 = s0.substring(0,16);
      lcd.print(s0);
    }
    lcd.setCursor(0,1);
    if (topIdx + 1 < lineCount) {
      String s1 = lines[topIdx + 1];
      if (s1.length() > 16) s1 = s1.substring(0,16);
      lcd.print(s1);
    }
  };

  render();

  bool browsing = true;
  while (browsing) {
    char k = myKeypad.getKey();
    if (k != NO_KEY) {
      if (k == '8') {
        if (topIdx + 2 < lineCount) {
          topIdx++;
          tone(buzzer, 1400, 30);
          render();
        } else {
          tone(buzzer, 900, 25);
        }
      } else if (k == '2') {
        if (topIdx > 0) {
          topIdx--;
          tone(buzzer, 1400, 30);
          render();
        } else {
          tone(buzzer, 900, 25);
        }
      } else if (k == '#' || k == 'D' || k == 'B') {
        browsing = false;
      }
    }
    delay(20);
  }

  if (historyCount > 0) {
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Clear history?");
    lcd.setCursor(0,1); lcd.print("A-Yes B-No");
    while (true) {
      char k = myKeypad.getKey();
      if (k == 'A') {
        for (int i = 0; i < HISTORY_SIZE; i++) {
          alarmHistory[i].triggered = "";
          alarmHistory[i].on = "";
          alarmHistory[i].off = "";
        }
        historyCount = 0; historyIndex = 0;
        tone(buzzer, 1500, 200);
        lcd.clear(); lcd.setCursor(0,0); lcd.print("History Cleared");
        delay(800); break;
      } else if (k == 'B' || k == 'D' || k == '#') {
        lcd.clear(); lcd.setCursor(0,0); lcd.print("Canceled");
        delay(600); break;
      }
      delay(40);
    }
  }
  displayIdleScreen(); screenOffMsg = 0;
}

// ---------------- Display time and idle ----------------
void displayTime() {
  if (!timerRunning) return;
  String t = getTimeString();
  lcd.setCursor(8,1); lcd.print(t);
}

void displayIdleScreen() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("A-Act B-Hst C-Ps"); // B=History, C=Password
  // Row 1 is dynamically rendered each loop (status/clock)
  displayTime();
  setRGB(false, false, false);
}

// ---------------- Distance Measurement (Area 1) ----------------
long getDistance() {
  digitalWrite(trigPin, LOW); delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long timeout = 20000UL;
  long dur = pulseIn(echoPin, HIGH, timeout);
  if (dur == 0) return 0;
  long dist = dur * 0.034 / 2;
  return dist;
}

// ---------------- PIR (Area 2) runtime-togglable polarity & stability ----------------
unsigned long pirWarmupUntil = 0;
unsigned long pirActiveStart  = 0;
bool pirWasActive = false;

void setupPIR() {
  // Use pull-up by default to fight floating outputs; invert via pirActiveHigh if needed
  pinMode(pirPin, INPUT_PULLUP);
  pirWarmupUntil = millis() + PIR_WARMUP_MS;
  pirWasActive = false;
}

bool pirActiveNow() {
  int raw = digitalRead(pirPin);     // with PULLUP: idle tends to read HIGH if floating
  bool active = pirActiveHigh ? (raw == HIGH) : (raw == LOW);
  return active;
}

// Returns true once per active-hold when PIR is stably active for PIR_STABLE_MS
bool pirMotionDetectedStable() {
  unsigned long now = millis();
  if (now < pirWarmupUntil) return false;

  bool active = pirActiveNow();

  if (active) {
    if (!pirWasActive) {
      pirActiveStart = now;
      pirWasActive = true;
    }
    if (now - pirActiveStart >= PIR_STABLE_MS) {
      // single-shot per active hold; wait for inactive before next event
      pirActiveStart = now + 0x7FFFFFFF;
      return true;
    }
  } else {
    pirWasActive = false;
  }
  return false;
}

// ---------------- Idle movement indicator rendering ----------------
void updateIdleIndicators() {
  unsigned long now = millis();

  // --- Sense A1 (ultrasonic) "near" ---
  long d = getDistance();
  bool a1_near = (d > 0 && d < MOTION_THRESHOLD_CM);
  if (a1_near) motionA1LastMs = now;

  // --- Sense A2 (PIR) ---
  // For the idle hint, we treat current "active" (not just stable) as motion,
  // but honor warm-up suppression.
  bool a2_active = (now >= pirWarmupUntil) && pirActiveNow();
  if (pirMotionDetectedStable()) {            // keep stability for actual "event"
    motionA2LastMs = now;
  } else if (a2_active) {
    // also refresh the "recent" timer if currently active
    motionA2LastMs = now;
  }

  // Decide what to show (most recent movement within window)
  bool showA1 = (now - motionA1LastMs) <= MOTION_DISPLAY_MS;
  bool showA2 = (now - motionA2LastMs) <= MOTION_DISPLAY_MS;

  // Pick the most recent one if both
  const char* msg = "        "; // 8 spaces
  if (showA1 && showA2) {
    msg = (motionA1LastMs >= motionA2LastMs) ? "M.Area1 " : "M.Area2 ";
  } else if (showA1) {
    msg = "M.Area1 ";
  } else if (showA2) {
    msg = "M.Area2 ";
  }

  // Render exactly 8 chars on the left; clock remains at col 8..15
  lcd.setCursor(0,1);
  for (int i = 0; i < 8; i++) {
    char c = msg[i];
    if (c == '\0') c = ' ';
    lcd.print(c);
  }
}

// ---------------- Password Change ----------------
void changePassword() {
  lcd.clear(); tone(buzzer, 2000, 100); passChangeMode = true;
  String temp = ""; int pos = 1;
  lcd.setCursor(0,0); lcd.print("Current Password");
  lcd.setCursor(0,1); lcd.print(">");

  while (passChangeMode) {
    char k = myKeypad.getKey();
    if (k != NO_KEY) {
      if (k >= '0' && k <= '9') {
        if (pos < 9) { // allow up to 8 digits
          temp += k; lcd.setCursor(pos,1); lcd.print("*"); pos++; tone(buzzer, 2000, 50);
        }
      } else if (k == '#') {
        temp = ""; pos = 1; lcd.setCursor(0,1); lcd.print(">            ");
      } else if (k == '*') {
        if (temp == password) {
          temp = ""; pos = 1;
          lcd.clear(); lcd.setCursor(0,0); lcd.print("Set New Password");
          lcd.setCursor(0,1); lcd.print(">");
          while (true) {
            char k2 = myKeypad.getKey();
            if (k2 != NO_KEY) {
              if (k2 >= '0' && k2 <= '9') {
                if (pos < 9) {
                  temp += k2; lcd.setCursor(pos,1); lcd.print("*"); pos++; tone(buzzer, 2000, 50);
                }
              } else if (k2 == '#') {
                temp = ""; pos = 1; lcd.setCursor(0,1); lcd.print(">            ");
              } else if (k2 == '*') {
                if (temp.length() > 0) {
                  password = temp;
                  lcd.clear(); lcd.setCursor(0,0); lcd.print("Password Saved");
                  tone(buzzer, 1500, 200); delay(800);
                } else {
                  lcd.clear(); lcd.setCursor(0,0); lcd.print("Cancelled"); delay(600);
                }
                passChangeMode = false; screenOffMsg = 0; return;
              }
            }
          }
        } else {
          lcd.clear(); lcd.setCursor(0,0); lcd.print("Wrong Password");
          tone(buzzer, 1200, 200); delay(1000);
          temp = ""; pos = 1;
          lcd.clear(); lcd.setCursor(0,0); lcd.print("Current Password");
          lcd.setCursor(0,1); lcd.print(">");
        }
      } else if (k == 'D') {
        passChangeMode = false; lcd.clear(); screenOffMsg = 0; return;
      }
    }
  }
}

// ---------------- RFID check ----------------
bool isAuthorizedTag() {
  if (!mfrc.PICC_IsNewCardPresent()) return false;
  if (!mfrc.PICC_ReadCardSerial()) return false;

  byte *uid = mfrc.uid.uidByte;
  byte uidSize = mfrc.uid.size;
  if (uidSize != 4) { mfrc.PICC_HaltA(); return false; }

  for (byte i = 0; i < authorizedCount; i++) {
    bool match = true;
    for (byte b = 0; b < 4; b++) {
      if (authorizedUID[i][b] != uid[b]) { match = false; break; }
    }
    if (match) { mfrc.PICC_HaltA(); delay(300); return true; }
  }
  mfrc.PICC_HaltA(); delay(200); return false;
}

// ---------------- Unified Authentication Prompt ----------------
bool authenticateWithPrompt(const char* title) {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(title);
  lcd.setCursor(0,1); lcd.print("Pass/Tag >");
  String temp = ""; int pos = 10; // after "Pass/Tag >"
  const int MAX_LEN = 8;

  while (true) {
    if (isAuthorizedTag()) {
      tone(buzzer, 1200, 150);
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Auth OK (Tag)");
      delay(600); return true;
    }

    char k = myKeypad.getKey();
    if (k != NO_KEY) {
      if (k >= '0' && k <= '9') {
        if ((int)temp.length() < MAX_LEN) {
          temp += k; lcd.setCursor(pos++,1); lcd.print("*"); tone(buzzer, 1800, 40);
        }
      } else if (k == '#') { // clear
        temp = ""; pos = 10; lcd.setCursor(10,1); lcd.print("        ");
      } else if (k == '*' || k == 'A') { // submit
        if (temp == password) {
          tone(buzzer, 1200, 150);
          lcd.clear(); lcd.setCursor(0,0); lcd.print("Auth OK (Pass)");
          delay(600); return true;
        } else {
          tone(buzzer, 800, 150);
          lcd.setCursor(0,1); lcd.print("Wrong!       ");
          delay(900);
          lcd.setCursor(0,1); lcd.print("Pass/Tag >");
          temp = ""; pos = 10; lcd.setCursor(10,1); lcd.print("        ");
        }
      } else if (k == 'D' || k == 'B') { // cancel
        lcd.clear(); lcd.setCursor(0,0); lcd.print("Canceled");
        delay(500); return false;
      }
    }
    delay(25);
  }
}

// ---------------- Setup ----------------
void setup() {
  pinMode(pinRGB_R, OUTPUT);
  pinMode(pinRGB_G, OUTPUT);
  pinMode(pinRGB_B, OUTPUT);
  setRGB(false, false, false);

  lcd.begin(16,2);
  pinMode(buzzer, OUTPUT);
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  setupPIR(); // <-- PIR input init

  pinMode(53, OUTPUT);
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, HIGH);

  SPI.begin();
  mfrc.PCD_Init();

  parseCompileTime();
  bootMillis = millis();
  timerRunning = true;

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("System Ready");
  delay(800);
  screenOffMsg = 0;
}

// ---------------- Main Loop ----------------
void loop() {
  // ------------------- Idle Menu -------------------
  if (!alarmActivated && !activateAlarm) {
    if (screenOffMsg == 0) { displayIdleScreen(); screenOffMsg = 1; }
    displayTime();
    updateIdleIndicators();  // <- shows "M.Area1" / "M.Area2" on the left
    // RGB off when idle & no recent A1 movement (visual calm)
    if ((millis() - motionA1LastMs) > MOTION_DISPLAY_MS) setRGB(false, false, false);

    keypressed = myKeypad.getKey();
    if (keypressed == 'A') {
      if (authenticateWithPrompt("Auth to ACTIVATE")) {
        tone(buzzer, 1000, 200);
        activateAlarm = true;  // go to countdown
        screenOffMsg = 0;
      } else {
        displayIdleScreen(); screenOffMsg = 1;
      }
    }
    else if (keypressed == 'B') { // B = History
      showAlarmHistory();
    }
    else if (keypressed == 'C') { // C = Password change
      changePassword();
    }
    else if (keypressed == 'D') {
      // Runtime toggle PIR polarity for quick hardware compatibility
      pirActiveHigh = !pirActiveHigh;
      tone(buzzer, 1400, 80);
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("PIR Polarity:");
      lcd.setCursor(0,1); lcd.print(pirActiveHigh ? "Active HIGH" : "Active LOW");
      delay(900);
      displayIdleScreen(); screenOffMsg = 1;
    }
  }

  // ------------------- Countdown before alarm -------------------
  if (activateAlarm) {
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Alarm will be");
    lcd.setCursor(0,1); lcd.print("activated in");

    int countdown = 9;
    while (countdown > 0 && activateAlarm) {
      lcd.setCursor(13,1); lcd.print(" "); lcd.setCursor(13,1); lcd.print(countdown);

      setRGB(false, true, false); // blink green (on)
      // Allow cancel by Tag (disarm) OR by B / C
      if (isAuthorizedTag()) {
        activateAlarm = false;
        lcd.clear(); lcd.setCursor(0,0); lcd.print("Disarmed by Tag");
        stopSiren(); // ensure off
        setRGB(false, false, false);
        delay(800); screenOffMsg = 0; break;
      }

      char k = myKeypad.getKey();
      if (k == 'B' || k == 'C') { // cancel countdown
        activateAlarm = false;
        lcd.clear(); lcd.setCursor(0,0); lcd.print("Activation Canceled");
        stopSiren(); // ensure off
        setRGB(false, false, false);
        delay(800); screenOffMsg = 0; break;
      }

      tone(buzzer, 700, 100);
      delay(500);
      setRGB(false, false, false); // blink off
      delay(500);
      countdown--;
    }

    if (countdown <= 0 && activateAlarm) {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Alarm Activated!");
      initialDistance = getDistance();         // baseline for Area 1
      activateAlarm = false; alarmActivated = true;

      // Record On time at activation
      alarmHistory[historyIndex].on = getTimeString();
      intrusionRecorded = false;
      intrusionArea = 0;
      stopSiren(); // not sounding until intrusion
      setRGB(false, true, false); // stable green
    }
  }

  // ------------------- Alarm Active -------------------
  if (alarmActivated) {
    if (!intrusionRecorded) {
      setRGB(false, true, false); // stable green
      stopSiren();                // ensure siren is off until intrusion
    }

    currentDistance = getDistance();
    int threshold = 10;

    bool a1_trigger = (currentDistance > 0 && currentDistance < initialDistance - threshold); // Area 1 (Ultrasonic)
    bool a2_trigger = pirMotionDetectedStable();                                              // Area 2 (PIR)

    if (a1_trigger || a2_trigger) {
      if (!intrusionRecorded) {
        intrusionRecorded = true;
        intrusionArea = a1_trigger ? 1 : 2;
        // record trigger with area note
        String t = getTimeString();
        alarmHistory[historyIndex].triggered = t + (intrusionArea == 1 ? " A1" : " A2");
        lastPoliceToggle = millis(); policeState = false;
        startSiren(); // start pulsing siren on first intrusion
      }

      // LCD text while intruding
      lcd.setCursor(0,0);
      if (intrusionArea == 1) lcd.print("*** INTRUSION A1***");
      else                    lcd.print("*** INTRUSION A2***");
      lcd.setCursor(0,1); lcd.print("B - Disarm ");
    } else if (!intrusionRecorded) {
      // armed but no intrusion
      lcd.setCursor(0,0); lcd.print("ALARM ARMED     ");
      lcd.setCursor(0,1); lcd.print("B - Disarm ");
    }

    // Police lights + siren while intrusion latched
    if (intrusionRecorded) {
      unsigned long now = millis();
      if (now - lastPoliceToggle >= POLICE_INTERVAL_MS) {
        lastPoliceToggle = now; policeState = !policeState;
        if (policeState) setRGB(true, false, false);
        else setRGB(false, false, true);
      }
      updateSiren();
    }

    // Tag always disarms
    if (isAuthorizedTag()) {
      alarmActivated = false;
      stopSiren();
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Disarmed by Tag");
      // record Off and roll index
      alarmHistory[historyIndex].off = getTimeString();
      historyIndex = (historyIndex + 1) % HISTORY_SIZE;
      if (historyCount < HISTORY_SIZE) historyCount++;
      setRGB(false, false, false);
      delay(800); screenOffMsg = 0;
    }

    // Press B to disarm (requires auth)
    char k = myKeypad.getKey();
    if (k == 'B') {
      if (authenticateWithPrompt("Auth to DISARM")) {
        alarmActivated = false;
        stopSiren();
        lcd.clear(); lcd.setCursor(0,0); lcd.print("Disarmed (OK)");
        // record Off and roll index
        alarmHistory[historyIndex].off = getTimeString();
        historyIndex = (historyIndex + 1) % HISTORY_SIZE;
        if (historyCount < HISTORY_SIZE) historyCount++;
        setRGB(false, false, false);
        delay(800); screenOffMsg = 0;
      } else {
        lcd.clear(); lcd.setCursor(0,0); lcd.print("Disarm canceled");
        delay(700);
      }
    }
  }
}
