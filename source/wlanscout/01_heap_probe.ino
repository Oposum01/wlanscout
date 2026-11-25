// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// HEAP PROBE  (diagnostic, remove once the culprit is known)
// ============================================================

/*
 * The internal-heap low watermark drops in rare bursts that the
 * 15 s heap line cannot attribute to anything. A sampling task
 * polls the heap faster than any single request can run, so the
 * dip is recorded together with whatever was executing at the
 * time. Set HEAP_PROBE to 0 to compile all of this out.
 */
#define HEAP_PROBE          1
#define HEAP_PROBE_PERIOD_MS 1

#if HEAP_PROBE

static const char *volatile probeLabel  = "idle";
static volatile uint32_t    probeIdleSince = 0;

/*
 * The request answered last and who asked: a low labelled "idle"
 * often comes just after one, while lwIP still holds its buffers.
 */
static const char *volatile probeLast = "";
static char                 probeWho[16] = "";
static TaskHandle_t         probeTarget = nullptr;

struct HeapLowRecord
{
    uint32_t watermark;      /* the allocator's own low mark */
    uint32_t freeNow;        /* free when the drop was noticed */
    uint32_t freeDma;        /* separates DMA buffers from ordinary heap */
    uint32_t idleForMs;      /* how long no handler had been running */
    uint32_t stackWords;
    uint32_t atMillis;
    char     label[40];
    char     last[40];
    char     who[16];
    bool     fresh;
};

static volatile uint32_t probeMinFree = 0xFFFFFFFFu;
static HeapLowRecord     probeLow     = {};

/* Only report a low that is meaningfully deeper than the last one. */
#define HEAP_PROBE_STEP_BYTES 2048u

static inline uint32_t internalFree()
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/*
 * The allocator maintains this itself on every malloc and free, so it
 * catches dips far shorter than any polling interval. Sampling it
 * instead of the instantaneous free size is the whole point: we cannot
 * see the dip, but we can see that one happened, and roughly when.
 */
static inline uint32_t internalWatermark()
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                           MALLOC_CAP_8BIT);
}

/*
 * Heap snapshot. The watermark says that a dip happened, not what
 * filled the heap, and by the time loop() prints it the memory is
 * back. So while the free internal heap is HEAP_SNAP_DROP below the
 * baseline, a walk taken while it was quiet, the probe walks it at
 * once and counts the used blocks by size against the baseline, and
 * notes the tasks that were ready to run, the channel, the frames
 * the callback got per second over the last 100 to 200 ms, the fill
 * of its queues and its longest run: frames the WiFi driver holds
 * show up as many blocks of about 2.3 kB, lwIP's pbufs up to 1.8 kB,
 * a request as a few large ones. loop() prints the deepest snapshot
 * as "[heapsnap]". The walk holds the heap lock for a few ms.
 */
#define HEAP_SNAP_DROP     (30 * 1024)   // below the baseline's free
#define HEAP_SNAP_QUIET    (70 * 1024)   // free enough for a baseline
#define HEAP_SNAP_BASE_MS  60000
#define HEAP_SNAP_BUCKETS  7
#define HEAP_SNAP_TASKS    40

static const uint32_t heapSnapLimit[HEAP_SNAP_BUCKETS] = {
    128, 512, 1024, 1800, 4096, 16384, 0xFFFFFFFFu
};

static const char *const heapSnapName[HEAP_SNAP_BUCKETS] = {
    "<=128", "<=512", "<=1k", "<=1.8k", "<=4k", "<=16k", ">16k"
};

struct HeapCensus
{
    uint32_t count[HEAP_SNAP_BUCKETS];
    uint32_t bytes[HEAP_SNAP_BUCKETS];
};

// The callback's counters and the channel, see 13_rx_path.ino
extern volatile uint32_t cbMgmtFrames;
extern volatile uint32_t cbDataFrames;
extern volatile uint32_t cbCtrlFrames;
extern volatile uint32_t cbMaxUs;
extern volatile uint32_t cbTotalUs;
extern volatile uint32_t cbStaleFrames;
extern volatile uint32_t cbStaleMaxUs;
extern volatile uint32_t fcsErrors;
extern volatile uint32_t cbRxEndBad;
extern uint16_t          currentChannelIndex;
extern uint32_t          lastChannelSwitch;

/*
 * The callback's counters as they stood at the start of the last two
 * 100 ms windows, so a snapshot gives the frames of the last 100 to
 * 200 ms, and the longest callback of those two windows.
 */
#define HEAP_SNAP_WINDOW_MS 100

struct RxWindow
{
    uint32_t at;
    uint32_t mgmt;
    uint32_t data;
    uint32_t ctrl;
    uint32_t fcs;
    uint32_t stale;
    uint32_t cbTotalUs;
    uint32_t cbMaxUs;
    uint32_t staleMaxUs;
};

static RxWindow rxWindow[2] = {};

struct HeapSnapRecord
{
    uint32_t   freeNow;
    uint32_t   maxBlock;
    uint32_t   atMillis;
    uint32_t   baseFree;
    uint8_t    channel;
    uint32_t   mgmtPerSec;
    uint32_t   dataPerSec;
    uint8_t    mgmtQueue;
    uint8_t    dataQueue;
    uint32_t   cbMaxUs;
    uint32_t   ctrlPerSec;
    uint32_t   fcsPerSec;
    uint32_t   stalePerSec;
    uint32_t   staleMaxMs;
    uint32_t   switchAgoMs;
    uint32_t   cbUsPerSec;
    HeapCensus census;
    HeapCensus base;
    char       ready[96];
    char       label[40];
    bool       fresh;
};

static HeapCensus     heapSnapBase      = {};
static uint32_t       heapSnapBaseFree  = 0;
static uint32_t       heapSnapBaseAt    = 0;
static HeapSnapRecord heapSnap          = {};
static HeapCensus     heapSnapWork      = {};
static TaskStatus_t   heapSnapTasks[HEAP_SNAP_TASKS];

static bool heapSnapCount(walker_heap_into_t, walker_block_info_t block,
                          void *user)
{
    HeapCensus *c = (HeapCensus *)user;

    if (!block.used)
        return true;

    uint8_t b = 0;

    while (block.size > heapSnapLimit[b])
        b++;

    c->count[b] += 1;
    c->bytes[b] += block.size;

    return true;
}

static void heapSnapWalk(struct HeapCensus &c)
{
    memset(&c, 0, sizeof(c));
    heap_caps_walk(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, heapSnapCount, &c);
}

// The tasks ready to run besides the probe, by name
static void heapSnapReady(char *out, size_t size)
{
    UBaseType_t n = uxTaskGetSystemState(heapSnapTasks, HEAP_SNAP_TASKS,
                                         nullptr);
    size_t len = 0;

    out[0] = 0;

    for (UBaseType_t i = 0; i < n && len + 2 < size; i++)
    {
        const TaskStatus_t &t = heapSnapTasks[i];

        if (t.eCurrentState != eReady && t.eCurrentState != eRunning)
            continue;

        if (t.xHandle == xTaskGetCurrentTaskHandle() ||
            strncmp(t.pcTaskName, "IDLE", 4) == 0)
            continue;

        int w = snprintf(out + len, size - len, "%s%s",
                         len ? " " : "", t.pcTaskName);

        if (w < 0)
            break;

        len += (size_t)w;
    }

    if (len >= size)
        out[size - 1] = 0;
}

static void heapSnapLoop(uint32_t now)
{
    if (now - rxWindow[0].at >= HEAP_SNAP_WINDOW_MS)
    {
        rxWindow[1]         = rxWindow[0];
        rxWindow[0].at      = now;
        rxWindow[0].mgmt       = cbMgmtFrames;
        rxWindow[0].data       = cbDataFrames;
        rxWindow[0].ctrl       = cbCtrlFrames;
        rxWindow[0].fcs        = fcsErrors;
        rxWindow[0].stale      = cbStaleFrames;
        rxWindow[0].cbTotalUs  = cbTotalUs;
        rxWindow[0].cbMaxUs    = cbMaxUs;
        rxWindow[0].staleMaxUs = cbStaleMaxUs;
        cbMaxUs                = 0;
        cbStaleMaxUs           = 0;
    }

    uint32_t freeNow = internalFree();

    if (freeNow >= HEAP_SNAP_QUIET)
    {
        if (heapSnapBaseAt == 0 || now - heapSnapBaseAt > HEAP_SNAP_BASE_MS)
        {
            heapSnapWalk(heapSnapWork);
            heapSnapBase     = heapSnapWork;
            heapSnapBaseFree = internalFree();
            heapSnapBaseAt   = now;
        }

        return;
    }

    // Keep the deepest one until loop() has printed it
    if (heapSnapBaseAt == 0 || freeNow + HEAP_SNAP_DROP >= heapSnapBaseFree ||
        (heapSnap.fresh && freeNow >= heapSnap.freeNow))
        return;

    uint32_t span = now - rxWindow[1].at;

    if (span == 0)
        span = 1;

    heapSnap.channel    = getChannelNumber(currentChannelIndex);
    heapSnap.mgmtPerSec = (cbMgmtFrames - rxWindow[1].mgmt) * 1000 / span;
    heapSnap.dataPerSec = (cbDataFrames - rxWindow[1].data) * 1000 / span;
    heapSnap.mgmtQueue  = mgmtQueueFill();
    heapSnap.dataQueue  = dataQueueFill();
    heapSnap.cbMaxUs    = max(max((uint32_t)cbMaxUs, rxWindow[0].cbMaxUs),
                              rxWindow[1].cbMaxUs);
    heapSnap.ctrlPerSec  = (cbCtrlFrames - rxWindow[1].ctrl) * 1000 / span;
    heapSnap.fcsPerSec   = (fcsErrors - rxWindow[1].fcs) * 1000 / span;
    heapSnap.stalePerSec = (cbStaleFrames - rxWindow[1].stale) * 1000 / span;
    heapSnap.staleMaxMs  = max(max((uint32_t)cbStaleMaxUs,
                                   rxWindow[0].staleMaxUs),
                               rxWindow[1].staleMaxUs) / 1000;
    heapSnap.switchAgoMs = now - lastChannelSwitch;
    heapSnap.cbUsPerSec  = (uint32_t)((uint64_t)(cbTotalUs -
                                                 rxWindow[1].cbTotalUs) *
                                      1000 / span);

    heapSnapWalk(heapSnapWork);

    heapSnap.freeNow  = freeNow;
    heapSnap.maxBlock = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heapSnap.atMillis = now;
    heapSnap.baseFree = heapSnapBaseFree;
    heapSnap.census   = heapSnapWork;
    heapSnap.base     = heapSnapBase;

    heapSnapReady(heapSnap.ready, sizeof(heapSnap.ready));

    strlcpy(heapSnap.label, (const char *)probeLabel,
            sizeof(heapSnap.label));

    heapSnap.fresh = true;
}

// From loop(): the snapshot as one line, built without the heap
static void heapSnapPrint()
{
    if (!heapSnap.fresh)
        return;

    static char line[400];

    uint32_t sec = heapSnap.atMillis / 1000;

    int len = snprintf(line, sizeof(line),
                       "[heapsnap] free %.1f kB  maxblock %.1f kB  at "
                       "%02u:%02u:%02u  label '%s'  ch %u  rx/s mgmt %lu "
                       "data %lu ctrl %lu fcs %lu  queues %u %u  cb max %lu "
                       "us, %lu us/s  switch %lu ms ago, stale %lu/s up to "
                       "%lu ms after  vs %.1f kB free:",
                       heapSnap.freeNow / 1024.0, heapSnap.maxBlock / 1024.0,
                       (unsigned)(sec / 3600), (unsigned)((sec / 60) % 60),
                       (unsigned)(sec % 60), heapSnap.label,
                       (unsigned)heapSnap.channel,
                       (unsigned long)heapSnap.mgmtPerSec,
                       (unsigned long)heapSnap.dataPerSec,
                       (unsigned long)heapSnap.ctrlPerSec,
                       (unsigned long)heapSnap.fcsPerSec,
                       (unsigned)heapSnap.mgmtQueue,
                       (unsigned)heapSnap.dataQueue,
                       (unsigned long)heapSnap.cbMaxUs,
                       (unsigned long)heapSnap.cbUsPerSec,
                       (unsigned long)heapSnap.switchAgoMs,
                       (unsigned long)heapSnap.stalePerSec,
                       (unsigned long)heapSnap.staleMaxMs,
                       heapSnap.baseFree / 1024.0);

    for (uint8_t b = 0; b < HEAP_SNAP_BUCKETS && len > 0 &&
                        len < (int)sizeof(line); b++)
    {
        int32_t dc = (int32_t)heapSnap.census.count[b] -
                     (int32_t)heapSnap.base.count[b];
        int32_t db = (int32_t)heapSnap.census.bytes[b] -
                     (int32_t)heapSnap.base.bytes[b];

        // Only what grew by more than 1 kB
        if (db <= 1024)
            continue;

        len += snprintf(line + len, sizeof(line) - len,
                        "  %s %+ld (%+.1f kB)", heapSnapName[b],
                        (long)dc, db / 1024.0);
    }

    if (len > 0 && len < (int)sizeof(line))
        snprintf(line + len, sizeof(line) - len, "  ready: %s",
                 heapSnap.ready[0] ? heapSnap.ready : "-");

    heapSnap.fresh = false;

    Serial.println(line);
}

static void heapProbeTask(void *)
{
    for (;;)
    {
        heapSnapLoop(millis());

        uint32_t w = internalWatermark();

        if (probeMinFree - w >= HEAP_PROBE_STEP_BYTES)
        {
            uint32_t now = millis();

            probeMinFree = w;

            probeLow.watermark = w;
            probeLow.freeNow   = internalFree();
            probeLow.freeDma   = heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            probeLow.stackWords = probeTarget
                                ? uxTaskGetStackHighWaterMark(probeTarget)
                                : 0;
            probeLow.atMillis  = now;
            probeLow.idleForMs = now - probeIdleSince;

            /* Copy, do not alias: the label may change immediately. */
            strncpy(probeLow.label,
                    (const char *)probeLabel,
                    sizeof(probeLow.label) - 1);
            probeLow.label[sizeof(probeLow.label) - 1] = 0;

            strlcpy(probeLow.last, (const char *)probeLast,
                    sizeof(probeLow.last));
            strlcpy(probeLow.who, probeWho, sizeof(probeLow.who));

            probeLow.fresh = true;
        }

        vTaskDelay(pdMS_TO_TICKS(HEAP_PROBE_PERIOD_MS));
    }
}

static void startHeapProbe()
{
    /*
     * The highest priority, above the WiFi task (23): the dips last
     * less than its turn, so below it the probe only saw that one
     * had happened (2.8.29, 25 kB free and no snapshot). At each tick
     * it now looks at the heap while the WiFi task or lwIP still
     * holds its buffers, which then shows among the ready tasks; the
     * walk of a snapshot keeps them waiting a few ms.
     */
    xTaskCreate(heapProbeTask, "heapprobe", 4096, nullptr,
                configMAX_PRIORITIES - 1, nullptr);
}

/*
 * Wraps a request handler so the sampler knows what was running.
 * Also reports the handler's own net effect, which separates a
 * leak (free does not return) from a transient peak.
 */
static void runProbed(const char *name, void (*fn)())
{
    if (!probeTarget)
        probeTarget = xTaskGetCurrentTaskHandle();

    uint32_t before = internalFree();

    strlcpy(probeWho, outToUsb ? "USB" : reqClientText().c_str(),
            sizeof(probeWho));

    probeLast  = name;
    probeLabel = name;
    fn();
    probeLabel     = "idle";
    probeIdleSince = millis();

    uint32_t after = internalFree();

    /*
     * lwIP still holds the response buffers when the handler
     * returns; they are freed once the client ACKs. Anything
     * below roughly 15 kB is that, not a leak.
     */
    if (before > after && before - after > 20480)
    {
        Serial.printf("[probe] %s did not return %.1f kB\r\n",
                      name, (before - after) / 1024.0);
    }
}

#define PROBED(fn) ([]() { runProbed(#fn, fn); })
#define PROBE_MARK(name) do { probeLabel = (name); } while (0)

/*
 * A write to the flash, timed: while the flash is written or
 * erased the cache is off and every task outside IRAM stands, the
 * WiFi task among them, while frames keep arriving into the
 * driver's buffers. The line says how long it took and where the
 * heap's low stood before and after, so a dip can be put down to a
 * write or cleared of it:
 *
 *   [flash] catalog save 52 ms, low 75.3 -> 14.9 kB
 */
struct FlashTimer
{
    const char *what;
    int64_t     start;
    uint32_t    lowBefore;

    explicit FlashTimer(const char *name)
        : what(name), start(esp_timer_get_time()),
          lowBefore(internalWatermark())
    {
    }

    ~FlashTimer()
    {
        uint32_t ms  = (uint32_t)((esp_timer_get_time() - start) / 1000);
        uint32_t low = internalWatermark();

        Serial.printf("[flash] %s %lu ms, low %.1f -> %.1f kB\r\n", what,
                      (unsigned long)ms, lowBefore / 1024.0, low / 1024.0);
    }
};

#define FLASH_TIMED(name) FlashTimer flashTimer(name)

#else   /* HEAP_PROBE */

#define PROBED(fn)       (fn)
#define PROBE_MARK(name) do { } while (0)
#define FLASH_TIMED(name) do { } while (0)
static void startHeapProbe() { }
static void heapSnapPrint() { }

#endif  /* HEAP_PROBE */

#if HEAP_PROBE
/*
 * What the callback got per second since the last [heap]
 * line, by type, so the counts are there without waiting for a dip.
 */
static void heapProbeRxLine()
{
    static uint32_t at, mgmt, data, ctrl, fcs, rxend, stale, us;

    uint32_t now  = millis();
    uint32_t span = now - at;

    if (at != 0 && span > 0)
        Serial.printf("[rx] per s  mgmt %lu data %lu ctrl %lu  fcs %lu "
                      "rxend %lu  stale %lu  cb %lu us  buffers %d\r\n",
                      (unsigned long)((cbMgmtFrames - mgmt) * 1000 / span),
                      (unsigned long)((cbDataFrames - data) * 1000 / span),
                      (unsigned long)((cbCtrlFrames - ctrl) * 1000 / span),
                      (unsigned long)((fcsErrors - fcs) * 1000 / span),
                      (unsigned long)((cbRxEndBad - rxend) * 1000 / span),
                      (unsigned long)((cbStaleFrames - stale) * 1000 / span),
                      (unsigned long)((uint64_t)(cbTotalUs - us) * 1000 /
                                      span),
                      WIFI_RX_BUFFERS);

    at = now; mgmt = cbMgmtFrames; data = cbDataFrames;
    ctrl = cbCtrlFrames; fcs = fcsErrors; rxend = cbRxEndBad;
    stale = cbStaleFrames; us = cbTotalUs;
}
#endif
