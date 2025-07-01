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
AsyncWebSocket ws("/ws");

BootState State = WAIT;
bool processStarted = false;

String currentStatus = "Check connection";
bool   statusChanged = true;

int progressPct   = 0;

typedef enum{
  UPLOAD_H,
  ERASE_H
}RECEIVE_POST;

RECEIVE_POST PostState = UPLOAD_H;

void setup() {
  Setup_Board();
  Setup_WIFI();
  Setup_Server();
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

/*////////////////////////////////////////////////////////////////////////////////////////////////////*/

/*State Machine*/

/*////////////////////////////////////////////////////////////////////////////////////////////////////*/
void init() {
  Serial.println("[INIT] Entering STM32 bootloader UART mode...");
  setProgress("Enter STM32 bootloader UART mode",0);

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
    setProgress("Connect->bootloader",0);
    State = ERASE;
  } else {
    Serial.println("[INIT] Failed. Returning to WAIT.");
    setProgress("Fail to connect bootloader",0);
    State = WAIT;
  }
}

void erase() {
  Serial.println("[ERASE] Sending Extended Erase command...");
  setProgress("Connect->Erase Progress",0);
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
  uint32_t t0 = millis();
  while (!ACKpolling(100)) {
    int pct = min((int)((millis() - t0) / 100), 99);
    setProgress("ERASING", pct);

    if (millis() - t0 > 10000) {
      setProgress("ERASE_TIMEOUT",pct); 
      State = WAIT; 
      return;
    }
  }
  setProgress("ERASING", 100);

  Serial.println("[ERASE] Done Erase");
  if(PostState == UPLOAD_H){
    State = WRITE;
  }else if (PostState == ERASE_H){
    State = RESET_STM32;
  }else {
    Serial.println("[ERASE] Erase NACK");
    State = WAIT;
  }
}

void write() {
  Serial.println("[WRITE] Sending .bin to STM32...");
  uint32_t base_addr = 0x08000000;
  const uint8_t block_size = 0xFF; /*add 256 byte 0 -255*/

  for (size_t offset = 0; offset < binLen; offset += block_size) {
    size_t chunkLen = min((size_t)block_size, binLen - offset);
    uint32_t addr = base_addr + offset;
    int pct = (offset * 100) / binLen;
    setProgress("WRITING Progress",pct);
    if (!writeMemory(addr, &binBuf[offset], chunkLen)) {
      Serial.printf("[WRITE] Failed at offset 0x%X\n", offset);
      State = WAIT;
      setProgress("WRITING Fail",pct);
      return;
    }  
  }
  setProgress("WRITING DONE",100);
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
  setProgress("DONE",100);
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
            //Serial.println("[ACK] Timeout waiting for response");
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










/*////////////////////////////////////////////////////////////////////////////////////////////////////*/


/*Setup*/


/*////////////////////////////////////////////////////////////////////////////////////////////////////*/
void wsBroadcast() {
  String json = String("{\"state\":\"") + currentStatus + "\",\"progress\":" + String(progressPct) + "}";
  ws.textAll(json);
}

inline void setProgress(const char *s,int pct) {
  currentStatus = s;
  progressPct = pct;
  wsBroadcast();
}

void Setup_Board(){
  pinMode(CTR_RESET_PIN, OUTPUT);
  pinMode(CTR_BOOT_0, OUTPUT);
  digitalWrite(CTR_RESET_PIN, HIGH);
  digitalWrite(CTR_BOOT_0, LOW);
  UARTSerial.begin(9600, SERIAL_8E1, UART_RX, UART_TX);
  Serial.begin(115200);
  delay(100);
  Serial.println("[SYS] Bootloader Controller Ready.");
}

void Setup_WIFI(){
  // Create WiFi Hotspot
  WiFi.softAP("ESP32_AP", "12345678");
  delay(100);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("[WiFi] SoftAP IP: http://");
  Serial.println(IP);
}

void Setup_Server(){

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

  ws.onEvent([](AsyncWebSocket*,AsyncWebSocketClient*,AwsEventType,void*,uint8_t*,size_t){});
  server.addHandler(&ws);
  server.begin();
  Serial.println("[Server] Started");
  setProgress("Pending",0);
}




/*////////////////////////////////////////////////////////////////////////////////////////////////////*/

/*HTML + CSS + JAVASCRIPT*/

/*////////////////////////////////////////////////////////////////////////////////////////////////////*/

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
//     #progress {
//       width: 100%;
//       background: #ddd;
//       border-radius: 6px;
//       margin-top: 10px;
//     }
//     #bar {
//       width: 0%;
//       background: #4adbd9;
//       color: #fff;
//       border-radius: 6px;
//       padding: 4px 0;
//     }


//   </style>
// </head>
// <body>
//   <h2>Upload .bin ESP32</h2>

//   <!-- show path -->
//   <div id="path" class="bar">Path</div>
//   <input id="file" type="file" name="firmware" onchange="showName(this)">
//   <br><br>

//   <!-- buttons -->
//   <button class="btn upload" onclick="sendUpload()">Upload</button>
//   <button class="btn erase" onclick="sendErase()">ERASE</button>

//   <!-- state monitor -->
//   <div id='status' class='bar'>State: NONE <span id="light"></span></div>

//   <script>
//     function showName(input){
//       const bar = document.getElementById('path');
//       bar.textContent = input.files.length ? input.files[0].name : 'Path';
//     }

//     async function sendUpload(){
//       const file = document.getElementById('file').files[0];
//       if(!file){ alert('Choose file please'); return; }

//       const fd = new FormData();
//       fd.append('firmware', file);
//       await fetch('/upload', {method:'POST', body:fd});
//     }

//     async function sendErase(){
//       await fetch('/erase', {method:'POST'});
//     }

//     const ws = new WebSocket('ws://' + location.host + '/ws');

//     ws.onmessage = function(e){
//   let j = JSON.parse(e.data);
//   document.getElementById('status').innerHTML = 'State: ' + j.state + ' <span id="light"></span>';
//   let b = document.getElementById('bar');
//   b.style.width = j.progress + '%';
//   b.textContent = j.progress + '%';
// };
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
    body {
      font-family: sans-serif;
      text-align: center;
      margin-top: 20px;
    }

    .bar {
      display: inline-block;
      width: 260px;
      padding: 6px;
      background: #d9d9d9;
      border-radius: 4px;
      margin: 4px 0;
    }

    .btn {
      padding: 8px 28px;
      border: 0;
      border-radius: 4px;
      font-weight: bold;
      cursor: pointer;
    }

    .upload {
      background: #4adbd9;
      color: #fff;
    }

    .erase {
      background: #f23c0f;
      color: #fff;
    }

    #progress {
      width: 100%;
      background: #ddd;
      border-radius: 6px;
      margin-top: 10px;
    }

    #bar {
      width: 0%;
      background: #4adbd9;
      color: #fff;
      border-radius: 6px;
      padding: 4px 0;
      font-size: 12px;
    }

    #light {
      width: 14px;
      height: 14px;
      border-radius: 50%;
      display: inline-block;
      background: #ccc;
      margin-left: 6px;
    }

    .green {
      background: #00c853 !important;
    }

    .red {
      background: #d50000 !important;
    }
  </style>
</head>
<body>
  <h2>Upload .bin ESP32</h2>

  <!-- File Path Display -->
  <div id="path" class="bar">Path</div>
  <input id="file" type="file" name="firmware" onchange="showName(this)">
  <br><br>

  <!-- Buttons -->
  <button id="btnUp" class="btn upload" onclick="sendUpload()">Upload</button>
  <button id="btnErase" class="btn erase" onclick="sendErase()">ERASE</button>

  <!-- Status -->
  <div id="status" class="bar">State: NONE <span id="light"></span></div>

  <!-- Progress Bar -->
  <div id="progress">
    <div id="bar">0%</div>
  </div>

  <script>
    function showName(input) {
      const bar = document.getElementById('path');
      bar.textContent = input.files.length ? input.files[0].name : 'Path';
    }

    function lockUI(lock) {
      document.getElementById('btnUp').disabled    = lock;
      document.getElementById('btnErase').disabled = lock;
    }

    async function sendUpload() {
      const file = document.getElementById('file').files[0];
      if (!file) { alert('Choose file please'); return; }

      lockUI(true);
      document.getElementById('status').innerHTML = 
        'State: UPLOADING… <span id="light" class=""></span>';

      const fd = new FormData();
      fd.append('firmware', file);
      try {
        await fetch('/upload', { method: 'POST', body: fd });
      } catch (e) {
        alert('Upload error');
        lockUI(false);
      }
    }

    async function sendErase() {
      lockUI(true);
      document.getElementById('status').innerHTML = 
        'State: ERASING… <span id="light" class=""></span>';

      try {
        await fetch('/erase', { method: 'POST' });
      } catch (e) {
        alert('Erase error');
        lockUI(false);
      }
    }

    const ws = new WebSocket('ws://' + location.host + '/ws');

    ws.onmessage = function(e) {
      let j = JSON.parse(e.data);
      document.getElementById('status').innerHTML = 
        'State: ' + j.state + ' <span id="light" class=""></span>';

      let b = document.getElementById('bar');
      b.style.width  = j.progress + '%';
      b.textContent  = j.progress + '%';

      let light = document.getElementById('light');
      if (j.state.includes('Fail') || j.state.includes('TIMEOUT')) {
        light.className = 'red';
      } else if (j.progress >= 100 && j.state !== 'NONE') {
        light.className = 'green';
      } else {
        light.className = '';
      }

      if (j.state === 'DONE' || j.state.includes('Fail') || j.state.includes('TIMEOUT')) {
        lockUI(false);
      }
    };

    ws.onclose = () => {
      document.getElementById('status').textContent = 'WS CLOSED';
    };
  </script>
</body>
</html>
)rawliteral";
}
