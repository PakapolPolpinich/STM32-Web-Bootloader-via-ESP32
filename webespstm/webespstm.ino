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

typedef enum {
  WAIT,
  INIT,
  ERASE,
  WRITE,
  RESET_STM32
} BootState;

HardwareSerial UARTSerial(1);
AsyncWebServer server(80);

BootState State = WAIT;
bool processStarted = false;

String currentStatus = "Check connection";
bool   statusChanged = true;

inline void updateStatus(const char *s) {
  currentStatus = s;
  statusChanged = true;
}

typedef enum{
  UPLOAD_H,
  ERASE_H
}RECEIVE_POST;

RECEIVE_POST PostState = UPLOAD_H;

void setup() {
  pinMode(CTR_RESET_PIN, OUTPUT);
  pinMode(CTR_BOOT_0, OUTPUT);

  digitalWrite(CTR_RESET_PIN, HIGH);
  digitalWrite(CTR_BOOT_0, LOW);

  UARTSerial.begin(9600, SERIAL_8E1, UART_RX, UART_TX);
  Serial.begin(115200);
  delay(100);

  Serial.println("[SYS] Bootloader Controller Ready.");

  // Create WiFi Hotspot
  WiFi.softAP("ESP32_AP", "12345678");
  delay(100);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("[WiFi] SoftAP IP: http://");
  Serial.println(IP);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html",uploadFormHTML());
  });

  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *request) { /*on request when web upload done*/
      request->send(200, "text/plain", "Upload Complete!");
      Serial.printf("[UPLOAD] DONE: %u bytes\n", binLen);
    },/*call when receive chunk*/
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
        PostState = UPLOAD_H;
      }
    }
  );
  server.on("/erase", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      binLen = 0;
      memset(binBuf, 0xFF, MAX_BIN_SIZE);
      processStarted = true;
      State = INIT;
      PostState = ERASE_H;
      request->send(200, "text/plain", "Flash buffer erased!");
      Serial.println("[ERASE] binBuf cleared");
    }
  );

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *req){
  if (statusChanged) {
    statusChanged = false;
    req->send(200, "application/json", "{\"state\":\"" + currentStatus + "\"}");
  } else {
    req->send(204); // No Content
  }
  });

  server.begin();
  Serial.println("[Server] Started");
  updateStatus("Pending");
}

void loop() {

 if (processStarted) {
    switch (State) {
      case WAIT:
        Serial.println("Returning to WAIT...");
        //updateStatus("Pending");
        processStarted = false;
        break;

      case INIT:
        updateStatus("Connect->Bootloader");
        init();
        break;

      case ERASE:
        erase();
        break;

      case WRITE:
        write();
        break;
      case RESET_STM32:
        ResetStm32();
        break;  

      default:
        processStarted = false;
        break;
    }     
  }
}

void init() {
  Serial.println("[INIT] Entering STM32 bootloader UART mode...");

  digitalWrite(CTR_BOOT_0, HIGH);
  delay(10);
  digitalWrite(CTR_RESET_PIN, LOW);
  delay(50);
  digitalWrite(CTR_RESET_PIN, HIGH);
  delay(100);

  while (UARTSerial.available()) UARTSerial.read();
  UARTSerial.write(0x7F);

  if (ACKpolling(1000)) {
    Serial.println("[INIT] Connect Bootloader UART mode");
    updateStatus("Connect->bootloader");
    State = ERASE;
  } else {
    Serial.println("[INIT] Failed. Returning to WAIT.");
    updateStatus("Fail to connect bootloader ");
    State = WAIT;
  }
}

void erase() {
  Serial.println("[ERASE] Sending Extended Erase command...");
  updateStatus("Connect->Erase Progress");
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
    if(PostState == UPLOAD_H){
      State = WRITE;
    }else if (PostState == ERASE_H){
      State = RESET_STM32;
    }
     else {
    Serial.println("[ERASE] Erase NACK");
     updateStatus("Fail erase");
    State = WAIT;
    }
    updateStatus("Connect->Erase done");
  }
}

void write() {
  Serial.println("[WRITE] Sending .bin to STM32...");
  updateStatus("Connect->Write Progress");
  uint32_t base_addr = 0x08000000;
  const uint8_t block_size = 0xFF; /*add 256 byte 0 -255*/

  for (size_t offset = 0; offset < binLen; offset += block_size) {
    size_t chunkLen = min((size_t)block_size, binLen - offset);
    uint32_t addr = base_addr + offset;

    if (!writeMemory(addr, &binBuf[offset], chunkLen)) {
      Serial.printf("[WRITE] Failed at offset 0x%X\n", offset);
      State = WAIT;
      updateStatus("Fail write");
      return;
    }
  }
  updateStatus("Connect: Upload Complete");
  Serial.println("[WRITE] Upload completed. Resetting STM32...");
  State = RESET_STM32;
}

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

  uint8_t block[1 + 256 + 1]; /*number,data,checksum*/
  block[0] = len - 1; /*protocol number - 1*/
  memcpy(&block[1], data, len);

  uint8_t cksum = block[0];
  for (size_t i = 0; i < len; i++) {
    cksum ^= data[i];
  }
  block[1 + len] = cksum;
  
  UARTSerial.write(block, len + 2);/*data , round to send*/

  Serial.printf("[WRITE] at address: 0x%X\n", address);

  return ACKpolling(5000);
}

void ResetStm32(){
  digitalWrite(CTR_BOOT_0, LOW);
  digitalWrite(CTR_RESET_PIN, LOW);
  delay(50);
  digitalWrite(CTR_RESET_PIN, HIGH);
  delay(100);
  State = WAIT;
}


uint8_t ACKpolling(uint32_t timeout_ms){
    uint8_t  response = 0U;
    uint8_t  result   = 0U;
    uint32_t start    = millis();
    uint32_t now      = 0U;

    while (UARTSerial.available() == 0)
    {
        now = millis();
        if ((now - start) > timeout_ms)
        {
            Serial.println("[ACK] Timeout waiting for response");
            result = 0U;
            break;
        }
    }

    if (UARTSerial.available() > 0)
    {
        response = (uint8_t)UARTSerial.read();

        if (response == ACK)
        {
            //Serial.println("[ACK] 0x79 received");
            result = 1U;
        }
        else if (response == NACK)
        {
            Serial.println("[ACK] 0x1F received");
            result = 0U;
        }
        else
        {
            Serial.print("[ACK] Unknown byte: 0x");
            if (response < 0x10U)
            {
                Serial.print("0");
            }
            Serial.println(response, HEX);
            result = 0U;
        }
    }

    return result;
}

// const char* uploadFormHTML() {
//   return R"rawliteral(
//     <h2>Upload .bin to ESP32</h2>
//     <form method="POST" action="/upload" enctype="multipart/form-data">
//       <input type="file" name="firmware">
//       <input type="submit" value="Upload">
//     </form>
//   )rawliteral";
// }

// const char* uploadFormHTML() {
//   return R"rawliteral(
// <!DOCTYPE html>
// <html>
// <head>
//   <meta charset="UTF-8">
//   <title>ESP32 OTA Upload</title>
//   <style>
//     .bar{
//       display:inline-block;
//       width:260px;
//       padding:6px;
//       background:#d9d9d9;
//       border-radius:4px
//     }
//     .btn{
//       padding:8px 28px;
//       border:0;
//       border-radius:4px;
//       font-weight:bold;
//       cursor:pointer
//     }
//     .upload {
//       background:#4adbd9;
//       color:#fff
//     }
//     .erase {
//       background:#f23c0f;
//       color:#fff
//     }
//   </style>
// </head>
// <body>
//   <h2>Upload .bin ESP32</h2>
//    <!-- show path -->
//   <div id="path" class="bar">Path</div>
//   <input id="file" type="file" name="firmware" onchange="showName(this)">
//   <br><br>

//   <!-- button when push then link to javascipt -->
//   <button class="btn upload" onclick="sendUpload()">Upload</button>
//   <button class="btn erase" onclick="sendErase()">ERASE</button>

//   <script>
//     // change name path
//     function showName(input){
//       const bar = document.getElementById('path');
//       bar.textContent = input.files.length ? input.files[0].name : 'Path';
//     }
//     // send file to esp32 path / upload
//     async function sendUpload(){
//       const file = document.getElementById('file').files[0];
//       if(!file){ alert('choose file please'); return; }

//       const fd = new FormData();
//       fd.append('firmware', file);

//       const r = await fetch('/upload', {method:'POST', body:fd});
//       alert(await r.text());
//     }
//     // send file to esp32 path /erase
//     async function sendErase(){
//       const r = await fetch('/erase', {method:'POST'});
//       alert(await r.text());
//     }
//   </script>
// </body>
// </html>
// )rawliteral";
// }
const char* uploadFormHTML() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>ESP32 OTA Upload</title>
  <style>
    .bar{
      display:inline-block;
      width:260px;
      padding:6px;
      background:#d9d9d9;
      border-radius:4px
    }
    .btn{
      padding:8px 28px;
      border:0;
      border-radius:4px;
      font-weight:bold;
      cursor:pointer
    }
    .upload {
      background:#4adbd9;
      color:#fff
    }
    .erase {
      background:#f23c0f;
      color:#fff
    }
  </style>
</head>
<body>
  <h2>Upload .bin ESP32</h2>

  <!-- show path -->
  <div id="path" class="bar">Path</div>
  <input id="file" type="file" name="firmware" onchange="showName(this)">
  <br><br>

  <!-- buttons -->
  <button class="btn upload" onclick="sendUpload()">Upload</button>
  <button class="btn erase" onclick="sendErase()">ERASE</button>

  <!-- state monitor -->
  <div id="status" class="bar" style="margin-top:10px">State: WAIT</div>

  <script>
    function showName(input){
      const bar = document.getElementById('path');
      bar.textContent = input.files.length ? input.files[0].name : 'Path';
    }

    async function sendUpload(){//
      const file = document.getElementById('file').files[0];
      if(!file){ alert('Choose file please'); return; }

      const fd = new FormData();
      fd.append('firmware', file);
      await fetch('/upload', {method:'POST', body:fd});
    }

    async function sendErase(){
      await fetch('/erase', {method:'POST'});
    }

    async function poll(){
      try {
        const r = await fetch('/status');
        if (r.status === 200) {
          const j = await r.json();
          document.getElementById('status').textContent = 'State: ' + j.state;
        }
      } catch (e) {
        document.getElementById('status').textContent = 'ERR';
      } finally {
        setTimeout(poll, 50);  // poll again every 150ms
      }
    }
    poll();
  </script>
</body>
</html>
)rawliteral";
}
