#include <WiFi.h>
#include <HTTPClient.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_TSC2007.h>
#include <Preferences.h>
#include <time.h>

// Starting globals:
unsigned long lastTouchTime = 0;
unsigned long lastClockMoveTime = 0;

// Create the touchscreen object
Adafruit_TSC2007 touch = Adafruit_TSC2007();

// Calibration values for mapping raw sensor data to pixel data
// You may need to tweak these numbers slightly depending on your exact panel
#define TS_MINX 300
#define TS_MAXX 3800
#define TS_MINY 200
#define TS_MAXY 3700

// NTP  early Setup
const char* ntpServer = "pool.ntp.org"; const char* timeZone   = "EST5EDT,M3.2.0,M11.1.0"; 

#ifdef ESP8266
  #define STMPE_CS 16
  #define TFT_CS   0
  #define TFT_DC   15
  #define SD_CS    2
#elif defined(ARDUINO_ADAFRUIT_FEATHER_ESP32C6)
  #define STMPE_CS 6
  #define TFT_CS   7
  #define TFT_DC   8
  #define SD_CS    5
#elif defined(ARDUINO_ADAFRUIT_FEATHER_ESP32S2) || \
      defined(ARDUINO_ADAFRUIT_FEATHER_ESP32S2_REVTFT) || \
      defined(ARDUINO_ADAFRUIT_FEATHER_ESP32S2_TFT) || \
      defined(ARDUINO_ADAFRUIT_FEATHER_ESP32S3) || \
      defined(ARDUINO_ADAFRUIT_FEATHER_ESP32S3_NOPSRAM) || \
      defined(ARDUINO_ADAFRUIT_FEATHER_ESP32S3_REVTFT) || \
      defined(ARDUINO_ADAFRUIT_FEATHER_ESP32S3_TFT)
  #define STMPE_CS 6
  #define TFT_CS   9
  #define TFT_DC   10
  #define SD_CS    5
#elif defined(ESP32)
  #define STMPE_CS 32
  #define TFT_CS   15
  #define TFT_DC   33
  #define SD_CS    14
#elif defined(TEENSYDUINO)
  #define TFT_DC   10
  #define TFT_CS   4
  #define STMPE_CS 3
  #define SD_CS    8
#elif defined(ARDUINO_STM32_FEATHER)
  #define TFT_DC   PB4
  #define TFT_CS   PA15
  #define STMPE_CS PC7
  #define SD_CS    PC5
#elif defined(ARDUINO_NRF52832_FEATHER) /* BSP 0.6.5 and higher! */
  #define TFT_DC   11
  #define TFT_CS   31
  #define STMPE_CS 30
  #define SD_CS    27
#elif defined(ARDUINO_MAX32620FTHR) || defined(ARDUINO_MAX32630FTHR)
  #define TFT_DC   P5_4
  #define TFT_CS   P5_3
  #define STMPE_CS P3_3
  #define SD_CS    P3_2
#else // Anything else, defaults!
  #define STMPE_CS 6
  #define TFT_CS   9
  #define TFT_DC   10
  #define SD_CS    5
#endif

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC);
int level = 0;
Preferences preferences;

// Instantiate Portal Objects globally so they are available inside the function
DNSServer dnsServer;
WebServer server(80);

// Forward declaration so setup() knows this function exists below it
void launchSetupPortal(); 
unsigned long testFillScreen();

// Simple HTML page hosted on the ESP32
const char PORTAL_HTML[] PROGMEM = R"===(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1">
<style>body{font-family:Arial;margin:30px;text-align:center;} input{width:100%;padding:10px;margin:10px 0;}</style>
</head><body><h2>Turret32 Wi-Fi Setup</h2>
<form action="/save" method="POST">
<input type="text" name="ssid" placeholder="Wi-Fi Name (SSID)" required>
<input type="password" name="password" placeholder="Password" required>
<input type="submit" value="Save and Connect"></form></body></html>
)===";


void setup() {
  tft.begin();
  delay(100);
  tft.setRotation(1);
  Serial.begin(115200);
  delay(500);

  // --- Splash Screen ---
  testFillScreen();
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(3);
  tft.setCursor(10, 100);
  tft.println("CMS Turret32");
  tft.setTextSize(2);
  tft.setCursor(10, 140);
  tft.println("Booting...");

  if (!touch.begin(0x48, &Wire)) {
    Serial.println("Couldn't find TSC2007 touch controller!");
    while (1); // Halt if touch hardware is disconnected
  }
  Serial.println("TSC2007 Touch Controller Found!");


  loading("Reading memory...");


  // Open preferences in READ-ONLY mode (true)
  preferences.begin("wifi", true);
  String stored_ssid = preferences.getString("ssid", "");
  String stored_password = preferences.getString("password", "");
  preferences.end();

  if (stored_ssid == "") {
    Serial.println("No saved SSID found, initiating wifi setup...");
    launchSetupPortal(); // <-- CALL 1: No stored details
  } else {
    Serial.println("Stored SSID found, scanning for networks...");
    loading("Scanning for networks...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int network_count = WiFi.scanNetworks();
    bool network_found = false;

    if (network_count == 0) {
      Serial.println("No networks found.");
    } else {
      Serial.print(network_count);
      Serial.println(" networks found.");
      loading("Reading network list...");
      for (int i = 0; i < network_count; ++i) {
        if (WiFi.SSID(i) == stored_ssid) {
          network_found = true; // <-- FIXED: Added this so the boolean actually updates!
          break;
        }
      }
    }
    WiFi.scanDelete();

    if (network_found) {
      Serial.println("Attempting to connect to found, stored network");
      loading("Connecting to network...");
      WiFi.begin(stored_ssid.c_str(), stored_password.c_str());


      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 50) {
        delay(500);
        Serial.print(".");
        attempts++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

        // Get NTP time
        configTzTime(timeZone, ntpServer);
        loading("Syncing NTP...");
        Serial.print("Syncing time via NTP");
        struct tm timeinfo;
        int retryCount = 0;
        while(!getLocalTime(&timeinfo) && retryCount < 20) {
          Serial.print(".");
          delay(500);
          retryCount++;
        }
        Serial.println("\nTime Synced");

      } else {
        Serial.println("\nA saved network was found but we failed to connect.");
        launchSetupPortal(); // <-- CALL 2: Password changed or bad connection
      }
    } else {
      Serial.println("Did not find the stored network. Initiating wifi setup...");
      launchSetupPortal(); // <-- CALL 3: Stored network out of range
    }
  }

  lastTouchTime = millis();
  lastClockMoveTime = millis();
  
  delay(1000); // show status for 1 second before looping
  testFillScreen();
}

int lastSecond = -1;
int clockOffsetX = 0;
int clockOffsetY = 0;
int swipeStartX = -1;
int swipeStartY = -1;
int lastTouchX = 0;
int lastTouchY = 0;

void loop(void) {
  // Capture the touch coordinates
  TS_Point touch = checkTouchInput();

  if (millis() - lastTouchTime > 10800000) { //10800000 for 3 hours
    goToSleep();
  }

  if (millis() - lastClockMoveTime > 1800000) { //1800000 for 30 minutes
    testFillScreen();
    clockOffsetX = random(-50, 50);
    clockOffsetY = random(-50, 50);
    lastClockMoveTime = millis();
  }

  if (touch.z > 0) {
    lastTouchX = touch.x;
    lastTouchY = touch.y;
  }

  if (touch.z > 0 && swipeStartX == -1) {
      swipeStartX = touch.x;
      swipeStartY = touch.y;
      lastTouchTime = millis();
  }

  // Touch ended (finger lifted)
  if (touch.z == 0 && swipeStartX != -1) {
      int dx = lastTouchX - swipeStartX;
      int dy = lastTouchY - swipeStartY;

      // Horizontal swipe detection
      if (abs(dx) > 60 && abs(dx) > abs(dy)) {
          if (dx > 0) {
              // Swipe RIGHT
              level--;
          } else {
              // Swipe LEFT
              level++;
          }
      }

      // Reset swipe tracking
      swipeStartX = -1;
      swipeStartY = -1;
  }



  switch (level) {
    case 0:
      // Clock initialization script
      tft.fillScreen(ILI9341_BLACK); 
      tft.setTextColor(ILI9341_WHITE); 
      tft.setTextSize(4);
      tft.setCursor(100, 100);
      configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org");
      level = 1;
      break;
    case 1: {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        
        // Only update the display if the second has actually changed
        if (timeinfo.tm_sec != lastSecond) {
          lastSecond = timeinfo.tm_sec; // Update our tracker
          
          // Clear just the text area to prevent flickering (instead of full screen wipe)
          tft.fillRect(70 + clockOffsetX, 100 + clockOffsetY, 200, 30, ILI9341_BLACK); 
          
          // Draw the new time
          tft.setCursor(70 + clockOffsetX, 100 + clockOffsetY);
          tft.printf("%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        }
      }
      break;
    } case 2: {
      tft.fillScreen(ILI9341_BLACK);
      tft.setCursor(25, 25);
      tft.printf("Syncing...");
      configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org");
      level = 0;
      break;
    }
    default:
      Serial.println("Error in switch case code -> Invalid input");
      tft.fillScreen(ILI9341_BLACK);
      tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(2);
      tft.setCursor(30, 30);
      tft.print("Error in switch case code -> Invalid input");
      delay(5000);
      break;
  }
}
// Custom Provisioning Function
void launchSetupPortal() {
  // Update your display to alert the user
  tft.fillScreen(ILI9341_BLUE);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 50);
  tft.println("Wi-Fi Setup Needed");
  tft.setCursor(10, 90);
  tft.println("Connect to Access Point:");
  tft.setTextColor(ILI9341_YELLOW);
  tft.println("  Turret32-Setup");

  WiFi.mode(WIFI_AP);
  WiFi.softAP("Turret32-Setup");
  delay(500);

  // Direct all traffic to our ESP32's Access Point IP
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Serve root page
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", PORTAL_HTML);
  });

  // Handle Form Post
  server.on("/save", HTTP_POST, []() {
    if (server.hasArg("ssid") && server.hasArg("password")) {
      String new_ssid = server.arg("ssid");
      String new_password = server.arg("password");

      // Open preferences in READ/WRITE mode (false) to overwrite
      preferences.begin("wifi", false);
      preferences.putString("ssid", new_ssid);
      preferences.putString("password", new_password);
      preferences.end();

      server.send(200, "text/html", "<html><body><h1>Saved!</h1><p>Rebooting...</p></body></html>");
      delay(2000);
      ESP.restart(); // Reboot device to restart back into setup()
    }
  });

  // Captive Portal Redirect Catch-all
  server.onNotFound([]() {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", ""); 
  });

  server.begin();

  // Infinite processing loop. It hangs here until the user submits details and the MCU reboots.
  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(2); 
  }
}

void loading(String text) {
  tft.setCursor(10, 210);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_CYAN);
  tft.fillRect(0, 210, 400, 80, ILI9341_BLACK); 
  tft.println(text);
}

void goToSleep() {
  // Turn off TFT
  tft.fillRect(0, 0, tft.width(), tft.height(), ILI9341_BLACK);
  tft.writeCommand(ILI9341_DISPOFF);
  tft.writeCommand(ILI9341_SLPIN);

  // Enable wake on TSC2007 touch
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_32, 0);
  delay(500);
  esp_light_sleep_start();

  tft.writeCommand(ILI9341_SLPOUT);
  delay(120);
  tft.writeCommand(ILI9341_DISPON);
  Serial.println("Waking up here.");

  lastTouchTime = millis();
}


unsigned long testFillScreen() {
  unsigned long start = micros();
  tft.fillScreen(ILI9341_BLACK); yield(); delay(50);
  tft.fillScreen(ILI9341_RED); yield(); delay(50);
  tft.fillScreen(ILI9341_GREEN); yield(); delay(50);
  tft.fillScreen(ILI9341_BLUE); yield(); delay(50);
  tft.fillScreen(ILI9341_BLACK); yield(); delay(50);
  return micros() - start;
}

TS_Point checkTouchInput() {
  uint16_t x, y, z1, z2;
  touch.read_touch(&x, &y, &z1, &z2);
  
  if (z1 > 5 && z1 < 5000) { 
    int pixel_x = map(y, 200, 3700, 0, tft.width());
    int pixel_y = map(x, 3800, 300, 0, tft.height());
    
    pixel_x = constrain(pixel_x, 0, tft.width());
    pixel_y = constrain(pixel_y, 0, tft.height());

    // SPIKE FILTER
    if (pixel_x >= 320 || pixel_x <= 0 || pixel_y >= 240 || pixel_y <= 0) {
      return TS_Point(0, 0, 0); 
    }

    delay(30); // Debounce
    return TS_Point(pixel_x, pixel_y, z1); // Return the active point
  }
  
  return TS_Point(0, 0, 0); // Return empty point if no touch
}