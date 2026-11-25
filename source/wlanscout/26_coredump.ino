// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// Core dump
// ============================================================

/*
 * When the firmware crashes, ESP-IDF writes a core dump into the
 * coredump partition (see partitions.csv) before the chip
 * restarts: the registers and the stacks of all tasks, as ELF.
 * It stays there through any number of restarts until it is
 * deleted or a later crash replaces it. The start after a crash
 * reads its summary for the event log and the settings page;
 * /api/coredump hands out the whole image, to be decoded with
 * esp-coredump against the .elf of the firmware that crashed.
 */
struct CrashInfo
{
    bool     valid;
    bool     sameFirmware;    // the running firmware is the one that crashed
    char     task[16];
    uint32_t pc;
    uint32_t mcause;
    uint32_t mtval;
    uint32_t ra;
    uint32_t addr;            // of the image in flash, and its size
    uint32_t size;
    char     reason[96];      // ESP-IDF's panic reason, if it kept one
    char     elf[9];          // start of the crashed firmware's ELF SHA-256
};

CrashInfo crashInfo = {};

// What a RISC-V trap cause means; the top bit marks an interrupt
const char *mcauseText(uint32_t cause)
{
    if (cause & 0x80000000u)
        return "interrupt (watchdog)";

    switch (cause)
    {
        case 0:  return "instruction address misaligned";
        case 1:  return "instruction access fault";
        case 2:  return "illegal instruction";
        case 3:  return "breakpoint";
        case 4:  return "load address misaligned";
        case 5:  return "load access fault";
        case 6:  return "store address misaligned";
        case 7:  return "store access fault";
        case 8:
        case 9:
        case 11: return "environment call";
    }

    return "exception";
}

// The short cause for the event log and the page
const char *crashCauseText()
{
    return crashInfo.reason[0] ? crashInfo.reason
                               : mcauseText(crashInfo.mcause);
}

void coreDumpSetup()
{
    crashInfo = {};

    size_t addr = 0;
    size_t size = 0;

    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0)
        return;

    if (esp_core_dump_image_check() != ESP_OK)
    {
        Serial.println("Core dump: present but damaged, ignored");
        return;
    }

    // About 1.1 kB with the stack excerpt, too much for setup()'s stack
    esp_core_dump_summary_t *sum =
        (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));

    if (sum == nullptr)
        return;

    if (esp_core_dump_get_summary(sum) == ESP_OK)
    {
        crashInfo.valid = true;
        crashInfo.addr  = addr;
        crashInfo.size  = size;
        crashInfo.pc    = sum->exc_pc;
        crashInfo.mcause = sum->ex_info.mcause;
        crashInfo.mtval = sum->ex_info.mtval;
        crashInfo.ra    = sum->ex_info.ra;

        snprintf(crashInfo.task, sizeof(crashInfo.task), "%.*s",
                 (int)sizeof(sum->exc_task) - 1, sum->exc_task);

        memcpy(crashInfo.elf, sum->app_elf_sha256, 8);
        crashInfo.elf[8] = 0;

        char mine[17] = "";
        esp_app_get_elf_sha256(mine, sizeof(mine));
        crashInfo.sameFirmware = strncmp(mine, crashInfo.elf, 8) == 0;
    }

    free(sum);

    if (!crashInfo.valid)
        return;

    char reason[200] = "";

    if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) == ESP_OK)
    {
        // One line is enough, the dump has the rest
        reason[strcspn(reason, "\r\n")] = 0;
        strlcpy(crashInfo.reason, reason, sizeof(crashInfo.reason));
    }

    Serial.printf("Core dump: %s in %s at 0x%08lx (ra 0x%08lx, mtval "
                  "0x%08lx), %lu bytes, firmware %s%s\r\n",
                  crashCauseText(), crashInfo.task,
                  (unsigned long)crashInfo.pc, (unsigned long)crashInfo.ra,
                  (unsigned long)crashInfo.mtval,
                  (unsigned long)crashInfo.size, crashInfo.elf,
                  crashInfo.sameFirmware ? " (this one)" : "");

    /*
     * Logged once per dump, not on every start until it is
     * deleted: the NVS keeps a fingerprint of the last one logged.
     */
    uint32_t mark = crashInfo.pc ^ (crashInfo.mtval << 1) ^
                    (crashInfo.size << 7) ^
                    strtoul(crashInfo.elf, nullptr, 16);

    prefs.begin("analyzer", false);

    bool fresh = prefs.getUInt("cdlog", 0) != mark;

    if (fresh)
        prefs.putUInt("cdlog", mark);

    prefs.end();

    if (fresh)
        logEventFrom("system", "crash in %s at 0x%08lx: %.40s; core dump "
                     "in Settings", crashInfo.task,
                     (unsigned long)crashInfo.pc, crashCauseText());
}

/*
 * GET /api/coredump: the image as it is in flash, for
 *   esp-coredump info_corefile -t raw -c <file> wlanscout.ino.elf
 * ?erase=1 deletes it.
 */
void handleCoreDump()
{
    if (reqHasArg("erase"))
    {
        bool had = crashInfo.valid;

        esp_err_t err = esp_core_dump_image_erase();

        crashInfo = {};

        if (had)
            logRequest("core dump deleted");

        reqSend(err == ESP_OK ? 200 : 500, "text/plain",
                err == ESP_OK ? "Core dump deleted." : "Could not delete it.");
        return;
    }

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                 nullptr);

    if (!crashInfo.valid || part == nullptr ||
        crashInfo.addr < part->address ||
        crashInfo.addr + crashInfo.size > part->address + part->size)
    {
        reqSend(404, "text/plain", "No core dump stored.");
        return;
    }

    char name[48];

    snprintf(name, sizeof(name), "wlanscout-coredump-%s.bin", crashInfo.elf);

    reqStart(200, "application/octet-stream", crashInfo.size, name);

    static uint8_t part_buf[1024];

    uint32_t offset = crashInfo.addr - part->address;
    uint32_t left   = crashInfo.size;

    while (left > 0)
    {
        uint32_t n = min(left, (uint32_t)sizeof(part_buf));

        if (esp_partition_read(part, offset, part_buf, n) != ESP_OK)
            break;

        reqContent(part_buf, n);

        offset += n;
        left   -= n;
    }

    reqDone();

    lastRequestServed = millis();
}
