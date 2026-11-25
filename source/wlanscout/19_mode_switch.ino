// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

void applyPendingMode()
{
    if (pendingMode >= 0)
    {
        PROBE_MARK("mode switch");

        int8_t wanted = pendingMode;
        bool   asked  = pendingModeAsked;

        pendingMode      = -1;
        pendingModeAsked = false;

        if (wanted == MODE_BLE)
            enterBleMode();
        else if (wanted == MODE_ZB)
            enterZbMode();
        else
            enterWifiMode();

        // Kept for the next start: a mode a page picked and that
        // came up, not the automatic return to Wi-Fi
        if (keepMode && asked && scanMode != savedMode)
        {
            savedMode = scanMode;
            saveSettings();
        }
    }

    if (pendingZbScan >= 0)
    {
        uint8_t wanted = (uint8_t)pendingZbScan;

        pendingZbScan = -1;

        zbScanMode = wanted;

        /*
         * Switch the sweep, do NOT cycle the radio. Disabling
         * and re-enabling threw away every reading and looked
         * like the mode had not changed at all.
         */
        if (scanMode == MODE_ZB && zbEnabled)
        {
            zbNote[0] = 0;

            zbEdPending = false;
            zbEdReady   = false;
            zbEdResult  = 127;

            zbChannelIndex = 0;

            for (int i = 0; i < ZB_NUM_CHANNELS; i++)
                edCurrent[i] = 127;

            edRateStart = millis();
            edRateCount = 0;

            armEdWatch();

#if HAVE_IEEE802154
            if (zbScanMode == ZB_SCAN_ENERGY)
            {
                esp_ieee802154_sleep();
                esp_ieee802154_set_channel(zbChannelNumber(0));

                zbEdLast = millis();

                Serial.println("802.15.4: switched to energy sweep");
            }
            else
            {
                zbSelectChannel(0);

                Serial.println("802.15.4: switched to frame capture");
            }
#endif
        }
    }

    if (pendingZbChannel != -2)
    {
        int8_t wanted = pendingZbChannel;

        pendingZbChannel = -2;

        zbLockedChannel = wanted;

        if (scanMode == MODE_ZB && zbEnabled)
        {
            if (wanted >= 0)
                zbSelectChannel(wanted - ZB_FIRST_CHANNEL);
        }
    }

    if (pendingActive)
    {
        pendingActive = false;

        if (scanMode == MODE_BLE && bleScan != nullptr)
        {
            stopBleScan();
            startBleScan();
        }
    }
}
