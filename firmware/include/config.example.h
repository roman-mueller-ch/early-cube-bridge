// Copy to config.h (git-ignored) and fill in.
#pragma once

// WiFi networks to try, e.g. home and work; the strongest one in range wins.
// 2.4 GHz WPA2-Personal only (no captive portals or 802.1X login).
#define WIFI_NETWORKS {      \
    {"", ""}, /* home */    \
    {"", ""}, /* work */    \
}

// EARLY web app → Settings → API
#define EARLY_API_KEY ""
#define EARLY_API_SECRET ""

// Activity id for each cube side 1-8 (list them with GET /api/v4/activities).
// "" stops tracking; so does the cradle (side 0).
#define SIDE_ACTIVITIES {"", "", "", "", "", "", "", ""}
