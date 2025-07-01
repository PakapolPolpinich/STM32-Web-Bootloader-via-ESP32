import serial
import time
import struct
import os

# === CONFIGURATION ===
PORT = 'COM13'
BAUDRATE = 9600
TIMEOUT = 1
#BIN_FILE = 'testled.bin'
BIN_FILE = 'test2led.bin'
START_ADDRESS = 0x08000000

# === BOOTLOADER CONSTANTS ===
CMD_INIT = b'\x7F'
CMD_GET_ID = b'\x02\xFD'
CMD_EXT_ERASE = b'\x44\xBB'
CMD_WRITE_MEMORY = b'\x31\xCE'
ACK = b'\x79'
NACK = b'\x1F'

# === FUNCTION HELPERS ===
def read_byte(ser, label, timeout_ms=3000):
    start = time.time()
    while True:
        b = ser.read(1)
        if b:
            print(f"[✓] {label}: 0x{b.hex().upper()}")
            return b
        if (time.time() - start) * 1000 > timeout_ms:
            print(f"[✗] {label}: Timeout > {timeout_ms} ms")
            return None

def checksum(data):
    return bytes([reduce(lambda x, y: x ^ y, data)])

def reset_bootloader(ser):
    input("[🛑] Please RESET the STM32 now, then press Enter...")
    ser.reset_input_buffer()
    print("[*] Sending INIT (0x7F)...")
    ser.write(CMD_INIT)
    ser.flush()
    time.sleep(0.2)
    return read_byte(ser, "INIT (expect 0x79)")

def send_extended_erase(ser):
    print("[*] Sending EXTENDED ERASE command (0x44 + 0xBB)...")
    ser.write(CMD_EXT_ERASE)
    ser.flush()

    ack1 = read_byte(ser, "ACK for EXTENDED ERASE")
    if not ack1:
        print("[✗] No response for Extended Erase command")
        return False
    print(f"[↩] Received: 0x{ack1.hex().upper()}")
    if ack1 != ACK:
        print("[✗] Not ACK for Extended Erase command")
        return False

    print("[*] Sending mass erase (0xFFFF + checksum)...")
    ser.write(bytes([0xFF, 0xFF, 0x00]))  # 0xFF ^ 0xFF ^ 0x00 = 0x00
    ser.flush()
    time.sleep(0.1)

    ack2 = read_byte(ser, "ACK for MASS ERASE", timeout_ms=30000)

    if not ack2:
        print("[✗] No response for mass erase")
        return False
    print(f"[↩] Received: 0x{ack2.hex().upper()}")
    if ack2 != ACK:
        print("[✗] Not ACK for mass erase")
        return False

    print("[✓] Mass Erase successful")
    return True

def send_write_block(ser, addr, data_block):
    ser.write(CMD_WRITE_MEMORY)
    if ser.read(1) != ACK:
        print("[✗] No ACK on Write command")
        return False
    #send address first + check sum
    addr_bytes = addr.to_bytes(4, 'big')
    addr_checksum = bytes([addr_bytes[0] ^ addr_bytes[1] ^ addr_bytes[2] ^ addr_bytes[3]])
    ser.write(addr_bytes + addr_checksum)
    if ser.read(1) != ACK:
        print("[✗] No ACK on address")
        return False

    n = len(data_block) - 1 #ส่งเท่าไร 256 -1 255
    block = bytes([n]) + data_block #add เข้าด้วยกัน
    #n = 3
    #data_block = b'\xAA\xBB\xCC\xDD'
    #block = b'\x03\xAA\xBB\xCC\xDD'ติดกันทุกตัว
    cksum = 0
    for b in block:
        cksum ^= b
    ser.write(block + bytes([cksum]))
    if ser.read(1) != ACK:
        print("[✗] No ACK on data block")
        return False

    print(f"[✓] Written {len(data_block)} bytes at 0x{addr:08X}")
    return True

def write_bin_file(ser, filepath, start_addr):
    with open(filepath, 'rb') as f:
        bin_data = f.read()
# ได้ มาแบบนี้ b'\x00\x00\x02 i\x08\x00\x08\xe5\x07\x00\x08\xed\x07\x00\x08\... แล้วเรื่อยๆ  
    print(f"[*] Uploading {len(bin_data)} bytes from {filepath} to 0x{start_addr:08X}")
#ส่งได้ที่ละ 256 byte ตอน write data     
    for i in range(0, len(bin_data), 256): #increase per 256
        chunk = bin_data[i:i+256]# [0:256] [256:512]
        if not send_write_block(ser, start_addr + i, chunk):
            print(f"[✗] Failed at 0x{start_addr + i:08X}")
            return
    print("[✓] Upload complete")

# === MAIN ===
def main():
    try:
        with serial.Serial(
            PORT,
            BAUDRATE,
            timeout=TIMEOUT,
            parity=serial.PARITY_EVEN,
            bytesize=serial.EIGHTBITS,
            stopbits=serial.STOPBITS_ONE
        ) as ser:
            if reset_bootloader(ser) != ACK:
                print("[✗] Bootloader not ready. Abort.")
                return

            if not send_extended_erase(ser):
                print("[✗] Erase failed")
                return

            write_bin_file(ser, BIN_FILE, START_ADDRESS)

    except serial.SerialException as e:
        print(f"[✗] Serial error: {e}")
 
from functools import reduce
main()
