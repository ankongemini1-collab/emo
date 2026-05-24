/*
  EMO Desk Companion - NodeMCU ESP8266 + SH1106
  Adapted from emo_robot_v10_fixed.ino for a simpler hardware setup.

  USER WIRING (NodeMCU labels):
  D1 = OLED SCL
  D2 = OLED SDA
  D5 = Touch sensor (digital)
  D6 = Passive buzzer
  D7 = Button 1
  D3 = Button 2
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <time.h>

// =================== USER CONFIG ===================
#define WIFI_SSID   "Ankon"
#define WIFI_PASS   "Ankon016268"

// Dhaka, Bangladesh (UTC+6)
#define TZ_OFFSET_SEC 21600
#define NTP1 "pool.ntp.org"
#define NTP2 "time.nist.gov"

// Open-Meteo location: Dhaka
#define WX_LAT "23.8103"
#define WX_LON "90.4125"

// =================== PINS ===================
#define PIN_OLED_SCL D1
#define PIN_OLED_SDA D2
#define PIN_TOUCH    D5
#define PIN_BUZZER   D6
#define PIN_BTN1     D7
#define PIN_BTN2     D3

// =================== DISPLAY ===================
#define SCREEN_W 128
#define SCREEN_H 64
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);

// =================== NETWORK ===================
WebSocketsServer wsServer(81);

// =================== STATE ===================
enum Mood {
  MOOD_IDLE=0, MOOD_HAPPY, MOOD_EXCITED, MOOD_SAD,
  MOOD_SURPRISED, MOOD_SLEEPY, MOOD_ANGRY, MOOD_LOVE,
  MOOD_CURIOUS, MOOD_PROUD, MOOD_SCARED
};

const char* moodNames[] = {
  "idle","happy","excited","sad","surprised","sleepy",
  "angry","love","curious","proud","scared"
};

#define PAGE_EYES    0
#define PAGE_CLOCK   1
#define PAGE_WEATHER 2
#define PAGE_MUSIC   3
#define PAGE_STATS   4
#define PAGE_WIFI    5
#define NUM_PAGES    6

Mood currentMood = MOOD_IDLE;
int currentPage = PAGE_EYES;
bool sleepMode = false;

unsigned long nowMs = 0;
unsigned long lastUiMs = 0;
unsigned long lastWeatherMs = 0;
unsigned long bootMs = 0;

String wxTemp = "--";
String wxDesc = "--";
String wxWind = "--";

uint8_t volume = 8; // 0..10
uint32_t wsCount = 0;

// =================== BUTTON/TOUCH ===================
struct KeyState {
  bool raw;
  bool stable;
  bool lastStable;
  unsigned long changedMs;
  unsigned long downMs;
  bool longFired;
};

KeyState kBtn1 = {false,false,false,0,0,false};
KeyState kBtn2 = {false,false,false,0,0,false};
KeyState kTouch= {false,false,false,0,0,false};

#define DEBOUNCE_MS 45
#define LONG_MS 700

// =================== SOUND ===================
#define REST 0
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_G5  784

const int tune_boot[] = {
  NOTE_C4,8, NOTE_E4,8, NOTE_G4,8, NOTE_C5,4
};
const int tune_happy[] = {
  NOTE_C5,12, NOTE_E5,12, NOTE_G5,10
};
const int tune_sad[] = {
  NOTE_E4,8, NOTE_D4,8, NOTE_C4,4
};
const int tune_click[] = {
  NOTE_G4,16
};

void playToneScaled(int freq, int durMs) {
  if (freq == REST) { delay(durMs); return; }
  int eff = map(volume, 0, 10, 2, 10);
  tone(PIN_BUZZER, freq, durMs);
  delay(durMs * eff / 10);
  noTone(PIN_BUZZER);
}

void playTune(const int* tune, int pairCount, int bpm) {
  int wholenote = (60000 * 4) / bpm;
  for (int i=0;i<pairCount;i++) {
    int note = tune[i*2];
    int div = tune[i*2+1];
    int dur = wholenote / div;
    playToneScaled(note, dur);
    delay(10);
  }
}

// =================== DRAW ===================
void drawEyes(Mood m) {
  int lx = 18, rx = 70, y = 14, w = 40, h = 36;
  display.fillRect(0,0,SCREEN_W,SCREEN_H,SH110X_BLACK);

  auto eye = [&](int x, int y0, int ww, int hh) {
    display.fillRoundRect(x,y0,ww,hh,9,SH110X_WHITE);
  };

  switch (m) {
    case MOOD_HAPPY:
      eye(lx,y,w,h); eye(rx,y,w,h);
      display.fillRect(lx, y+26, w, 10, SH110X_BLACK);
      display.fillRect(rx, y+26, w, 10, SH110X_BLACK);
      break;
    case MOOD_SLEEPY:
      display.fillRoundRect(lx,y+16,w,10,5,SH110X_WHITE);
      display.fillRoundRect(rx,y+16,w,10,5,SH110X_WHITE);
      break;
    case MOOD_SURPRISED:
      display.drawCircle(lx+20,y+18,14,SH110X_WHITE);
      display.drawCircle(rx+20,y+18,14,SH110X_WHITE);
      break;
    case MOOD_ANGRY:
      eye(lx,y,w,h); eye(rx,y,w,h);
      display.fillTriangle(lx,y,lx+w,y,lx+w,y+14,SH110X_BLACK);
      display.fillTriangle(rx,y,rx+w,y,rx,y+14,SH110X_BLACK);
      break;
    case MOOD_LOVE:
      display.fillCircle(lx+13,y+12,8,SH110X_WHITE); display.fillCircle(lx+27,y+12,8,SH110X_WHITE);
      display.fillTriangle(lx+5,y+16,lx+35,y+16,lx+20,y+32,SH110X_WHITE);
      display.fillCircle(rx+13,y+12,8,SH110X_WHITE); display.fillCircle(rx+27,y+12,8,SH110X_WHITE);
      display.fillTriangle(rx+5,y+16,rx+35,y+16,rx+20,y+32,SH110X_WHITE);
      break;
    default:
      eye(lx,y,w,h); eye(rx,y,w,h);
      break;
  }

  display.display();
}

void pageClock() {
  time_t t = time(nullptr);
  struct tm* tmv = localtime(&t);
  char hhmm[10], dmy[20];
  strftime(hhmm, sizeof(hhmm), "%H:%M:%S", tmv);
  strftime(dmy, sizeof(dmy), "%d-%m-%Y", tmv);

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0); display.print("CLOCK (Dhaka)");
  display.setTextSize(2);
  display.setCursor(0,20); display.print(hhmm);
  display.setTextSize(1);
  display.setCursor(0,50); display.print(dmy);
  display.display();
}

void pageWeather() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0); display.print("WEATHER (Dhaka)");
  display.setCursor(0,16); display.print("Temp: "); display.print(wxTemp); display.print(" C");
  display.setCursor(0,28); display.print("Cond: "); display.print(wxDesc);
  display.setCursor(0,40); display.print("Wind: "); display.print(wxWind); display.print(" km/h");
  display.setCursor(0,54); display.print("Touch=refresh");
  display.display();
}

void pageMusic() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0); display.print("MUSIC / BUZZER");
  display.setCursor(0,16); display.print("Btn1: happy tune");
  display.setCursor(0,28); display.print("Btn2: sad tune");
  display.setCursor(0,40); display.print("Touch: click");
  display.setCursor(0,54); display.print("Vol: "); display.print(volume);
  display.display();
}

void pageStats() {
  unsigned long up = (millis() - bootMs) / 1000;
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0); display.print("STATS");
  display.setCursor(0,14); display.print("Mood: "); display.print(moodNames[currentMood]);
  display.setCursor(0,26); display.print("Page: "); display.print(currentPage);
  display.setCursor(0,38); display.print("Uptime: "); display.print(up); display.print("s");
  display.setCursor(0,50); display.print("WS msgs: "); display.print(wsCount);
  display.display();
}

void pageWifi() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0); display.print("WIFI");
  display.setCursor(0,14); display.print(WiFi.status()==WL_CONNECTED?"Connected":"Offline");
  display.setCursor(0,26); display.print("IP: "); display.print(WiFi.localIP());
  display.setCursor(0,38); display.print("RSSI: "); display.print(WiFi.RSSI()); display.print(" dBm");
  display.setCursor(0,50); display.print("WS: 81");
  display.display();
}

void showPage() {
  if (sleepMode) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    display.setTextSize(2);
    display.setCursor(15,24); display.print("SLEEP");
    display.display();
    return;
  }
  switch (currentPage) {
    case PAGE_EYES: drawEyes(currentMood); break;
    case PAGE_CLOCK: pageClock(); break;
    case PAGE_WEATHER: pageWeather(); break;
    case PAGE_MUSIC: pageMusic(); break;
    case PAGE_STATS: pageStats(); break;
    case PAGE_WIFI: pageWifi(); break;
  }
}

// =================== WEATHER ===================
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + WX_LAT +
               "&longitude=" + WX_LON +
               "&current=temperature_2m,weather_code,wind_speed_10m";

  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(1024);
    if (!deserializeJson(doc, http.getString())) {
      wxTemp = String((float)doc["current"]["temperature_2m"], 1);
      int wcode = doc["current"]["weather_code"] | -1;
      wxWind = String((float)doc["current"]["wind_speed_10m"], 1);
      if (wcode < 2) wxDesc = "Clear";
      else if (wcode < 4) wxDesc = "Cloudy";
      else if (wcode < 70) wxDesc = "Rain";
      else wxDesc = "Other";
    }
  }
  http.end();
}

// =================== INPUT ===================
bool updateKey(KeyState &k, bool raw, void (*onTap)(), void (*onLong)()) {
  bool fired = false;
  if (raw != k.raw) { k.raw = raw; k.changedMs = nowMs; }
  if ((nowMs - k.changedMs) > DEBOUNCE_MS && k.stable != k.raw) {
    k.lastStable = k.stable;
    k.stable = k.raw;
    if (k.stable) { k.downMs = nowMs; k.longFired = false; }
    else if (!k.longFired) { onTap(); fired = true; }
  }
  if (k.stable && !k.longFired && (nowMs - k.downMs >= LONG_MS)) {
    k.longFired = true;
    onLong();
    fired = true;
  }
  return fired;
}

void nextPage() { currentPage = (currentPage + 1) % NUM_PAGES; playTune(tune_click,1,130); }
void prevPage() { currentPage = (currentPage - 1 + NUM_PAGES) % NUM_PAGES; playTune(tune_click,1,130); }
void nextMood() { currentMood = (Mood)((currentMood + 1) % 11); if(currentPage==PAGE_EYES) showPage(); }
void randomMood() { currentMood = (Mood)random(0,11); if(currentPage==PAGE_EYES) showPage(); }
void touchTap() { if (currentPage==PAGE_WEATHER) fetchWeather(); else playTune(tune_click,1,140); }
void touchLong() { sleepMode = !sleepMode; if(!sleepMode) showPage(); }

void handleInputs() {
  bool btn1 = (digitalRead(PIN_BTN1) == LOW);
  bool btn2 = (digitalRead(PIN_BTN2) == LOW);
  bool tch  = (digitalRead(PIN_TOUCH) == HIGH);

  updateKey(kBtn1, btn1, nextPage, prevPage);
  updateKey(kBtn2, btn2, nextMood, randomMood);
  updateKey(kTouch, tch, touchTap, touchLong);
}

// =================== WS ===================
Mood moodFrom(const String& s) {
  for (int i=0;i<11;i++) if (s.equalsIgnoreCase(moodNames[i])) return (Mood)i;
  return MOOD_IDLE;
}

void wsEvent(uint8_t client, WStype_t type, uint8_t * payload, size_t len) {
  if (type == WStype_TEXT) {
    wsCount++;
    DynamicJsonDocument d(512);
    if (deserializeJson(d, payload, len)) return;
    String t = d["t"] | "";
    if (t == "mood") {
      currentMood = moodFrom(String((const char*)d["v"]));
      if (currentPage==PAGE_EYES) showPage();
    } else if (t == "page") {
      int p = d["v"] | 0;
      if (p>=0 && p<NUM_PAGES) { currentPage=p; showPage(); }
    } else if (t == "sleep") {
      sleepMode = true; showPage();
    } else if (t == "wake") {
      sleepMode = false; showPage();
    } else if (t == "volume") {
      int v = d["v"] | volume;
      volume = constrain(v, 0, 10);
      EEPROM.write(0, volume);
      EEPROM.commit();
    } else if (t == "song") {
      String n = d["name"] | "";
      if (n=="happy") playTune(tune_happy,3,140);
      else if (n=="sad") playTune(tune_sad,3,120);
      else playTune(tune_boot,4,130);
    }
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long st = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-st < 15000) {
    delay(250);
  }
}

void setup() {
  pinMode(PIN_BTN1, INPUT_PULLUP);
  pinMode(PIN_BTN2, INPUT_PULLUP);
  pinMode(PIN_TOUCH, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  display.begin(0x3C, true);
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0); display.print("EMO NodeMCU boot...");
  display.display();

  EEPROM.begin(64);
  uint8_t vv = EEPROM.read(0);
  if (vv <= 10) volume = vv;

  connectWiFi();
  configTime(TZ_OFFSET_SEC, 0, NTP1, NTP2);

  wsServer.begin();
  wsServer.onEvent(wsEvent);

  bootMs = millis();
  fetchWeather();
  playTune(tune_boot,4,125);
  showPage();
}

void loop() {
  nowMs = millis();
  wsServer.loop();
  handleInputs();

  if (nowMs - lastUiMs > 250) {
    lastUiMs = nowMs;
    if (currentPage != PAGE_EYES || (nowMs/1000)%5==0) showPage();
  }

  if (nowMs - lastWeatherMs > 600000UL) {
    lastWeatherMs = nowMs;
    fetchWeather();
  }
}
