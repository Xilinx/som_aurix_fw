#!/usr/bin/env python3
"""aurix_query.py — query AURIX sideband interface from Ryzen"""
import serial, struct, sys

def crc8_xor(data):
    c = 0
    for b in data:
        c ^= b
    return c

def send_cmd(ser, cmd, payload=b''):
    frame = bytes([cmd, len(payload)]) + payload
    frame += bytes([crc8_xor(frame)])
    ser.write(frame)
    # Read response: CMD(1) + STATUS(1) + LEN(1) + PAYLOAD(LEN) + CRC(1)
    hdr = ser.read(3)
    if len(hdr) < 3:
        return None, None
    rcmd, status, rlen = hdr
    body = ser.read(rlen + 1)  # payload + crc
    return status, body[:rlen]

if __name__ == '__main__':
    port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyS4'
    ser = serial.Serial(port, 115200, timeout=1)

    # Ping
    st, data = send_cmd(ser, 0x01)
    if st == 0 and data:
        fw_ver, uptime = struct.unpack('<II', data)
        print(f"FW: 0x{fw_ver:08X}, Uptime: {uptime}s")

    # Get voltages
    st, data = send_cmd(ser, 0x03)
    if st == 0 and data:
        for i in range(24):
            mv = struct.unpack_from('<H', data, i*2)[0]
            if mv > 0:
                print(f"  CH{i:2d} = {mv} mV")

    ser.close()