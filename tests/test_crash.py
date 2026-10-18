"""The core dump box in the event log section of Settings."""

CRASH = {"task": "loopTask", "cause": "load access fault",
         "pc": "0x4201a2b4", "ra": "0x4201a290", "mtval": "0x00000000",
         "mcause": 5, "size": 9216, "elf": "3fa1c0de", "same": 1}


def crash_box(app, extra):
    app.api["system"] = dict(app.api["system"], **extra)
    app.js("showSettings()")
    app.js("updateSystem()")
    app.page.wait_for_timeout(300)

    return app.page.locator("#crashBox")


def test_crash_box_names_cause_task_and_firmware(opened):
    box = crash_box(opened, {"crash": CRASH})

    assert box.is_visible()
    text = box.inner_text()

    assert "load access fault in task loopTask at 0x4201a2b4" in text
    assert "return address 0x4201a290" in text
    assert "Written by the firmware running now (ELF 3fa1c0de)" in text
    assert box.locator("button", has_text="Download core dump").count() == 1
    assert opened.errors == []


def test_crash_from_another_firmware(opened):
    box = crash_box(opened, {"crash": dict(CRASH, same=0)})

    assert "Written by another firmware than the one running now" in \
        box.inner_text()


def test_no_box_without_a_dump(opened):
    assert not crash_box(opened, {}).is_visible()


def test_delete_asks_and_sends_erase(opened):
    asked = []

    def answer(route):
        url = route.request.url
        asked.append(url[url.index("/api/"):])
        opened.api["system"].pop("crash", None)
        route.fulfill(content_type="text/plain", body="Core dump deleted.")

    opened.page.route(opened.base + "/api/coredump*", answer)

    box = crash_box(opened, {"crash": CRASH})

    opened.page.on("dialog", lambda d: d.accept())
    opened.click_until(box.locator("button", has_text="Delete"),
                       lambda: asked)
    opened.page.wait_for_function(
        "() => document.getElementById('crashBox').style.display === 'none'",
        # The command waits for its turn in the poll loop, which on
        # all cores at once can take a while
        timeout=15000)

    assert asked == ["/api/coredump?erase=1"]
