/**
 * esp8266_rover.ino
 * Wi-Fi rover firmware for ESP8266 (NodeMCU / Wemos D1 mini).
 *
 * REPLACES BOTH:
 *   - wave_rover_controller.ino   (Arduino Uno Q firmware: motors + LED matrix)
 *   - wave_rover_python_main.py   (Uno Q Python daemon: HTTP server on port 7600)
 *
 * The ESP8266 joins your Wi-Fi and runs the same web commands the Python
 * daemon offered (/drive, /stop, /ok, /off, /arrow, /wiggle, /deviceInfo,
 * /set_rover_ip, /cmd). It listens on port 80 AND port 7600.
 * It also has a test page at  http://<rover-ip>/ctrl  with drive buttons.
 *
 * Board in Arduino IDE: "NodeMCU 1.0 (ESP-12E Module)", Serial Monitor 115200.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

// ============================================================================
// 1. SETTINGS - CHANGE THESE
// ============================================================================
// Wi-Fi the rover should join (use your laptop's 2.4 GHz hotspot).
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// If joining fails (or FORCE_AP_MODE is true) the rover makes its OWN Wi-Fi.
// Connect the laptop to it and the rover is at 192.168.4.1.
const char* AP_SSID = "RoverCar";
const char* AP_PASS = "rover1234";          // at least 8 characters
const bool  FORCE_AP_MODE = false;
const unsigned long WIFI_TIMEOUT_MS = 15000;

// Motor driver type:
//   2 = two input pins per side (L298N with ENA/ENB jumpers ON, L298N mini,
//       MX1508, L9110S)  <- most common, default
//   1 = one PWM pin + one DIRECTION pin per side
#define DRIVER_MODE 2

// If the car goes the wrong way, flip these.
const bool LEFT_INVERT  = false;
const bool RIGHT_INVERT = false;

const float SPEED_SCALE = 1.0;              // 0.5 = half speed
const unsigned long WATCHDOG_MS = 1500;     // stop if no command for this long

// Pins (NodeMCU labels). Raw GPIO numbers are used so any ESP8266 board works.
const int PIN_L_A = 5;    // D1  left  input A  (or PWM in mode 1)
const int PIN_L_B = 4;    // D2  left  input B  (or DIR in mode 1)
const int PIN_R_A = 14;   // D5  right input A  (or PWM in mode 1)
const int PIN_R_B = 12;   // D6  right input B  (or DIR in mode 1)

// ============================================================================
// 2. STATE
// ============================================================================
ESP8266WebServer server80(80);
ESP8266WebServer server7600(7600);

unsigned long lastDriveMs = 0;
bool moving = false;

// ============================================================================
// 3. MOTORS
// ============================================================================
void setSide(int pinA, int pinB, float v, bool invert) {
  v = fminf(1.0f, fmaxf(-1.0f, v));
  if (invert) v = -v;
  int pwm = (int)(fabsf(v) * SPEED_SCALE * 255.0f + 0.5f);
  if (pwm > 255) pwm = 255;
  bool fwd = (v >= 0);
#if DRIVER_MODE == 1
  digitalWrite(pinB, fwd ? HIGH : LOW);
  analogWrite(pinA, pwm);
#else
  analogWrite(pinA, fwd ? pwm : 0);
  analogWrite(pinB, fwd ? 0 : pwm);
#endif
}

// Accepts -1.0..1.0 (like the Python daemon) or -255..255.
float norm(float v) {
  if (fabsf(v) > 1.0f) v /= 255.0f;
  return fminf(1.0f, fmaxf(-1.0f, v));
}

void driveMotors(float l, float r) {
  l = norm(l);
  r = norm(r);
  setSide(PIN_L_A, PIN_L_B, l, LEFT_INVERT);
  setSide(PIN_R_A, PIN_R_B, r, RIGHT_INVERT);
  moving = (l != 0.0f || r != 0.0f);
  lastDriveMs = millis();
}

void stopMotors() {
  driveMotors(0, 0);
  moving = false;
}

void wiggle() {          // same moves as do_wiggle() in the Python file
  driveMotors(-0.85f, 0.85f);
  delay(350);
  driveMotors(0.85f, -0.85f);
  delay(350);
  stopMotors();
}

void statusLed(bool on) {          // built-in LED is active-low
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
}

// ============================================================================
// 4. WEB API (same commands as the Uno Q Python daemon)
// ============================================================================
String myIP() {
  return (WiFi.getMode() == WIFI_AP) ? WiFi.softAPIP().toString()
                                     : WiFi.localIP().toString();
}

void sendJson(ESP8266WebServer &s, const String &body, int code = 200) {
  s.sendHeader("Access-Control-Allow-Origin", "*");
  s.sendHeader("Cache-Control", "no-store");
  s.send(code, "application/json", body);
}

const char CTRL_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Rover test</title>
<style>
body{font-family:sans-serif;text-align:center}
button{width:30%;height:90px;font-size:28px;margin:4px;touch-action:manipulation}
</style>
<h3>ESP8266 rover - test drive</h3>
<div><button id=f>&#9650;</button></div>
<div><button id=l>&#9664;</button><button id=s>&#9632;</button><button id=r>&#9654;</button></div>
<div><button id=b>&#9660;</button></div>
<p>Speed <input id=sp type=range min=30 max=100 value=70></p>
<script>
let t;
function go(l,r){const k=document.getElementById('sp').value/100;
  fetch('/drive?left='+(l*k)+'&right='+(r*k)).catch(()=>{});}
function bind(id,l,r){const e=document.getElementById(id);
  e.onpointerdown=()=>{go(l,r);clearInterval(t);t=setInterval(()=>go(l,r),250);};
  const up=()=>{clearInterval(t);fetch('/stop').catch(()=>{});};
  e.onpointerup=up;e.onpointerleave=up;e.onpointercancel=up;}
bind('f',1,1);bind('b',-1,-1);bind('l',-1,1);bind('r',1,-1);
document.getElementById('s').onclick=()=>fetch('/stop');
</script>
)rawliteral";

void registerRoutes(ESP8266WebServer &s) {
  s.on("/", [&s]() {
    sendJson(s, String("{\"status\":\"ok\",\"service\":\"esp8266_rover\",\"rover_ip\":\"") + myIP() + "\"}");
  });

  s.on("/ctrl", [&s]() {
    s.send_P(200, "text/html", CTRL_PAGE);
  });

  // Wave Rover style: /cmd?inputA=1&inputB=<left>&inputC=<right>
  s.on("/cmd", [&s]() {
    if (s.arg("inputA").toInt() == 1) {
      driveMotors(s.arg("inputB").toFloat(), s.arg("inputC").toFloat());
    }
    sendJson(s, "{\"status\":\"ok\"}");
  });

  // Daemon style: /drive?left=<-1..1>&right=<-1..1>
  s.on("/drive", [&s]() {
    float l = s.arg("left").toFloat();
    float r = s.arg("right").toFloat();
    driveMotors(l, r);
    sendJson(s, String("{\"status\":\"ok\",\"left\":") + String(l, 2) + ",\"right\":" + String(r, 2) + "}");
  });

  s.on("/stop", [&s]() {
    stopMotors();
    statusLed(true);
    sendJson(s, "{\"status\":\"ok\",\"action\":\"stop\"}");
  });

  // The Uno Q had a 13x8 LED matrix. The ESP8266 has none, so these only
  // answer "ok" (and /ok, /off use the small built-in LED instead).
  s.on("/ok", [&s]() {
    statusLed(true);
    sendJson(s, "{\"status\":\"ok\",\"matrix\":\"ok\"}");
  });
  s.on("/off", [&s]() {
    statusLed(false);
    sendJson(s, "{\"status\":\"ok\",\"matrix\":\"off\"}");
  });
  s.on("/arrow", [&s]() {
    sendJson(s, String("{\"status\":\"ok\",\"dir\":") + s.arg("dir").toInt() + "}");
  });

  s.on("/wiggle", [&s]() {
    sendJson(s, "{\"status\":\"ok\",\"action\":\"wiggle_started\"}");
    wiggle();
  });

  s.on("/set_rover_ip", [&s]() {
    // Not needed here: this board IS the rover. Kept so the dashboard does not error.
    sendJson(s, String("{\"status\":\"ok\",\"rover_ip\":\"") + myIP() + "\"}");
  });

  s.on("/deviceInfo", [&s]() {
    // No battery sensor is wired, so voltage is reported as 0.
    sendJson(s, String("{\"V\":0.0,\"battery\":\"not_connected\",\"status\":\"online\",\"rssi\":")
                + WiFi.RSSI() + ",\"uptime_ms\":" + millis() + "}");
  });

  s.onNotFound([&s]() {
    s.sendHeader("Access-Control-Allow-Origin", "*");
    s.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    s.sendHeader("Access-Control-Allow-Headers", "*");
    s.sendHeader("Access-Control-Allow-Private-Network", "true");
    if (s.method() == HTTP_OPTIONS) s.send(204);
    else s.send(404, "application/json", "{\"error\":\"not_found\"}");
  });
}

// ============================================================================
// 5. WI-FI
// ============================================================================
void startWifi() {
  bool apMode = FORCE_AP_MODE;

  if (!apMode) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);      // lower latency
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print(F("Joining Wi-Fi "));
    Serial.print(WIFI_SSID);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("Could not join Wi-Fi. Starting own network instead."));
      apMode = true;
    }
  }

  if (apMode) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print(F("Own Wi-Fi started: "));
    Serial.println(AP_SSID);
  }

  Serial.print(F("ROVER IP ADDRESS: "));
  Serial.println(myIP());
}

// ============================================================================
// 6. SETUP & LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  analogWriteRange(255);
  analogWriteFreq(1000);
  pinMode(PIN_L_A, OUTPUT);
  pinMode(PIN_L_B, OUTPUT);
  pinMode(PIN_R_A, OUTPUT);
  pinMode(PIN_R_B, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  stopMotors();
  statusLed(false);

  startWifi();

  if (MDNS.begin("rover")) {                 // optional: http://rover.local
    MDNS.addService("http", "tcp", 80);
  }

  registerRoutes(server80);
  registerRoutes(server7600);
  server80.begin();
  server7600.begin();
  statusLed(true);
  Serial.println(F("Ready. Open http://<rover-ip>/ctrl to test."));
}

void loop() {
  server80.handleClient();
  server7600.handleClient();
  MDNS.update();

  // Safety: if commands stop arriving (Wi-Fi drop, laptop closed), stop the car.
  if (moving && millis() - lastDriveMs > WATCHDOG_MS) {
    stopMotors();
  }
}
