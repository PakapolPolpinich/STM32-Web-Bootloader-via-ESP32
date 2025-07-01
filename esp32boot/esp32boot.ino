#define CTR_RESET_PIN 18
#define CTR_BOOT_0    19
#define UART_TX       17
#define UART_RX       16

#define ACK  0x79
#define NACK 0x1F

HardwareSerial UARTSerial(1);
bool processStarted = false;

typedef enum {
  WAIT,
  INIT,
  ERASE,
  WRITE
} BootState;

BootState State = WAIT;

void setup() {
  pinMode(CTR_RESET_PIN, OUTPUT);
  pinMode(CTR_BOOT_0, OUTPUT);

  digitalWrite(CTR_RESET_PIN,HIGH);
  digitalWrite(CTR_BOOT_0, LOW);

  UARTSerial.begin(9600, SERIAL_8E1, UART_RX, UART_TX);
  Serial.begin(115200);
  delay(500);  // Let Serial settle

  Serial.println("Bootloader Controller Ready.");
}

void loop() {
  // Listen for ENTER key to start bootloader process
  if (!processStarted && Serial.available()) {
  char c = Serial.read();
  if (c == 's' || c == 'S') {
    Serial.println("[*] Starting bootloader process...");
    processStarted = true;
    State = INIT;
  }
}

  // Main state machine
  if (processStarted) {
    switch (State) {
      case WAIT:
        Serial.println("[*] Returning to WAIT...");
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

      default:
        break;
    }
  }
}


void wait() {
  Serial.println("[WAIT] Waiting...");
  delay(2000); // simulate button press or condition to start
  State = INIT;
}

void init() {
  Serial.println("[INIT] Entering bootloader mode...");

  // Enter bootloader mode
  digitalWrite(CTR_BOOT_0, HIGH);
  delay(10);
  digitalWrite(CTR_RESET_PIN, LOW);
  delay(50);
  digitalWrite(CTR_RESET_PIN,HIGH);
  delay(100);

  // Send 0x7F (INIT command)
  UARTSerial.write(0x7F);

  if (ACKpolling(1000)) {
    State = ERASE;
  } else {
    Serial.println("[INIT] Failed. Going back to WAIT.");
    State = WAIT;
  }
}

void erase() {
  Serial.println("[ERASE] Sending Extended Erase command...");

  UARTSerial.write(0x44);  // Extended Erase command
  UARTSerial.write(0xBB);  // XOR checksum

  if (!ACKpolling(1000)) {
    Serial.println("[ERASE] CMD NACK");
    State = WAIT;
    return;
  }

  UARTSerial.write(0xFF);  // Mass erase MSB
  UARTSerial.write(0xFF);  // Mass erase LSB
  UARTSerial.write(0x00);  // Checksum (XOR of 0xFF ^ 0xFF = 0x00)

  if (ACKpolling(100000)) {
    Serial.println("[ERASE] Done Erase");
    State = WRITE;
  } else {
    Serial.println("[ERASE] Erase NACK");
    State = WAIT;
  }
}

void write() {
  Serial.println("[WRITE] Placeholder. Add write memory logic here.");
  digitalWrite(CTR_BOOT_0,LOW);
  digitalWrite(CTR_RESET_PIN, LOW);
  delay(50);
  digitalWrite(CTR_RESET_PIN,HIGH);
  delay(100);
  State = WAIT;  // Or loop/write more data
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
