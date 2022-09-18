#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <detail/mimetable.h>
#include <Helpers/general.h>
#include <Preferences.h>
#include <TaskScheduler.h>
#include <Thread.h>
#include "JITP/JITP.h"
#include "Helpers/Web.h"

void setup() {
  Serial.begin(115200);

  LittleFS.begin(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin("SSID", "PASSWORD");

  while(!WiFi.isConnected()) {
    Serial.print(".");
    delay(1000);
  }
  
  Serial.println("");

  //jitp.DebugStream = &Serial;
  // jitp.OnDeviceProvisioningStarted = [](){ Serial.println("TL: Online Started"); };
  jitp.OnDeviceProvisioningFailed = [](ProvisionStatus status){ Serial.printf("TL: Failed: %d\n", status); };
  jitp.OnProvisioned = [](MQTTClient& client){ Serial.printf("TL: Got MQTT Client: \n"); };
  jitp.OnDeviceProvisioningProgress = [](int progress){ Serial.printf("TL: progress: %d%\n", progress); };

  jitp.GetProvisionAsync();

  Serial.println("Entering loop");
}

void loop() {
}
