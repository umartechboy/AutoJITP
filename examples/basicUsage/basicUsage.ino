#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <detail/mimetable.h>
#include <Helpers/general.h>
#include <Preferences.h>
#include <TaskScheduler.h>
#include <Thread.h>
#include <AutoJITP.h>
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

  // autoJitp.DebugStream = &Serial;
  // autoJitp.OnDeviceProvisioningStarted = [](){ Serial.println("TL: Online Started"); };
  autoJitp.OnDeviceProvisioningFailed = [](ProvisionStatus status){ Serial.printf("TL: Failed: %d\n", status); };
  autoJitp.OnProvisioned = [](MQTTClient& client, String& deviceName){ Serial.printf("TL: Got MQTT Client: \n"); };
  autoJitp.OnDeviceProvisioningProgress = [](int progress){ Serial.printf("TL: progress: %d%\n", progress); };

  autoJitp.GetProvisionAsync();

  Serial.println("Entering loop");
}

void loop() {
}
