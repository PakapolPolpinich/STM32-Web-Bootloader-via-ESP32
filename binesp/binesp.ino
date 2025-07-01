#include <WiFi.h>
#include <ESPAsyncWebServer.h>

#define MAX_BIN_SIZE 60 * 1024
uint8_t binBuf[MAX_BIN_SIZE];
size_t binLen = 0;

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);

  // ✅ 1. สร้าง WiFi Hotspot ด้วยชื่อ "ESP32_AP" และรหัสผ่าน "12345678"
  WiFi.softAP("ESP32_AP", "12345678");
  delay(100);  // รอให้ระบบสร้าง AP เสร็จ

  IPAddress IP = WiFi.softAPIP();
  Serial.print("[WiFi] SoftAP IP: http://");
  Serial.println(IP);  // ปกติคือ 192.168.4.1

  // ✅ 2. ให้หน้าเว็บ upload แสดงขึ้นที่ root "/"
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", R"rawliteral(
      <h2>Upload .bin to ESP32</h2>
      <form method="POST" action="/upload" enctype="multipart/form-data">
        <input type="file" name="firmware">
        <input type="submit" value="Upload">
      </form>
    )rawliteral");
  });

  // ✅ 3. รับข้อมูล .bin และเก็บไว้ใน RAM
  server.on(
    "/upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      request->send(200, "text/plain", "Upload Complete!");
      Serial.printf("[UPLOAD] DONE: %u bytes\n", binLen);
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (index == 0) {
        binLen = 0;
        Serial.println("[UPLOAD] START");
      }

      if (binLen + len <= MAX_BIN_SIZE) {
        memcpy(&binBuf[binLen], data, len);
        binLen += len;
      } else {
        Serial.println("❌ Buffer Overflow");
      }

      if (final) {
        Serial.printf("[UPLOAD] File: %s, Size: %u bytes\n", filename.c_str(), binLen);
        Serial.println("[DEBUG] Dump full binBuf[]:");
        for (size_t i = 0; i < binLen; i++) {
          Serial.printf("%02X ", binBuf[i]);
          if ((i + 1) % 16 == 0) Serial.println();
        }
        if (binLen % 16 != 0) Serial.println();
      }
    }
  );

  // ✅ 4. เริ่มต้น Web Server
  server.begin();
  Serial.println("[Server] Started");
}

void loop() {
  // ไม่ต้องทำอะไรใน loop เพราะ WebServer เป็น async
}
