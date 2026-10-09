#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// pins setup
const int PIN_V = 34, PIN_I = 35;
const int PIN_RELAY = 26, PIN_LED = 27, PIN_BUZZ = 25;
const int PIN_UP = 32, PIN_DN = 33, PIN_SEL = 14;

LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences prefs;

// setings part
struct Settings {
  float ov = 250.0;                 // over voltage limit (V)
  float uv = 180.0;                 // under voltage limit (V)
  float oc = 6.0;                   // over current limit (A)
  unsigned long tripDelay = 500;    // ms
  unsigned long reconDelay = 5000;  // ms
} cfg;

const float HYST_V = 5.0, HYST_I = 0.3;
const float INSTANT_V = 1.15, INSTANT_I = 1.5;

enum State { NORMAL, FAULT, COOLDOWN };
enum FaultType { NONE, OVER_V, UNDER_V, OVER_I };

State state = COOLDOWN;
FaultType lastFault = NONE, pending = NONE;
unsigned long pendingSince = 0, coolStart = 0;
unsigned int faultCount = 0;
float v = 0, i = 0;

// parameter setup
const int NPARAM = 5;
const char* PNAME[NPARAM] = {"OV", "UV", "OC", "DELAY", "RECON"};
const float PSTEP[NPARAM] = {5, 5, 0.5, 100, 500};

float getParam(int k) {
  switch (k) {
    case 0: return cfg.ov;
    case 1: return cfg.uv;
    case 2: return cfg.oc;
    case 3: return cfg.tripDelay;
    default: return cfg.reconDelay;
  }
}

bool setParam(int k, float val) {   
  switch (k) {
    case 0: if (val >= 100 && val <= 300 && val > cfg.uv + 10) { cfg.ov = val; return true; } break;
    case 1: if (val >= 0 && val < cfg.ov - 10) { cfg.uv = val; return true; } break;
    case 2: if (val >= 0.5 && val <= 10) { cfg.oc = val; return true; } break;
    case 3: if (val >= 0 && val <= 5000) { cfg.tripDelay = (unsigned long)val; return true; } break;
    case 4: if (val >= 1000 && val <= 60000) { cfg.reconDelay = (unsigned long)val; return true; } break;
  }
  return false;
}

int findParam(const String& name) {
  for (int k = 0; k < NPARAM; k++) if (name == PNAME[k]) return k;
  return -1;
}

// 
void saveSettings() {
  prefs.putFloat("ov", cfg.ov);
  prefs.putFloat("uv", cfg.uv);
  prefs.putFloat("oc", cfg.oc);
  prefs.putULong("td", cfg.tripDelay);
  prefs.putULong("rd", cfg.reconDelay);
}

void loadSettings() {
  cfg.ov = prefs.getFloat("ov", cfg.ov);
  cfg.uv = prefs.getFloat("uv", cfg.uv);
  cfg.oc = prefs.getFloat("oc", cfg.oc);
  cfg.tripDelay = prefs.getULong("td", cfg.tripDelay);
  cfg.reconDelay = prefs.getULong("rd", cfg.reconDelay);
  if (cfg.ov <= cfg.uv + 10 || cfg.oc < 0.5) cfg = Settings();  // corrupted -> defaults
}

// fault logs
struct LogEntry { uint32_t t; uint8_t type; float v; float i; };
const int LOGSZ = 10;
LogEntry logbuf[LOGSZ];
uint8_t logHead = 0, logN = 0;

const char* faultName(FaultType f) {
  switch (f) {
    case OVER_V: return "OVER-VOLTAGE";
    case UNDER_V: return "UNDER-VOLTAGE";
    case OVER_I: return "OVER-CURRENT";
    default: return "NONE";
  }
}

void saveLog() {
  prefs.putBytes("log", logbuf, sizeof(logbuf));
  prefs.putUChar("lh", logHead);
  prefs.putUChar("ln", logN);
}

void loadLog() {
  if (prefs.getBytesLength("log") == sizeof(logbuf)) {
    prefs.getBytes("log", logbuf, sizeof(logbuf));
    logHead = prefs.getUChar("lh", 0) % LOGSZ;
    logN = min((int)prefs.getUChar("ln", 0), LOGSZ);
  }
}

void addLog(FaultType f) {
  logbuf[logHead] = { (uint32_t)millis(), (uint8_t)f, v, i };
  logHead = (logHead + 1) % LOGSZ;
  if (logN < LOGSZ) logN++;
  saveLog();
}

void printLog() {
  if (!logN) { Serial.println("Log empty"); return; }
  int start = (logHead - logN + LOGSZ) % LOGSZ;
  for (int k = 0; k < logN; k++) {
    LogEntry& e = logbuf[(start + k) % LOGSZ];
    Serial.printf("#%d  uptime=%lu ms  %s  V=%.1f  I=%.2f\n",
                  k + 1, (unsigned long)e.t, faultName((FaultType)e.type), e.v, e.i);
  }
}


float readVoltage() { return analogRead(PIN_V) * 300.0 / 4095.0; }
float readCurrent() { return analogRead(PIN_I) * 10.0 / 4095.0; }

const char* stateName() {
  return state == NORMAL ? "NORMAL" : state == FAULT ? "FAULT" : "COOLDOWN";
}

FaultType detect(bool clearing) {
  float hv = clearing ? HYST_V : 0, hi = clearing ? HYST_I : 0;
  if (v > cfg.ov - hv) return OVER_V;
  if (i > cfg.oc - hi) return OVER_I;
  if (cfg.uv > 0 && v < cfg.uv + hv) return UNDER_V;
  return NONE;
}

void trip(FaultType f) {
  digitalWrite(PIN_RELAY, LOW);     // disconnect first
  state = FAULT;
  lastFault = f;
  faultCount++;
  Serial.printf("[%lu ms] TRIP: %s  V=%.1f I=%.2f  (persisted %lu ms)\n",
                millis(), faultName(f), v, i, millis() - pendingSince);
  addLog(f);
}

// protection
void sampleTask() {
  static unsigned long last = 0;
  if (millis() - last < 10) return;
  last = millis();
  v = 0.2 * readVoltage() + 0.8 * v;
  i = 0.2 * readCurrent() + 0.8 * i;
}

void protectionTask() {
  unsigned long now = millis();
  switch (state) {
    case NORMAL: {
      FaultType f = detect(false);
      if (f == NONE) { pending = NONE; break; }
      if (f != pending) { pending = f; pendingSince = now; }
      bool instant = (v > cfg.ov * INSTANT_V) || (i > cfg.oc * INSTANT_I);
      if (instant || now - pendingSince >= cfg.tripDelay) trip(f);
      break;
    }
    case FAULT:
      if (detect(true) == NONE) {
        state = COOLDOWN; coolStart = now;
        Serial.printf("[%lu ms] Fault cleared, cooldown started\n", now);
      }
      break;
    case COOLDOWN: {
      FaultType f = detect(true);
      if (f != NONE) {
        lastFault = f; state = FAULT;
      } else if (now - coolStart >= cfg.reconDelay) {
        state = NORMAL; pending = NONE;
        digitalWrite(PIN_RELAY, HIGH);
        Serial.printf("[%lu ms] Load reconnected\n", now);
      }
      break;
    }
  }
  digitalWrite(PIN_LED, state != NORMAL);
  digitalWrite(PIN_BUZZ, state == FAULT && ((now / 300) % 2));
}

// buttons and menu
bool menuOn = false;
int menuIdx = 0;
unsigned long lastBtn = 0;

void exitMenu(const char* why) {
  menuOn = false;
  saveSettings();
  Serial.printf("Menu closed (%s), settings saved\n", why);
}

void onPress(int k) {                       // 0=UP 1=DOWN 2=SELECT
  lastBtn = millis();
  if (k == 2) {
    if (!menuOn) { menuOn = true; menuIdx = 0; }
    else if (++menuIdx >= NPARAM) exitMenu("done");
  } else if (menuOn) {
    float step = (k == 0 ? 1 : -1) * PSTEP[menuIdx];
    setParam(menuIdx, getParam(menuIdx) + step);   
  }
}

void buttonTask() {
  static unsigned long last = 0;
  static bool raw[3], stable[3];
  static unsigned long chg[3], rep[3];
  const int pins[3] = {PIN_UP, PIN_DN, PIN_SEL};
  unsigned long now = millis();
  if (now - last < 10) return;
  last = now;
  for (int k = 0; k < 3; k++) {
    bool r = !digitalRead(pins[k]);
    if (r != raw[k]) { raw[k] = r; chg[k] = now; }
    if (now - chg[k] >= 30 && stable[k] != r) {         
      stable[k] = r;
      if (r) { onPress(k); rep[k] = now + 500; }
    } else if (stable[k] && k < 2 && now >= rep[k]) {    
      onPress(k); rep[k] = now + 120;
    }
  }
  if (menuOn && now - lastBtn > 10000) exitMenu("timeout");
}

// display setup
void displayTask() {
  static unsigned long last = 0;
  if (millis() - last < 200) return;
  last = millis();
  lcd.setCursor(0, 0);
  if (menuOn) {
    lcd.printf("SET %-12s", PNAME[menuIdx]);
    lcd.setCursor(0, 1);
    if (menuIdx >= 3) lcd.printf("%-5lu ms  ^v    ", (unsigned long)getParam(menuIdx));
    else lcd.printf("%-7.1f  ^v      ", getParam(menuIdx));
  } else {
    lcd.printf("V:%5.1f I:%4.2f  ", v, i);
    lcd.setCursor(0, 1);
    lcd.printf("%-16s", state == FAULT ? faultName(lastFault) : stateName());
  }
}

// serial commands
void printSettings() {
  Serial.printf("OV=%.1f V  UV=%.1f V  OC=%.2f A  DELAY=%lu ms  RECON=%lu ms\n",
                cfg.ov, cfg.uv, cfg.oc, cfg.tripDelay, cfg.reconDelay);
}

void handleCommand(String s) {
  s.trim(); s.toUpperCase();
  if (s == "HELP") {
    Serial.println("STATUS | GET | SET <OV|UV|OC|DELAY|RECON> <value> | LOG | DEFAULT | REBOOT");
  } else if (s == "STATUS") {
    Serial.printf("State=%s  V=%.1f  I=%.2f  Relay=%s  LastFault=%s  Trips=%u\n",
                  stateName(), v, i, digitalRead(PIN_RELAY) ? "ON" : "OFF",
                  faultName(lastFault), faultCount);
  } else if (s == "GET") {
    printSettings();
  } else if (s == "LOG") {
    printLog();
  } else if (s == "DEFAULT") {
    cfg = Settings(); saveSettings();
    Serial.print("Defaults restored. "); printSettings();
  } else if (s == "REBOOT") {
    Serial.println("Rebooting..."); delay(100); ESP.restart();
  } else if (s.startsWith("SET ")) {
    int sp = s.indexOf(' ', 4);
    if (sp < 0) { Serial.println("ERR usage: SET <name> <value>"); return; }
    int k = findParam(s.substring(4, sp));
    if (k >= 0 && setParam(k, s.substring(sp + 1).toFloat())) {
      saveSettings();
      Serial.print("OK "); printSettings();
    } else Serial.println("ERR invalid name or value out of range");
  } else {
    Serial.println("ERR unknown command (try HELP)");
  }
}

void serialTask() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (line.length()) handleCommand(line); line = ""; }
    else line += c;
  }
}

// main
void setup() {
  Serial.begin(115200);
  pinMode(PIN_RELAY, OUTPUT); digitalWrite(PIN_RELAY, LOW);   // load OFF at boot
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZ, OUTPUT);
  pinMode(PIN_UP, INPUT_PULLUP);
  pinMode(PIN_DN, INPUT_PULLUP);
  pinMode(PIN_SEL, INPUT_PULLUP);
  lcd.init(); lcd.backlight();

  prefs.begin("vprot", false);
  loadSettings();
  loadLog();

  v = readVoltage(); i = readCurrent();
  coolStart = millis();
  Serial.println("Voltage Protector ready. Type HELP.");
  printSettings();
}

void loop() {
  sampleTask();
  protectionTask();
  buttonTask();
  displayTask();
  serialTask();
}
