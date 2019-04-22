#ifndef WirelessLight_h
#define WirelessLight_h

#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include "Main.h"
#include "base\Utils.h"
#include "base\Application.h"

const char appDataPredefPassword[] PROGMEM = "ewcXoCt4HHjZUvY1";

#include "data\status1.html.gz.h"
#include "data\config1.html.gz.h"

#include <ESP8266HTTPClient.h>
#include "VolatileTicker.h"
#include <PubSubClient.h>
#include <Adafruit_MCP23017.h>

//Number of light can't be more than 8
#define NUMBER_OF_LIGHTS 6
//Number of Event is the number of events to retain for send
#define NUMBER_OF_EVENTS 16
//GPIO number of ESP pin that receive MCP interrupt
#define MCP_INT_PIN 13
//number of retry to send event to Home Automation
#define MAX_RETRY_NUMBER 3

class Lights : public Application
{
public:
  typedef struct
  {
    byte lightNumber; //light Number (zero based)
    bool lightOn;     //light switched ON or OFF (used only for SingleClick)
    byte eventCode;   //1:SingleClick; 2:DoubleClick; etc.
    bool sent1;       //lightOn event sent to HA or not
    bool sent2;       //eventCode event sent to HA or not
    byte retryLeft1;  //number of retries left to send lightOn to Home Automation
    byte retryLeft2;  //number of retries left to send eventCode to Home Automation
  } Event;

private:
#define HA_HTTP_GENERIC 0
#define HA_HTTP_JEEDOM 1

  typedef struct
  {
    byte type = HA_HTTP_GENERIC;
    bool tls = false;
    byte fingerPrint[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    //Ids of light indicator in Home Automation
    uint16_t lightsId[NUMBER_OF_LIGHTS];

    struct
    {
      //Ids of command/event to throw in Home Automation for SingleClick, DoubleClick, TripleClick and QuadrupleClick
      uint16_t clickCmdId[NUMBER_OF_LIGHTS][4];
      char uriPattern[150 + 1] = {0};
    } generic;
    struct
    {
      //Ids of virtual in Jeedom that will receive 1,2,3,4 for SingleClick, DoubleClick, TripleClick and QuadrupleClick
      uint16_t clickVirtualIds[NUMBER_OF_LIGHTS];
      char apiKey[48 + 1] = {0};
    } jeedom;
  } HTTP;

#define HA_MQTT_GENERIC_1 0 //All Lights in the same topic (/command1; /command2; ... /status1; /status2; ...)
#define HA_MQTT_GENERIC_2 1 //Lights seperated (/1/command; /1/status; ... /2/command: /2/status; ...)

  typedef struct
  {
    byte type = HA_MQTT_GENERIC_1;
    uint32_t port = 1883;
    char username[128 + 1] = {0};
    char password[150 + 1] = {0};
    struct
    {
      char baseTopic[64 + 1] = {0};
    } generic;
  } MQTT;

#define HA_PROTO_DISABLED 0
#define HA_PROTO_HTTP 1
#define HA_PROTO_MQTT 2

  typedef struct
  {
    byte protocol = HA_PROTO_DISABLED;
    char hostname[64 + 1] = {0};
    HTTP http;
    MQTT mqtt;
  } HomeAutomation;

  //false for a normal switch; true for push button
  volatile bool _pushButtonMode[NUMBER_OF_LIGHTS];
  volatile uint16_t _multiClickTime[NUMBER_OF_LIGHTS];
  char _lightNames[NUMBER_OF_LIGHTS][25];

  HomeAutomation _ha;

  //Declare run/status properties
  Adafruit_MCP23017 _mcp23017;
  volatile uint8_t _previousGPIOA, _previousGPIOB;
  volatile byte _lightStatus[NUMBER_OF_LIGHTS]; //(0:waiting, 1 : SingleClickInProgress, 2 : DoubleClickInProgress, ...)
  volatile VolatileTicker _timers[NUMBER_OF_LIGHTS];

  volatile Event _eventsList[NUMBER_OF_EVENTS]; //events list filled in by interrupts and processes by Run in loop
  volatile byte _nextEventPos = 0;

  int _haSendResult = 0;
  WiFiClient _wifiClient;
  WiFiClientSecure _wifiClientSecure;

  PubSubClient *_pubSubClient = NULL;

  //Declare required private methods
  static void McpInt();
  static void VolTickerInt(byte input);
  bool MQTTConnectAndSubscribe(bool init = false);
  static void MQTTCallback(char *topic, byte *payload, unsigned int length);

  void SetConfigDefaultValues();
  void ParseConfigJSON(DynamicJsonDocument &doc);
  bool ParseConfigWebRequest(AsyncWebServerRequest *request);
  String GenerateConfigJSON(bool forSaveFile);
  String GenerateStatusJSON();
  bool AppInit(bool reInit);
  const uint8_t *GetHTMLContent(WebPageForPlaceHolder wp);
  size_t GetHTMLContentSize(WebPageForPlaceHolder wp);
  void AppInitWebServer(AsyncWebServer &server, bool &shouldReboot, bool &pauseApplication);
  void AppRun();

public:
  Lights(char appId, String fileName);
};

#endif
