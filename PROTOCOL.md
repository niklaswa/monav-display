# Monav Display protocol (version 1)

How Monav on the iPhone talks to the round display in the car. The phone does all the work —
position, plugins, forecasts, navigation — and sends what is to be shown; the display draws it,
counts down on its own and keeps going through a short gap in the link.

Bluetooth LE only. The display is the peripheral and advertises; Monav is the central and keeps a
pending connection to the paired display, so iOS connects (and wakes Monav in the background)
as soon as the display powers up with the car.

## GATT

| | UUID | |
| --- | --- | --- |
| Service | `FCAA986B-5D31-472A-86AC-5426528CF28D` | advertised (complete list of 128-bit UUIDs) |
| `inbox` | `1359E742-830F-4294-BA43-4FE4F7A9E4A9` | phone → display: Write, Write Without Response |
| `outbox` | `260A8427-49CC-4B70-B611-AF0A5215D625` | display → phone: Notify |
| `info` | `6DE16D5B-8847-40EF-AE08-31A8A73E3C4A` | Read: `{"fw":"0.1.0","proto":1,"id":"4F2A","paired":true}` |
| `bulk` | `F1B9AD33-D140-459E-82B0-F9BF8E8C4466` | phone → display: Write Without Response — binary transfers (firmware 0.2.0+) |

Advertising data carries the flags and the service UUID; the scan response carries the name
`Monav Display 4F2A`. The four hex digits (`id`) are the last two bytes of the Bluetooth address:
the display shows them while waiting, the app shows them in the list of displays found.

No Bluetooth bonding: reflashing the display must never leave a stale bond on the phone that only
Settings › Bluetooth › Forget can clear. Pairing is done in the protocol (`hello`, below).

## Framing

Every write and every notification is one chunk:

```
byte 0     bit 7  FIRST — the chunk starts a message
           bit 6  LAST  — the chunk ends a message
           bits 0-5  sequence number, +1 per chunk, wraps at 64
bytes 1…   payload
```

A message is the payloads from a FIRST chunk to a LAST chunk, concatenated: one UTF-8 JSON object,
at most 1024 bytes. A chunk is at most the ATT MTU minus 3 bytes (iOS negotiates 185 or more; the
display must not assume more than 20 until the MTU exchange is done). A FIRST chunk in the middle
of a message drops what was collected; so does a sequence gap. Unknown message types and unknown
keys are ignored on both sides — that is how the protocol grows without a new version.

## Time

The phone sends its clock with every `v` message (`now`, milliseconds since 1970). The display
keeps `offset = now − own clock` and moves it only towards larger values (a message's `now` is
older than its arrival by the latency, so the fastest message gives the largest offset and is
closest to the truth) with a slow drift back down, so a late message cannot shift the countdowns.
Every other time in the protocol is phone time in milliseconds since 1970.

## Phone → display

### `hello` — first message on every connection

```json
{"t":"hello","proto":1,"token":"9f0c…32 hex…","lang":"de","now":1761220000123}
```

- No token stored on the display (fresh, or reset): it stores this one and answers `welcome` with
  `"fresh":true`. That is the pairing.
- Same token: `welcome`.
- Another token: `denied`. The display says it belongs to another phone and how to reset it.

Until `welcome`, the display ignores every other message from this connection. `lang` is `de` or
`en` and picks the language of the display's own texts. Optional: `"tz":120`, the phone's offset
from UTC in minutes, for the display's clock (standby, arrival time); left out, the display assumes
German time (CET/CEST). `cfg` may carry it too when it changes.

### `v` — vehicle, 5–10 times a second while a drive runs

```json
{"t":"v","now":1761220000123,"kmh":87.4,"lim":100}
```

`kmh` is the speed (from the car when the phone has it, otherwise GPS). `lim` is the speed limit in
km/h, left out when none is known.

From firmware 0.2.0 (the map) `v` also carries the car's place, already put on the road the phone
matched (the route or the OpenStreetMap way), and its course:

```json
{"t":"v","now":1761220000123,"kmh":48.2,"lim":50,"lat":51.214231,"lon":6.782815,"hdg":87.5}
```

`hdg` is degrees from north, clockwise. Fixes come about once a second and are repeated in between;
the display runs the car on along `hdg` with `kmh` between two of them and eases towards each new
one, so the map glides instead of stepping.

### `s` — scene, whenever it changes and at least every 2 s while a drive runs

```json
{
  "t": "s", "at": 1761220000123,
  "light": {"st": "red", "from": 1761219990000, "to": 1761220012000, "d": 220, "adv": 48, "turn": "left"},
  "cam": {"k": "fixed", "d": 340, "vmax": 50, "label": "Fester Blitzer"},
  "sec": {"vmax": 80, "avg": 76.5},
  "turn": {"k": "right", "d": 300, "road": "Herzogstraße", "exit": 2, "eta": 1761220900000}
}
```

Every part is optional; a part left out means there is nothing of that kind to show.

- `at`: the phone time the distances (`d`, metres) refer to. With the speed from `v` the display
  can run them down between two scenes.
- `light`: the next traffic light on the way. `st` is `red`, `green`, `uncertain` (the forecast
  is not sure) or `unknown`; `from` is when the current phase began, `to` when it ends (left out:
  not within the forecast). `adv` is the speed that catches the green phase, `0` when green cannot
  be reached (stop), left out when the current speed is right. `turn` (`left`, `right`,
  `straight`) is the lane the light is for, left out when it is for all.
- `cam`: the speed camera ahead. `k` is `fixed`, `mobile`, `redlight`, `section`, `distance` or
  `other`; `vmax` is left out when unknown; `label` is a ready-made text in the phone's language.
- `sec`: an average speed check being driven through (`avg` so far, km/h).
- `turn`: the next manoeuvre while navigating. `k` is `straight`, `slightLeft`, `left`,
  `sharpLeft`, `slightRight`, `right`, `sharpRight`, `uTurn`, `keepLeft`, `keepRight`,
  `roundabout` or `arrive`; `road` is the street after it (or "Richtung …"); `exit` is the exit to
  take at a roundabout; `eta` is the arrival at the destination.

### `cfg` — settings from the app, after `welcome` and when they change

```json
{"t":"cfg","bright":80}
```

`bright` 5–100 sets the brightness (the display's own swipe changes it too and reports back with
`status`). From 0.2.0: `"tilt":true` lays the map down in perspective, `false` keeps it flat.
From 0.2.3: `"rot":-25` turns the whole picture that many degrees clockwise on the glass (any
integer, wrapped into 0–359), for a display that hangs at an angle. It applies from the next frame,
touches are turned back with it, and the display stores the angle once it has stopped changing, so
the boot and waiting screens stand straight too. Monav sends it with every step of its slider.
From 0.2.6: `"lang":"en"` switches the display's own texts like `hello`'s `lang` does, without a new
connection (the app's display language setting).

### `chg` — charging stations, when the list changes (firmware 0.2.4)

```json
{"t":"chg","st":[[51.21423,6.78282,3],[51.21502,6.78011,0]]}
{"t":"chg","st":[[51.21423,6.78282,3,6]]}
```

Stations a charger plug-in reports live, nearest first, at most 16 within a few kilometres:
`[lat, lon, free]`, or `[lat, lon, free, total]` when the user wants the total too. The map layout
draws each as a pin with a bolt and the number — green while one is free, grey when full. An empty
list takes them off; `bye` drops them. Monav sends the list again after every connection.

### `bye` — the drive ends or Monav quits

```json
{"t":"bye"}
```

The display shows its standby screen and dims.

## Display → phone

```json
{"t":"welcome","fw":"0.1.0","proto":1,"id":"4F2A","fresh":true,"bright":80,"layout":"hud"}
{"t":"denied","id":"4F2A"}
{"t":"status","bright":60}
{"t":"status","layout":"map"}
```

`layout` (0.2.0) is what a tap on the glass picked: `hud`, `focus` or `map`. In `welcome` and
whenever it changes — the phone renders and sends map images only while it is `map`.

## Pairing reset on the display

Holding a finger on the glass for three seconds asks whether to forget the paired phone; a tap
confirms. Holding the BOOT button for three seconds does the same without asking. The display then
accepts the next `hello` as a new pairing; if a phone is connected at that moment, the display
drops the connection, so the phone connects again and its `hello` pairs it (`"fresh":true`).

## Binary transfers (firmware 0.2.0)

Firmware images, map images and the route are too big for JSON. Each goes as one transfer: a JSON
message on `inbox` announces it with an id (1–255, the phone counts up and wraps), then the bytes
follow on `bulk`, each write

```
byte 0       transfer id
bytes 1–4    offset of the first data byte, little endian
bytes 5…     data (up to MTU − 3 − 5 bytes)
```

Flow control is the display's acknowledgement, on `outbox`, every 16 KiB it has taken in (stored or
written to flash) and once more when the last byte is in:

```json
{"t":"bulk","id":7,"got":49152}
```

The phone keeps at most 32 KiB beyond the last `got` in flight. A write whose offset is not the
next expected one is dropped, and the display asks once per gap for the rest:

```json
{"t":"bulk","id":7,"from":32768}
```

The phone goes back to `from`. No acknowledgement for 3 s: the phone resends from the last `got`.
A new transfer of the same kind replaces one still running; bytes for an id the display does not
know (any more) are dropped.

### Firmware update (`ota`)

```json
{"t":"ota","op":"begin","id":7,"size":1360624,"sha256":"9f0c…64 hex…","version":"0.2.1"}
```

The display opens the other app partition and answers `{"t":"ota","st":"ready","id":7}` (or
`{"t":"ota","st":"error","id":7,"err":"…"}`); only then do the bytes follow. It shows an update
screen with the progress while they come. Once the last byte is acknowledged:

```json
{"t":"ota","op":"end","id":7}
```

The display checks the SHA-256 and the image, makes the new partition the boot partition, answers
`{"t":"ota","st":"done","id":7}` and restarts half a second later. The phone sees it connect again;
the `welcome` names the new version. `{"t":"ota","op":"abort","id":7}` (or a lost connection)
leaves the running firmware as it is.

The new firmware starts on probation (the bootloader's app rollback): it marks itself good once it
has drawn its first frame, advertises and has run for 15 s. If it crashes or hangs before that, the
next start goes back to the previous firmware — the phone then sees the old version in `welcome` and
reports the update as rolled back.

### Map image (`map`)

```json
{"t":"map","op":"begin","id":8,"size":81234,"w":1024,"h":1024,"lat":51.2142,"lon":6.7828,"mpp":1.2}
```

A baseline JPEG, north up, whose centre pixel is at `lat`/`lon`, `mpp` metres per pixel. From 0.2.5 an
optional `"attr":"© OpenStreetMap"` (at most 39 bytes) names whom the picture is credited to; the
display writes it under the map, "© Apple Maps" when it is left out. The display
decodes it into its second map buffer and switches over once decoded, answering
`{"t":"map","st":"ready","id":8}`. At most 1024 × 1024 pixels and 1 MiB; an image the display cannot
take or decode is answered with `{"t":"map","st":"error","id":8,"err":"…"}`. Positions convert with the same flat projection on both sides,
around the image centre: east = (lon − lon₀) · 111 320 · cos(lat₀), north = (lat − lat₀) · 110 540,
in metres. The phone picks `mpp` by speed (closer in town, further out on the motorway) and centres
the next image ahead of the car, before the car comes near the edge of the current one.

The display draws the map turned to the driving direction, the car low in the circle, the route on
top, and in the `map` layout the speed, the limit and the card over a darkened picture.

### Route (`route`)

```json
{"t":"route","op":"begin","id":9,"size":1200,"lat":51.2142,"lon":6.7828}
{"t":"route","op":"clear"}
```

The bytes are pairs of int16 little endian — metres east, metres north of `lat`/`lon` — the
route's line from a little behind the car to some kilometres ahead, thinned to about 10 m. Sent with
each map image while navigating; `clear` when the guidance ends.
