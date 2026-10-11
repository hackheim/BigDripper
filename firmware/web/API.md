# BigDripper API

The board's API, reachable two ways:

- **HTTP**, on the board's own access point (`BigDripper`, password `dripdrip1`) at `http://bigdripper.local/` (or `http://192.168.4.1/`). The web UI in this directory is embedded in the firmware and served there.
- **Bluetooth LE**, for the same web UI served from anywhere else over HTTPS, using Web Bluetooth (Chrome on Android and desktop; not iOS). The phone stays on its own network. See [Bluetooth](#bluetooth).

Both carry the same operations (`src/api.h`). Each takes a JSON object of arguments and replies with a status code and a JSON body:

| Operation | HTTP | Arguments | Reply |
|---|---|---|---|
| `status` | `GET /api/status` | | [Status](#status) |
| `queue` | `POST /api/queue` | `text`, `font`, `invert` | Status |
| `mode` | `POST /api/mode` | `mode` | Status |
| `lines` | `POST /api/lines` | `spacing` | Status |
| `settings` | `GET /api/settings` | | [Settings](#settings) (password) |
| `save_settings` | `POST /api/settings` | any settings | Settings (password) |
| `history` | `GET /api/history?since=N` | `since` | [History](#history) |
| `emoji` | `GET /api/emoji` | | [Emoji](#other) |

Conventions:

- Over HTTP, replies are never cached (`Cache-Control: no-store`), except `/api/emoji`. `POST` bodies are JSON objects, sent with `Content-Type: application/json`. Without that header the request isn't recognised and gets a 404.
- Errors come back as `{"error": "Reason, fit to show a user."}` with status `400` (bad request), `401` (settings password missing or wrong), `404` (no such operation or path) or `409` (the queue is full).
- A request that fails changes nothing.
- Every call goes through the print task (`src/printer.h`), so what a reply shows already reflects the request.

`settings` and `save_settings` need the settings password, `letmeprint9`. Over HTTP, as basic auth with user `admin`. A wrong or missing one gets a 401 without `WWW-Authenticate`, so a browser doesn't show its own password box. Over Bluetooth, in the request's `auth` field.

## Status

### `GET /api/status`

What the print engine is doing. Also the reply to `queue`, `mode` and `lines`, and the `status` event on the stream.

```json
{
  "boot": 2864173519,
  "mode": "text",
  "current": {"text": " BIG DRIPPER ", "font": "Drip", "invert": false},
  "default": true,
  "paused": false,
  "pending": [{"text": "HI", "font": "Drip bold", "invert": true}],
  "lines_spacing": 10,
  "max_text_len": 64,
  "mm_per_count": 0.25,
  "history_size": 512,
  "dry_run": false
}
```

| Field | Meaning |
|---|---|
| `boot` | Random per boot. If it changes, the board rebooted and history sequence numbers started over. |
| `mode` | `off`, `prime`, `trace`, `lines` or `text`. |
| `current` | What Text mode prints now, or `null` during the pause between two messages. |
| `default` | `current` is the default text, printed on repeat while nothing is queued. |
| `paused` | Between two messages. |
| `pending` | Messages queued behind `current`, in print order. |
| `lines_spacing` | Lines mode: a full column every this many columns. |
| `max_text_len` | Longest text `POST /api/queue` accepts, in printed characters (an emoji counts as 1). |
| `mm_per_count` | Ground distance per encoder count, from the scale setting. Multiply a history record's speed by this to get mm/s. |
| `history_size` | How many history records the board keeps. |
| `dry_run` | A dry-run build: no valve ever opens. |

## Commands

### `queue` (`POST /api/queue`)

```json
{"text": "HELLO :heart:", "font": "drip_bold", "invert": false}
```

Renders the text and queues it after anything already queued. It interrupts the default text straight away. `font` is `spleen`, `drip` (the default) or `drip_bold`; `invert` defaults to `false`. Replies with the status.

Errors:
- `400`: nothing to print, too long, or a bad field.
- `409`: the queue is full (8 messages).

### `mode` (`POST /api/mode`)

```json
{"mode": "lines"}
```

Sets exactly that mode, so two phones setting the same one don't undo each other. Setting the mode it's already in does nothing. Replies with the status.

### `lines` (`POST /api/lines`)

```json
{"spacing": 5}
```

Lines mode's spacing, in columns. This is the one setting the main page may change without the password. It's saved, and applies from the next column on. Replies with the status.

## Settings

### `settings` (`GET /api/settings`)

```json
{
  "scale_mm_per_detent": 1.0,
  "max_text_len": 64,
  "print_pause_ms": 2000,
  "clicks_per_column": 100,
  "column_burst_ms": 20,
  "burst_duty_pct": 50,
  "min_burst_ms": 4,
  "trace_gap_ms": 1000,
  "test_line_spacing": 10,
  "letter_spacing": 1,
  "min_column_ms": 0,
  "max_stretch_pct": 300
}
```

### `save_settings` (`POST /api/settings`)

Any subset of the fields above; missing ones keep their saved value. The result is checked as a whole: either everything is saved and handed to the print engine, or a `400` says why and nothing changes. Replies with all the settings.

Rules:
- `scale_mm_per_detent` must be above 0.
- `max_text_len` is 1–255.
- `clicks_per_column` is at least 1.
- `min_column_ms` (0 = off) stretches text at speed: each column is made wide enough to take at least this long to cross, up to `max_stretch_pct` (100–1000) of its normal width. Burst length follows the stretched column.
- `letter_spacing` is 0–8 blank columns between characters. A change applies to messages queued from then on and to the default text (which restarts if it's printing); messages already queued keep their spacing.
- `burst_duty_pct` is 5–95.
- `min_burst_ms` is between 1 and `column_burst_ms`.
- Everything else is a whole number, 0 or more.

## History

The print task records everything it does into a ring of the last `history_size` records. Each record has a sequence number, one more than the previous, starting from 0 at boot.

### `history` (`GET /api/history?since=N`)

Every record from sequence number `N` on, or as many as the board still has.

```json
{"first": 1200, "next": 1203, "lost": 0, "reset": false, "now": 81234567,
 "r": [[0, 81200000, 17, 65280, 9800, 0, 4000], [1, 81210000, 2, 0, 0, 0, 0], ...]}
```

| Field | Meaning |
|---|---|
| `first` | Sequence number of `r[0]`; the rest follow on. |
| `next` | Ask for this next time. |
| `lost` | Records between `since` and `first` that the board overwrote before they were asked for. |
| `reset` | `since` was ahead of the board, e.g. from before a reboot. Start over from `first`. |
| `now` | Board time, in microseconds since boot: the clock `at` is on. |

Each record is `[kind, at, value, bits, open_us, overrun, speed, stretch]`:

| `kind` | Meaning |
|---|---|
| `0` coils | The coils were set: a column, a Trace pulse, Prime, or closing everything. `bits` is which are open (bit 15 = COIL1). `value` is the column index, or -1 if this isn't a column. `open_us` is how long they stay open: 0 if `bits` is 0, -1 if until the next coils record (Prime). `overrun` is 1 if the previous burst was still open. `speed` is the speed estimate in encoder counts per second, 0 if there isn't one. `stretch` is, for a column, its width as a percentage of the normal width (100 = not stretched). |
| `1` skipped | The wheel jumped past `value` columns that never fired. |
| `2` mode | The mode changed to `value`: 0 off, 1 prime, 2 trace, 3 lines, 4 text. |
| `3` message started | `value` is 1 for the default text, 0 for a queued message. |
| `4` message finished | A queued message was printed to its end. |

`at` is when the print task issued it, in microseconds since boot.

## Event stream

Over HTTP, `GET /api/events`: [server-sent events](https://html.spec.whatwg.org/multipage/server-sent-events.html). Browsers reconnect by themselves.

| Event | Data |
|---|---|
| `status` | As `GET /api/status`. Sent on connecting, and again whenever it changes. |
| `history` | As `GET /api/history`: new records, in order, about 10 times a second while there are any. |
| `clock` | `{"now": 81234567}` every 2 seconds, to keep a page's idea of board time in step and show the connection is alive. |

A client keeps the `next` it has got to. If a `history` event's `first` is past that, events were dropped or the connection was lost for a while. Fetch `GET /api/history?since=next` to fill in what's missing; only that reply's `lost` is really gone. On connecting, a client fetches history itself; the stream only carries what's new from then on.

Over Bluetooth the same events arrive as notifications; see below.

## Other

| | |
|---|---|
| `emoji` (`GET /api/emoji`) | The emoji the fonts have: `[{"code": ":smile:", "width": 13, "columns": [...]}]`. Columns as stored, bit 15 = top row. |
| `GET /`, `/style.css`, `/*.js` | The web UI. |
| `GET /params` | Redirects to `/#settings` (the settings used to be a page of their own). |

## Bluetooth

The board advertises as `BigDripper` with one GATT service, `c44f5ed8-6e56-4c8b-ae22-7164774163b9`, which has two characteristics:

| Characteristic | UUID | |
|---|---|---|
| request | `2edbd9b7-7914-4c4d-8ae1-97fb8b1dc19b` | Write (with response). Requests. |
| events | `b6a173f9-9607-4151-8654-f814ffd5bdf6` | Notify. Replies and the event stream. |

Up to 3 phones can be connected at once. There's no pairing: anyone in range can connect, as on the open parts of the HTTP API.

Every write and every notification starts with one byte: the message type, with `0x80` set if the message continues in the next one. A message too big for one write or notification is split, and its pieces are joined in order. Each connection's messages never interleave. Writes carry at most 512 bytes, and notifications are cut to the connection's MTU.

| Type | Direction | Content |
|---|---|---|
| `0` request | to the board | JSON `{"id": 7, "op": "queue", "args": {...}, "auth": "..."}`. `auth` is only needed for the settings operations. |
| `1` reply | from the board | JSON `{"id": 7, "code": 200, "body": {...}}`: the request's `id`, the status code and body as over HTTP. |
| `2` status | from the board | JSON, as `status`. Sent on subscribing and whenever it changes. |
| `3` history | from the board | New history records, packed (below). |
| `4` clock | from the board | int64 board time in microseconds, every 2 seconds. |

Subscribe to events before sending requests: replies only go to a subscribed connection. As over HTTP, a client fetches what came before with the `history` operation, and the stream carries what's new.

History events are packed, little endian: a 21-byte header, then 26 bytes per record, with the same fields as the JSON history:

```
header  u32 first, u32 next, u32 lost, u8 reset, i64 now
record  u8 kind, u8 overrun, u16 bits, i32 value, i64 at,
        i32 open_us (-1 = held), u32 speed, u16 stretch
```

`web/transport.js` is the reference client.
