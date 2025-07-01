#include <WiFi.h>
#include <ESPAsyncWebServer.h>

#define MAX_BIN_SIZE 60 * 1024
uint8_t binBuf[MAX_BIN_SIZE];
size_t binLen = 0;

// STM32 Bootloader control pins
#define CTR_RESET_PIN 18
#define CTR_BOOT_0    19
#define UART_TX       17
#define UART_RX       16

#define ACK  0x79
#define NACK 0x1F

HardwareSerial UARTSerial(1);
AsyncWebServer server(80);

bool processStarted = false;

typedef enum {
  WAIT,
  INIT,
  ERASE,
  WRITE
} BootState;

BootState State = WAIT;
uint8_t flag = 0;
// ======================= SETUP =======================
void setup() {
  pinMode(CTR_RESET_PIN, OUTPUT);
  pinMode(CTR_BOOT_0, OUTPUT);

  digitalWrite(CTR_RESET_PIN, HIGH);
  digitalWrite(CTR_BOOT_0, LOW);

  UARTSerial.begin(9600, SERIAL_8E1, UART_RX, UART_TX);
  Serial.begin(115200);
  delay(500);

  Serial.println("[SYS] Bootloader Controller Ready.");

  // Create WiFi Hotspot
  WiFi.softAP("ESP32_AP", "12345678");
  delay(100);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("[WiFi] SoftAP IP: http://");
  Serial.println(IP);

  // Serve Upload Page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", R"rawliteral(
      <h2>Upload .bin to ESP32</h2>
      <form method="POST" action="/upload" enctype="multipart/form-data">
        <input type="file" name="firmware">
        <input type="submit" value="Upload">
      </form>
    )rawliteral");
  });

  // Handle File Upload
  server.on(
    "/upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      request->send(200, "text/plain", "Upload Complete!");
      Serial.printf("[UPLOAD] DONE: %u bytes\n", binLen);
      //flag == true;
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
        Serial.println("[UPLOAD] Triggering bootloader...");

        processStarted = true;
        State = INIT;
      }
    });

  server.begin();
  Serial.println("[Server] Started");
}

// ======================= LOOP =======================
void loop() {
  if (processStarted) {
    switch (State) {
      case WAIT:
        Serial.println("[*] Returning to WAIT...");
        processStarted = false;
        flag = false;
        break;

      case INIT:
        init();
        break;

      case ERASE:
        erase();
        break;

      case WRITE:
        write();
        break;

      default:
        break;
    }
     
  }
}

// ======================= BOOTLOADER STATE FUNCTIONS =======================

void init() {
  Serial.println("[INIT] Entering STM32 bootloader mode...");

  digitalWrite(CTR_BOOT_0, HIGH);
  delay(10);
  digitalWrite(CTR_RESET_PIN, LOW);
  delay(50);
  digitalWrite(CTR_RESET_PIN, HIGH);
  delay(100);

  UARTSerial.write(0x7F);

  if (ACKpolling(1000)) {
    State = ERASE;
  } else {
    Serial.println("[INIT] Failed. Returning to WAIT.");
    State = WAIT;
  }
}

void erase() {
  Serial.println("[ERASE] Sending Extended Erase command...");

  UARTSerial.write(0x44);
  UARTSerial.write(0xBB);

  if (!ACKpolling(1000)) {
    Serial.println("[ERASE] CMD NACK");
    State = WAIT;
    return;
  }

  UARTSerial.write(0xFF); // MSB
  UARTSerial.write(0xFF); // LSB
  UARTSerial.write(0x00); // XOR

  if (ACKpolling(10000)) {
    Serial.println("[ERASE] Done Erase");
    State = WRITE;
  } else {
    Serial.println("[ERASE] Erase NACK");
    State = WAIT;
  }
}

void write() {
  Serial.println("[WRITE] Sending .bin to STM32...");

  uint32_t base_addr = 0x08000000;
  const uint8_t block_size = 0xFF;

  for (size_t offset = 0; offset < binLen; offset += block_size) {
    size_t chunkLen = min((size_t)block_size, binLen - offset);
    uint32_t addr = base_addr + offset;

    if (!writeMemory(addr, &binBuf[offset], chunkLen)) {
      Serial.printf("[WRITE] Failed at offset 0x%X\n", offset);
      State = WAIT;
      return;
    }
  }

  Serial.println("[WRITE] Upload completed. Resetting STM32...");
  digitalWrite(CTR_BOOT_0, LOW);
  digitalWrite(CTR_RESET_PIN, LOW);
  delay(50);
  digitalWrite(CTR_RESET_PIN, HIGH);
  delay(100);
  State = WAIT;
}

// ======================= STM32 COMMAND HELPERS =======================

bool writeMemory(uint32_t address, uint8_t *data, size_t len) {
  UARTSerial.write(0x31);
  UARTSerial.write(0xCE);
  if (!ACKpolling(1000)) return false;

  uint8_t addr_buf[5];
  addr_buf[0] = (address >> 24) & 0xFF;
  addr_buf[1] = (address >> 16) & 0xFF;
  addr_buf[2] = (address >> 8)  & 0xFF;
  addr_buf[3] = (address)       & 0xFF;
  addr_buf[4] = addr_buf[0] ^ addr_buf[1] ^ addr_buf[2] ^ addr_buf[3];

  UARTSerial.write(addr_buf, 5);
  if (!ACKpolling(1000)) return false;

  uint8_t block[1 + 256 + 1];
  block[0] = len - 1;
  memcpy(&block[1], data, len);

  uint8_t cksum = block[0];
  for (size_t i = 0; i < len; i++) {
    cksum ^= data[i];
  }
  block[1 + len] = cksum;

  UARTSerial.write(block, len + 2);
  return ACKpolling(3000);
}

uint8_t ACKpolling(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (UARTSerial.available() == 0) {
    if (millis() - start > timeout_ms) {
      Serial.println("[ACK] Timeout waiting for response");
      return 0;
    }
  }

  uint8_t response = UARTSerial.read();

  if (response == ACK) {
    Serial.println("[ACK] 0x79 received");
    return 1;
  } else if (response == NACK) {
    Serial.println("[ACK] 0x1F received");
    return 0;
  } else {
    Serial.printf("[ACK] Unknown byte: 0x%02X\n", response);
    return 0;
  }
}