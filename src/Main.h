#ifndef Main_h
#define Main_h

#include <arduino.h>

//DomoChip Informations
//Lights Request Web Pages
//http://IP/setL?l1=0&l2=1&l3=t //that will switch OFF Light1, switch ON light2 and toggle light3

#define APPLICATION1_HEADER "WirelessLight.h"
#define APPLICATION1_NAME "WLight"
#define APPLICATION1_DESC "DomoChip Wireless Light"
#define APPLICATION1_CLASS Lights

#define VERSION_NUMBER "1.0.12"

#define DEFAULT_AP_SSID "WirelessLight"
#define DEFAULT_AP_PSK "PasswordLight"

//Enable developper mode (SPIFFS editor)
#define DEVELOPPER_MODE 0

//Log Serial Object
#define LOG_SERIAL Serial
//Choose Log Serial Speed
#define LOG_SERIAL_SPEED 115200

//Choose Pin used to boot in Rescue Mode
#define RESCUE_BTN_PIN 2

//Define time to wait for Rescue press (in s)
#define RESCUE_BUTTON_WAIT 0

//Status LED
//#define STATUS_LED_SETUP pinMode(XX, OUTPUT);pinMode(XX, OUTPUT);
//#define STATUS_LED_OFF digitalWrite(XX, HIGH);digitalWrite(XX, HIGH);
//#define STATUS_LED_ERROR digitalWrite(XX, HIGH);digitalWrite(XX, HIGH);
//#define STATUS_LED_WARNING digitalWrite(XX, HIGH);digitalWrite(XX, HIGH);
//#define STATUS_LED_GOOD digitalWrite(XX, HIGH);digitalWrite(XX, HIGH);

//construct Version text
#if DEVELOPPER_MODE
#define VERSION VERSION_NUMBER "-DEV"
#else
#define VERSION VERSION_NUMBER
#endif

#endif

