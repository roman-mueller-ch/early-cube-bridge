// Copy to config.h (git-ignored) and fill in.
#pragma once

// Use a private network, not the company WiFi.
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// EARLY web app → Settings → API
#define EARLY_API_KEY ""
#define EARLY_API_SECRET ""

// Activity id for each cube side 1-8 (list them with GET /api/v4/activities).
// "" stops tracking; so does the cradle (side 0).
#define SIDE_ACTIVITIES {"", "", "", "", "", "", "", ""}
