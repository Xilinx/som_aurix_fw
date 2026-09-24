#!/usr/bin/env python3
"""
aurix_update.py - AURIX TC387 firmware update over UART (SOTA)

Protocol (must match FwUpdate.h):
  Host -> AURIX
    SYNC   : 4 bytes   0x55AA55AA
    HEADER : 12 bytes  [imageSize:u32][imageCrc:u32][bank:u32]  bank = 0xFF (AURIX chooses)
    DATA   : 264 bytes [seqNum:u32][chunkCrc:u32][data:256]
  AURIX -> Host
    ACK    : 0x06060606
    NAK    : 0x15151515 + 1-byte error code

The AURIX always programs its INACTIVE bank (chosen from its own address map);
the host does not select a bank. Apply the update with `fwswap` on the CLI,
or pass --swap to have this script send it after a successful upload.

Requires: pip install pyserial intelhex
"""

import argparse
import binascii
import os
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("ERROR: pyserial not installed. Run: pip install pyserial")
try:
    from intelhex import IntelHex
except ImportError:
    sys.exit("ERROR: intelhex not installed. Run: pip install intelhex")

# ---- Protocol constants (must match FwUpdate.h) -----------------------------
SYNC_MAGIC  = 0x55AA55AA
ACK_BYTES   = b"\x06\x06\x06\x06"
NAK_BYTES   = b"\x15\x15\x15\x15"
CHUNK_SIZE  = 256            # == FWUPDATE_CHUNK_SIZE
BANK_AUTO   = 0xFF           # AURIX writes its inactive bank
BANK_WINDOW = 0x400000       # 4 MB swap window (PF0 + first 1 MB of PF1)

NAK_ERRORS = {
    0: "NONE", 1: "BAD_SYNC", 2: "BAD_HEADER", 3: "IMG_TOO_BIG", 4: "ERASE_FAIL",
    5: "SEQ_NUM", 6: "CHUNK_CRC", 7: "WRITE_FAIL", 8: "IMG_CRC", 9: "SWAP_FAIL",
    10: "TIMEOUT", 11: "META_FAIL",
}

SYNC_TIMEOUT   = 7.0
HEADER_TIMEOUT = 240.0       # includes the bank erase
CHUNK_TIMEOUT  = 10.0
FINAL_TIMEOUT  = 60.0        # flash read-back CRC + metadata write


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def load_image(path: str, verbose: bool) -> bytes:
    """Flatten an Intel HEX into a bank image starting at bank offset 0.
    0x8000_0000 (cached) and 0xA000_0000 (non-cached) alias the same PFlash;
    records outside the 4 MB swap window (BMHD, UCB, DFlash) are skipped."""
    if not path.lower().endswith((".hex", ".ihex")):
        with open(path, "rb") as f:
            return f.read()

    ih = IntelHex(path)
    data, skipped = {}, 0
    for addr in ih.addresses():
        if (addr & 0xF0000000) not in (0x80000000, 0xA0000000):
            skipped += 1
            continue
        off = addr & 0x0FFFFFFF
        if off >= BANK_WINDOW:
            skipped += 1
            continue
        data[off] = ih[addr]
    if not data:
        raise ValueError("no PFlash data found in hex")

    end = max(data) + 1
    img = bytearray(b"\xFF" * end)           # gaps -> erased pattern
    for off, val in data.items():
        img[off] = val
    print(f"  HEX -> image: {len(data)} data bytes, {end} bytes span, "
          f"{skipped} bytes outside the bank skipped", file=sys.stderr)
    return bytes(img)


def wait_response(ser: serial.Serial, timeout: float):
    """Scan the incoming stream for ACK/NAK. Works with or without console
    text interleaved (--cli mode). Returns ("ACK"|"NAK"|"TIMEOUT", code)."""
    deadline = time.time() + timeout
    buf = b""
    ser.timeout = 0.01
    while time.time() < deadline:
        chunk = ser.read(max(1, ser.in_waiting))
        if not chunk:
            continue
        buf += chunk
        if ACK_BYTES in buf:
            return ("ACK", None)
        idx = buf.find(NAK_BYTES)
        if idx >= 0:
            if idx + 4 < len(buf):
                return ("NAK", buf[idx + 4])
            extra = ser.read(1)              # error code may still be in flight
            return ("NAK", extra[0] if extra else 0xFF)
        buf = buf[-8:]                       # keep only what a split marker needs
    return ("TIMEOUT", None)


def err_name(code):
    return NAK_ERRORS.get(code, f"0x{code:02X}") if code is not None else "?"


def print_progress(done: int, total: int, t0: float) -> None:
    elapsed = time.time() - t0
    pct = 100.0 * done / total if total else 100.0
    bar = "#" * int(pct / 2) + "-" * (50 - int(pct / 2))
    eta = f"{(total - done) / (done / elapsed):.0f}s" if done and elapsed else "---"
    print(f"\r  [{bar}] {pct:5.1f}%  {done}/{total}  ETA {eta}  ", end="", file=sys.stderr)
    if done >= total:
        print(f"\n  Done: {total} bytes in {elapsed:.1f}s ({total / elapsed:.0f} B/s)",
              file=sys.stderr)


def upload(port, baud, path, retries, verbose, cli_mode, do_swap) -> bool:
    if not os.path.isfile(path):
        print(f"ERROR: file not found: {path}", file=sys.stderr)
        return False
    try:
        image = load_image(path, verbose)
    except Exception as e:
        print(f"ERROR: cannot load image: {e}", file=sys.stderr)
        return False

    pad = (CHUNK_SIZE - len(image) % CHUNK_SIZE) % CHUNK_SIZE
    image += b"\xFF" * pad
    if verbose and pad:
        print(f"  Padded {pad} bytes to {len(image)} (0x{len(image):X})", file=sys.stderr)

    image_size = len(image)                  # padded: what the AURIX writes and CRCs
    image_crc  = crc32(image)
    chunks     = image_size // CHUNK_SIZE

    print(f"Image:  {path}", file=sys.stderr)
    print(f"  Size: {image_size} bytes ({image_size / 1024:.1f} KB)", file=sys.stderr)
    print(f"  CRC:  0x{image_crc:08X}", file=sys.stderr)
    print(f"  Chunks: {chunks} x {CHUNK_SIZE} bytes", file=sys.stderr)
    print("  Target bank: inactive bank (chosen by AURIX)", file=sys.stderr)

    try:
        ser = serial.Serial(port=port, baudrate=baud, bytesize=8, parity="N",
                            stopbits=1, timeout=0.01, dsrdtr=False, rtscts=False)
    except serial.SerialException as e:
        print(f"ERROR: cannot open {port}: {e}", file=sys.stderr)
        return False
    print(f"Port:   {port} @ {baud} baud", file=sys.stderr)

    try:
        if cli_mode:
            print("Sending 'fwupdate' CLI command...", file=sys.stderr)
            ser.write(b"\r\nfwupdate\r\n")
            time.sleep(4)
        ser.reset_input_buffer()
        ser.reset_output_buffer()

        print("\n[1/4] Sending SYNC...", end=" ", file=sys.stderr)
        ser.write(struct.pack("<I", SYNC_MAGIC)); ser.flush()
        res, code = wait_response(ser, SYNC_TIMEOUT)
        if res != "ACK":
            print(f"FAILED ({res}, err={err_name(code)})", file=sys.stderr)
            return False
        print("ACK", file=sys.stderr)

        print("[2/4] Sending HEADER (bank erase may take several seconds)...",
              end=" ", file=sys.stderr)
        ser.write(struct.pack("<III", image_size, image_crc, BANK_AUTO)); ser.flush()
        res, code = wait_response(ser, HEADER_TIMEOUT)
        if res != "ACK":
            print(f"FAILED ({res}, err={err_name(code)})", file=sys.stderr)
            return False
        print("ACK", file=sys.stderr)

        print(f"[3/4] Uploading {chunks} chunks...", file=sys.stderr)
        t0 = time.time()
        for seq in range(chunks):
            chunk = image[seq * CHUNK_SIZE:(seq + 1) * CHUNK_SIZE]
            frame = struct.pack("<II", seq, crc32(chunk)) + chunk
            for attempt in range(retries + 1):
                ser.write(frame); ser.flush()
                res, code = wait_response(ser, CHUNK_TIMEOUT)
                if res == "ACK":
                    break
                if verbose or attempt == retries:
                    print(f"\n  Chunk {seq}: {res} (err={err_name(code)}), "
                          f"attempt {attempt + 1}/{retries + 1}", file=sys.stderr)
            else:
                print("  All retries exhausted - aborting", file=sys.stderr)
                return False
            print_progress((seq + 1) * CHUNK_SIZE, image_size, t0)

        print("\n[4/4] Waiting for flash read-back + commit...", end=" ", file=sys.stderr)
        res, code = wait_response(ser, FINAL_TIMEOUT)
        if res != "ACK":
            print(f"FAILED ({res}, err={err_name(code)})", file=sys.stderr)
            if code == 8:
                print("  Read-back CRC mismatch - update NOT applied, board stays on "
                      "the current bank", file=sys.stderr)
            return False
        print("ACK", file=sys.stderr)
        print(f"\nUpload complete: {image_size} bytes, CRC 0x{image_crc:08X}", file=sys.stderr)

        if do_swap:
            time.sleep(0.5)                  # let the AURIX return to the CLI
            print("Sending 'fwswap' - AURIX will reset into the new bank", file=sys.stderr)
            ser.write(b"\r\nfwswap\r\n"); ser.flush()
        else:
            print("Update staged in the inactive bank. Run 'fwswap' on the CLI to apply.",
                  file=sys.stderr)
        return True
    finally:
        ser.close()


def main():
    ap = argparse.ArgumentParser(description="Upload firmware to AURIX TC387 over UART")
    ap.add_argument("-p", "--port", required=True, help="Serial port (COM8, /dev/ttyUSB0)")
    ap.add_argument("-f", "--file", required=True, help="Firmware .hex (or raw .bin)")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="Baud (default 115200)")
    ap.add_argument("--retries", type=int, default=3, help="Retries per chunk (default 3)")
    ap.add_argument("--cli", action="store_true", help="Send 'fwupdate' first (debug UART)")
    ap.add_argument("--swap", action="store_true", help="Send 'fwswap' after success")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    ok = upload(a.port, a.baud, a.file, a.retries, a.verbose, a.cli, a.swap)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()