import serial
import struct
import time

# === CONFIGURATION ===
PORT = 'COM13'
BAUDRATE = 9600
TIMEOUT = 1

# === CONSTANTS ===
CMD_INIT        = b'\x7F'
CMD_GET_ID      = b'\x02'
CMD_GET_ID_CSUM = b'\xFD'   # 0x02 ^ 0xFF
ACK             = b'\x79'
NACK            = b'\x1F'

# === FUNCTION HELPERS ===
def read_byte(ser, label):
    b = ser.read(1)
    if not b:
        print(f"[✗] {label}: No response")
        return None
    print(f"[✓] {label}: 0x{b.hex().upper()}")
    return b

def reset_bootloader(ser):
    input("[🛑] Please RESET the STM32 now, then press Enter...")

    ser.reset_input_buffer()
    print("[*] Sending INIT (0x7F)...")
    ser.write(CMD_INIT)
    ser.flush()
    time.sleep(0.2)
    return read_byte(ser, "INIT (expect 0x79)")

def send_get_id(ser):
    print("[*] Sending GET ID (0x02 + 0xFD)...")

    ser.reset_input_buffer()
    ser.write(bytes([0x02, 0xFD]))
    ser.flush()

    ack = ser.read(1)
    if ack != ACK:
        print(f"[✗] GET ID: No ACK received (got: {ack.hex() if ack else 'None'})")
        return

    n_byte = ser.read(1)
    if not n_byte:
        print("[✗] Timeout reading length byte")
        return
    n = n_byte[0]

    chip_id = ser.read(n + 1)
    if len(chip_id) != (n + 1):
        print("[✗] Timeout or incomplete chip ID")
        return

    final_ack = ser.read(1)
    if final_ack != ACK:
        print(f"[✗] Final ACK not received (got: {final_ack.hex() if final_ack else 'None'})")
        return

    print(f"[✓] Chip ID: {chip_id.hex().upper()} (length: {n+1})")
    print(f"[✓] Final ACK: 0x{final_ack.hex().upper()}")


# === MAIN ===
def main():
    try:
        with serial.Serial(
            PORT,
            BAUDRATE,
            timeout=TIMEOUT,
            parity=serial.PARITY_EVEN,         # ← ตั้งค่าให้ใช้ Even Parity
            bytesize=serial.EIGHTBITS,         # ← ค่า default (ไม่ต้องเปลี่ยนก็ได้)
            stopbits=serial.STOPBITS_ONE       # ← ค่า default
        ) as ser:
            if reset_bootloader(ser) != ACK:
                print("[✗] Bootloader not ready. Abort.")
                return
            send_get_id(ser)

    except serial.SerialException as e:
        print(f"[✗] Serial error: {e}")

if __name__ == '__main__':
    main()
