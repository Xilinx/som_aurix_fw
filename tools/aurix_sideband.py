#!/usr/bin/env python3
"""aurix_sideband.py — Ryzen ↔ AURIX sideband interface"""

import serial
import struct
import sys
import time
import argparse

# Command codes
CMD_PING          = 0x01
CMD_GET_STATUS    = 0x02
CMD_GET_VOLTAGES  = 0x03
CMD_GET_TEMP      = 0x04
CMD_GET_PM_STATE  = 0x05
CMD_GET_TLF       = 0x06
CMD_GET_USBPD     = 0x07
CMD_SHUTDOWN      = 0x10
CMD_WARM_RESET    = 0x11
CMD_COLD_RESET    = 0x12
CMD_FUSA_REG_READ = 0x30

# PM states
PM_STATES = {
    0: "OFF", 1: "POWER_UP", 2: "RAMP_ALW", 3: "RAMP_VR3V3",
    4: "RAMP_S5", 5: "RAMP_S3", 6: "RAMP_S0", 7: "ON",
    8: "S5", 9: "DN_S0_S3", 10: "DN_S3_S5", 11: "DN_S5_OFF",
    12: "FAULT", 13: "WARM_RESET"
}

# Voltage channel names (from SoM pin map)
VOLT_NAMES = {
    0: "APU_VDDCR",        1: "APU_VDDCR_CCD",    2: "APU_VDDCR_SOC",
    3: "APU_VDDCR_SR",     4: "GND_REF",
    8: "VDD_MEM_CHA",      9: "VDD_MEMQ_CHA",     10: "VDDIO_MEM_CHA",
    11: "GND_REF",
    16: "VDD_MEM_CHB",     17: "VDD_MEMQ_CHB",    18: "VDDIO_MEM_CHB",
    19: "GND_REF",
    24: "VDD_MISC_0V75",   25: "VDD_0V75_S5",     26: "VDD_1V2",
    27: "VDD_1V2_S5",      28: "VDD_1V8",          29: "VDD_1V8_S5",
    31: "VDDBT_RTC",
    40: "VDDIO_3V3",       41: "VDDIO_3V3_S5",    42: "VDDIO_AUDIO",
    43: "VDDIO_MEM_VAA",
}

def crc8(data):
    c = 0
    for b in data:
        c ^= b
    return c

class AurixSideband:
    def __init__(self, port, baud=115200, timeout=1.0):
        self.ser = serial.Serial(port, baud, timeout=timeout)

    def send_cmd(self, cmd, payload=b''):
        frame = bytes([cmd, len(payload)]) + payload
        frame += bytes([crc8(frame)])
        self.ser.write(frame)

        # Read response header
        hdr = self.ser.read(3)
        if len(hdr) < 3:
            return None, None
        rcmd, status, rlen = hdr
        body = self.ser.read(rlen + 1)
        if len(body) < rlen + 1:
            return status, None
        return status, body[:rlen]

    def ping(self):
        st, data = self.send_cmd(CMD_PING)
        if st == 0 and data and len(data) >= 8:
            fw, uptime = struct.unpack('<II', data[:8])
            return {'fw_version': f'0x{fw:08X}', 'uptime_s': uptime}
        return None

    def get_status(self):
        st, data = self.send_cmd(CMD_GET_STATUS)
        if st == 0 and data and len(data) >= 16:
            pm, cause, tlf, uptime = struct.unpack('<IIII', data[:16])
            return {
                'pm_state': PM_STATES.get(pm, f'UNKNOWN({pm})'),
                'reset_cause': cause,
                'tlf_devstat': f'0x{tlf:02X}',
                'uptime_s': uptime
            }
        return None

    def get_voltages(self):
        st, data = self.send_cmd(CMD_GET_VOLTAGES)
        if st == 0 and data and len(data) >= 48:
            voltages = {}
            for ch in range(24):
                mv = struct.unpack_from('<H', data, ch * 2)[0]
                if mv > 0:
                    name = VOLT_NAMES.get(ch, f'CH{ch}')
                    voltages[name] = mv
            return voltages
        return None

    def get_temp(self):
        st, data = self.send_cmd(CMD_GET_TEMP)
        if st == 0 and data and len(data) >= 4:
            temp = struct.unpack('<i', data[:4])[0]
            return temp
        return None

    def get_pm_state(self):
        st, data = self.send_cmd(CMD_GET_PM_STATE)
        if st == 0 and data and len(data) >= 12:
            pm, cause, retries = struct.unpack('<III', data[:12])
            return {
                'state': PM_STATES.get(pm, f'UNKNOWN({pm})'),
                'reset_cause': cause,
                'retry_count': retries
            }
        return None

    def get_tlf(self):
        st, data = self.send_cmd(CMD_GET_TLF)
        if st == 0 and data and len(data) >= 8:
            devstat, flags = struct.unpack('<II', data[:8])
            return {
                'devstat': f'0x{devstat:02X}',
                'err_active': bool(flags & 1),
                'safe_state': bool(flags & 2)
            }
        return None

    def shutdown(self):
        st, _ = self.send_cmd(CMD_SHUTDOWN)
        return st == 0

    def warm_reset(self):
        st, _ = self.send_cmd(CMD_WARM_RESET)
        return st == 0

    def cold_reset(self):
        st, _ = self.send_cmd(CMD_COLD_RESET)
        return st == 0

    def close(self):
        self.ser.close()


def cmd_status(aurix):
    """Full status dump"""
    info = aurix.ping()
    if not info:
        print("ERROR: No response from AURIX")
        return

    print(f"=== AURIX Status ===")
    print(f"  Firmware:  {info['fw_version']}")
    print(f"  Uptime:    {info['uptime_s']}s")

    status = aurix.get_status()
    if status:
        print(f"  PM State:  {status['pm_state']}")
        print(f"  TLF:       {status['tlf_devstat']}")

    tlf = aurix.get_tlf()
    if tlf:
        print(f"  ERR pin:   {'ACTIVE' if tlf['err_active'] else 'inactive'}")
        print(f"  Safe State:{'ACTIVE' if tlf['safe_state'] else 'inactive'}")

    temp = aurix.get_temp()
    if temp is not None:
        print(f"  APU Temp:  {temp} °C")

    print(f"====================")


def cmd_voltages(aurix):
    """Voltage monitor dump"""
    volts = aurix.get_voltages()
    if not volts:
        print("ERROR: No response")
        return

    print("=== Voltage Monitor ===")
    for name, mv in sorted(volts.items(), key=lambda x: x[1], reverse=True):
        bar = '█' * (mv // 100)
        print(f"  {name:20s} {mv:5d} mV  {bar}")
    print("=======================")


def cmd_monitor(aurix, interval=2.0):
    """Continuous monitoring loop"""
    print("Monitoring (Ctrl+C to stop)...")
    try:
        while True:
            info = aurix.ping()
            status = aurix.get_status()
            temp = aurix.get_temp()
            volts = aurix.get_voltages()

            ts = time.strftime('%H:%M:%S')
            pm = status['pm_state'] if status else '?'
            t = f"{temp}°C" if temp else '?'
            nv = len(volts) if volts else 0

            print(f"[{ts}] PM={pm} Temp={t} Rails={nv} Up={info['uptime_s']}s"
                  if info else f"[{ts}] No response")

            time.sleep(interval)
    except KeyboardInterrupt:
        print("\nStopped.")


def main():
    parser = argparse.ArgumentParser(description='AURIX Sideband Interface')
    parser.add_argument('-p', '--port', default='/dev/ttyS4')
    parser.add_argument('-b', '--baud', type=int, default=115200)

    sub = parser.add_subparsers(dest='command')
    sub.add_parser('status',   help='Full status dump')
    sub.add_parser('voltages', help='Voltage monitor')
    sub.add_parser('temp',     help='APU temperature')
    sub.add_parser('pm',       help='Power manager state')
    sub.add_parser('tlf',      help='TLF PMIC status')
    sub.add_parser('shutdown', help='Request graceful shutdown')
    sub.add_parser('reset',    help='Request warm reset')
    sub.add_parser('coldreset',help='Request cold reset')
    sub.add_parser('monitor',  help='Continuous monitoring')
    sub.add_parser('ping',     help='Ping AURIX')

    args = parser.parse_args()
    aurix = AurixSideband(args.port, args.baud)

    try:
        if args.command == 'status' or args.command is None:
            cmd_status(aurix)
        elif args.command == 'voltages':
            cmd_voltages(aurix)
        elif args.command == 'temp':
            t = aurix.get_temp()
            print(f"APU Temperature: {t} °C" if t is not None else "No response")
        elif args.command == 'pm':
            pm = aurix.get_pm_state()
            if pm:
                print(f"State: {pm['state']}, Cause: {pm['reset_cause']}, "
                      f"Retries: {pm['retry_count']}")
        elif args.command == 'tlf':
            tlf = aurix.get_tlf()
            if tlf:
                print(f"DEVSTAT: {tlf['devstat']}, ERR: {tlf['err_active']}, "
                      f"SS: {tlf['safe_state']}")
        elif args.command == 'shutdown':
            print("Requesting shutdown..." if aurix.shutdown() else "Failed")
        elif args.command == 'reset':
            print("Requesting warm reset..." if aurix.warm_reset() else "Failed")
        elif args.command == 'coldreset':
            print("Requesting cold reset..." if aurix.cold_reset() else "Failed")
        elif args.command == 'monitor':
            cmd_monitor(aurix)
        elif args.command == 'ping':
            info = aurix.ping()
            print(f"FW: {info['fw_version']}, Uptime: {info['uptime_s']}s"
                  if info else "No response")
    finally:
        aurix.close()

if __name__ == '__main__':
    main()