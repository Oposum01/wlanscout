// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// HTML
//
// The page lives in index_html.h. Keeping a raw string this
// large in the .ino made the Arduino prototype generator cut
// it in half, so it was moved out of the way.
//
// Only the web server hands it out: a build without Ethernet
// leaves it out, the page then comes from the static site.
// ============================================================

#if ENABLE_ETHERNET

#include "index_html.h"
#include "index_html_gz.h"

// The page goes out packed (index_html_gz.h); the unpacked one is the
// source, and the copy has to be of this very page
static_assert(sizeof(INDEX_HTML) == INDEX_HTML_SOURCE_SIZE,
              "index_html.h changed: run tools/make_page_gz.py");


// ============================================================
// HTTP handlers
// ============================================================

void handleFavicon()
{
    lastRequestServed = millis();

    server.send_P(200, "image/svg+xml", FAVICON_SVG);
}

void handleRoot()
{
    lastRequestServed = millis();

    noteWebClient((uint32_t)server.client().remoteIP());

    // Packed, a quarter of the size: every browser unpacks gzip
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char *)INDEX_HTML_GZ,
                  sizeof(INDEX_HTML_GZ));
}

#endif

void handleNotFound()
{
    reqSend(404, "text/plain", "Not found");
}
