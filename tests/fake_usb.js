/*
 * A stand-in for WebUSB with the sensor on its USB port, for the
 * tests of the USB transport. It answers the request lines the
 * way the firmware does (see "Requests over Ethernet or USB" in
 * wlanscout.ino), from the same fixtures the HTTP tests use, set
 * as window.__usbFixtures by an init script before this one.
 *
 * Answers go out in pieces of 50 bytes with an empty packet after
 * each, as the ESP's USB Serial/JTAG ends its transfers that way.
 *
 * window.__usbRequests  every request line received
 * window.__usbControl   the last control transfer (DTR/RTS)
 * window.__usbUnplug()  fires the disconnect event
 * window.__usbPlugIn()  fires the connect event
 *
 * A firmware (PUT /api/ota) whose first byte is an image's 0xE9 is
 * taken as version 9.9.9: the answer says so, then the sensor
 * "restarts", gone from USB and back 300 ms later, /api/system now
 * answering 9.9.9.
 */
(() => {
    const fixtures = window.__usbFixtures || {};
    const requests = window.__usbRequests = [];

    const pending = [];
    const waiters = [];

    function flush()
    {
        while (waiters.length && pending.length)
            waiters.shift()(pending.shift());
    }

    function push(bytes)
    {
        pending.push(bytes);
        flush();
    }

    function frame(status, type, filename, body)
    {
        const head = new TextEncoder().encode(
            "\x02" + status + " " + type + (filename ? " " + filename : "") +
            "\n");

        const parts = [head];

        for (let i = 0; i < body.length; i += 1000)
        {
            const part = body.slice(i, i + 1000);

            parts.push(new Uint8Array([part.length & 0xff, part.length >> 8]));
            parts.push(part);
        }

        parts.push(new Uint8Array([0, 0]));

        const total = parts.reduce((n, p) => n + p.length, 0);
        const out = new Uint8Array(total);

        let at = 0;

        parts.forEach(p => { out.set(p, at); at += p.length; });

        return out;
    }

    let put = null;

    window.__usbPuts = [];

    function finishPut(p)
    {
        requests.push(p.line);

        const size = p.chunks.reduce((n, c) => n + c.length, 0);
        const body = new Uint8Array(size);
        let at = 0;

        p.chunks.forEach(c => { body.set(c, at); at += c.length; });

        if (p.line.startsWith("PUT /api/ota"))
        {
            window.__usbPuts.push({ line: p.line, size: body.length,
                                    first: body[0] });

            const ok = body[0] === 0xE9;
            const json = ok ? { ok: 1, version: "9.9.9", bytes: body.length }
                            : { ok: 0, error: "not an ESP32-C5 firmware image" };

            setTimeout(() => push(frame(200, "application/json", "",
                new TextEncoder().encode(JSON.stringify(json)))), 0);

            if (ok)
                setTimeout(() =>
                {
                    fixtures.system = Object.assign({}, fixtures.system,
                                                    { version: "9.9.9" });
                    window.__usbUnplug();
                    setTimeout(() => window.__usbPlugIn(), 300);
                }, 200);

            return;
        }

        const text = new TextDecoder().decode(body);

        window.__usbPuts.push({ line: p.line, body: text });

        const ok = text.startsWith('{"wlanscout_snapshot":1');
        const json = ok ? { ok: 1, seq: 20 + window.__usbPuts.length }
                        : { ok: 0, error: "not a wlanscout+ snapshot" };

        const bytes = frame(200, "application/json", "",
                            new TextEncoder().encode(JSON.stringify(json)));

        setTimeout(() => push(bytes), 0);
    }

    function answer(line)
    {
        requests.push(line);

        const url = line.split(" ")[1] || "";
        const path = url.split("?")[0];

        let bytes;

        if (path === "/api/pcap" || path === "/api/catalog.pcap")
        {
            // A pcap global header is enough to tell it arrived whole
            const body = new Uint8Array(24);

            body.set([0xd4, 0xc3, 0xb2, 0xa1, 2, 0, 4, 0]);

            bytes = frame(200, "application/vnd.tcpdump.pcap",
                          path === "/api/pcap" ? "capture.pcap"
                                               : "element-catalog.pcap",
                          body);
        }
        else if (path === "/api/nope")
        {
            bytes = frame(404, "text/plain", "",
                          new TextEncoder().encode("Not found"));
        }
        else
        {
            const key = path.replace("/api/", "");
            const json = key in fixtures ? fixtures[key] : { ok: 1 };

            bytes = frame(200, "application/json", "",
                          new TextEncoder().encode(JSON.stringify(json)));
        }

        for (let i = 0; i < bytes.length; i += 50)
        {
            push(bytes.slice(i, i + 50));
            push(new Uint8Array(0));
        }
    }

    const configuration = {
        interfaces: [
            { interfaceNumber: 0, alternates: [{ interfaceClass: 2,
              endpoints: [{ direction: "in", endpointNumber: 3,
                            packetSize: 64 }] }] },
            { interfaceNumber: 1, alternates: [{ interfaceClass: 10,
              endpoints: [{ direction: "in", endpointNumber: 1,
                            packetSize: 64 },
                          { direction: "out", endpointNumber: 2,
                            packetSize: 64 }] }] }
        ]
    };

    const device = {
        vendorId: 0x303a,
        productId: 0x1001,
        productName: "USB JTAG/serial debug unit",
        configuration: null,

        async open() { },
        async close() { },
        async selectConfiguration() { this.configuration = configuration; },
        async claimInterface() { },

        async controlTransferOut(setup)
        {
            window.__usbControl = setup;
            return { status: "ok" };
        },

        /*
         * A request line, or for a PUT the line and then its bytes,
         * possibly spread over several transfers. The PUTs are kept
         * in window.__usbPuts as {line, body} for the tests.
         */
        async transferOut(endpoint, data)
        {
            const bytes = new Uint8Array(data.buffer || data,
                                         data.byteOffset || 0,
                                         data.byteLength);

            if (put)
            {
                put.chunks.push(bytes.slice());
                put.left -= bytes.length;

                /*
                 * As the sensor: more than its receive queue holds
                 * without a go-ahead in between is lost there, so the
                 * test fails; each whole piece taken gets a go-ahead,
                 * but the last
                 */
                put.unacked += bytes.length;

                if (put.unacked > 4096)
                    window.__usbOverrun = true;

                while (put.unacked >= 2048 && put.left > 0)
                {
                    put.unacked -= 2048;
                    setTimeout(() => push(new Uint8Array([0x06])), 0);
                }

                if (put.left <= 0)
                {
                    const done = put;

                    put = null;
                    finishPut(done);
                }

                return { status: "ok", bytesWritten: bytes.length };
            }

            const line = new TextDecoder().decode(bytes).trim();

            if (line.startsWith("PUT "))
            {
                const len = Number((line.match(/[?&]len=(\d+)/) || [])[1] || 0);

                put = { line, left: len, chunks: [], unacked: 0 };

                if (len === 0)
                {
                    const done = put;

                    put = null;
                    finishPut(done);
                }

                return { status: "ok", bytesWritten: bytes.length };
            }

            setTimeout(() => answer(line), 0);

            return { status: "ok", bytesWritten: bytes.length };
        },

        transferIn(endpoint, length)
        {
            return new Promise(resolve =>
            {
                waiters.push(bytes => resolve(bytes === null
                    ? { status: "stall" }
                    : { status: "ok",
                        data: new DataView(bytes.buffer, bytes.byteOffset,
                                           bytes.byteLength) }));

                flush();
            });
        }
    };

    const listeners = {};

    const usb = {
        async requestDevice(options)
        {
            window.__usbFilters = options.filters;
            return device;
        },

        addEventListener(type, fn)
        {
            (listeners[type] = listeners[type] || []).push(fn);
        }
    };

    // Unplugged, the reads still waiting end as on a real device
    window.__usbUnplug = () =>
    {
        waiters.splice(0).forEach(w => w(null));
        (listeners.disconnect || []).forEach(fn => fn({ device }));
    };

    window.__usbPlugIn = () =>
        (listeners.connect || []).forEach(fn => fn({ device }));

    Object.defineProperty(navigator, "usb",
                          { value: usb, configurable: true });
})();
