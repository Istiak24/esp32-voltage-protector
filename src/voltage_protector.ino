#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// pins
const int PIN_V = 34, PIN_I = 35;
const int PIN_RELAY = 26, PIN_LED = 27, PIN_BUZZ = 25;
const int PIN_UP = 32, PIN_DN = 33, PIN_SEL = 14;

LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences prefs;

// settings
float ovLimit = 250;               // volts
float ocLimit = 6;                 // amps
unsigned long tripDelay = 500;     // ms
unsigned long reconDelay = 5000;   // ms

// state: 0 = normal, 1 = fault, 2 = cooldown
int state = 2;
bool pending = false;
unsigned long pendingStart = 0, coolStart = 0;
unsigned int tripCount = 0;
const char* faultText = "NONE";
float v = 0, i = 0;

// menu
bool menuOn = false;
int menuIdx = 0;                   // 0=OV 1=OC 2=DELAY 3=RECON
unsigned long lastPress = 0;
const char* menuName[4] = {"OV limit", "OC limit", "Trip delay", "Recon delay"};

// settings
void saveSettings() {
  prefs.putFloat("ov", ovLimit);
  prefs.putFloat("oc", ocLimit);
  prefs.putULong("td", tripDelay);
  prefs.putULong("rd", reconDelay);
}

void loadSettings() {
  ovLimit = prefs.getFloat("ov", 250);
  ocLimit = prefs.getFloat("oc", 6);
  tripDelay = prefs.getULong("td", 500);
  reconDelay = prefs.getULong("rd", 5000);
}

// set one setting, returns false if the value is out of range
bool setValue(int k, float val) {
  if (k == 0 && val >= 100 && val <= 300) ovLimit = val;
  else if (k == 1 && val >= 0.5 && val <= 10) ocLimit = val;
  else if (k == 2 && val >= 0 && val <= 5000) tripDelay = (unsigned long)val;
  else if (k == 3 && val >= 1000 && val <= 60000) reconDelay = (unsigned long)val;
  else return false;
  return true;
}

float getValue(int k) {
  if (k == 0) return ovLimit;
  if (k == 1) return ocLimit;
  if (k == 2) return tripDelay;
  return reconDelay;
}

void printSettings() {
  Serial.printf("OV=%.1f V  OC=%.2f A  DELAY=%lu ms  RECON=%lu ms\n",
                ovLimit, ocLimit, tripDelay, reconDelay);
}

// sensors
void readSensors() {
  static unsigned long last = 0;
  if (millis() - last < 10) return;
  last = millis();
  float newV = analogRead(PIN_V) * 300.0 / 4095.0;   // 0-300 V
  float newI = analogRead(PIN_I) * 10.0 / 4095.0;    // 0-10 A
  v = 0.8 * v + 0.2 * newV;                          // smooth the noise
  i = 0.8 * i + 0.2 * newI;
}

// protection 
void trip() {
  digitalWrite(PIN_RELAY, LOW);                      // disconnect first
  state = 1;
  tripCount++;
  Serial.printf("[%lu ms] TRIP: %s  V=%.1f I=%.2f  (fault lasted %lu ms)\n",
                millis(), faultText, v, i, millis() - pendingStart);
}

void protection() {
  unsigned long now = millis();
  bool overV = v > ovLimit;
  bool overI = i > ocLimit;

  if (state == 0) {                                  // normal
    if (overV || overI) {
      if (!pending) {
        pending = true;
        pendingStart = now;
      }
      faultText = overV ? "OVER-VOLTAGE" : "OVER-CURRENT";
      bool bigSurge = (v > ovLimit * 1.15) || (i > ocLimit * 1.5);
      if (bigSurge || now - pendingStart >= tripDelay) trip();
    } else {
      pending = false;
    }
  }
  else if (state == 1) {                             // fault: wait until safe
    if (v < ovLimit - 5 && i < ocLimit - 0.3) {
      state = 2;
      coolStart = now;
      Serial.printf("[%lu ms] Fault cleared, cooldown started\n", now);
    }
  }
  else {                                             // cooldown
    if (v >= ovLimit - 5 || i >= ocLimit - 0.3) {
      state = 1;                                     // fault came back
    } else if (now - coolStart >= reconDelay) {
      state = 0;
      pending = false;
      digitalWrite(PIN_RELAY, HIGH);                 // reconnect
      Serial.printf("[%lu ms] Load reconnected\n", now);
    }
  }

  digitalWrite(PIN_LED, state != 0);
  digitalWrite(PIN_BUZZ, state == 1 && ((now / 300) % 2));
}

// buttons and menu 
void pressed(int b) {                                // 0=UP 1=DOWN 2=SELECT
  lastPress = millis();
  if (b == 2) {
    if (!menuOn) {
      menuOn = true;
      menuIdx = 0;
    } else {
      menuIdx++;
      if (menuIdx > 3) {
        menuOn = false;
        saveSettings();
        Serial.println("Menu closed, settings saved");
      }
    }
  } else if (menuOn) {
    float steps[4] = {5, 0.5, 100, 500};
    float change = (b == 0) ? steps[menuIdx] : -steps[menuIdx];
    setValue(menuIdx, getValue(menuIdx) + change);
  }
}

void buttons() {
  static unsigned long lastCheck = 0;
  static bool wasDown[3] = {false, false, false};
  int pins[3] = {PIN_UP, PIN_DN, PIN_SEL};

  if (millis() - lastCheck < 40) return;             // simple debounce
  lastCheck = millis();

  for (int b = 0; b < 3; b++) {
    bool down = (digitalRead(pins[b]) == LOW);
    if (down && !wasDown[b]) pressed(b);
    wasDown[b] = down;
  }

  if (menuOn && millis() - lastPress > 10000) {      // timeout
    menuOn = false;
    saveSettings();
    Serial.println("Menu timeout, settings saved");
  }
}

// display part
void showDisplay() {
  static unsigned long last = 0;
  if (millis() - last < 200) return;
  last = millis();

  lcd.setCursor(0, 0);
  if (menuOn) {
    lcd.printf("%-16s", menuName[menuIdx]);
    lcd.setCursor(0, 1);
    lcd.printf("%-10.1f UP/DN  ", getValue(menuIdx));
  } else {
    lcd.printf("V:%5.1f I:%4.2f  ", v, i);
    lcd.setCursor(0, 1);
    if (state == 0) lcd.printf("%-16s", "NORMAL");
    else if (state == 1) lcd.printf("%-16s", faultText);
    else lcd.printf("%-16s", "COOLDOWN");
  }
}

// serial commands
void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "HELP") {
    Serial.println("Commands: STATUS | GET | SET <OV|OC|DELAY|RECON> <value> | REBOOT");
  }
  else if (cmd == "STATUS") {
    const char* names[3] = {"NORMAL", "FAULT", "COOLDOWN"};
    Serial.printf("State=%s  V=%.1f  I=%.2f  Relay=%s  LastFault=%s  Trips=%u\n",
                  names[state], v, i, digitalRead(PIN_RELAY) ? "ON" : "OFF",
                  faultText, tripCount);
  }
  else if (cmd == "GET") {
    printSettings();
  }
  else if (cmd == "REBOOT") {
    Serial.println("Rebooting...");
    delay(100);
    ESP.restart();
  }
  else if (cmd.startsWith("SET ")) {
    int space = cmd.indexOf(' ', 4);
    if (space < 0) {
      Serial.println("ERR usage: SET <name> <value>");
      return;
    }
    String name = cmd.substring(4, space);
    float val = cmd.substring(space + 1).toFloat();

    int k = -1;
    if (name == "OV") k = 0;
    else if (name == "OC") k = 1;
    else if (name == "DELAY") k = 2;
    else if (name == "RECON") k = 3;

    if (k >= 0 && setValue(k, val)) {
      saveSettings();
      Serial.print("OK ");
      printSettings();
    } else {
      Serial.println("ERR invalid name or value out of range");
    }
  }
  else {
    Serial.println("ERR unknown command (type HELP)");
  }
}

void readSerial() {
  static String line = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length() > 0) handleCommand(line);
      line = "";
    } else {
      line += c;
    }
  }
}

// main
void setup() {
  Serial.begin(115200);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);                      // load off at boot
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZ, OUTPUT);
  pinMode(PIN_UP, INPUT_PULLUP);
  pinMode(PIN_DN, INPUT_PULLUP);
  pinMode(PIN_SEL, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();

  prefs.begin("vprot", false);
  loadSettings();

  v = analogRead(PIN_V) * 300.0 / 4095.0;
  i = analogRead(PIN_I) * 10.0 / 4095.0;
  coolStart = millis();

  Serial.println("Voltage Protector ready. Type HELP.");
  printSettings();
}

void loop() {
  readSensors();
  protection();
  buttons();
  showDisplay();
  readSerial();
}
