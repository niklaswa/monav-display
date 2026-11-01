#!/usr/bin/env python3
"""Mac test client for the Monav Display (development only).

Speaks PROTOCOL.md from the Mac: hello (with the dev build's token), chunked JSON on inbox,
notifications on outbox, and the binary transfers on bulk with the protocol's flow control
(32 KiB window beyond the last `got`, `from` resend requests, 3 s timeout).

Needs a display running the amoled175-dev build (it advertises as "Monav Dev XXXX" with the service
UUID one digit off, and accepts DEV_TOKEN without touching the pairing) and bleak:  uv venv /tmp/monav-ble && uv pip install --python /tmp/monav-ble/bin/python bleak

    python3 tools/ble/monav_ble.py info
    python3 tools/ble/monav_ble.py layout map
    python3 tools/ble/monav_ble.py map tools/preview/maps/sample-berliner-allee.jpg
    python3 tools/ble/monav_ble.py drive --seconds 40          # map + route + a drive along it
    python3 tools/ble/monav_ble.py ota .pio/build/amoled175-dev/firmware.bin
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import math
import struct
import sys
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner

# The dev build advertises its service one digit off ("Monav Dev XXXX"), so the iPhone leaves it alone.
SERVICE = "fcaa986b-5d31-472a-86ac-5426528cf28e"
NAME = "Monav Dev"
INBOX = "1359e742-830f-4294-ba43-4fe4f7a9e4a9"
OUTBOX = "260a8427-49cc-4b70-b611-af0a5215d625"
INFO = "6de16d5b-8847-40ef-ae08-31a8a73e3c4a"
BULK = "f1b9ad33-d140-459e-82b0-f9bf8e8c4466"

DEV_TOKEN = "monav-dev-token"  # platformio.ini, env amoled175-dev
ACK_EVERY = 16384
WINDOW = 32768


def now_ms() -> int:
    return int(time.time() * 1000)


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class Display:
    def __init__(self, client: BleakClient):
        self.c = client
        self.seq = 0
        self.rx = bytearray()
        self.rx_seq = None
        self.history: list[dict] = []  # every message from the display, for wait_for
        self.new_message = asyncio.Event()
        self.acks: dict[int, int] = {}
        self.froms: dict[int, int] = {}
        self.ack_events: dict[int, asyncio.Event] = {}
        self.send_lock = asyncio.Lock()
        self.verbose = False
        self.quiet = False  # do not log every message (drive loops)

    async def ready_to_write(self):
        """CoreBluetooth drops writes without response once its queue is full (bleak does not
        wait): wait for canSendWriteWithoutResponse, as the iOS app does."""
        peripheral = getattr(getattr(self.c, "_backend", None), "_peripheral", None)
        if peripheral is None or not hasattr(peripheral, "canSendWriteWithoutResponse"):
            return
        for _ in range(2000):
            if peripheral.canSendWriteWithoutResponse():
                return
            await asyncio.sleep(0.002)

    @property
    def chunk(self) -> int:
        ch = self.c.services.get_characteristic(BULK)
        return max(20, ch.max_write_without_response_size)

    def _on_notify(self, _sender, data: bytearray):
        hdr, payload = data[0], bytes(data[1:])
        first, last, seq = hdr & 0x80, hdr & 0x40, hdr & 0x3F
        if first:
            self.rx = bytearray()
        elif self.rx_seq is None or seq != (self.rx_seq + 1) & 0x3F:
            self.rx = bytearray()
            self.rx_seq = None
            return
        self.rx_seq = seq
        self.rx += payload
        if last:
            try:
                msg = json.loads(self.rx.decode())
            except Exception:
                log("!! outbox: not JSON:", bytes(self.rx))
                return
            if msg.get("t") == "bulk":
                if "got" in msg:
                    self.acks[msg["id"]] = max(self.acks.get(msg["id"], 0), msg["got"])
                if "from" in msg:
                    self.froms[msg["id"]] = msg["from"]
                self.ack_event(msg["id"]).set()
            elif not self.quiet or msg.get("t") in ("ota", "welcome", "denied"):
                log("<-", json.dumps(msg, ensure_ascii=False))
            self.history.append(msg)
            self.new_message.set()

    def ack_event(self, tid: int) -> asyncio.Event:
        return self.ack_events.setdefault(tid, asyncio.Event())

    def mark(self) -> int:
        """Position in the message history: pass it to wait_for to catch answers to what is sent next."""
        return len(self.history)

    async def start(self):
        await self.c.start_notify(OUTBOX, self._on_notify)

    async def send(self, obj: dict):
        data = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode()
        size = self.chunk - 1
        async with self.send_lock:  # one message's chunks in a row
            for off in range(0, len(data), size):
                part = data[off : off + size]
                hdr = self.seq & 0x3F
                if off == 0:
                    hdr |= 0x80
                if off + size >= len(data):
                    hdr |= 0x40
                self.seq = (self.seq + 1) & 0x3F
                await self.c.write_gatt_char(INBOX, bytes([hdr]) + part, response=True)

    async def wait_for(self, pred, timeout: float, start: int | None = None):
        """First message from `start` on (default: from now) that matches."""
        i = self.mark() if start is None else start
        end = time.time() + timeout
        while True:
            while i < len(self.history):
                msg = self.history[i]
                i += 1
                if pred(msg):
                    return msg
            left = end - time.time()
            if left <= 0:
                return None
            self.new_message.clear()
            if i < len(self.history):
                continue
            try:
                await asyncio.wait_for(self.new_message.wait(), left)
            except asyncio.TimeoutError:
                return None

    async def hello(self, lang="de", token=DEV_TOKEN):
        m = self.mark()
        await self.send({"t": "hello", "proto": 1, "token": token, "lang": lang, "now": now_ms()})
        return await self.wait_for(lambda m: m.get("t") in ("welcome", "denied"), 5, m)

    async def bulk(self, tid: int, data: bytes, label: str, stop_after: int = 0) -> float:
        """Sends data on bulk with the protocol's flow control. Returns seconds taken.
        stop_after: return once that many bytes are acknowledged (to test aborts)."""
        size = self.chunk - 5
        self.acks[tid] = 0
        sent = got = 0
        t0 = time.time()
        last_progress = time.time()
        last_print = 0.0
        resends = 0
        while got < len(data):
            while sent < len(data) and sent - got < WINDOW:
                n = min(size, len(data) - sent)
                await self.ready_to_write()
                await self.c.write_gatt_char(BULK, struct.pack("<BI", tid, sent) + data[sent : sent + n], response=False)
                sent += n
            ev = self.ack_event(tid)
            ev.clear()
            if self.acks.get(tid, 0) > got or tid in self.froms:
                ev.set()
            try:
                await asyncio.wait_for(ev.wait(), 3.0)
            except asyncio.TimeoutError:
                resends += 1
                log(f"   {label}: no acknowledgement for 3 s - resending from {got}")
                sent = got
                continue
            if self.acks.get(tid, 0) > got:
                got = self.acks[tid]
                last_progress = time.time()
                if self.verbose:
                    log(f"   {label}: got {got} (sent {sent})")
            if tid in self.froms:
                frm = self.froms.pop(tid)
                log(f"   {label}: display asks from {frm}")
                sent = frm
            if time.time() - last_print > 1.0:
                last_print = time.time()
                log(f"   {label}: {got}/{len(data)} ({got * 100 // len(data)} %), "
                    f"{got / 1024 / max(0.001, time.time() - t0):.1f} kB/s")
            if time.time() - last_progress > 20:
                raise RuntimeError(f"{label}: stalled at {got}")
            if stop_after and got >= stop_after:
                log(f"   {label}: stopping after {got} acknowledged bytes")
                return time.time() - t0
        dt = time.time() - t0
        log(f"   {label}: {len(data)} bytes in {dt:.1f} s ({len(data) / 1024 / dt:.1f} kB/s, {resends} resends)")
        return dt


async def find(name_part: str | None):
    log("scanning ...")
    devs = await BleakScanner.discover(timeout=8.0, return_adv=True, service_uuids=[SERVICE])
    for _addr, (d, adv) in devs.items():
        name = adv.local_name or d.name or ""
        if NAME in name and (name_part is None or name_part.upper() in name.upper()):
            log("found", name, adv.rssi, "dBm")
            return d
    return None


async def connect(name_part: str | None) -> tuple[BleakClient, Display]:
    for _ in range(3):
        dev = await find(name_part)
        if dev is not None:
            client = BleakClient(dev)
            await client.connect(timeout=15.0)
            disp = Display(client)
            await disp.start()
            log("connected, write without response up to", disp.chunk, "bytes")
            return client, disp
    raise SystemExit("display not found")


# --- map geometry (as the iOS side does it) ---------------------------------------------------

def geo(lat0, lon0, east, north):
    return lat0 + north / 110540.0, lon0 + east / (111320.0 * math.cos(math.radians(lat0)))


def route_bytes(path_px, meta) -> bytes:
    """Route through map pixels, every ~10 m, int16 metres east/north of the image centre."""
    mpp, w, h = meta["mpp"], meta["w"], meta["h"]
    pts = []
    for (x0, y0), (x1, y1) in zip(path_px, path_px[1:]):
        steps = max(1, int(math.hypot(x1 - x0, y1 - y0) * mpp / 10))
        for k in range(steps):
            x, y = x0 + (x1 - x0) * k / steps, y0 + (y1 - y0) * k / steps
            pts.append((round((x - w / 2) * mpp), round((h / 2 - y) * mpp)))
    x, y = path_px[-1]
    pts.append((round((x - w / 2) * mpp), round((h / 2 - y) * mpp)))
    return b"".join(struct.pack("<hh", e, n) for e, n in pts)


async def send_map(disp: Display, tid: int, jpg: Path, wait: bool = True) -> dict:
    meta = json.loads(jpg.with_suffix(".json").read_text())
    data = jpg.read_bytes()
    m = disp.mark()
    await disp.send({"t": "map", "op": "begin", "id": tid, "size": len(data), "w": meta["w"], "h": meta["h"],
                     "lat": meta["lat"], "lon": meta["lon"], "mpp": meta["mpp"], "attr": meta.get("attr", "")})
    await disp.bulk(tid, data, f"map #{tid}")
    if wait:
        t0 = time.time()
        msg = await disp.wait_for(lambda x: x.get("t") == "map" and x.get("id") == tid, 15, m)
        log(f"   map #{tid}: {msg} after {time.time() - t0:.2f} s more (decode + swap)")
    return meta


# --- commands ---------------------------------------------------------------------------------

async def cmd_info(args):
    client, disp = await connect(args.id)
    info = await client.read_gatt_char(INFO)
    log("info:", info.decode())
    log("hello ->", await disp.hello())
    await client.disconnect()


async def cmd_layout(args):
    client, disp = await connect(args.id)
    await disp.hello()
    m = disp.mark()
    await disp.send({"t": "dev", "layout": args.layout})
    await disp.wait_for(lambda x: x.get("t") == "status", 3, m)
    await client.disconnect()


async def cmd_map(args):
    client, disp = await connect(args.id)
    await disp.hello()
    await send_map(disp, 31, Path(args.jpg))
    await client.disconnect()


SAMPLES = [Path("tools/preview/maps/sample-berliner-allee.jpg"), Path("tools/preview/maps/sample-koenigsallee.jpg"),
           Path("tools/preview/maps/sample-a46.jpg")]
def path_px(meta):
    """The drive follows the route in the sample's .json (real streets, see tools/preview/maps)."""
    return [tuple(p) for p in meta["route"]]


class Drive:
    """A drive as Monav sends it: v at 10 Hz (the fix moving once a second), a scene every 2 s, and
    (with map_every) a new map image + route every few seconds, cycling through the samples."""

    def __init__(self, disp: Display, args):
        self.disp = disp
        self.args = args
        self.meta = json.loads(Path(args.jpg).with_suffix(".json").read_text())
        self.stop = asyncio.Event()
        self.maps = 0
        self.decode_s: list[float] = []
        self.tid = 50

    def next_id(self) -> int:
        self.tid = 50 + (self.tid - 49) % 150  # 50..199, clear of the other commands' ids
        return self.tid

    async def send_map_and_route(self, jpg: Path):
        tid = self.next_id()
        t0 = time.time()
        m = self.disp.mark()
        meta = await send_map(self.disp, tid, jpg, wait=False)
        self.meta = meta
        rid = self.next_id()
        data = route_bytes(path_px(meta), meta)
        await self.disp.send({"t": "route", "op": "begin", "id": rid, "size": len(data), "lat": meta["lat"], "lon": meta["lon"]})
        await self.disp.bulk(rid, data, f"route #{rid}")
        ready = await self.disp.wait_for(lambda x: x.get("t") == "map" and x.get("id") == tid, 15, m)
        self.maps += 1
        log(f"   map #{tid} ({jpg.stem}): {ready.get('st') if ready else 'no answer'} {time.time() - t0:.1f} s after begin")

    async def map_loop(self):
        k = 0
        while not self.stop.is_set():
            await self.send_map_and_route(SAMPLES[k % len(SAMPLES)] if self.args.map_every else Path(self.args.jpg))
            k += 1
            if not self.args.map_every:
                return
            try:
                await asyncio.wait_for(self.stop.wait(), self.args.map_every)
            except asyncio.TimeoutError:
                pass

    def place(self, t: float):
        meta = self.meta
        path = path_px(meta)
        seg = [(math.hypot(x1 - x0, y1 - y0) * meta["mpp"], (x0, y0), (x1, y1)) for (x0, y0), (x1, y1) in zip(path, path[1:])]
        total = sum(x[0] for x in seg)
        d = (self.args.kmh / 3.6 * t) % (total * 0.95)
        for length, (x0, y0), (x1, y1) in seg:
            if d <= length:
                f = d / length
                x, y = x0 + (x1 - x0) * f, y0 + (y1 - y0) * f
                hdg = (math.degrees(math.atan2(x1 - x0, -(y1 - y0))) + 360) % 360
                break
            d -= length
        east = (x - meta["w"] / 2) * meta["mpp"] + (3000 if self.args.off_image else 0)
        lat, lon = geo(meta["lat"], meta["lon"], east, (meta["h"] / 2 - y) * meta["mpp"])
        return round(lat, 6), round(lon, 6), round(hdg, 1)

    async def v_loop(self):
        t0 = time.time()
        fix = self.place(0)
        next_fix = 1.0
        next_scene = 0.0
        while not self.stop.is_set():
            t = time.time() - t0
            if t >= next_fix:
                fix = self.place(t)
                next_fix += 1.0
            await self.disp.send({"t": "v", "now": now_ms(), "kmh": self.args.kmh, "lim": 50,
                                  "lat": fix[0], "lon": fix[1], "hdg": fix[2]})
            if t >= next_scene:
                next_scene += 2.0
                await self.disp.send({"t": "s", "at": now_ms(),
                                      "turn": {"k": "right", "d": max(10, int(400 - 10 * t) % 400), "road": "Bahnstraße"}})
            await asyncio.sleep(0.1)

    async def start(self):
        self.disp.quiet = True
        await self.disp.send({"t": "dev", "layout": "map"})
        await self.disp.send({"t": "cfg", "tilt": not self.args.flat})
        self.tasks = [asyncio.create_task(self.v_loop()), asyncio.create_task(self.map_loop())]

    async def finish(self):
        self.stop.set()
        for t in self.tasks:
            try:
                await asyncio.wait_for(t, 20)
            except Exception as e:  # noqa: BLE001
                log("   drive task:", repr(e))


def add_drive_args(s):
    s.add_argument("--jpg", default="tools/preview/maps/sample-berliner-allee.jpg")
    s.add_argument("--flat", action="store_true")
    s.add_argument("--kmh", type=float, default=42.0)
    s.add_argument("--map-every", type=float, default=0, help="a new map image + route every N s (cycling the samples)")
    s.add_argument("--off-image", action="store_true", help="put the car 3 km east of the image (the loading view)")


async def cmd_drive(args):
    client, disp = await connect(args.id)
    log("welcome:", await disp.hello())
    drive = Drive(disp, args)
    await drive.start()
    t0 = time.time()
    while time.time() - t0 < args.seconds:
        await asyncio.sleep(1.0)
        if not client.is_connected:
            raise SystemExit(f"connection lost after {time.time() - t0:.0f} s")
    await drive.finish()
    log(f"drive: {args.seconds:.0f} s, {drive.maps} map images")
    await disp.send({"t": "bye"})
    await client.disconnect()


async def cmd_ota(args):
    image = Path(args.bin).read_bytes()
    sha = hashlib.sha256(image).hexdigest()
    if args.corrupt_sha:
        sha = "0" * 64
    client, disp = await connect(args.id)
    welcome = await disp.hello()
    log("before:", welcome)
    drive = None
    if args.with_drive:
        drive = Drive(disp, args)
        await drive.start()
        await asyncio.sleep(3.0)  # the first map is on its way
    tid = 7
    m = disp.mark()
    await disp.send({"t": "ota", "op": "begin", "id": tid, "size": len(image), "sha256": sha, "version": args.version})
    ready = await disp.wait_for(lambda x: x.get("t") == "ota", 10, m)
    if not ready or ready.get("st") != "ready":
        raise SystemExit(f"not ready: {ready}")
    if args.abort_at:
        # Send part of it, then abort (or lose the connection): the running firmware must stay.
        await disp.bulk(tid, image, "ota", stop_after=args.abort_at)
        if args.abort_op:
            log("sending abort")
            await disp.send({"t": "ota", "op": "abort", "id": tid})
            await asyncio.sleep(2.0)
        else:
            log("dropping the connection")
        await client.disconnect()
        return
    await disp.bulk(tid, image, "ota")
    if drive:
        await drive.finish()
    m = disp.mark()
    t_end = time.time()
    await disp.send({"t": "ota", "op": "end", "id": tid})
    if args.drop_after_end:
        log("dropping the connection right after end")
        await client.disconnect()
        done = None
    else:
        done = await disp.wait_for(lambda x: x.get("t") == "ota", 30, m)
        log(f"end -> {done} after {time.time() - t_end:.2f} s")
        t_done = time.time()
        if args.stay:
            # Keep the link: the display restarting drops it.
            while client.is_connected and time.time() - t_done < 30:
                await asyncio.sleep(0.05)
            log(f"link dropped {time.time() - t_done:.2f} s after done" if not client.is_connected
                else "!! still connected 30 s after done")
        try:
            await client.disconnect()
        except Exception:  # noqa: BLE001
            pass
        if not done or done.get("st") != "done":
            return
    log("waiting for the display to come back ...")
    await asyncio.sleep(2)
    for _ in range(10):
        try:
            client, disp = await connect(args.id)
            log(f"after ({time.time() - t_end:.1f} s after end):", await disp.hello())
            await client.disconnect()
            return
        except (SystemExit, Exception) as e:  # noqa: BLE001 - a display restarting (again) drops the link
            log("   reconnect:", repr(e))
            await asyncio.sleep(1)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--id", help="the display's 4-digit id (default: the first Monav Display found)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    s = sub.add_parser("layout")
    s.add_argument("layout", choices=["hud", "focus", "map"])
    s = sub.add_parser("map")
    s.add_argument("jpg")
    s = sub.add_parser("drive")
    s.add_argument("--seconds", type=float, default=30)
    add_drive_args(s)
    s = sub.add_parser("ota")
    s.add_argument("bin")
    s.add_argument("--version", default="test")
    s.add_argument("--corrupt-sha", action="store_true", help="announce a wrong SHA-256 (must be refused)")
    s.add_argument("--abort-at", type=int, default=0, help="disconnect after this many bytes")
    s.add_argument("--abort-op", action="store_true", help="with --abort-at: send op abort instead of disconnecting")
    s.add_argument("--drop-after-end", action="store_true", help="disconnect right after sending end (before done)")
    s.add_argument("--stay", action="store_true", help="stay connected after done until the display drops the link")
    s.add_argument("--with-drive", action="store_true", help="drive in the map layout during the update")
    add_drive_args(s)
    args = p.parse_args()
    asyncio.run({"info": cmd_info, "layout": cmd_layout, "map": cmd_map, "drive": cmd_drive, "ota": cmd_ota}[args.cmd](args))


if __name__ == "__main__":
    main()
