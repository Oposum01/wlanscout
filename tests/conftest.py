"""
Browser tests for the web page in index_html.h.

The page is taken straight out of index_html.h and loaded into a
Playwright browser. Every request goes to a fake host and is
answered here, /api/... from the JSON files in fixtures/, so no
sensor is needed. A test can swap a fixture before the page loads.

    pip install -r tests/requirements.txt
    python -m playwright install firefox
    pytest tests

Firefox is the default because it is the browser the page is used
with on phones; --engine chromium switches (the option names avoid those of
pytest-playwright).
"""

import json
import pathlib
import re
import subprocess
import sys

import pytest
from playwright.sync_api import sync_playwright

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"
BASE = "http://wlanscout.test"

# The same page as a static copy, as from github.io: it talks USB
STATIC_BASE = "https://wlanscout.test"


def page_html():
    """The page as the firmware serves it."""
    source = (ROOT / "source" / "wlanscout" / "index_html.h").read_text(
        encoding="utf-8")

    match = re.search(r'R"HTML\((.*?)\)HTML"', source, re.S)

    assert match, "no R\"HTML(...)HTML\" block in index_html.h"

    return match.group(1)


def load_fixtures():
    return {p.stem: json.loads(p.read_text(encoding="utf-8"))
            for p in FIXTURES.glob("*.json")}


def pytest_addoption(parser):
    parser.addoption("--engine", default="firefox",
                     choices=["firefox", "chromium"],
                     help="browser engine for the page tests")
    parser.addoption("--browser-path", default=None,
                     help="browser executable instead of Playwright's")


# Tests whose timing does not hold up beside a dozen other browsers:
# with -n they are left out of the parallel run and run on their own
# right after it
SERIAL = ["test_eventlog.py", "test_graph_pin.py", "test_graphs.py"]


def parallel(config):
    return bool(getattr(config.option, "numprocesses", None))


def serial_files(config):
    """The SERIAL files among what the run was asked for."""
    here = pathlib.Path(__file__).resolve().parent
    asked = [pathlib.Path(a.split("::")[0]).resolve() for a in config.args]

    return [str(here / f) for f in SERIAL
            if any(a == here / f or a == here or a in (here / f).parents
                   for a in asked)]


def pytest_collection_modifyitems(config, items):
    # Only in an xdist worker; the run without -n keeps them
    if not hasattr(config, "workerinput"):
        return

    keep = [i for i in items if i.path.name not in SERIAL]

    config.hook.pytest_deselected(
        items=[i for i in items if i.path.name in SERIAL])
    items[:] = keep


def pytest_sessionfinish(session, exitstatus):
    config = session.config

    if hasattr(config, "workerinput") or not parallel(config):
        return

    files = serial_files(config)

    if not files:
        return

    command = [sys.executable, "-m", "pytest", "-q", "-p",
               "no:cacheprovider", "--engine",
               config.getoption("--engine")]

    if config.getoption("--browser-path"):
        command += ["--browser-path", config.getoption("--browser-path")]

    print("\nOn their own after the parallel run: " +
          ", ".join(pathlib.Path(f).name for f in files), flush=True)

    result = subprocess.run(command + files)

    if result.returncode != 0:
        session.exitstatus = pytest.ExitCode.TESTS_FAILED


@pytest.fixture(scope="session")
def browser(request):
    name = request.config.getoption("--engine")
    path = request.config.getoption("--browser-path")

    with sync_playwright() as p:
        engine = getattr(p, name)
        b = engine.launch(executable_path=path) if path else engine.launch()

        yield b

        b.close()


class App:
    """
    The loaded page. api holds the JSON answered per endpoint
    ("networks" for /api/networks), errors the script errors seen.
    """

    def __init__(self, page, api, base=BASE):
        self.page = page
        self.api = api
        self.base = base
        self.errors = []

        # API requests that went over HTTP
        self.http_api = []

        # GitHub Pages as the update check sees it: {"version.json": dict,
        # "wlanscout.ino.bin": bytes}; nothing there answers 404, so no
        # test goes out to the real one
        self.updates = {}
        self.update_requests = []

        page.on("pageerror", lambda e: self.errors.append(str(e)))
        page.route(base + "/**", self._answer)
        page.route("https://oposum01.github.io/**", self._update)

    def _update(self, route):
        name = route.request.url.split("/")[-1].split("?")[0]
        self.update_requests.append(name)
        body = self.updates.get(name)

        if body is None:
            route.fulfill(status=404, body="")
        elif isinstance(body, bytes):
            route.fulfill(content_type="application/octet-stream", body=body,
                          headers={"Access-Control-Allow-Origin": "*"})
        else:
            route.fulfill(content_type="application/json",
                          body=json.dumps(body),
                          headers={"Access-Control-Allow-Origin": "*"})

    def _answer(self, route):
        path = route.request.url[len(self.base):].split("?")[0]

        if path in ("/", "/index.html"):
            route.fulfill(content_type="text/html", body=page_html())
        elif path.startswith("/api/"):
            self.http_api.append(path)
            body = self.api.get(path[len("/api/"):], {"ok": 1})
            route.fulfill(content_type="application/json",
                          body=json.dumps(body))
        else:
            route.fulfill(status=404, body="")

    def open(self):
        self.page.goto(self.base + "/")

        # The poll loop fetches networks only every fourth round,
        # fetch them once right away instead of waiting for it
        self.page.wait_for_function("() => typeof networks !== 'undefined'")
        self.page.evaluate("updateNetworks()")
        self.page.wait_for_function("() => networks.length > 0")

        return self

    def js(self, expression, arg=None):
        return self.page.evaluate(expression, arg)

    def click_until(self, locator, done, tries=5, wait_ms=1500):
        """
        Click, and click again if nothing came of it: the page draws
        its tables and boxes anew on every poll, and a click landing
        just then hits an element already replaced.
        """
        for _ in range(tries):
            locator.click()

            for _ in range(wait_ms // 100):
                if done():
                    return

                self.page.wait_for_timeout(100)

        assert done()

    def tab(self, label):
        self.page.locator(".tabs .tab", has_text=label).click()

        # Same for the catalog, polled every tenth round
        if label == "IE catalog":
            self.page.evaluate("updateCatalog()")


@pytest.fixture
def app(browser):
    """
    A page with the default fixtures, not opened yet, so a test
    can change app.api first; call app.open().
    """
    context = browser.new_context(viewport={"width": 1400, "height": 900})
    page = context.new_page()

    yield App(page, load_fixtures())

    context.close()


@pytest.fixture
def opened(app):
    """A page opened with the default fixtures."""
    return app.open()


@pytest.fixture
def phone(browser):
    """A page opened at phone width, 390 px like most phones."""
    context = browser.new_context(viewport={"width": 390, "height": 800},
                                  has_touch=True)
    page = context.new_page()

    yield App(page, load_fixtures()).open()

    context.close()


@pytest.fixture
def usb(browser):
    """
    The page as a static https copy with a simulated sensor on
    WebUSB (fake_usb.js), not connected yet: press the USB button.
    """
    context = browser.new_context(viewport={"width": 1400, "height": 900},
                                  accept_downloads=True)
    fixtures = load_fixtures()

    context.add_init_script(
        script="window.__usbFixtures = " + json.dumps(fixtures) + ";")
    context.add_init_script(
        path=str(pathlib.Path(__file__).resolve().parent / "fake_usb.js"))

    page = context.new_page()
    app = App(page, fixtures, base=STATIC_BASE)

    page.goto(STATIC_BASE + "/")
    page.wait_for_function("() => typeof networks !== 'undefined'")

    yield app

    context.close()
