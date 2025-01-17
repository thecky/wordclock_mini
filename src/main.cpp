// Compiler Setup WeMos D1 R2 & mini, 80Mhz, 4M (1MB SPIFFS), 230400
#include <Arduino.h>
#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#define DEBUG 1

#include <Homie.h>                 // version 2.0.0, manual install from homepage
#include "debug.h"
#include <time.h>
#include <TimeLib.h>
#include <Timezone.h>
#include <elapsedMillis.h>         // version 1.0.4, by Paul Stoffregen
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <SPI.h>
#include "LedMatrix.h"
#include "animations.h"
#define FASTLED_ALLOW_INTERRUPTS 0
#define FASTLED_ESP8266_D1_PIN_ORDER
#include "FastLED.h"
#include <AceButton.h>

FASTLED_USING_NAMESPACE

const char compiletime[] = __TIME__;
const char compiledate[] = __DATE__;
#define SW_VERSION_DEF "0.3.2"
String SW_VERSION = SW_VERSION_DEF;


#define NUMBER_OF_DEVICES 1
#define CS_PIN D2
// LED Matrix is to be connected to Wemos D1 Mini as follows:
//         CS - D2
// MOSI (DIN) - D7
//        CLK - D5
// Buttons are:
//       Left - D0
//      Right - D8
// WS2812B:
//       Data - D1
#define LEFT_PIN D0
#define RIGHT_PIN D8
#define LED_PIN 3 // 3 = RX, was D3
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB

#define STATE_BOOT       0
#define STATE_INTRO      1
#define STATE_TIME       2
#define STATE_MESSAGE    3
#define STATE_NO_WIFI    4
#define STATE_NO_NTP     5

#define FADE_NORMAL  0
#define FADE_OUT     1
#define FADE_IN      2
#define FADE_BLACK   3

LedMatrix ledMatrix = LedMatrix(NUMBER_OF_DEVICES, CS_PIN);

CRGB leds[1];
CHSV led;
CHSV color_message_pending = CHSV(0, 255, 128);
CHSV color_standard = CHSV(42,255,160);
CHSV color_next;

elapsedMillis notifier_timer = 0;
bool notifier_pulsate = false;
byte notifier_mode = FADE_NORMAL;
byte notifier_speed = 3;    // brightness increase/decrease per frame
byte notifier_frame = 10;   // ms of frame for fading/pulsating notifier led
byte notifier_brightness = 128;
CHSV notifier_color;

const String configFile = "config.json";


HomieNode infoNode("info", "info", "info");
HomieNode configNode("config", "config", "config");
HomieNode buttonNode("button", "button", "button");
int wifiSetup = 0;

byte state = STATE_BOOT;
byte next_state = STATE_TIME;
bool next_frame = false;
long effect_time = 0;
long next_tick = 0;
byte brightness = 15;
int8_t fader = brightness;
elapsedMillis fade_timer = 0;
elapsedMillis tick_timer = 0;
elapsedMillis state_timer = 0;
elapsedMillis animation_timer = 0;
elapsedMillis reboot_counter_reset_timer = 0;
byte reboot_counter = 0;
bool reboot_counter_reset = false;
byte fade_mode = FADE_NORMAL;
byte fader_black_frames = 0;
long segments = 0;
long newSegments = 0;
byte active_animation = 0;
word active_animation_frame = 0;
byte active_animation_speed = 0;
byte active_animation_repeat = 0;
byte active_animation_cycle = 0;
bool animation_done = true;
String newMessage = "";
String allMessages = "";
String lastMessage = "";

#define MS_TIME_BOOT_MESSAGE 7600
#define MS_TIME_REBOOT_RESET 10000
#define MS_FADER_FRAME  40
#define FADER_BLACK_FRAMES 3
#define SHOW_ON_LED false
#define DONT_SHOW_ON_LED true

#define BTN_LEFT 1
#define BTN_RIGHT 2
#define BTN_BOTH 3
ace_button::Encoded4To2ButtonConfig buttonConfig(LEFT_PIN, RIGHT_PIN, LOW);
ace_button::AceButton leftButton(&buttonConfig, BTN_LEFT, LOW);
ace_button::AceButton rightButton(&buttonConfig, BTN_RIGHT, LOW);
ace_button::AceButton bothButton(&buttonConfig, BTN_BOTH, LOW); //emulated "middle" button, when both buttons are pressed
uint8_t btnMQTTMask[3] = {255, 255, 255}; // which events should be sent via MQTT

// timezone definitions
TimeChangeRule CEST = { "CEST", Last, Sun, Mar, 2, 120 };     //Central European Summer Time
TimeChangeRule CET = { "CET ", Last, Sun, Oct, 3, 60 };       //Central European Standard Time
Timezone CE(CEST, CET);
TimeChangeRule *tcr;        //pointer to the time change rule, use to get the TZ abbrev
time_t utc, local, last_time;

void readConfig(String filename) {
  //read configuration from FS json
  DPRINTLN(F("mounting FS..."));

  if (SPIFFS.begin()) {
    DPRINTLN(F("mounted file system"));
    if (SPIFFS.exists(filename)) {
      //file exists, reading and loading
      DPRINTLN(F("reading config file"));
      File configFile = SPIFFS.open(filename, "r");
      if (configFile) {
        DPRINTLN(F("opened config file"));
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DPRINTLN(buf.get());
        DynamicJsonDocument json(2048);
        DeserializationError error = deserializeJson(json, buf.get());
        serializeJson(json,Serial);
//        JsonObject& json = jsonBuffer.parseObject(buf.get());
#ifdef DEBUG
//        deserializeJson(json,Serial);
#endif
        if (!error) {
          DPRINTLN(F("\nparsed json"));

            brightness = json["brightness"];
            reboot_counter = json["reboot_counter"];
            color_standard.hue = json["color"];
            color_standard.value = json["notifierBrightness"];
            
            DPRINT(F("config values for brightness:")); DPRINTLN(brightness);
            DPRINT(F("config values for reboot_counter:")); DPRINTLN(reboot_counter);
            DPRINT(F("config values for color:")); DPRINTLN(color_standard.hue);

        } else {
          DPRINTLN(F("failed to load json config"));
        }
      }
    }
  } else {
    DPRINTLN(F("failed to mount FS, reformating it"));
    SPIFFS.format();
  }
}

void writeConfig(String filename) {
  DPRINTLN(F("saving config"));
  DynamicJsonDocument json(2048);

  json["brightness"] = brightness;
  json["reboot_counter"] = reboot_counter;
  json["color"] = color_standard.hue;
  json["notifierBrightness"] = color_standard.value;

  File configFile = SPIFFS.open(filename, "w");
  if (!configFile) {
    DPRINTLN(F("failed to open config file for writing"));
  }

#ifdef DEBUG
  serializeJson(json,Serial);
#endif
  serializeJson(json,configFile);
  configFile.close();  
}

void onHomieEvent(const HomieEvent& event) {
  switch(event.type) {
    case HomieEventType::STANDALONE_MODE:
      Serial << "Standalone mode started" << endl;
      break;
    case HomieEventType::CONFIGURATION_MODE:
      Serial << "Configuration mode started" << endl;
      break;
    case HomieEventType::NORMAL_MODE:
      Serial << "Normal mode started" << endl;
      break;
    case HomieEventType::OTA_STARTED:
      Serial << "OTA started" << endl;
      break;
    case HomieEventType::OTA_PROGRESS:
      Serial << "OTA progress, " << event.sizeDone << "/" << event.sizeTotal << endl;
      break;
    case HomieEventType::OTA_FAILED:
      Serial << "OTA failed" << endl;
      break;
    case HomieEventType::OTA_SUCCESSFUL:
      Serial << "OTA successful" << endl;
      break;
    case HomieEventType::ABOUT_TO_RESET:
      Serial << "About to reset" << endl;
      break;
    case HomieEventType::WIFI_CONNECTED:
      Serial << "Wi-Fi connected, IP: " << event.ip << ", gateway: " << event.gateway << ", mask: " << event.mask << endl;
      wifiSetup = 1;
      break;
    case HomieEventType::WIFI_DISCONNECTED:
      Serial << "Wi-Fi disconnected, reason: " << (int8_t)event.wifiReason << endl;
      setSyncInterval(5);
      wifiSetup = 0;
      break;
    case HomieEventType::MQTT_READY:
      Serial << "MQTT connected" << endl;
      break;
    case HomieEventType::MQTT_DISCONNECTED:
      Serial << "MQTT disconnected, reason: " << (int8_t)event.mqttReason << endl;
      break;
    case HomieEventType::MQTT_PACKET_ACKNOWLEDGED:
//      Serial << "MQTT packet acknowledged, packetId: " << event.packetId << endl;
      break;
    case HomieEventType::READY_TO_SLEEP:
      Serial << "Ready to sleep" << endl;
      break;

  }
}

#define SEGMENT_FUENF 1
#define SEGMENT_ZEHN 2
#define SEGMENT_FUENFZEHN 3
#define SEGMENT_VOR 4
#define SEGMENT_NACH 5
#define SEGMENT_HALB 6
#define SEGMENT_NUM_1 11
#define SEGMENT_NUM_2 12
#define SEGMENT_NUM_3 13
#define SEGMENT_NUM_4 14
#define SEGMENT_NUM_5 15
#define SEGMENT_NUM_6 16
#define SEGMENT_NUM_7 17
#define SEGMENT_NUM_8 18
#define SEGMENT_NUM_9 19
#define SEGMENT_NUM_10 20
#define SEGMENT_NUM_11 21
#define SEGMENT_NUM_12 22

void showSegment(byte segmentId, bool noShow) {
  newSegments += 1 << segmentId;
  if (noShow) return;
  switch (segmentId) {
    case SEGMENT_FUENF:
      ledMatrix.setPixel(0,0);
      ledMatrix.setPixel(1,0);
      ledMatrix.setPixel(2,0);
      ledMatrix.setPixel(3,0);
      break;  
    case SEGMENT_ZEHN:
      ledMatrix.setPixel(4,0);
      ledMatrix.setPixel(5,0);
      ledMatrix.setPixel(6,0);
      ledMatrix.setPixel(7,0);
      break;  
    case SEGMENT_VOR:
      ledMatrix.setPixel(1,1);
      ledMatrix.setPixel(2,1);
      ledMatrix.setPixel(3,1);
      break;  
    case SEGMENT_NACH:
      ledMatrix.setPixel(4,1);
      ledMatrix.setPixel(5,1);
      ledMatrix.setPixel(6,1);
      ledMatrix.setPixel(7,1);
      break;  
    case SEGMENT_HALB:
      ledMatrix.setPixel(0,2);
      ledMatrix.setPixel(1,2);
      ledMatrix.setPixel(2,2);
      ledMatrix.setPixel(3,2);
      break;  
    case SEGMENT_NUM_1:
      ledMatrix.setPixel(0,4);
      ledMatrix.setPixel(1,4);
      ledMatrix.setPixel(2,4);
      ledMatrix.setPixel(3,4);
      break;  
    case SEGMENT_NUM_2:
      ledMatrix.setPixel(0,4);
      ledMatrix.setPixel(1,4);
      ledMatrix.setPixel(0,3);
      ledMatrix.setPixel(1,3);
      break;  
    case SEGMENT_NUM_3:
      ledMatrix.setPixel(4,7);
      ledMatrix.setPixel(5,7);
      ledMatrix.setPixel(6,7);
      ledMatrix.setPixel(7,7);
      break;  
    case SEGMENT_NUM_4:
      ledMatrix.setPixel(4,2);
      ledMatrix.setPixel(5,2);
      ledMatrix.setPixel(6,2);
      ledMatrix.setPixel(7,2);
      break;  
    case SEGMENT_NUM_5:
      ledMatrix.setPixel(4,3);
      ledMatrix.setPixel(5,3);
      ledMatrix.setPixel(6,3);
      ledMatrix.setPixel(7,3);
      break;  
    case SEGMENT_NUM_6:
      ledMatrix.setPixel(3,4);
      ledMatrix.setPixel(4,4);
      ledMatrix.setPixel(5,4);
      ledMatrix.setPixel(6,4);
      ledMatrix.setPixel(7,4);
      break;  
    case SEGMENT_NUM_7:
      ledMatrix.setPixel(1,6);
      ledMatrix.setPixel(2,6);
      ledMatrix.setPixel(3,6);
      ledMatrix.setPixel(4,6);
      ledMatrix.setPixel(5,6);
      ledMatrix.setPixel(6,6);
      break;  
    case SEGMENT_NUM_8:
      ledMatrix.setPixel(0,7);
      ledMatrix.setPixel(1,7);
      ledMatrix.setPixel(2,7);
      ledMatrix.setPixel(3,7);
      break;  
    case SEGMENT_NUM_9:
      ledMatrix.setPixel(4,5);
      ledMatrix.setPixel(5,5);
      ledMatrix.setPixel(6,5);
      ledMatrix.setPixel(7,5);
      break;  
    case SEGMENT_NUM_10:
      ledMatrix.setPixel(1,5);
      ledMatrix.setPixel(2,5);
      ledMatrix.setPixel(3,5);
      ledMatrix.setPixel(4,5);
      break;  
    case SEGMENT_NUM_11:
      ledMatrix.setPixel(0,4);
      ledMatrix.setPixel(0,5);
      ledMatrix.setPixel(0,6);
      break;  
    case SEGMENT_NUM_12:
      ledMatrix.setPixel(0,3);
      ledMatrix.setPixel(1,3);
      ledMatrix.setPixel(2,3);
      ledMatrix.setPixel(3,3);
      ledMatrix.setPixel(4,3);
      break;  
  }
}

void showWordsOnLED(bool noShow) {
  if (!noShow) {
    ledMatrix.clear();
//    DPRINTLN("now showing new segments");
  }
  newSegments = 0;  
  if (minute(local)<5) {
    // bei Punkt x keine besonderen Elemente
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 11) % 12) + 1, noShow);
  } else if (minute(local)<10) {
    showSegment(SEGMENT_FUENF, noShow);
    showSegment(SEGMENT_NACH, noShow);
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 11) % 12) + 1, noShow);
  } else if (minute(local)<15) {
    showSegment(SEGMENT_ZEHN, noShow);
    showSegment(SEGMENT_NACH, noShow);
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 11) % 12) + 1, noShow);
  } else if (minute(local)<20) {
    showSegment(SEGMENT_FUENF, noShow);
    showSegment(SEGMENT_ZEHN, noShow);
    showSegment(SEGMENT_NACH, noShow);
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 11) % 12) + 1, noShow);
  } else if (minute(local)<25) {
    showSegment(SEGMENT_ZEHN, noShow);
    showSegment(SEGMENT_VOR, noShow);
    showSegment(SEGMENT_HALB, noShow);
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 12) % 12) + 1, noShow);
  } else if (minute(local)<30) {
    showSegment(SEGMENT_FUENF, noShow);
    showSegment(SEGMENT_VOR, noShow);
    showSegment(SEGMENT_HALB, noShow);
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 12) % 12) + 1, noShow);
  } else if (minute(local)<35) {
    showSegment(SEGMENT_HALB, noShow);
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 12) % 12) + 1, noShow);
  } else if (minute(local)<40) {
    showSegment(SEGMENT_FUENF, noShow);
    showSegment(SEGMENT_NACH, noShow);
    showSegment(SEGMENT_HALB, noShow);
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 12) % 12) + 1, noShow);
  } else if (minute(local)<45) {
    showSegment(SEGMENT_ZEHN, noShow);
    showSegment(SEGMENT_NACH, noShow);
    showSegment(SEGMENT_HALB, noShow);
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 12) % 12) + 1, noShow);
  } else if (minute(local)<50) {
    showSegment(SEGMENT_FUENF, noShow);
    showSegment(SEGMENT_ZEHN, noShow);
    showSegment(SEGMENT_VOR, noShow);
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 12) % 12) + 1, noShow);
  } else if (minute(local)<55) {
    showSegment(SEGMENT_ZEHN, noShow);
    showSegment(SEGMENT_VOR, noShow);
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 12) % 12) + 1, noShow);
  } else if (minute(local)<60) {
    showSegment(SEGMENT_FUENF, noShow);
    showSegment(SEGMENT_VOR, noShow);
    showSegment(SEGMENT_NUM_1 - 1 + ((hour(local) + 12) % 12) + 1, noShow);
  }
  if (!noShow) ledMatrix.commit();
}

bool animationDone() {
  return animation_done;
}

void showImage(byte imageId) {
  for (int i=0; i<8; i++) {
    ledMatrix.setColumn(i,ani_images[imageId][i]);
  }
  ledMatrix.commit();  
}

void animationShowFrame() {
  if (active_animation_frame >= ani_start[active_animation + 1]) {
    active_animation_frame = ani_start[active_animation];
    active_animation_cycle++;
    if (active_animation_cycle >= active_animation_repeat) {
      if (active_animation_repeat == 0) {
        active_animation_cycle = 0;        
      } else {
        animation_done = true;
      }
    }
  }
  if (!animation_done) {
    showImage(ani_frames[active_animation_frame]);
    active_animation_frame++;
  }
}

void animationLoop() {
  if (animation_timer > active_animation_speed) {
    animation_timer -= active_animation_speed;
    animationShowFrame();
  }
}

void animationStart(byte animation, byte repeats) {
  DPRINT(F("(init frame) "));
  animation_timer = 0;
  active_animation = animation;
  DPRINT(F("(access frame memory) "));
  active_animation_frame = ani_start[active_animation];
  active_animation_speed = ani_speed[active_animation];
  active_animation_repeat = repeats;
  active_animation_cycle = 0;
  animation_done = false;
  DPRINT(F("(showing frame) "));
  animationShowFrame();
}

void debugOutTime(String s, time_t t) {
  DPRINT(s);
  DPRINT(day(t));
  DPRINT(".");
  DPRINT(month(t));
  DPRINT(".");
  DPRINT(year(t));
  DPRINT("  ");
  DPRINT(hour(t));
  DPRINT(":");
  DPRINT(minute(t));
  DPRINT(":");
  DPRINTLN(second(t));
}

const char validCharset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 #*+\\|\",-_:;=?!$&/()<>'"; 

bool getNextMessagePart() {
  // called if an old message part is done (scrolled through or animation done)
  // prepares the text oder animation for display 
  // returns true if another part has been prepared or false if there are no other message parts 
  String result = "";
  int pos = -1;
/*  while (!s.equals("")) {
    pos = s.substr
  }*/
}

void getNextMessage() {
  //
  if (allMessages.equals("")) return;
  DPRINT(F("allMessages: "));DPRINTLN(allMessages);
  int pos = allMessages.indexOf("^EoM");
  DPRINT(F("pos: "));DPRINTLN(pos);
  if (pos >= 0) {
    newMessage = allMessages.substring(0, pos);
    DPRINT(F("newMessage: "));DPRINTLN(newMessage);
    if (allMessages.length() > newMessage.length()+5) {
      allMessages = allMessages.substring(pos + 5);
    } else {
      allMessages = "";
    }
    DPRINT(F("allMessages: "));DPRINTLN(allMessages);
  } else {
    newMessage = allMessages;
    DPRINT(F("newMessage direkt übernommen: "));DPRINTLN(newMessage);
    allMessages = "";
  }
}

bool isMessagePending() {
  return !allMessages.equals("") || !newMessage.equals("");
//  return !allMessages.equals("");
}

void checkNotifiers() {
  if (isMessagePending()) {
    notifier_pulsate = true;
    color_next = color_message_pending;
    if (notifier_mode == FADE_NORMAL) notifier_mode = FADE_OUT;
  } else {
    color_next = color_standard;
    notifier_pulsate = false;
  }
}

String escapeString(String s) {
  String result = "";
  int pos = -1;
/*  while (!s.equals("")) {
    pos = s.substr
  }*/
}

bool messageHandler(const HomieRange& range, const String& value) {
  if (value == "") return false;
//  newMessage = value;
  lastMessage = value;
  allMessages += value + "^EoM^";
  DPRINT(F("Received new message: ")); DPRINTLN(value);
  DPRINT(F("allMessages: ")); DPRINTLN(allMessages);
  infoNode.setProperty("message").send(value);
  infoNode.setProperty("status").send("RECEIVED");
  return true;
}

bool saveHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;
  DPRINT(F("Received a save config request..."));
  writeConfig(configFile);
  DPRINTLN(F("done"));
  return true;
}

bool setBrightnessHandler(const HomieRange& range, const String& value) {
  if (value == "") return false;
  int newVal = value.toInt();
  if ((newVal < 1) || (newVal>15)) return false;
  brightness = newVal;
  if (fade_mode == FADE_NORMAL) ledMatrix.setIntensity(brightness); // range is 0-15
  DPRINT(F("CONFIG: new brightness: ")); DPRINTLN(value);
  configNode.setProperty("brightness").send(value);
  return true;
}

bool setNotifierBrightnessHandler(const HomieRange& range, const String& value) {
  if (value == "") return false;
  int newVal = value.toInt();
  if ((newVal < 1) || (newVal>255)) return false;
  notifier_color.value = newVal;
  color_standard.value = newVal;
  if (notifier_mode == FADE_NORMAL) {
    leds[0] = notifier_color;
    FastLED.show();
  }
  DPRINT(F("CONFIG: new notifier brightness: ")); DPRINTLN(value);
  configNode.setProperty("notifierBrightness").send(value);
  return true;
}

bool setNotifierColorHandler(const HomieRange& range, const String& value) {
  if (value == "") return false;
  int newVal = value.toInt();
  if ((newVal < 1) || (newVal>255)) return false;
//  notifier_color.hue = newVal;
  color_standard.hue = newVal;
  if (notifier_mode == FADE_NORMAL) {
 //   leds[0] = notifier_color;
 //   FastLED.show();
    color_next = color_standard;
    notifier_mode = FADE_OUT;
  }
  DPRINT(F("CONFIG: new notifier hue: ")); DPRINTLN(value);
  configNode.setProperty("notifierColor").send(value);
  return true;
}

boolean isNumeric(String str) {
  unsigned int stringLength = str.length();
  if (stringLength == 0) return false;

  boolean seenDecimal = false;
  for(unsigned int i = 0; i < stringLength; ++i) {
    if (isDigit(str.charAt(i))) continue;
    if (str.charAt(i) == '.') {
      if (seenDecimal) return false;
      seenDecimal = true;
      continue;
    }
    return false;
  }
  return true;
}

bool buttonSetMaskHandler(const uint8_t btn, const String& value) {
  DPRINT(F("  buttonNode SetMaskEventHandler called... Button:"));
  DPRINT(btn);
  DPRINT(F("; Value:"));
  DPRINTLN(value);
  int v = value.toInt();
  if (!isNumeric(value)) return false;
  if ((v>=0) && (v<256)) {
    btnMQTTMask[btn-1] = v;
    return true;
  }
  return false;
}

bool button1SetMaskHandler(const HomieRange& range, const String& value) {
  return buttonSetMaskHandler(1, value);
}

bool button2SetMaskHandler(const HomieRange& range, const String& value) {
  return buttonSetMaskHandler(2, value);
}

bool button3SetMaskHandler(const HomieRange& range, const String& value) {
  return buttonSetMaskHandler(3, value);
}


void checkMessages() {
  if (isMessagePending() && ((next_state != STATE_MESSAGE) && (state != STATE_MESSAGE))) {
    DPRINTLN(F("Show the pending message. ")); 
    getNextMessage();
    next_state = STATE_MESSAGE;
    fade_mode = FADE_OUT;
    next_tick = 1;     
  }
}

void handleEvent(ace_button::AceButton* button, uint8_t eventType, uint8_t buttonState) {
  uint8_t btn = button->getPin();
  String eventNames[] = {
    "press", "idle", "click", "doubleclick", "longpress"
  };
  if ((btn < 1) || (btn > 3)) return;
  if ((eventType >= 0) && (eventType < 5)) {
    DPRINT(F(" Button "));
    DPRINT(btn);
    DPRINT(F(" detected "));
    DPRINT(eventNames[eventType]);
    if (btnMQTTMask[btn-1] & 1 << eventType) {
      buttonNode.setProperty("btn" + String(btn)).send(eventNames[eventType]);
      if (eventType != 1) buttonNode.setProperty("btn" + String(btn)).send(eventNames[1]);
      DPRINTLN(F(" and sent via MQTT."));
    } else {
      DPRINTLN(F(" but was masked for MQTT."));
    }
  }
}

void setup() {
  // Open serial communications and wait for port to open
#ifdef DEBUG
  Serial.begin(115200);
#endif

  DPRINTLN(F("Wordclock Mini"));
  DPRINT(F("version "));
  DPRINTLN(SW_VERSION);
  DPRINT(F("compiled "));
  DPRINT(compiledate);
  DPRINT(F(" at "));
  DPRINTLN(compiletime);
  DPRINTLN(F("starting..."));
  //GPIO 3 (RX) swap the pin to a GPIO.

  DPRINT(F("setting up pins..."));
  pinMode(3, FUNCTION_3);
  pinMode(LEFT_PIN, INPUT);
  pinMode(RIGHT_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  DPRINTLN(F("done."));

  DPRINT(F("setting up FastLED..."));
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, 1).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(255);
  FastLED.setDither(0);
  FastLED.setTemperature(UncorrectedTemperature);
  leds[0] = color_standard;
  FastLED.show();
  DPRINTLN(F("done."));
  
  DPRINT(F("setting up ledMatrix..."));
  delay(200);  // LED Matrix needs a few milliseconds to properly power up...
  ledMatrix.init();
  DPRINTLN(F("done."));

  DPRINT(F("showing power on logo..."));
  ledMatrix.setIntensity(0);
  animationStart(ANI_POWERON, 1);
  DPRINTLN(F("done."));

  DPRINT(F("showing boot animation..."));
  for (int i=1; i<16; i++) {
    delay(25);
    ledMatrix.setIntensity(i);
  }
  while (!animationDone()) {
    delay(10);
    animationLoop();
  }
  animationStart(ANI_CLOCK, 1);
  while (!animationDone()) {
    delay(10);
    animationLoop();
  }
  DPRINTLN(F("done."));

  DPRINT(F("reading config..."));
  showImage(IMG_REBOOT_0);
  delay(800);
  readConfig(configFile);
  leds[0] = color_standard;
  FastLED.show();
  DPRINTLN(F("done."));

  reboot_counter++;
  DPRINT(F("Reboot no. "));
  DPRINTLN(reboot_counter);
  DPRINT(F("saving reboot counter..."));
  for (int i=0; i<5; i++) {
    delay(200);
    showImage(IMG_REBOOT_0 + reboot_counter);
    delay(200);
    showImage(IMG_REBOOT_0);
  }
  writeConfig(configFile);
  DPRINTLN(F("done."));

  DPRINT(F("checking if setup mode is requested..."));
  ledMatrix.setIntensity(brightness); // range is 0-15
  if (reboot_counter > 2) {
    DPRINTLN (F("OK, hard reset after 3 reboots. Deleting existing config."));
    SPIFFS.remove("/homie/config.json"); 
    reboot_counter = 0;
    writeConfig(configFile);
  }
  DPRINTLN(F("done."));

  DPRINT(F("setting up buttons..."));
  buttonConfig.setEventHandler(handleEvent);
  buttonConfig.setFeature(ace_button::ButtonConfig::kFeatureClick);
  buttonConfig.setFeature(ace_button::ButtonConfig::kFeatureDoubleClick);
  buttonConfig.setFeature(ace_button::ButtonConfig::kFeatureLongPress);
  buttonConfig.setFeature(ace_button::ButtonConfig::kFeatureSuppressAll);
  DPRINTLN(F("done."));

  // Homie Setup
  DPRINT(F("setting up Homie nodes..."));
  WiFi.disconnect();
  Homie_setFirmware("Wordclock Mini", SW_VERSION_DEF); // The underscore is not a typo! See Magic bytes
  Homie.disableLedFeedback(); // before Homie.setup()
  infoNode.advertise("message").settable(messageHandler);
  configNode.advertise("brightness").settable(setBrightnessHandler); // brightness of display 1-15
  configNode.advertise("notifierBrightness").settable(setNotifierBrightnessHandler); // intensity of color led 1-128=fade from black to color, 129-255 fade from color to white
  configNode.advertise("notifierColor").settable(setNotifierColorHandler); // hue of color led 
  configNode.advertise("save").settable(saveHandler); // save the configuration 
  buttonNode.advertise("btn1"); 
  buttonNode.advertise("btn2"); 
  buttonNode.advertise("btn3"); 
  buttonNode.advertise("btn1_mask").settable(button1SetMaskHandler);
  buttonNode.advertise("btn2_mask").settable(button2SetMaskHandler); 
  buttonNode.advertise("btn3_mask").settable(button3SetMaskHandler);
  DPRINTLN(F("done."));

//  Homie.onEvent(onHomieEvent);
  DPRINT(F("setting up Homie..."));
  Homie.setup();
  DPRINTLN(F("done."));

  // ntp sync
  DPRINT(F("setting up NTP..."));
//  ntpStart(5);
  configTime(0*3600,0,"de.pool.ntp.org"); // UTC only, we'll make it local time later on
  DPRINTLN(F("done."));
  
  DPRINT(F("setting up boot message..."));
  ledMatrix.setTextProportional(F("Wordclock Mini"));
  tick_timer = 0;
  state_timer = 0;
  next_tick = 70;
  while (state_timer < MS_TIME_BOOT_MESSAGE) {
    Homie.loop();
    if (tick_timer > next_tick) {
      ledMatrix.clear();
      ledMatrix.scrollTextLeftProportional();
      ledMatrix.drawTextProportional();
      ledMatrix.commit();
      tick_timer = 0;
    }
    delay(10);
  }
  state = STATE_TIME;
  effect_time = MS_TIME_BOOT_MESSAGE;
  state_timer = 0;
  tick_timer = 0;
  next_tick = 70;
  // 1552646380 = Testzeit 15.03.2019 10:39:39
//  timeval tv = { 1552646380, 0 };
//  settimeofday(&tv, 0);
  reboot_counter = 0;
  writeConfig(configFile);
  reboot_counter_reset = true;
  DPRINTLN(F("done."));

  DPRINTLN(F("Moving over to main loop."));

}

bool hasTimeBeenSet() {
  return (year(local) != 1970);
}

bool checkWifiConnection() {
  if ((state != STATE_NO_WIFI) && (WiFi.status() != WL_CONNECTED)) {
    DPRINTLN(F("No wifi detected"));
    state = STATE_NO_WIFI;
    animationStart(ANI_WIFI, 0);
    segments = 0;
    return false;
  }  
  return (WiFi.status() == WL_CONNECTED);
}

bool checkNTP() {
  if (hasTimeBeenSet() || (state == STATE_NO_NTP)) return true;
  state = STATE_NO_NTP;
  animationStart(ANI_CLOCK, 0);
  return false;
}

void loopFaderMatrix() {
  if (fade_mode != FADE_NORMAL) {
    if (fade_timer > MS_FADER_FRAME) {
      switch (fade_mode) {
        case FADE_OUT:
          fader--;
          DPRINTLN(F("Fading out"));
          if (fader<=0) {
            state = next_state;
            fader = 0;
            fade_mode = FADE_BLACK;
            ledMatrix.mute();
            fader_black_frames = 0;
            next_tick = 0;
            segments = 0;
            next_frame = true;
            DPRINTLN(F("Fade out done, switching to black"));
          }
          break;
        case FADE_BLACK:
          fader_black_frames++;
          ledMatrix.mute();
          DPRINT(F("Black frame no. ")); DPRINTLN(fader_black_frames);
          fader = 0;
          if (fader_black_frames > FADER_BLACK_FRAMES) {
            ledMatrix.unmute();
            fade_mode = FADE_IN;
            DPRINTLN(F("Fade black done, preparing to fade in"));
          }
          break;
        case FADE_IN:
          fader++;
          DPRINTLN(F("Fading in"));
          if (fader >= brightness) {
            fader = brightness;
            fade_mode = FADE_NORMAL;
            DPRINT(F("Fading in done - fader: "));
            DPRINTLN(fader);
          }
          break;
      };
      ledMatrix.setIntensity(fader);
      fade_timer = 0;
    }
  }
}

void loopFaderNotifier() {
  checkNotifiers();
  if (notifier_mode != FADE_NORMAL) {
    if (notifier_timer >= notifier_frame) {
      notifier_timer = 0;
      switch (notifier_mode) {
        case FADE_IN:
          if (notifier_color.value < (int)(notifier_brightness - notifier_speed)) {
            notifier_color.value += notifier_speed;
          } else {
            notifier_color.value = notifier_brightness;
            if (notifier_pulsate || (notifier_color.hue != color_next.hue)) {
              notifier_mode = FADE_OUT;
            } else {
              notifier_mode = FADE_NORMAL;
            }
          }
          break;
        case FADE_OUT:
          if (notifier_color.value > notifier_speed) {
            notifier_color.value -= notifier_speed;
          } else {
            notifier_color.value = 0;
            notifier_mode = FADE_BLACK;
          }
          break;
        case FADE_BLACK:
          notifier_color = color_next;
          notifier_color.value = 0;
          notifier_mode = FADE_IN;
          break;
      }
      leds[0] = notifier_color;
      FastLED.show();
    }
  }  
}

void loop() {
  Homie.loop();
  leftButton.check();
  rightButton.check();
  bothButton.check();

  if (checkWifiConnection()) checkNTP();
  loopFaderMatrix();
  loopFaderNotifier();

/*  // Button handling will be moved...
  if (digitalRead(LEFT_PIN)==HIGH) { 
    // 1552646380 = Testzeit 15.03.2019 10:39:39
    timeval tv = { 1552646380, 0 };
    settimeofday(&tv, 0);
//    DPRINT("aktuell t_time"); DPRINTLN(local);
    led.hue += 254;
    leds[0] = led;
    FastLED.show();
    delay(15);
  }
  if (digitalRead(RIGHT_PIN)==HIGH) {
    debugOutTime("aktuell: ", local);
    DPRINT("aktuell t_time: "); DPRINTLN(local);
    led.hue += 1;
    leds[0] = led;
    FastLED.show();
    DPRINT("LED hue: "); DPRINTLN(led.hue);
    DPRINT("WiFi.status() = "); DPRINTLN(WiFi.status()); 
    if (isMessagePending() && ((next_state != STATE_MESSAGE) && (state != STATE_MESSAGE))) {
      DPRINTLN(F("Show the pending message. ")); 
      getNextMessage();
      next_state = STATE_MESSAGE;
      fade_mode = FADE_OUT;
      next_tick = 1;     
    }
    delay(15);
  }
*/

  if ((tick_timer > next_tick) && (fade_mode != FADE_OUT)) {
    tick_timer = 0;
    local = CE.toLocal(time(nullptr), &tcr);
    switch (state) {
      case STATE_MESSAGE:
        ledMatrix.clear();
        ledMatrix.unmute();
        if (next_frame) {
          ledMatrix.setTextProportional(newMessage);
          DPRINT(F("set text to: ")); DPRINTLN(newMessage); 
          next_frame = false; 
        } else {
          ledMatrix.scrollTextLeftProportional();
          DPRINTLN(F("scrolling text "));  
          ledMatrix.drawTextProportional();
          ledMatrix.commit();
          if (ledMatrix.isScrollingDone()) {
            DPRINTLN(F("scrolling text done, now fade out over to time"));  
            fade_mode = FADE_OUT;
            next_state = STATE_TIME;
            segments = 0;
            newMessage = "";
          }
        }
        next_tick = 70;
        break;
      case STATE_INTRO:
        ledMatrix.clear();
        ledMatrix.scrollTextLeftProportional();
        ledMatrix.drawTextProportional();
        ledMatrix.commit();
        next_tick = 70;
        break;
      case STATE_TIME:
        showWordsOnLED(DONT_SHOW_ON_LED);
        if (segments != newSegments) {
          if (fade_mode == FADE_NORMAL) {
            fade_mode = FADE_OUT;
            next_state = STATE_TIME;
//            DPRINTLN("Switching to fade out");
          }
          if (next_frame) {
            segments = newSegments;
//            DPRINTLN("fade out done, should now be fade in");
//            debugOutTime("alt: ", last_time);
//            debugOutTime("neu: ", local);
//            DPRINTLN(local);
            showWordsOnLED(SHOW_ON_LED);
            last_time = local;
            next_frame = false;            
          }
        }
        next_tick = 500;
        break;
      case STATE_NO_WIFI:
        animationLoop();
        if (WiFi.status() == WL_CONNECTED) {
          segments = 0;
          state = STATE_TIME;
          //animationStart(ANI_CLOCK, 0);
        }
        break;
      case STATE_NO_NTP:
        animationLoop();
        if (hasTimeBeenSet()) {
          state = STATE_TIME;
          segments = 0;
        }
        break;
    }
  }

  delay(1);
}
