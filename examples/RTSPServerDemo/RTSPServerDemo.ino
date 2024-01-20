#include "EasyRTSPServer.h"

char* ssid = "TP-LINK_212"; /* Input your WiFi ssid and password */
char* password = "212212212";
OV2640 cam;

EasyRTSPServer RTSPSetver(554);

void chipInfo() {
  int chipId = 0;
  // put your main code here, to run repeatedly:
  for (int i = 0; i < 17; i = i + 8) {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }

  Serial.printf("ESP32 Chip model = %s Rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("This chip has %d cores\n", ESP.getChipCores());
  Serial.print("Chip ID: ");
  Serial.println(chipId);
  Serial.printf("Psram Size = %d KB\n", ESP.getPsramSize() / 1024);
  Serial.printf("Flash Size = %d KB\n", ESP.getFlashChipSize() / 1024);
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  while (!Serial) {
    ;
  }
  chipInfo();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(500);
    Serial.print(F("."));
  }
  IPAddress ip = WiFi.localIP();
  Serial.println(F("WiFi connected"));
  Serial.println("");
  Serial.println(ip);

  cam.init(esp32cam_aithinker_config);  /* Select your ESP32 CAM module or input custom config in OV2640.cpp file */

  RTSPSetver.setStreamSuffix("mjpeg/1");
  RTSPSetver.setFrameRate(FRAMERATE_20HZ); /* 20Hz for 800x600 or lower (RTSP OVER UDP) */
  //RTSPSetver.setAuthAccount("Easy", "RTSPServer"); /* Uncomment the line to enable basic authentication*/
  RTSPSetver.init(&cam);
}

void loop() {
  RTSPSetver.run();
}
