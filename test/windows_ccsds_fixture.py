"""Windows wire-level fixture for the F Prime ComCcsds bridge.

This is a protocol test process, not the Hadron F Prime deployment. It uses
the generated topology dictionary and F Prime's standard CCSDS packet layout.
"""

import argparse
import json
import select
import socket
import struct
import time


def checksum(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 0xFFFF
    return crc


def tm_frame(channels, sequence, seconds):
    # Fw::TlmPacket: APID descriptor, then ID / Fw::Time / channel value.
    payload = struct.pack(">H", 1)
    for channel_id, packed in channels:
        payload += struct.pack(">IHBII", channel_id, 0, 0, seconds, 0) + packed
    packet = struct.pack(">HHH", 1, 0xC000 | (sequence & 0x3FFF), len(payload) - 1) + payload
    frame = struct.pack(">HBBH", (68 << 4) | (1 << 1), sequence & 0xFF,
                        sequence & 0xFF, 0x1800) + packet
    idle_size = 1024 - len(frame) - 2
    if idle_size < 7:
        raise ValueError("telemetry does not fit one TM frame")
    frame += struct.pack(">HHH", 0x07FF, 0xC000, idle_size - 7)
    frame += bytes([0x55]) * (idle_size - 6)
    return frame + struct.pack(">H", checksum(frame))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dictionary", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=50101)
    args = parser.parse_args()
    print("Hadron SITL Windows fixture starting", flush=True)
    with open(args.dictionary, encoding="utf-8") as source:
        dictionary = json.load(source)
    ids = {channel["name"].split(".")[-1]: channel["id"]
           for channel in dictionary["telemetryChannels"]}
    opcode = next(command["opcode"] for command in dictionary["commands"]
                  if command["name"].endswith(".plantModel.SET_RUNNING"))
    required = ("DemoRunning", "DemoAltitudeM", "DemoVelocityMps", "DemoTemperatureC")
    if any(name not in ids for name in required):
        raise ValueError("Hadron demo channels missing from dictionary")
    running = False
    altitude = velocity = 0.0
    sequence = 0
    incoming = bytearray()
    while True:
        try:
            connection = socket.create_connection((args.host, args.port), timeout=1)
            break
        except OSError:
            time.sleep(0.5)
    connection.setblocking(False)
    print(f"Windows CCSDS fixture connected to {args.host}:{args.port}", flush=True)
    next_sample = time.monotonic()
    try:
        while True:
            timeout = max(0, next_sample - time.monotonic())
            readable, _, _ = select.select([connection], [], [], min(timeout, 0.5))
            if readable:
                data = connection.recv(4096)
                if not data:
                    raise ConnectionError("bridge closed the connection")
                incoming.extend(data)
                while len(incoming) >= 5:
                    length = (struct.unpack_from(">H", incoming, 2)[0] & 0x03FF) + 1
                    if length < 18 or length > 1024:
                        del incoming[0]
                        continue
                    if len(incoming) < length:
                        break
                    frame = bytes(incoming[:length])
                    del incoming[:length]
                    if checksum(frame[:-2]) != struct.unpack_from(">H", frame, length - 2)[0]:
                        continue
                    if struct.unpack_from(">I", frame, 13)[0] == opcode:
                        running = bool(frame[17])
                        print(f"SET_RUNNING received: {running}", flush=True)
            if time.monotonic() >= next_sample:
                if running:
                    velocity += 0.4
                    altitude += velocity * 0.5
                channels = [
                    (ids["DemoRunning"], struct.pack(">B", int(running))),
                    (ids["DemoAltitudeM"], struct.pack(">f", altitude)),
                    (ids["DemoVelocityMps"], struct.pack(">f", velocity)),
                    (ids["DemoTemperatureC"], struct.pack(">f", 22 + altitude * 0.001)),
                ]
                connection.sendall(tm_frame(channels, sequence, int(time.time())))
                sequence += 1
                next_sample += 0.5
    finally:
        connection.close()


if __name__ == "__main__":
    main()
