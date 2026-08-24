#!/usr/bin/env python3
"""
aurix_update.py — AURIX TC387 firmware update tool

Uploads a firmware binary to the AURIX via the dedicated transfer UART,
using the framed protocol defined in FwUpdate.h.

Protocol:
  Host → AURIX:
    SYNC    : 4 bytes   0x55AA55AA
    HEADER  : 12 bytes  [imageSize:u32] [imageCrc:u32] [targetBank:u32]
    DATA    : 264 bytes [seqNum:u32] [chunkCrc:u32] [data:256 bytes]

  AURIX → Host:
    ACK     : 4 bytes   0x06060606
    NAK     : 5 bytes   0x15151515 + 1-byte error code

Usage:
    python3 aurix_update.py --port /dev/ttyUSB0 --file firmware.bin
    python3 aurix_update.py --port COM3 --file firmware.bin --baud 921600
"""

import argparse
import struct
import sys
import time
import os

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed.  Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
#  Protocol constants — must match FwUpdate.h
# ---------------------------------------------------------------------------
SYNC_MAGIC  = 0x55AA55AA
ACK_MAGIC   = 0x06060606
NAK_MAGIC   = 0x15151515

CHUNK_SIZE  = 256       # PFlash burst size
HEADER_SIZE = 12        # imageSize(4) + imageCrc(4) + targetBank(4)
DATA_FRAME_SIZE = 264   # seqNum(4) + chunkCrc(4) + data(256)

# Target bank constants
BANK_A = 0
BANK_B = 1

# NAK error code names (from FwUpdate.h FwUpdate_Error_t)
NAK_ERRORS = {
    0:  "NONE",
    1:  "BAD_SYNC",
    2:  "BAD_HEADER",
    3:  "IMG_TOO_BIG",
    4:  "ERASE_FAIL",
    5:  "SEQ_NUM",
    6:  "CHUNK_CRC",
    7:  "WRITE_FAIL",
    8:  "IMG_CRC",
    9:  "SWAP_FAIL",
    10: "TIMEOUT",
    11: "META_FAIL",
}

# Default timeouts (seconds)
SYNC_TIMEOUT    = 2.0
HEADER_TIMEOUT  = 30.0   # Includes bank erase (~4 s)
CHUNK_TIMEOUT   = 5.0
FINAL_TIMEOUT   = 30.0   # CRC verify + meta write + swap


# ---------------------------------------------------------------------------
#  CRC-32 (standard Ethernet polynomial, matches AURIX software CRC)
# ---------------------------------------------------------------------------
def crc32(data: bytes) -> int:
    """Compute CRC-32 matching the AURIX firmware's software CRC implementation."""
    import binascii
    return binascii.crc32(data) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
#  Serial helpers
# ---------------------------------------------------------------------------
def send_u32(ser: serial.Serial, value: int) -> None:
    """Send a 32-bit little-endian word."""
    ser.write(struct.pack("<I", value))


def send_bytes(ser: serial.Serial, data: bytes) -> None:
    """Send raw bytes."""
    ser.write(data)


def wait_response(ser: serial.Serial, timeout: float) -> tuple:
    """
    Wait for an ACK or NAK from the AURIX.

    Returns:
        ("ACK", None)           on success
        ("NAK", error_code)     on NAK
        ("TIMEOUT", None)       on timeout
        ("BAD", raw_bytes)      on unrecognized response
    """
    ser.timeout = timeout
    resp = ser.read(4)

    if len(resp) < 4:
        return ("TIMEOUT", None)

    magic = struct.unpack("<I", resp)[0]

    if magic == ACK_MAGIC:
        return ("ACK", None)

    if magic == NAK_MAGIC:
        # Read the 1-byte error code
        err = ser.read(1)
        if len(err) == 1:
            return ("NAK", err[0])
        return ("NAK", 0xFF)

    return ("BAD", resp)


# ---------------------------------------------------------------------------
#  Progress bar
# ---------------------------------------------------------------------------
def print_progress(current: int, total: int, start_time: float, prefix: str = "") -> None:
    """Print a simple progress bar to stderr."""
    elapsed = time.time() - start_time
    pct = (current / total) * 100 if total > 0 else 0
    filled = int(pct / 2)
    bar = "█" * filled + "░" * (50 - filled)

    if current > 0 and elapsed > 0:
        rate = current / elapsed
        remaining = (total - current) / rate if rate > 0 else 0
        eta = f"{remaining:.0f}s"
    else:
        eta = "---"

    print(f"\r{prefix}[{bar}] {pct:5.1f}%  {current}/{total}  ETA {eta}  ", end="", file=sys.stderr)

    if current >= total:
        bps = total / elapsed if elapsed > 0 else 0
        print(f"\n{prefix}Done: {total} bytes in {elapsed:.1f}s ({bps:.0f} B/s)", file=sys.stderr)


# ---------------------------------------------------------------------------
#  Main upload flow
# ---------------------------------------------------------------------------
def upload_firmware(port: str, baud: int, filepath: str, target_bank: int,
                    retries: int = 3, verbose: bool = False) -> bool:
    """
    Upload a firmware binary to the AURIX.

    Args:
        port:         Serial port (e.g. /dev/ttyUSB0, COM3)
        baud:         Baud rate (must match UART_XFER_BAUD on the AURIX)
        filepath:     Path to the firmware binary
        target_bank:  BANK_A (0) or BANK_B (1)
        retries:      Number of retry attempts per chunk on NAK
        verbose:      Print detailed diagnostics

    Returns:
        True on success, False on failure.
    """

    # -- Load and validate the image -----------------------------------------
    if not os.path.isfile(filepath):
        print(f"ERROR: File not found: {filepath}", file=sys.stderr)
        return False

    with open(filepath, "rb") as f:
        image = f.read()

    image_size = len(image)
    if image_size == 0:
        print("ERROR: Image file is empty", file=sys.stderr)
        return False

    # Pad to CHUNK_SIZE boundary
    pad_len = (CHUNK_SIZE - (image_size % CHUNK_SIZE)) % CHUNK_SIZE
    if pad_len > 0:
        image += b"\xFF" * pad_len   # 0xFF = erased PFlash state
        if verbose:
            print(f"  Padded {pad_len} bytes to {len(image)} (0x{len(image):X})", file=sys.stderr)

    total_chunks = len(image) // CHUNK_SIZE
    image_crc = crc32(image)

    print(f"Image:  {filepath}", file=sys.stderr)
    print(f"  Size: {image_size} bytes ({image_size / 1024:.1f} KB)", file=sys.stderr)
    print(f"  CRC:  0x{image_crc:08X}", file=sys.stderr)
    print(f"  Chunks: {total_chunks} x {CHUNK_SIZE} bytes", file=sys.stderr)
    print(f"  Target bank: {'A' if target_bank == BANK_A else 'B'}", file=sys.stderr)

    # -- Open serial port ----------------------------------------------------
    try:
        ser = serial.Serial(
            port=port,
            baudrate=baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=SYNC_TIMEOUT,
        )
    except serial.SerialException as e:
        print(f"ERROR: Cannot open {port}: {e}", file=sys.stderr)
        return False

    print(f"Port:   {port} @ {baud} baud", file=sys.stderr)

    # Flush any stale data
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    time.sleep(0.1)
    ser.reset_input_buffer()

    # -- Step 1: SYNC --------------------------------------------------------
    print("\n[1/4] Sending SYNC...", file=sys.stderr, end=" ")
    send_u32(ser, SYNC_MAGIC)
    ser.flush()

    result, err = wait_response(ser, SYNC_TIMEOUT)
    if result != "ACK":
        print(f"FAILED ({result}, err={err})", file=sys.stderr)
        print("  Is the AURIX running and listening on the transfer UART?", file=sys.stderr)
        ser.close()
        return False
    print("ACK", file=sys.stderr)

    # -- Step 2: HEADER ------------------------------------------------------
    print("[2/4] Sending HEADER (bank erase may take ~4s)...", file=sys.stderr, end=" ")
    header = struct.pack("<III", image_size, image_crc, target_bank)
    send_bytes(ser, header)
    ser.flush()

    result, err = wait_response(ser, HEADER_TIMEOUT)
    if result != "ACK":
        err_name = NAK_ERRORS.get(err, f"0x{err:02X}") if err is not None else "?"
        print(f"FAILED ({result}, err={err_name})", file=sys.stderr)
        if err == 3:
            print(f"  Image too large for target bank", file=sys.stderr)
        elif err == 4:
            print(f"  Bank erase failed — check flash protection", file=sys.stderr)
        ser.close()
        return False
    print("ACK", file=sys.stderr)

    # -- Step 3: DATA chunks -------------------------------------------------
    print(f"[3/4] Uploading {total_chunks} chunks...", file=sys.stderr)
    start_time = time.time()

    for seq in range(total_chunks):
        offset = seq * CHUNK_SIZE
        chunk = image[offset : offset + CHUNK_SIZE]
        chunk_crc = crc32(chunk)

        # Build DATA frame: seqNum(4) + chunkCrc(4) + data(256)
        frame = struct.pack("<II", seq, chunk_crc) + chunk

        attempt = 0
        while attempt <= retries:
            send_bytes(ser, frame)
            ser.flush()

            result, err = wait_response(ser, CHUNK_TIMEOUT)

            if result == "ACK":
                break

            attempt += 1
            err_name = NAK_ERRORS.get(err, f"0x{err:02X}") if err is not None else "?"

            if attempt <= retries:
                if verbose:
                    print(f"\n  Chunk {seq}: {result} (err={err_name}), retry {attempt}/{retries}",
                          file=sys.stderr)
                time.sleep(0.05)
            else:
                print(f"\n  Chunk {seq}: {result} (err={err_name}), all retries exhausted",
                      file=sys.stderr)
                ser.close()
                return False

        print_progress(offset + CHUNK_SIZE, len(image), start_time, prefix="  ")

    # -- Step 4: Wait for final ACK (CRC verify + meta + swap) ---------------
    print(f"\n[4/4] Waiting for verification + commit...", file=sys.stderr, end=" ")

    result, err = wait_response(ser, FINAL_TIMEOUT)
    if result != "ACK":
        err_name = NAK_ERRORS.get(err, f"0x{err:02X}") if err is not None else "?"
        print(f"FAILED ({result}, err={err_name})", file=sys.stderr)
        if err == 8:
            print("  Full-image CRC mismatch — data corruption during transfer", file=sys.stderr)
        elif err == 9:
            print("  UCB_SWAP write failed — flash protection?", file=sys.stderr)
        ser.close()
        return False

    print("ACK", file=sys.stderr)

    elapsed = time.time() - start_time
    bps = image_size / elapsed if elapsed > 0 else 0
    print(f"\n✓ Upload complete: {image_size} bytes in {elapsed:.1f}s ({bps:.0f} B/s)", file=sys.stderr)
    print(f"  AURIX will reset and boot from bank {'A' if target_bank == BANK_A else 'B'}",
          file=sys.stderr)

    ser.close()
    return True


# ---------------------------------------------------------------------------
#  CLI
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Upload firmware to AURIX TC387 via UART",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --port /dev/ttyUSB0 --file build/firmware.bin
  %(prog)s --port COM3 --file build/firmware.bin --baud 921600
  %(prog)s --port /dev/ttyUSB0 --file build/firmware.bin --bank A
        """,
    )
    parser.add_argument("--port", "-p", required=True,
                        help="Serial port (e.g. /dev/ttyUSB0, COM3)")
    parser.add_argument("--file", "-f", required=True,
                        help="Firmware binary file to upload")
    parser.add_argument("--baud", "-b", type=int, default=115200,
                        help="Baud rate (default: 115200)")
    parser.add_argument("--bank", choices=["A", "B"], default="B",
                        help="Target flash bank (default: B = inactive)")
    parser.add_argument("--retries", type=int, default=3,
                        help="Retry count per chunk on NAK (default: 3)")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Print detailed diagnostics")

    args = parser.parse_args()

    target_bank = BANK_A if args.bank == "A" else BANK_B

    success = upload_firmware(
        port=args.port,
        baud=args.baud,
        filepath=args.file,
        target_bank=target_bank,
        retries=args.retries,
        verbose=args.verbose,
    )

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()