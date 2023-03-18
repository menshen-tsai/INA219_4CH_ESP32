/*
 Font draw speed and flicker test, draws all numbers 0-999 in each font
 (0-99 in font 8)
 Average time in milliseconds to draw a character is shown in red
 A total of 2890 characters are drawn in each font (190 in font 8)
 
 Needs fonts 2, 4, 6, 7 and 8

 Make sure all the display driver and pin connections are correct by
 editing the User_Setup.h file in the TFT_eSPI library folder.

 Note that yield() or delay(0) must be called in long duration for/while
 loops to stop the ESP8266 watchdog triggering.

 #########################################################################
 ###### DON'T FORGET TO UPDATE THE User_Setup.h FILE IN THE LIBRARY ######
 #########################################################################
 */
#define DEBUG_NTPClient

#include <TFT_eSPI.h> // Graphics and font library for ST7735 driver chip
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiMulti.h>
#include "FS.h"
#include "SD.h"


//#include <WiFiClient.h>
//#include <NTPClient.h>
//#include <WiFiUdp.h>
//#include "RTClib.h"
////#include "SdFat.h"
#include "Ticker.h"
#include <time.h>

#include "config.h"

#define USE_SDFS
#define INCLUDE_FALLBACK_INDEX_HTM

#ifdef INCLUDE_FALLBACK_INDEX_HTM
#include "extras/index_htm.h"
#endif

#define DBG_OUTPUT_PORT Serial

////#include "ESPDateTime.h"

#define SD_CS 26 // SD chip select
#define LCD_CS 5
Adafruit_INA219 ina219_0(0x40);
Adafruit_INA219 ina219_1(0x41);
Adafruit_INA219 ina219_2(0x44);
Adafruit_INA219 ina219_3(0x45);

Adafruit_INA219 INA219[4] = {Adafruit_INA219(0x40), Adafruit_INA219(0x41), Adafruit_INA219(0x44), Adafruit_INA219(0x45)};
TFT_eSPI tft = TFT_eSPI();  // Invoke library, pins defined in User_Setup.h

WebServer server(80);
WiFiMulti wifiMulti;

static bool fsOK;
String unsupportedFiles = String();

File uploadFile;

static const char TEXT_PLAIN[] PROGMEM = "text/plain";
static const char FS_INIT_ERROR[] PROGMEM = "FS INIT ERROR";
static const char FILE_NOT_FOUND[] PROGMEM = "FileNotFound";
const char* host = "fsbrowser";

const int chipSelect = 16; 
File file;
Ticker periodicI2C;

long timezone = 8; 
byte daysavetime = 0;


// WiFi connect timeout per AP. Increase when connecting takes longer.
const uint32_t connectTimeoutMs = 5000;
unsigned long drawTime = 0;

boolean ina219Status[4];
boolean sdStatus=false;
char logFilename[30];

boolean _i2cRead = false;
static uint32_t count = 0;


typedef struct 
{
  float shuntvoltage = 0;
  float busvoltage = 0;
  float current_mA = 0;
  float loadvoltage = 0;
  float power_mW = 0;
} Measurement;

typedef struct
{
  Measurement measurement[4];
  struct tm _timeInfo;
} INA219Measurement;

INA219Measurement ina219Measurement;




void periodicI2C_Read();


void returnOK() {
  server.send(200, "text/plain", "");
}

void returnFail(String msg) {
  server.send(500, "text/plain", msg + "\r\n");
}

bool loadFromSdCard(String path) {
  String dataType = "text/plain";
  if (path.endsWith("/")) {
    path += "index.htm";
  }

  if (path.endsWith(".src")) {
    path = path.substring(0, path.lastIndexOf("."));
  } else if (path.endsWith(".htm")) {
    dataType = "text/html";
  } else if (path.endsWith(".css")) {
    dataType = "text/css";
  } else if (path.endsWith(".js")) {
    dataType = "application/javascript";
  } else if (path.endsWith(".png")) {
    dataType = "image/png";
  } else if (path.endsWith(".gif")) {
    dataType = "image/gif";
  } else if (path.endsWith(".jpg")) {
    dataType = "image/jpeg";
  } else if (path.endsWith(".ico")) {
    dataType = "image/x-icon";
  } else if (path.endsWith(".xml")) {
    dataType = "text/xml";
  } else if (path.endsWith(".pdf")) {
    dataType = "application/pdf";
  } else if (path.endsWith(".zip")) {
    dataType = "application/zip";
  }

  File dataFile = SD.open(path.c_str());
  if (dataFile.isDirectory()) {
    path += "/index.htm";
    dataType = "text/html";
    dataFile = SD.open(path.c_str());
  }

  if (!dataFile) {
    return false;
  }

  if (server.hasArg("download")) {
    dataType = "application/octet-stream";
  }

  if (server.streamFile(dataFile, dataType) != dataFile.size()) {
    DBG_OUTPUT_PORT.println("Sent less data than expected!");
  }

  dataFile.close();
  return true;
}

void printDirectory() {
  if (!server.hasArg("dir")) {
    return returnFail("BAD ARGS");
  }
  String path = server.arg("dir");
  if (path != "/" && !SD.exists((char *)path.c_str())) {
    return returnFail("BAD PATH");
  }
  File dir = SD.open((char *)path.c_str());
  path = String();
  if (!dir.isDirectory()) {
    dir.close();
    return returnFail("NOT DIR");
  }
  dir.rewindDirectory();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/json", "");
  WiFiClient client = server.client();

  server.sendContent("[");
  for (int cnt = 0; true; ++cnt) {
    File entry = dir.openNextFile();
    if (!entry) {
      break;
    }

    String output;
    if (cnt > 0) {
      output = ',';
    }

    output += "{\"type\":\"";
    output += (entry.isDirectory()) ? "dir" : "file";
    output += "\",\"name\":\"";
    output += entry.path();
    output += "\"";
    output += "\",\"size\":\"";
    output += entry.size();
    output += "\"";
    output += "}";
    server.sendContent(output);
    entry.close();
  }
  server.sendContent("]");
  dir.close();
}

void handleNotFound() {
  if (sdStatus && loadFromSdCard(server.uri())) {
    return;
  }
  String message = "SDCARD Not Detected\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " NAME:" + server.argName(i) + "\n VALUE:" + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  DBG_OUTPUT_PORT.print(message);
}
 
void setup(void) {
  unsigned long epochTime;
  File root;
  IPAddress ip;
  char bufIP[20];
  struct tm tmstruct ;
  
  Serial.begin(115200);
  while (!Serial) {
  }

  Serial.print(__FILE__);
  Serial.print(" created at ");
  Serial.print(__DATE__);
  Serial.print(" ");
  Serial.println(__TIME__);

  if (!SD.begin(SD_CS)) {
    tft.drawString("SD Failed", 0,100, 1);
    Serial.println("SD.begin failed");
    sdStatus = false;
  } else {
    sdStatus = true;
    Serial.println("SD Initialized!");
  }
 
  // Don't save WiFi configuration in flash - optional
  WiFi.persistent(false);

  // Set WiFi to station mode
  WiFi.mode(WIFI_STA);

  // Register multi WiFi networks
  wifiMulti.addAP(SSID1, PASS1);
  wifiMulti.addAP(SSID2, PASS2);


  // Maintain WiFi connection
  while (wifiMulti.run(connectTimeoutMs) != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("WiFi connected: ");
  Serial.print(WiFi.SSID());
  
  ip = WiFi.localIP();
  Serial.print("IP: "); Serial.println(ip);
  sprintf(bufIP, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  ////////////////////////////////
  // MDNS INIT
  if (MDNS.begin(host)) {
    MDNS.addService("http", "tcp", 80);
  }


  server.on("/list", HTTP_GET, printDirectory);
//  server.on("/edit", HTTP_DELETE, handleDelete);
//  server.on("/edit", HTTP_PUT, handleCreate);
//  server.on("/edit", HTTP_POST, []() {
//    returnOK();
//  }, handleFileUpload);
  server.onNotFound(handleNotFound);

  // Start server
  server.begin();
  Serial.println("HTTP server started");
  
  Serial.println("Contacting Time Server");
  configTime(3600*timezone, daysavetime*3600, "time.nist.gov", "0.pool.ntp.org", "1.pool.ntp.org");

  delay(2000);
  tmstruct.tm_year = 0;
  getLocalTime(&tmstruct, 5000);
  Serial.printf("\nNow is : %d-%02d-%02d %02d:%02d:%02d\n",(tmstruct.tm_year)+1900,( tmstruct.tm_mon)+1, tmstruct.tm_mday,tmstruct.tm_hour , tmstruct.tm_min, tmstruct.tm_sec);
  Serial.println("");

  Serial.print("seconds since 1970: ");
  time_t now;
  time(&now);
  Serial.println(now);
  struct tm timeInfo;
  getLocalTime(&timeInfo);

  sprintf(logFilename, "/INA219-%04d%02d%02d%02d%02d%02d.log",
                timeInfo.tm_year+1900, timeInfo.tm_mon+1,
                timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
  printf("Filename: %s\n", logFilename);
  
  // Initialize the INA219.
  // By default the initialization will use the largest range (32V, 2A).  However
  // you can call a setCalibration function to change this range (see comments).
  if (! ina219_0.begin()) {
    Serial.println("Failed to find INA219 0x40 chip");
    ina219Status[0] = false;
  } else {
    ina219Status[0] = true;
  }

  if (! ina219_1.begin()) {
    Serial.println("Failed to find INA219 0x41 chip");
    ina219Status[1] = false;
  } else {
    ina219Status[1] = true;
  }
  if (! ina219_2.begin()) {
    Serial.println("Failed to find INA219 0x44 chip");
    ina219Status[2] = false;
  } else {
    ina219Status[2] = true;
  }
  if (! ina219_3.begin()) {
    Serial.println("Failed to find INA219 0x45 chip");
    ina219Status[3] = false;
  } else {
    ina219Status[3] = true;
  }
  

  // To use a slightly lower 32V, 1A range (higher precision on amps):
  //ina219.setCalibration_32V_1A();
  // Or to use a lower 16V, 400mA range (higher precision on volts and amps):
  //ina219.setCalibration_16V_400mA();

  Serial.println("Measuring voltage and current with INA219 ...");


  tft.init();
  tft.setRotation(2);
  tft.fillScreen(TFT_BLUE);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  //tft.drawString("1234567890123456", 0, 0, 2);
  //tft.drawString("CH1 (0x40)", 0, 0, 1);
  tft.drawString("mV", 28, 0, 1); 
  tft.drawString("mA", 64, 0, 1); 
  tft.drawString("mW", 108, 0, 1); 
  
  tft.drawString("1", 0, 18, 2);
  tft.drawString("2", 0, 36, 2);
  tft.drawString("3", 0, 54, 2);
  tft.drawString("4", 0, 72, 2);


  tft.drawString(bufIP, 0, 120, 1);
  
  if (sdStatus == true) {
    // List files in SD root.
    root = SD.open("/");
    listFiles(root, 0);
  }

  periodicI2C.attach_ms(1000, periodicI2C_Read);
}



void loop() {
  
  String ina219_S[4];
  char buf[20];
  static uint32_t i;
  float shuntvoltage = 0;
  float busvoltage = 0;
  float current_mA = 0;
  float loadvoltage = 0;
  float power_mW = 0;
  String ina219_0S, ina219_1S, ina219_2S, ina219_3S;
  File root;


  server.handleClient();

  struct tm timeInfo = ina219Measurement._timeInfo;

  String currentDate = String(timeInfo.tm_year+1900) + "," + String(timeInfo.tm_mon+1) + "," + String(timeInfo.tm_mday);
  String timeString = currentDate + "," +
         String(timeInfo.tm_hour) + "," + String(timeInfo.tm_min) + "," + String(timeInfo.tm_sec) + "," ;  

  if (_i2cRead == false) {
    delay(100);
    return;
  }
  _i2cRead = false;


  for(int i=0; i<4; i++) {
    ina219_S[i] = String(ina219Measurement.measurement[i].busvoltage*1000) + "," +
                  String(ina219Measurement.measurement[i].shuntvoltage,2) + "," +
                  String(ina219Measurement.measurement[i].current_mA,3) + "," +
                  String(ina219Measurement.measurement[i].power_mW);
  }
  String dataString = timeString+ina219_S[0]+"," + ina219_S[1]+","+
                                 ina219_S[2]+"," + ina219_S[3]      ;

  if (sdStatus == true) {
    count ++;
    if (count > 9) {
      count = 0;
      Serial.println("File lists");
      root = SD.open("/");
      listFiles(root, 0);
    }
    // print to the serial port too:
    Serial.println(dataString);

    printf("Open %s for writting\n", logFilename);
    file = SD.open(logFilename, FILE_APPEND) ;
    file.print(dataString);
    file.println();
    file.close();
  }

  for(int i=0; i<4; i++) {
    if (ina219Measurement.measurement[i].busvoltage > -9.9) {
      sprintf(buf, "%4d ", int(ina219Measurement.measurement[i].busvoltage*1000));
      tft.drawString(buf, 12, 18+i*18, 2);
      sprintf(buf, "%4d ", int(ina219Measurement.measurement[i].current_mA));
      tft.drawString(buf, 48, 18+i*18, 2);
  
      sprintf(buf, "%5d", int(ina219Measurement.measurement[i].power_mW));
      tft.drawString(buf, 84, 18+i*18, 2);
    } else {
      tft.drawString("N/A", 16, 18+i*18, 2);
      tft.drawString("N/A", 52, 18+i*18, 2);
      tft.drawString("N/A", 88, 18+i*18, 2);
    }
  }

  sprintf(buf, "%4d/%2d/%2d %2d:%02d:%02d", timeInfo.tm_year+1900, timeInfo.tm_mon+1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
  tft.drawString(buf, 0, 110, 1);


}




void listFiles(File dir, int numTabs) {
  while (true) {

    File entry =  dir.openNextFile();
    if (! entry) {
      // no more files
      break;
    }
    for (uint8_t i = 0; i < numTabs; i++) {
      Serial.print('\t');
    }
    Serial.print(entry.name());
    if (entry.isDirectory()) {
      Serial.println("/");
      listFiles(entry, numTabs + 1);
    } else {
      // files have sizes, directories do not
      Serial.print("\t\t");
      Serial.print(entry.size(), DEC);
      ////time_t cr = entry.getCreationTime();
      time_t lw = entry.getLastWrite();
      struct tm *tmstruct = localtime(&lw);
      Serial.printf("\tLAST WRITE: %d-%02d-%02d %02d:%02d:%02d\n", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
    }
    entry.close();
  }
}



void periodicI2C_Read(){

  getLocalTime(&ina219Measurement._timeInfo);
  
  if (_i2cRead == false) {
    if (ina219Status[0] == true) {
      ina219Measurement.measurement[0].shuntvoltage = ina219_0.getShuntVoltage_mV();
      ina219Measurement.measurement[0].busvoltage = ina219_0.getBusVoltage_V();
      ina219Measurement.measurement[0].current_mA = ina219_0.getCurrent_mA();
      ina219Measurement.measurement[0].power_mW = ina219_0.getPower_mW();
      ina219Measurement.measurement[0].loadvoltage = ina219Measurement.measurement[0].busvoltage + (ina219Measurement.measurement[0].shuntvoltage / 1000);
    } else {
      ina219Measurement.measurement[0].shuntvoltage = -9.999;
      ina219Measurement.measurement[0].busvoltage = -9.999;
      ina219Measurement.measurement[0].current_mA = -9.999;
      ina219Measurement.measurement[0].power_mW = -9.999;    
    }

    if (ina219Status[1] == true) {
      ina219Measurement.measurement[1].shuntvoltage = ina219_1.getShuntVoltage_mV();
      ina219Measurement.measurement[1].busvoltage = ina219_1.getBusVoltage_V();
      ina219Measurement.measurement[1].current_mA = ina219_1.getCurrent_mA();
      ina219Measurement.measurement[1].power_mW = ina219_1.getPower_mW();
      ina219Measurement.measurement[1].loadvoltage = ina219Measurement.measurement[1].busvoltage + (ina219Measurement.measurement[1].shuntvoltage / 1000);
    } else {
      ina219Measurement.measurement[1].shuntvoltage = -9.999;
      ina219Measurement.measurement[1].busvoltage = -9.999;
      ina219Measurement.measurement[1].current_mA = -9.999;
      ina219Measurement.measurement[1].power_mW = -9.999;    
    }

    if (ina219Status[2] == true) {
      ina219Measurement.measurement[2].shuntvoltage = ina219_2.getShuntVoltage_mV();
      ina219Measurement.measurement[2].busvoltage = ina219_2.getBusVoltage_V();
      ina219Measurement.measurement[2].current_mA = ina219_2.getCurrent_mA();
      ina219Measurement.measurement[2].power_mW = ina219_2.getPower_mW();
      ina219Measurement.measurement[2].loadvoltage = ina219Measurement.measurement[2].busvoltage + (ina219Measurement.measurement[2].shuntvoltage / 1000);
    } else {
      ina219Measurement.measurement[2].shuntvoltage = -9.999;
      ina219Measurement.measurement[2].busvoltage = -9.999;
      ina219Measurement.measurement[2].current_mA = -9.999;
      ina219Measurement.measurement[2].power_mW = -9.999;    
    }
    if (ina219Status[3] == true) {
      ina219Measurement.measurement[3].shuntvoltage = ina219_3.getShuntVoltage_mV();
      ina219Measurement.measurement[3].busvoltage = ina219_3.getBusVoltage_V();
      ina219Measurement.measurement[3].current_mA = ina219_3.getCurrent_mA();
      ina219Measurement.measurement[3].power_mW = ina219_3.getPower_mW();
      ina219Measurement.measurement[3].loadvoltage = ina219Measurement.measurement[3].busvoltage + (ina219Measurement.measurement[3].shuntvoltage / 1000);
    } else {
      ina219Measurement.measurement[3].shuntvoltage = -9.999;
      ina219Measurement.measurement[3].busvoltage = -9.999;
      ina219Measurement.measurement[3].current_mA = -9.999;
      ina219Measurement.measurement[3].power_mW = -9.999;    
    }
    _i2cRead = true;
  } else {
    Serial.println("I2C is busy");
  }
  
}
