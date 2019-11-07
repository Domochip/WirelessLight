#include "WirelessLight.h"

//variable used to compare new state of Inputs to previous

//global pointers required for ISRs
Adafruit_MCP23017 *gmcp23017;
volatile uint8_t *gpreviousGPIOA;
volatile uint8_t *gpreviousGPIOB;
volatile bool *gpushButtonMode;
volatile uint16_t *gmultiClickTime;
volatile byte *glightStatus;
volatile VolatileTicker *gtimers;
volatile Lights::Event *geventsList;
volatile byte *gnextEventPos;

//------------------------------------------
//STATIC - ISR for MCp setupInterrupts
void Lights::McpInt()
{
    //read GPIOB
    uint8_t newGPIOB = gmcp23017->readGPIO(1);

    //for each lights
    for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
    {
        //if this input changed and (not a push button OR pushButton input is falling)
        if (((*gpreviousGPIOB & (1 << i)) != (newGPIOB & (1 << i))) && (!gpushButtonMode[i] || ((*gpreviousGPIOB & (1 << i)) > 0)))
        {
            //we need to take it into account

            //stop timer of this input
            if (gtimers[i].active())
                gtimers[i].detach();

            //increment status (0Waiting->1SingleClick; 1SingleClick->2DoubleClick; etc.)
            glightStatus[i]++;

            //start timer
            gtimers[i].once_ms(gmultiClickTime[i], VolTickerInt, i);
        }
    }

    //put current GPIOB into previous variable
    *gpreviousGPIOB = newGPIOB;
}

//------------------------------------------
//STATIC - ISR for VolatileTicker
void Lights::VolTickerInt(byte input)
{
    //Special Remote Restart order with 7 clicks
    if (glightStatus[input] == 6)
    {
        LOG_SERIAL.println(F("Restart by 6 clicks"));
        ESP.restart();
    }
    //Special Remote Restart order with 9 clicks
    if (glightStatus[input] == 8)
    {
        LOG_SERIAL.println(F("Restart in Rescue Mode by 8 clicks"));
        EEPROM.begin(4);
        EEPROM.write(0, 1);
        EEPROM.end();
        ESP.restart();
    }

    //change light if SingleClick
    if (glightStatus[input] == 1)
    {
        //Write output to the opposite of the current state
        gmcp23017->digitalWrite(NUMBER_OF_LIGHTS - 1 - input, ((*gpreviousGPIOA & (1 << (NUMBER_OF_LIGHTS - 1 - input))) > 0 ? LOW : HIGH));
        //refresh previousGPIOA (outputs)
        *gpreviousGPIOA = gmcp23017->readGPIO(0);
    }

    //move cursor to the next event Position
    (*gnextEventPos) = (++(*gnextEventPos)) % NUMBER_OF_EVENTS;

    byte myEventPos = ((*gnextEventPos) == 0 ? NUMBER_OF_EVENTS : (*gnextEventPos)) - 1;

    //log event in the list
    geventsList[myEventPos].lightNumber = input;
    geventsList[myEventPos].lightOn = (*gpreviousGPIOA & (1 << (NUMBER_OF_LIGHTS - 1 - input))) > 0;
    if (glightStatus[input] <= 4) //we can have more than quadruple-click
        geventsList[myEventPos].eventCode = glightStatus[input];
    else
        geventsList[myEventPos].eventCode = 4;
    geventsList[myEventPos].sent1 = false;
    geventsList[myEventPos].sent2 = false;
    geventsList[myEventPos].retryLeft1 = MAX_RETRY_NUMBER;
    geventsList[myEventPos].retryLeft2 = MAX_RETRY_NUMBER;

    //reset light Status to Waiting
    glightStatus[input] = 0;
}

//------------------------------------------
// Connect then Subscribe to MQTT
bool Lights::MqttConnect(bool init)
{
    if (!WiFi.isConnected())
        return false;

    char sn[9];
    sprintf_P(sn, PSTR("%08x"), ESP.getChipId());

    //generate clientID
    String clientID(F(APPLICATION1_NAME));
    clientID += sn;

    //Connect
    if (!_ha.mqtt.username[0])
        _mqttClient.connect(clientID.c_str());
    else
        _mqttClient.connect(clientID.c_str(), _ha.mqtt.username, _ha.mqtt.password);

    if (_mqttClient.connected())
    {
        //Subscribe to needed topic
        //prepare topic subscription
        String subscribeTopic = _ha.mqtt.generic.baseTopic;
        byte xPos = 0;
        //check for final slash
        if (subscribeTopic.length() && subscribeTopic.charAt(subscribeTopic.length() - 1) != '/')
            subscribeTopic += '/';

        //Replace placeholders
        if (subscribeTopic.indexOf(F("$sn$")) != -1)
            subscribeTopic.replace(F("$sn$"), sn);

        if (subscribeTopic.indexOf(F("$mac$")) != -1)
            subscribeTopic.replace(F("$mac$"), WiFi.macAddress());

        if (subscribeTopic.indexOf(F("$model$")) != -1)
            subscribeTopic.replace(F("$model$"), APPLICATION1_NAME);

        switch (_ha.mqtt.type) //switch on MQTT type
        {
        case HA_MQTT_GENERIC_1: // mytopic/command1
            subscribeTopic += F("commandX");
            xPos = subscribeTopic.length() - 1;
            break;

        case HA_MQTT_GENERIC_2: // mytopic/1/command
            subscribeTopic += F("X/command");
            xPos = subscribeTopic.length() - 9;
            break;
        }

        //subscribe topics for each lights
        for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
        {
            subscribeTopic[xPos] = i + '1';
            if (init)
                _mqttClient.publish(subscribeTopic.c_str(), ""); //make empty publish only for init
            _mqttClient.subscribe(subscribeTopic.c_str());
        }
    }

    return _mqttClient.connected();
}

//------------------------------------------
//Callback used when an MQTT message arrived
void Lights::MqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
    //if payload is not 1 byte long return
    if (length != 1)
        return;

    byte lightNumber = 255;

    //if topic match HA_MQTT_GENERIC_1 (toto/command3)
    if (strlen(topic) >= 8 && !strncmp_P(topic + strlen(topic) - 8, PSTR("command"), 7))
        lightNumber = topic[strlen(topic) - 1] - '1';

    //if topic match HA_MQTT_GENERIC_2 (toto/3/command)
    if (strlen(topic) >= 9 && !strncmp_P(topic + strlen(topic) - 8, PSTR("/command"), 8))
        lightNumber = topic[strlen(topic) - 9] - '1';

    //check light number parsed
    if (lightNumber >= NUMBER_OF_LIGHTS)
        return;

    //check payload
    if (payload[0] != '0' && payload[0] != '1' && payload[0] != 't' && payload[0] != 'T')
        return;

    //read current ouput state
    bool currentLightON = (*gpreviousGPIOA & (1 << (NUMBER_OF_LIGHTS - 1 - lightNumber))) > 0;

    //do we need to do something
    //yes if ligth is on and order is different of light on
    //yes if ligth is off and order is different of light off
    //yes if order is toggle
    if ((currentLightON && payload[0] != '1') || (!currentLightON && payload[0] != '0') || payload[0] == 't' || payload[0] == 'T')
    {
        //apply change to output
        gmcp23017->digitalWrite(NUMBER_OF_LIGHTS - 1 - lightNumber, (payload[0] == '1') ? HIGH : (payload[0] == '0' ? LOW : (currentLightON ? LOW : HIGH)));

        //refresh previousGPIOA (outputs)
        *gpreviousGPIOA = gmcp23017->readGPIO(0);

        //increment nextEventPos to prevent problem if interrupt occurs
        (*gnextEventPos) = (++(*gnextEventPos)) % NUMBER_OF_EVENTS;

        byte myEventPos = ((*gnextEventPos) == 0 ? NUMBER_OF_EVENTS : (*gnextEventPos)) - 1;

        //fillin our "reserved" event line
        geventsList[myEventPos].lightNumber = lightNumber;
        geventsList[myEventPos].lightOn = (payload[0] == '1') ? true : (payload[0] == '0' ? false : !currentLightON);
        geventsList[myEventPos].eventCode = 0; //no click occured
        geventsList[myEventPos].sent1 = false;
        geventsList[myEventPos].sent2 = false;
        geventsList[myEventPos].retryLeft1 = MAX_RETRY_NUMBER;
        geventsList[myEventPos].retryLeft2 = MAX_RETRY_NUMBER;
    }
}

//------------------------------------------
//Used to initialize configuration properties to default values
void Lights::SetConfigDefaultValues()
{
    for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
    {
        _pushButtonMode[i] = false;
        _multiClickTime[i] = 300;
        _lightNames[i][0] = 0;
    }

    _ha.protocol = HA_PROTO_DISABLED;
    _ha.hostname[0] = 0;

    _ha.http.type = HA_HTTP_GENERIC;
    _ha.http.tls = false;
    memset(_ha.http.fingerPrint, 0, 20);

    for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
    {
        _ha.http.lightsId[i] = 0;
        _ha.http.generic.clickCmdId[i][0] = 0;
        _ha.http.generic.clickCmdId[i][1] = 0;
        _ha.http.generic.clickCmdId[i][2] = 0;
        _ha.http.generic.clickCmdId[i][3] = 0;
        _ha.http.jeedom.clickVirtualIds[i] = 0;
    }

    _ha.http.generic.uriPattern[0] = 0;
    _ha.http.jeedom.apiKey[0] = 0;

    _ha.mqtt.type = HA_MQTT_GENERIC_1;
    _ha.mqtt.port = 1883;
    _ha.mqtt.username[0] = 0;
    _ha.mqtt.password[0] = 0;
    _ha.mqtt.generic.baseTopic[0] = 0;
};
//------------------------------------------
//Parse JSON object into configuration properties
void Lights::ParseConfigJSON(DynamicJsonDocument &doc)
{
    byte i;
    char btmStr[5] = {'b', 't', 'm', 'X', 0};       //pushButtonMode string
    char mctStr[5] = {'m', 'c', 't', 'X', 0};       //multiClickTime string
    char lnStr[4] = {'l', 'n', 'X', 0};             //lightNames string
    char lidStr[5] = {'l', 'i', 'd', 'X', 0};       //lightsId string
    char mcidStr[6] = {'X', 'c', 'i', 'd', 'X', 0}; //clickCmdId string
    char cvidStr[6] = {'c', 'v', 'i', 'd', 'X', 0}; //clickVirtualIds string

    for (i = 0; i < NUMBER_OF_LIGHTS; i++)
    {
        //Parse pushButtonMode
        btmStr[3] = i + '1';
        if (!doc[btmStr].isNull())
            _pushButtonMode[i] = doc[btmStr];

        //Parse multiClickTime
        mctStr[3] = i + '1';
        if (!doc[mctStr].isNull())
            _multiClickTime[i] = doc[mctStr];

        //Parse lightNames
        lnStr[2] = i + '1';
        if (!doc[lnStr].isNull())
            strlcpy(_lightNames[i], doc[lnStr], sizeof(_lightNames[i]));
    }

    //Parse Home Automation config
    if (!doc[F("haproto")].isNull())
        _ha.protocol = doc[F("haproto")];
    if (!doc[F("hahost")].isNull())
        strlcpy(_ha.hostname, doc["hahost"], sizeof(_ha.hostname));

    if (!doc[F("hahtype")].isNull())
        _ha.http.type = doc[F("hahtype")];
    if (!doc[F("hahtls")].isNull())
        _ha.http.tls = doc[F("hahtls")];
    if (!doc[F("hahfp")].isNull())
        Utils::FingerPrintS2A(_ha.http.fingerPrint, doc[F("hahfp")]);

    for (i = 0; i < NUMBER_OF_LIGHTS; i++)
    {
        lidStr[3] = i + '1';
        if (!doc[lidStr].isNull())
            _ha.http.lightsId[i] = doc[lidStr];

        mcidStr[4] = i + '1';
        mcidStr[0] = 's';
        if (!doc[mcidStr].isNull())
            _ha.http.generic.clickCmdId[i][0] = doc[mcidStr];
        mcidStr[0] = 'd';
        if (!doc[mcidStr].isNull())
            _ha.http.generic.clickCmdId[i][1] = doc[mcidStr];
        mcidStr[0] = 't';
        if (!doc[mcidStr].isNull())
            _ha.http.generic.clickCmdId[i][2] = doc[mcidStr];
        mcidStr[0] = 'q';
        if (!doc[mcidStr].isNull())
            _ha.http.generic.clickCmdId[i][3] = doc[mcidStr];

        cvidStr[4] = i + '1';
        if (!doc[cvidStr].isNull())
            _ha.http.jeedom.clickVirtualIds[i] = doc[cvidStr];
    }

    if (!doc[F("hahgup")].isNull())
        strlcpy(_ha.http.generic.uriPattern, doc[F("hahgup")], sizeof(_ha.http.generic.uriPattern));

    if (!doc[F("hahjak")].isNull())
        strlcpy(_ha.http.jeedom.apiKey, doc[F("hahjak")], sizeof(_ha.http.jeedom.apiKey));

    if (!doc[F("hamtype")].isNull())
        _ha.mqtt.type = doc[F("hamtype")];
    if (!doc[F("hamport")].isNull())
        _ha.mqtt.port = doc[F("hamport")];
    if (!doc[F("hamu")].isNull())
        strlcpy(_ha.mqtt.username, doc[F("hamu")], sizeof(_ha.mqtt.username));
    if (!doc[F("hamp")].isNull())
        strlcpy(_ha.mqtt.password, doc[F("hamp")], sizeof(_ha.mqtt.password));

    if (!doc[F("hamgbt")].isNull())
        strlcpy(_ha.mqtt.generic.baseTopic, doc[F("hamgbt")], sizeof(_ha.mqtt.generic.baseTopic));
};
//------------------------------------------
//Parse HTTP POST parameters in request into configuration properties
bool Lights::ParseConfigWebRequest(AsyncWebServerRequest *request)
{
    byte i;
    char btmStr[5] = {'b', 't', 'm', 'X', 0};       //pushButtonMode string
    char mctStr[5] = {'m', 'c', 't', 'X', 0};       //multiClickTime string
    char lnStr[4] = {'l', 'n', 'X', 0};             //lightNames string
    char lidStr[5] = {'l', 'i', 'd', 'X', 0};       //lightsId string
    char mcidStr[6] = {'X', 'c', 'i', 'd', 'X', 0}; //clickCmdId string
    char cvidStr[6] = {'c', 'v', 'i', 'd', 'X', 0}; //clickVirtualIds string

    for (i = 0; i < NUMBER_OF_LIGHTS; i++)
    {
        //Parse pushButtonMode
        btmStr[3] = i + '1';
        if (request->hasParam(btmStr, true))
            _pushButtonMode[i] = (request->getParam(btmStr, true)->value() == F("on"));
        else
            _pushButtonMode[i] = false;

        //Parse multiClickTime
        mctStr[3] = i + '1';
        if (request->hasParam(mctStr, true))
            _multiClickTime[i] = request->getParam(mctStr, true)->value().toInt();

        //Parse lightNames
        lnStr[2] = i + '1';
        if (request->hasParam(lnStr, true) && request->getParam(lnStr, true)->value().length() < sizeof(_lightNames[i]))
            strcpy(_lightNames[i], request->getParam(lnStr, true)->value().c_str());
        else
            _lightNames[i][0] = 0;
    }

    //Parse HA protocol
    if (request->hasParam(F("haproto"), true))
        _ha.protocol = request->getParam(F("haproto"), true)->value().toInt();
    //if an home Automation protocol has been selected then get common param
    if (_ha.protocol != HA_PROTO_DISABLED)
    {
        if (request->hasParam(F("hahost"), true) && request->getParam(F("hahost"), true)->value().length() < sizeof(_ha.hostname))
            strcpy(_ha.hostname, request->getParam(F("hahost"), true)->value().c_str());
    }

    //Now get specific param
    switch (_ha.protocol)
    {
    case HA_PROTO_HTTP:

        if (request->hasParam(F("hahtype"), true))
            _ha.http.type = request->getParam(F("hahtype"), true)->value().toInt();
        if (request->hasParam(F("hahtls"), true))
            _ha.http.tls = (request->getParam(F("hahtls"), true)->value() == F("on"));
        else
            _ha.http.tls = false;
        if (request->hasParam(F("hahfp"), true))
            Utils::FingerPrintS2A(_ha.http.fingerPrint, request->getParam(F("hahfp"), true)->value().c_str());
        for (i = 0; i < NUMBER_OF_LIGHTS; i++)
        {
            lidStr[3] = i + '1';
            if (request->hasParam(lidStr, true))
                _ha.http.lightsId[i] = request->getParam(lidStr, true)->value().toInt();
        }

        switch (_ha.http.type)
        {
        case HA_HTTP_GENERIC:
            for (i = 0; i < NUMBER_OF_LIGHTS; i++)
            {
                mcidStr[4] = i + '1';
                mcidStr[0] = 's';
                if (request->hasParam(mcidStr, true))
                    _ha.http.generic.clickCmdId[i][0] = request->getParam(mcidStr, true)->value().toInt();
                mcidStr[0] = 'd';
                if (request->hasParam(mcidStr, true))
                    _ha.http.generic.clickCmdId[i][1] = request->getParam(mcidStr, true)->value().toInt();
                mcidStr[0] = 't';
                if (request->hasParam(mcidStr, true))
                    _ha.http.generic.clickCmdId[i][2] = request->getParam(mcidStr, true)->value().toInt();
                mcidStr[0] = 'q';
                if (request->hasParam(mcidStr, true))
                    _ha.http.generic.clickCmdId[i][3] = request->getParam(mcidStr, true)->value().toInt();
            }
            if (request->hasParam(F("hahgup"), true) && request->getParam(F("hahgup"), true)->value().length() < sizeof(_ha.http.generic.uriPattern))
                strcpy(_ha.http.generic.uriPattern, request->getParam(F("hahgup"), true)->value().c_str());
            if (!_ha.hostname[0] || !_ha.http.generic.uriPattern[0])
                _ha.protocol = HA_PROTO_DISABLED;
            break;
        case HA_HTTP_JEEDOM:

            for (i = 0; i < NUMBER_OF_LIGHTS; i++)
            {
                cvidStr[4] = i + '1';
                if (request->hasParam(cvidStr, true))
                    _ha.http.jeedom.clickVirtualIds[i] = request->getParam(cvidStr, true)->value().toInt();
            }

            char tempApiKey[48 + 1];
            //put apiKey into temporary one for predefpassword
            if (request->hasParam(F("hahjak"), true) && request->getParam(F("hahjak"), true)->value().length() < sizeof(tempApiKey))
                strcpy(tempApiKey, request->getParam(F("hahjak"), true)->value().c_str());
            //check for previous apiKey (there is a predefined special password that mean to keep already saved one)
            if (strcmp_P(tempApiKey, appDataPredefPassword))
                strcpy(_ha.http.jeedom.apiKey, tempApiKey);
            if (!_ha.hostname[0] || !_ha.http.jeedom.apiKey[0])
                _ha.protocol = HA_PROTO_DISABLED;
            break;
        }
        break;

    case HA_PROTO_MQTT:

        if (request->hasParam(F("hamtype"), true))
            _ha.mqtt.type = request->getParam(F("hamtype"), true)->value().toInt();
        if (request->hasParam(F("hamport"), true))
            _ha.mqtt.port = request->getParam(F("hamport"), true)->value().toInt();
        if (request->hasParam(F("hamu"), true) && request->getParam(F("hamu"), true)->value().length() < sizeof(_ha.mqtt.username))
            strcpy(_ha.mqtt.username, request->getParam(F("hamu"), true)->value().c_str());
        char tempPassword[64 + 1] = {0};
        //put MQTT password into temporary one for predefpassword
        if (request->hasParam(F("hamp"), true) && request->getParam(F("hamp"), true)->value().length() < sizeof(tempPassword))
            strcpy(tempPassword, request->getParam(F("hamp"), true)->value().c_str());
        //check for previous password (there is a predefined special password that mean to keep already saved one)
        if (strcmp_P(tempPassword, appDataPredefPassword))
            strcpy(_ha.mqtt.password, tempPassword);

        switch (_ha.mqtt.type)
        {
        case HA_MQTT_GENERIC_1:
        case HA_MQTT_GENERIC_2:
            if (request->hasParam(F("hamgbt"), true) && request->getParam(F("hamgbt"), true)->value().length() < sizeof(_ha.mqtt.generic.baseTopic))
                strcpy(_ha.mqtt.generic.baseTopic, request->getParam(F("hamgbt"), true)->value().c_str());

            if (!_ha.hostname[0] || !_ha.mqtt.generic.baseTopic[0])
                _ha.protocol = HA_PROTO_DISABLED;
            break;
        }
        break;
    }

    return true;
};
//------------------------------------------
//Generate JSON from configuration properties
String Lights::GenerateConfigJSON(bool forSaveFile = false)
{
    String gc('{');
    byte i;
    char fpStr[60];

    if (!forSaveFile)
        gc = gc + F("\"nol\":") + NUMBER_OF_LIGHTS + ','; //send number of light for web page

    for (i = 0; i < NUMBER_OF_LIGHTS; i++)
    {
        if (i)
            gc += ',';
        //Generate Button mode
        gc = gc + F("\"btm") + (i + 1) + F("\":") + _pushButtonMode[i];
        //Generate multi Click Time
        gc = gc + F(",\"mct") + (i + 1) + F("\":") + _multiClickTime[i];
        //Generate light Names
        gc = gc + F(",\"ln") + (i + 1) + F("\":\"") + _lightNames[i] + '"';
    }

    //Generate Home Automation config information
    gc = gc + F(",\"haproto\":") + _ha.protocol;
    gc = gc + F(",\"hahost\":\"") + _ha.hostname + '"';

    //if for WebPage or protocol selected is HTTP
    if (!forSaveFile || _ha.protocol == HA_PROTO_HTTP)
    {
        gc = gc + F(",\"hahtype\":") + _ha.http.type;
        gc = gc + F(",\"hahtls\":") + _ha.http.tls;
        gc = gc + F(",\"hahfp\":\"") + Utils::FingerPrintA2S(fpStr, _ha.http.fingerPrint, forSaveFile ? 0 : ':') + '"';
        for (i = 0; i < NUMBER_OF_LIGHTS; i++)
            gc = gc + F(",\"lid") + (i + 1) + F("\":") + _ha.http.lightsId[i];

        for (i = 0; i < NUMBER_OF_LIGHTS; i++)
        {
            gc = gc + F(",\"scid") + (i + 1) + F("\":") + _ha.http.generic.clickCmdId[i][0];
            gc = gc + F(",\"dcid") + (i + 1) + F("\":") + _ha.http.generic.clickCmdId[i][1];
            gc = gc + F(",\"tcid") + (i + 1) + F("\":") + _ha.http.generic.clickCmdId[i][2];
            gc = gc + F(",\"qcid") + (i + 1) + F("\":") + _ha.http.generic.clickCmdId[i][3];
        }
        gc = gc + F(",\"hahgup\":\"") + _ha.http.generic.uriPattern + '"';

        for (i = 0; i < NUMBER_OF_LIGHTS; i++)
            gc = gc + F(",\"cvid") + (i + 1) + F("\":") + _ha.http.jeedom.clickVirtualIds[i];
        if (forSaveFile)
            gc = gc + F(",\"hahjak\":\"") + _ha.http.jeedom.apiKey + '"';
        else
            gc = gc + F(",\"hahjak\":\"") + (__FlashStringHelper *)appDataPredefPassword + '"'; //predefined special password (mean to keep already saved one)
    }

    //if for WebPage or protocol selected is MQTT
    if (!forSaveFile || _ha.protocol == HA_PROTO_MQTT)
    {
        gc = gc + F(",\"hamtype\":") + _ha.mqtt.type;
        gc = gc + F(",\"hamport\":") + _ha.mqtt.port;
        gc = gc + F(",\"hamu\":\"") + _ha.mqtt.username + '"';
        if (forSaveFile)
            gc = gc + F(",\"hamp\":\"") + _ha.mqtt.password + '"';
        else
            gc = gc + F(",\"hamp\":\"") + (__FlashStringHelper *)appDataPredefPassword + '"'; //predefined special password (mean to keep already saved one)

        gc = gc + F(",\"hamgbt\":\"") + _ha.mqtt.generic.baseTopic + '"';
    }

    gc += '}';

    return gc;
};
//------------------------------------------
//Generate JSON of application status
String Lights::GenerateStatusJSON()
{
    String gs('{');

    gs = gs + F("\"nol\":") + NUMBER_OF_LIGHTS;

    for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
    {
        gs = gs + F(",\"ln") + (i + 1) + F("\":\"") + _lightNames[i] + '"';
        gs = gs + F(",\"l") + (i + 1) + F("\":") + ((_previousGPIOA & (1 << (NUMBER_OF_LIGHTS - 1 - i))) > 0);
    }

    gs = gs + F(",\"has1\":\"");
    switch (_ha.protocol)
    {
    case HA_PROTO_DISABLED:
        gs = gs + F("Disabled");
        break;
    case HA_PROTO_HTTP:
        gs = gs + F("Last HTTP request : ") + (_haSendResult ? F("OK") : F("Failed"));
        break;
    case HA_PROTO_MQTT:
        gs = gs + F("MQTT Connection State : ");
        switch (_mqttClient.state())
        {
        case MQTT_CONNECTION_TIMEOUT:
            gs = gs + F("Timed Out");
            break;
        case MQTT_CONNECTION_LOST:
            gs = gs + F("Lost");
            break;
        case MQTT_CONNECT_FAILED:
            gs = gs + F("Failed");
            break;
        case MQTT_CONNECTED:
            gs = gs + F("Connected");
            break;
        case MQTT_CONNECT_BAD_PROTOCOL:
            gs = gs + F("Bad Protocol Version");
            break;
        case MQTT_CONNECT_BAD_CLIENT_ID:
            gs = gs + F("Incorrect ClientID ");
            break;
        case MQTT_CONNECT_UNAVAILABLE:
            gs = gs + F("Server Unavailable");
            break;
        case MQTT_CONNECT_BAD_CREDENTIALS:
            gs = gs + F("Bad Credentials");
            break;
        case MQTT_CONNECT_UNAUTHORIZED:
            gs = gs + F("Connection Unauthorized");
            break;
        }

        if (_mqttClient.state() == MQTT_CONNECTED)
            gs = gs + F("\",\"has2\":\"Last Publish Result : ") + (_haSendResult ? F("OK") : F("Failed"));

        break;
    }
    gs += '"';

    gs += '}';

    return gs;
};
//------------------------------------------
//code to execute during initialization and reinitialization of the app
bool Lights::AppInit(bool reInit)
{
    if (reInit)
    {
        detachInterrupt(digitalPinToInterrupt(MCP_INT_PIN));
        for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
            if (_timers[i].active())
                _timers[i].detach();
    }

    //Stop MQTT Reconnect
    _mqttReconnectTicker.detach();
    if (_mqttClient.connected()) //Issue #598 : disconnect() crash if client not yet set
        _mqttClient.disconnect();

    //if MQTT used so configure it
    if (_ha.protocol == HA_PROTO_MQTT)
    {
        //setup MQTT client
        _mqttClient.setClient(_wifiClient).setServer(_ha.hostname, _ha.mqtt.port).setCallback(std::bind(&Lights::MqttCallback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        //Connect
        MqttConnect(true);
    }

    for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
    {
        _lightStatus[i] = 0; //waiting button movement
    }

    for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
    {
        _eventsList[i].lightNumber = i;
        _eventsList[i].lightOn = false;
        _eventsList[i].eventCode = 0; //no click occured
        _eventsList[i].sent1 = false;
        _eventsList[i].sent2 = false;
        _eventsList[i].retryLeft1 = MAX_RETRY_NUMBER;
        _eventsList[i].retryLeft2 = MAX_RETRY_NUMBER;
    }
    for (byte i = NUMBER_OF_LIGHTS; i < NUMBER_OF_EVENTS; i++)
    {
        _eventsList[i].lightNumber = 0;
        _eventsList[i].lightOn = false;
        _eventsList[i].eventCode = 0;
        _eventsList[i].sent1 = true;
        _eventsList[i].sent2 = true;
        _eventsList[i].retryLeft1 = 0;
        _eventsList[i].retryLeft2 = 0;
    }
    _nextEventPos = NUMBER_OF_LIGHTS;

    if (!reInit)
    {
        //Init ESP8266 interrupt pin for MCP23017
        pinMode(MCP_INT_PIN, INPUT);

        //init I2C and MCP23017
        _mcp23017.begin();
        _mcp23017.setupInterrupts(false, false, LOW);

        for (byte i = 0; i < 8; i++)
        {
            _mcp23017.pinMode(i + 8, INPUT); //GPIOB are input from optocoupler
            _mcp23017.pullUp(i + 8, HIGH);   //100K pullup on inputs
        }
        delay(500);
    }

    for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
    {
        if (!reInit)
            _mcp23017.pinMode(i, OUTPUT); //GPIOA are Output to relays

        _mcp23017.digitalWrite(i, LOW);

        //interrupt detection varies because of switch type (normal switch or push button)
        _mcp23017.setupInterruptPin(i + 8, CHANGE);
        //clear Interrupt by reading GPIOB
        _mcp23017.readGPIO(1);
    }

    if (!reInit)
    {
        gmcp23017 = &_mcp23017;
        gpreviousGPIOA = &_previousGPIOA;
        gpreviousGPIOB = &_previousGPIOB;
        gpushButtonMode = _pushButtonMode;
        gmultiClickTime = _multiClickTime;
        glightStatus = _lightStatus;
        gtimers = _timers;
        geventsList = _eventsList;
        gnextEventPos = &_nextEventPos;
    }

    //initialize _previousGPIOA and B for future comparison
    _previousGPIOA = _mcp23017.readGPIO(0);
    _previousGPIOB = _mcp23017.readGPIO(1);

    attachInterrupt(digitalPinToInterrupt(MCP_INT_PIN), McpInt, FALLING);

    return true;
};
//------------------------------------------
//Return HTML Code to insert into Status Web page
const uint8_t *Lights::GetHTMLContent(WebPageForPlaceHolder wp)
{
    switch (wp)
    {
    case status:
        return (const uint8_t *)status1htmlgz;
        break;
    case config:
        return (const uint8_t *)config1htmlgz;
        break;
    default:
        return nullptr;
        break;
    };
    return nullptr;
};
//and his Size
size_t Lights::GetHTMLContentSize(WebPageForPlaceHolder wp)
{
    switch (wp)
    {
    case status:
        return sizeof(status1htmlgz);
        break;
    case config:
        return sizeof(config1htmlgz);
        break;
    default:
        return 0;
        break;
    };
    return 0;
};
//------------------------------------------
//code to register web request answer to the web server
void Lights::AppInitWebServer(AsyncWebServer &server, bool &shouldReboot, bool &pauseApplication)
{

    server.on("/setL", HTTP_GET, [this](AsyncWebServerRequest *request) {
        //bits to know if a param has been passed
        byte lightPassed = 0;
        //corresponding ordered passed
        byte lightOrders[NUMBER_OF_LIGHTS];

        char paramName[3] = {'l', 'X', 0};
        //for each lights
        for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
        {
            paramName[1] = i + '1';
            //check that an order is present
            if (request->hasParam(paramName) && request->getParam(paramName)->value().length() == 1)
            {
                lightOrders[i] = request->getParam(paramName)->value()[0];
                if (lightOrders[i] == '0' || lightOrders[i] == '1' || lightOrders[i] == 't' || lightOrders[i] == 'T')
                    lightPassed += (1 << i);
            }
        }

        //if no light order passed
        if (!lightPassed)
        {
            //answer with error and return
            request->send(400, F("text/html"), F("No valid order received"));
            return;
        }

        //for each light
        for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
        {
            //light has been passed then apply
            if (lightPassed & (1 << i))
            {
                //read current ouput state
                bool currentLightON = (_previousGPIOA & (1 << (NUMBER_OF_LIGHTS - 1 - i))) > 0;

                //do we need to do something
                //yes if ligth is on and order is different of light on
                //yes if ligth is off and order is different of light off
                //yes if order is toggle
                if ((currentLightON && lightOrders[i] != '1') || (!currentLightON && lightOrders[i] != '0') || lightOrders[i] == 't' || lightOrders[i] == 'T')
                {
                    //apply change to output
                    _mcp23017.digitalWrite(NUMBER_OF_LIGHTS - 1 - i, (lightOrders[i] == '1') ? HIGH : (lightOrders[i] == '0' ? LOW : (currentLightON ? LOW : HIGH)));

                    //refresh previousGPIOA (outputs)
                    _previousGPIOA = _mcp23017.readGPIO(0);

                    //increment nextEventPos to prevent problem if interrupt occurs
                    _nextEventPos = (++_nextEventPos) % NUMBER_OF_EVENTS;

                    byte myEventPos = (_nextEventPos == 0 ? NUMBER_OF_EVENTS : _nextEventPos) - 1;

                    //fillin our "reserved" event line
                    _eventsList[myEventPos].lightNumber = i;
                    _eventsList[myEventPos].lightOn = (lightOrders[i] == '1') ? true : (lightOrders[i] == '0' ? false : !currentLightON);
                    _eventsList[myEventPos].eventCode = 0; //no click occured
                    _eventsList[myEventPos].sent1 = false;
                    _eventsList[myEventPos].sent2 = false;
                    _eventsList[myEventPos].retryLeft1 = MAX_RETRY_NUMBER;
                    _eventsList[myEventPos].retryLeft2 = MAX_RETRY_NUMBER;
                }
            }
        }

        //return OK
        request->send(200);
    });

    server.on("/setAllON", HTTP_GET, [this](AsyncWebServerRequest *request) {
        //for each light
        for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
        {
            //do we need to do something
            //yes if ligth is off
            if (!(_previousGPIOA & (1 << i)))
            {
                //apply change to output
                _mcp23017.digitalWrite(NUMBER_OF_LIGHTS - 1 - i, HIGH);

                //refresh previousGPIOA (outputs)
                _previousGPIOA = _mcp23017.readGPIO(0);

                //increment nextEventPos to prevent problem if interrupt occurs
                _nextEventPos = (++_nextEventPos) % NUMBER_OF_EVENTS;

                byte myEventPos = (_nextEventPos == 0 ? NUMBER_OF_EVENTS : _nextEventPos) - 1;

                //fillin our "reserved" event line
                _eventsList[myEventPos].lightNumber = i;
                _eventsList[myEventPos].lightOn = true;
                _eventsList[myEventPos].eventCode = 0; //no click occured
                _eventsList[myEventPos].sent1 = false;
                _eventsList[myEventPos].sent2 = false;
                _eventsList[myEventPos].retryLeft1 = MAX_RETRY_NUMBER;
                _eventsList[myEventPos].retryLeft2 = MAX_RETRY_NUMBER;
            }
        }

        //return OK
        request->send(200);
    });

    server.on("/setAllOFF", HTTP_GET, [this](AsyncWebServerRequest *request) {
        //for each light
        for (byte i = 0; i < NUMBER_OF_LIGHTS; i++)
        {
            //do we need to do something
            //yes if ligth is on
            if (_previousGPIOA & (1 << i))
            {
                //apply change to output
                _mcp23017.digitalWrite(NUMBER_OF_LIGHTS - 1 - i, LOW);

                //refresh previousGPIOA (outputs)
                _previousGPIOA = _mcp23017.readGPIO(0);

                //increment nextEventPos to prevent problem if interrupt occurs
                _nextEventPos = (++_nextEventPos) % NUMBER_OF_EVENTS;

                byte myEventPos = (_nextEventPos == 0 ? NUMBER_OF_EVENTS : _nextEventPos) - 1;

                //fillin our "reserved" event line
                _eventsList[myEventPos].lightNumber = i;
                _eventsList[myEventPos].lightOn = false;
                _eventsList[myEventPos].eventCode = 0; //no click occured
                _eventsList[myEventPos].sent1 = false;
                _eventsList[myEventPos].sent2 = false;
                _eventsList[myEventPos].retryLeft1 = MAX_RETRY_NUMBER;
                _eventsList[myEventPos].retryLeft2 = MAX_RETRY_NUMBER;
            }
        }

        //return OK
        request->send(200);
    });
};

//------------------------------------------
//Run for timer
void Lights::AppRun()
{
    if (_needMqttReconnect)
    {
        _needMqttReconnect = false;
        LOG_SERIAL.print(F("MQTT Reconnection : "));
        if (MqttConnect())
            LOG_SERIAL.println(F("OK"));
        else
            LOG_SERIAL.println(F("Failed"));
    }

    //if MQTT required but not connected and reconnect ticker not started
    if (_ha.protocol == HA_PROTO_MQTT && !_mqttClient.connected() && !_mqttReconnectTicker.active())
    {
        LOG_SERIAL.println(F("MQTT Disconnected"));
        //set Ticker to reconnect after 20 or 60 sec (Wifi connected or not)
        _mqttReconnectTicker.once_scheduled((WiFi.isConnected() ? 20 : 60), [this]() { _needMqttReconnect = true; _mqttReconnectTicker.detach(); });
    }

    if (_ha.protocol == HA_PROTO_MQTT)
        _mqttClient.loop();

    //for each events in the list starting by nextEventPos
    for (byte evPos = _nextEventPos, counter = 0; counter < NUMBER_OF_EVENTS; counter++, evPos = (evPos + 1) % NUMBER_OF_EVENTS)
    {
        //if eventCode above 1 then don't need to send it
        if (_eventsList[evPos].eventCode > 1 && !_eventsList[evPos].sent1)
            _eventsList[evPos].sent1 = true;

        //if lightOn not yet sent and retryLeft over 0
        if (!_eventsList[evPos].sent1 && _eventsList[evPos].retryLeft1)
        {
            //switch on protocol
            switch (_ha.protocol)
            {
            case HA_PROTO_DISABLED: //nothing to do
                _eventsList[evPos].sent1 = true;
                break;

            case HA_PROTO_HTTP:
                //if there is lightIds
                if (_ha.http.lightsId[_eventsList[evPos].lightNumber])
                {
                    String completeURI;

                    switch (_ha.http.type) //switch on HTTP type
                    {
                    case HA_HTTP_GENERIC:
                        completeURI = _ha.http.generic.uriPattern;
                        break;
                    case HA_HTTP_JEEDOM:
                        completeURI = F("http$tls$://$host$/core/api/jeeApi.php?apikey=$apikey$&type=virtual&id=$id$&value=$val$");
                        break;
                    }

                    //Replace placeholders
                    if (completeURI.indexOf(F("$tls$")) != -1)
                        completeURI.replace(F("$tls$"), _ha.http.tls ? "s" : "");

                    if (completeURI.indexOf(F("$host$")) != -1)
                        completeURI.replace(F("$host$"), _ha.hostname);

                    if (completeURI.indexOf(F("$id$")) != -1)
                        completeURI.replace(F("$id$"), String(_ha.http.lightsId[_eventsList[evPos].lightNumber]));

                    if (completeURI.indexOf(F("$val$")) != -1)
                        completeURI.replace(F("$val$"), (_eventsList[evPos].lightOn ? "1" : "0"));

                    if (completeURI.indexOf(F("$apikey$")) != -1)
                        completeURI.replace(F("$apikey$"), _ha.http.jeedom.apiKey);

                    //create HTTP request
                    HTTPClient http;

                    //if tls is enabled or not, we need to provide certificate fingerPrint
                    if (!_ha.http.tls)
                        http.begin(_wifiClient, completeURI);
                    else
                    {
                        if (Utils::IsFingerPrintEmpty(_ha.http.fingerPrint))
                            _wifiClientSecure.setInsecure();
                        else
                            _wifiClientSecure.setFingerprint(_ha.http.fingerPrint);
                        http.begin(_wifiClientSecure, completeURI);
                    }

                    //if request successfull then sent is OK
                    if (http.GET() == 200)
                        _eventsList[evPos].sent1 = true;

                    http.end();
                }
                else
                    _eventsList[evPos].sent1 = true; //else nothing to do
                break;

            case HA_PROTO_MQTT:

                //if we are connected
                if (_mqttClient.connected())
                {
                    //prepare topic
                    String completeTopic = _ha.mqtt.generic.baseTopic;

                    //check for final slash
                    if (completeTopic.length() && completeTopic.charAt(completeTopic.length() - 1) != '/')
                        completeTopic += '/';

                    switch (_ha.mqtt.type) //switch on MQTT type
                    {
                    case HA_MQTT_GENERIC_1: // mytopic/status1
                        completeTopic += F("status");
                        completeTopic += _eventsList[evPos].lightNumber + 1;
                        break;

                    case HA_MQTT_GENERIC_2: // mytopic/1/status
                        completeTopic += _eventsList[evPos].lightNumber + 1;
                        completeTopic += F("/status");
                        break;
                    }

                    //prepare sn for placeholder
                    char sn[9];
                    sprintf_P(sn, PSTR("%08x"), ESP.getChipId());

                    //Replace placeholders
                    if (completeTopic.indexOf(F("$sn$")) != -1)
                        completeTopic.replace(F("$sn$"), sn);

                    if (completeTopic.indexOf(F("$mac$")) != -1)
                        completeTopic.replace(F("$mac$"), WiFi.macAddress());

                    if (completeTopic.indexOf(F("$model$")) != -1)
                        completeTopic.replace(F("$model$"), APPLICATION1_NAME);

                    //send
                    if ((_haSendResult = _mqttClient.publish(completeTopic.c_str(), _eventsList[evPos].lightOn ? "1" : "0")))
                        _eventsList[evPos].sent1 = true;
                }

                break;
            }

            //if sent failed decrement retry count
            if (!_eventsList[evPos].sent1)
                _eventsList[evPos].retryLeft1--;
        }

        //if eventCode not yet sent and retryLeft over 0
        if (!_eventsList[evPos].sent2 && _eventsList[evPos].retryLeft2)
        {
            //if eventCode bellow 1 then don't need to send it
            if (_eventsList[evPos].eventCode < 1 && !_eventsList[evPos].sent2)
                _eventsList[evPos].sent2 = true;

            //switch on protocol
            switch (_ha.protocol)
            {
            case HA_PROTO_DISABLED: //nothing to do
                _eventsList[evPos].sent2 = true;
                break;

            case HA_PROTO_HTTP:
                //if there is ids (depend of type of HTTP protocol used)
                if ((_ha.http.type == HA_HTTP_GENERIC && _ha.http.generic.clickCmdId[_eventsList[evPos].lightNumber][_eventsList[evPos].eventCode - 1]) || (_ha.http.type == HA_HTTP_JEEDOM && _ha.http.jeedom.clickVirtualIds[_eventsList[evPos].lightNumber]))
                {
                    String completeURI;

                    switch (_ha.http.type) //switch on HTTP type
                    {
                    case HA_HTTP_GENERIC:
                        completeURI = _ha.http.generic.uriPattern;

                        if (completeURI.indexOf(F("$id$")) != -1)
                            completeURI.replace(F("$id$"), String(_ha.http.generic.clickCmdId[_eventsList[evPos].lightNumber][_eventsList[evPos].eventCode - 1]));

                        if (completeURI.indexOf(F("$val$")) != -1)
                            completeURI.replace(F("$val$"), "1");
                        break;
                    case HA_HTTP_JEEDOM:
                        completeURI = F("http$tls$://$host$/core/api/jeeApi.php?apikey=$apikey$&type=virtual&id=$id$&value=$val$");

                        if (completeURI.indexOf(F("$id$")) != -1)
                            completeURI.replace(F("$id$"), String(_ha.http.jeedom.clickVirtualIds[_eventsList[evPos].lightNumber]));
                        if (completeURI.indexOf(F("$val$")) != -1)
                            completeURI.replace(F("$val$"), String(_eventsList[evPos].eventCode));
                        break;
                    }

                    //Replace others placeholders
                    if (completeURI.indexOf(F("$tls$")) != -1)
                        completeURI.replace(F("$tls$"), _ha.http.tls ? "s" : "");

                    if (completeURI.indexOf(F("$host$")) != -1)
                        completeURI.replace(F("$host$"), _ha.hostname);

                    if (completeURI.indexOf(F("$apikey$")) != -1)
                        completeURI.replace(F("$apikey$"), _ha.http.jeedom.apiKey);

                    //create HTTP request
                    HTTPClient http;

                    //if tls is enabled or not, we need to provide certificate fingerPrint
                    if (!_ha.http.tls)
                        http.begin(_wifiClient, completeURI);
                    else
                    {
                        if (Utils::IsFingerPrintEmpty(_ha.http.fingerPrint))
                            _wifiClientSecure.setInsecure();
                        else
                            _wifiClientSecure.setFingerprint(_ha.http.fingerPrint);
                        http.begin(_wifiClientSecure, completeURI);
                    }

                    //if request successfull then sent is OK
                    if (http.GET() == 200)
                        _eventsList[evPos].sent2 = true;

                    http.end();
                }
                else
                    _eventsList[evPos].sent2 = true; //Nothing to do
                break;

            case HA_PROTO_MQTT:

                //if we are connected
                if (_mqttClient.connected())
                {
                    //prepare topic
                    String completeTopic = _ha.mqtt.generic.baseTopic;

                    //check for final slash
                    if (completeTopic.length() && completeTopic.charAt(completeTopic.length() - 1) != '/')
                        completeTopic += '/';

                    switch (_ha.mqtt.type) //switch on MQTT type
                    {
                    case HA_MQTT_GENERIC_1: // mytopic/click1
                        completeTopic += F("click");
                        completeTopic += _eventsList[evPos].lightNumber + 1;
                        break;

                    case HA_MQTT_GENERIC_2: // mytopic/1/click
                        completeTopic += _eventsList[evPos].lightNumber + 1;
                        completeTopic += F("/click");
                        break;
                    }

                    //prepare sn for placeholder
                    char sn[9];
                    sprintf_P(sn, PSTR("%08x"), ESP.getChipId());

                    //Replace placeholders
                    if (completeTopic.indexOf(F("$sn$")) != -1)
                        completeTopic.replace(F("$sn$"), sn);

                    if (completeTopic.indexOf(F("$mac$")) != -1)
                        completeTopic.replace(F("$mac$"), WiFi.macAddress());

                    if (completeTopic.indexOf(F("$model$")) != -1)
                        completeTopic.replace(F("$model$"), APPLICATION1_NAME);

                    //send
                    if ((_haSendResult = _mqttClient.publish(completeTopic.c_str(), String(_eventsList[evPos].eventCode).c_str())))
                        _eventsList[evPos].sent2 = true;
                }
                break;
            }

            //if sent failed decrement retry count
            if (!_eventsList[evPos].sent2)
                _eventsList[evPos].retryLeft2--;
        }
    }
}

//------------------------------------------
//Constructor
Lights::Lights(char appId, String appName) : Application(appId, appName) {}