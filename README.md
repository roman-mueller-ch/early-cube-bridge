# early-cube-bridge

## Why

I track my working time with the [EARLY](https://early.app) cube: flip it to the side of what I'm working on, put it in the cradle when I stop. Then our IT asked me to remove the EARLY desktop app from my work laptop, which is what the cube talks to over Bluetooth. I didn't want to give up the cube, and pairing it with my phone instead wasn't an option either, so I put a small ESP32 next to it. It does the app's job and talks to EARLY directly. Nothing has to be installed on the work laptop, where I only open the EARLY web app to review my times.

## What it does

Use the [EARLY](https://early.app) (formerly Timeular) time-tracking cube without the EARLY desktop app. An ESP32 listens to the cube over Bluetooth LE and calls the [EARLY public API](https://developers.early.app/) over WiFi: flipping the cube stops the running entry and starts the activity mapped to the new side. Putting the cube in its cradle stops tracking.

Tested with an ESP32 DevKit (ESP-WROOM-32) and a Timeular Tracker (model TZ02K016, firmware 2.0.0).

## How it works

The cube needs no pairing. It exposes the side facing up as a single byte (1-8, 0 in the cradle) on characteristic `c7e70012-c847-11e6-8175-8c89a55d403c` (service `c7e70010-…`) and sends an indication on every flip. The bridge waits 2 s for the cube to settle, then calls `POST /tracking/stop` and `POST /tracking/{activityId}/start`. EARLY rejects entries shorter than a minute; those are dropped with `DELETE /tracking`.

Before switching, the bridge checks `GET /tracking`: if the activity for the side is already running, it leaves it alone. So after a restart or a lost connection the bridge simply syncs EARLY to the cube without splitting the current entry.

## Setup

1. Create an API key and secret in the EARLY web app (Settings → API).
2. `cp firmware/include/config.example.h firmware/include/config.h` and fill in WiFi, API key and secret, and the activity id for each side. List your activities with:
   ```sh
   TOKEN=$(curl -s -X POST https://api.early.app/api/v4/developer/sign-in \
     -H 'Content-Type: application/json' -d '{"apiKey":"…","apiSecret":"…"}' | jq -r .token)
   curl -s https://api.early.app/api/v4/activities -H "Authorization: Bearer $TOKEN" | jq '.activities[] | {id, name}'
   ```
3. To find which number each side reports, flash `scanner/` and flip the cube; it logs every side change over serial:
   ```sh
   cd scanner && pio run -e esp32 -t upload && pio device monitor
   ```
4. `cd firmware && pio run -t upload`

The cube connects to one device at a time, so disconnect it from the EARLY phone or desktop app first.
