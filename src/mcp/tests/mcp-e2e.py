#!/usr/bin/env python3
"""End-to-end tests for the Wig MCP server.

Drives a real browser over the stdio transport and asserts on what the tools
return. Everything here needs a live WebKit: the behaviour worth protecting is
DOM traversal against real layout, WebKit's load-event ordering, and how long
injected state survives, none of which survives being mocked.

Run through meson (`meson test -C _build mcp-e2e`), which wraps this in
dbus-run-session so the tests get their own session bus. That matters twice
over: a developer's own running wig is never touched, and the browser under
test is always cold-started, which is the only state in which some navigation
bugs appear at all.

Usage: mcp-e2e.py <path-to-wig> <fixtures-dir>
"""

import functools
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

TESTS = []
PROTOCOL_VERSION = "2025-11-25"
BOOT_TIMEOUT = 60
CALL_TIMEOUT = 60


def test(name):
    def register(fn):
        TESTS.append((name, fn))
        return fn

    return register


def profile_env(profile, enable_mcp):
    """Point every XDG directory into the profile, with MCP turned on or off."""
    env = dict(os.environ)
    env["XDG_DATA_HOME"] = os.path.join(profile, "data")
    env["XDG_CACHE_HOME"] = os.path.join(profile, "cache")
    env["XDG_CONFIG_HOME"] = os.path.join(profile, "config")
    env["XDG_STATE_HOME"] = os.path.join(profile, "state")

    settings_dir = os.path.join(env["XDG_CONFIG_HOME"], "com.igalia.wig")
    os.makedirs(settings_dir, exist_ok=True)
    with open(os.path.join(settings_dir, "settings.ini"), "w") as handle:
        handle.write(f"[Settings]\nenable-mcp={'true' if enable_mcp else 'false'}\n")

    return env


class Client:
    """A minimal MCP client speaking newline-delimited JSON-RPC over stdio."""

    def __init__(self, wig, profile):
        self.wig = wig
        env = profile_env(profile, enable_mcp=True)

        # A private session bus keeps the suite off any wig the developer is
        # already running, and guarantees the browser under test is cold.
        # Scoping it to this subprocess leaves stdout, which meson reads as
        # TAP, owned solely by the test script: services the bus activates
        # announce themselves on the stdout they inherit, which here is the
        # protocol stream, where non-JSON lines are ignored.
        self.log = open(os.path.join(profile, "wig.log"), "w")
        self.proc = subprocess.Popen(
            ["dbus-run-session", "--", wig, "--mcp-stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log,
            text=True,
            bufsize=1,
            env=env,
        )

        self.pending = {}
        self.signal = threading.Condition()
        self.next_id = 0
        threading.Thread(target=self._read, daemon=True).start()

        handshake = self.request(
            "initialize",
            {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "mcp-e2e", "version": "0"},
            },
            timeout=BOOT_TIMEOUT,
        )
        negotiated = handshake["result"]["protocolVersion"]
        if negotiated != PROTOCOL_VERSION:
            raise RuntimeError(f"server negotiated {negotiated}, expected {PROTOCOL_VERSION}")
        self.notify("notifications/initialized")

        tools = self.request("tools/list")["result"]["tools"]
        self.tab_tools = {tool["name"] for tool in tools
                          if "tab_handle" in tool["inputSchema"].get("required", [])}
        self.tab = None

    def _read(self):
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "id" in message:
                with self.signal:
                    self.pending[message["id"]] = message
                    self.signal.notify_all()

    def request(self, method, params=None, timeout=CALL_TIMEOUT):
        self.next_id += 1
        request_id = self.next_id
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write(json.dumps(message) + "\n")
        self.proc.stdin.flush()
        with self.signal:
            if not self.signal.wait_for(lambda: request_id in self.pending, timeout):
                raise AssertionError(f"{method} timed out after {timeout}s")
            return self.pending.pop(request_id)

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write(json.dumps(message) + "\n")
        self.proc.stdin.flush()

    def call(self, name, **arguments):
        """Invoke a tool. Returns (blocks, is_error) with blocks as text.

        A tool that requires a tab_handle is sent the client's own tab unless
        the caller names one."""
        if name in self.tab_tools and "tab_handle" not in arguments and self.tab is not None:
            arguments["tab_handle"] = self.tab
        reply = self.request("tools/call", {"name": name, "arguments": arguments})
        if "error" in reply:
            raise AssertionError(f"{name} returned a protocol error: {reply['error']}")
        result = reply["result"]
        blocks = []
        for block in result.get("content", []):
            if block.get("type") == "image":
                blocks.append(f"<image:{len(block.get('data', ''))}>")
            else:
                blocks.append(block.get("text", ""))
        return blocks, bool(result.get("isError"))

    def ok(self, name, **arguments):
        """Invoke a tool that must succeed, returning its first block."""
        blocks, is_error = self.call(name, **arguments)
        if is_error:
            raise AssertionError(f"{name} failed unexpectedly: {blocks[0][:400]}")
        return blocks[0]

    def json(self, name, **arguments):
        return json.loads(self.ok(name, **arguments))

    def close(self):
        # An open window keeps the application alive on its own, so the tabs
        # have to go before closing stdin or the browser outlives the run.
        try:
            for tab in json.loads(self.ok("list_tabs")):
                self.ok("close_tab", tab_handle=tab["tab_handle"])
        except (AssertionError, KeyError, ValueError, OSError):
            pass

        try:
            self.proc.stdin.close()
        except (OSError, ValueError):
            pass
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class QuietServer(http.server.ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        # The browser drops connections it no longer needs, such as a
        # speculative favicon fetch. A traceback here would say nothing about
        # the code under test.
        pass


def serve(directory):
    """Serve the fixtures on an ephemeral port, returning the base URL."""
    handler = functools.partial(QuietHandler, directory=directory)
    server = QuietServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_address[1]}"


def wait_until(description, predicate, timeout=15):
    """Poll until `predicate` returns a truthy value."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {description}")


def uid_of(tree, needle):
    """The handle on the first tree line mentioning `needle`."""
    for line in tree.splitlines():
        if needle in line:
            match = re.search(r"uid=(\S+)", line)
            if match:
                return match.group(1)
    raise AssertionError(f"no uid on a line containing {needle!r} in:\n{tree}")


# Must run first: the cold-start path exists only until a window and tab have
# been created, and no later test can reach it. Every later test runs in the
# tab it creates.
@test("navigate_to_url waits for the page in the first tab of a new window")
def test_cold_start_navigation(client, base):
    client.tab = json.loads(client.ok("create_tab"))["tab_handle"]
    result = json.loads(client.ok("navigate_to_url", url=f"{base}/tree.html"))
    assert result["url"] == f"{base}/tree.html", f"returned {result['url']}"
    assert result["loading"] is False, "still loading"
    assert "Probe Page" in result["text"], f"no page text: {result['text'][:200]!r}"


@test("navigate_to_url resolves a redirect")
def test_redirect(client, base):
    result = json.loads(client.ok("navigate_to_url", url=f"{base}/sub"))
    assert result["url"] == f"{base}/sub/", f"returned {result['url']}"
    assert "Redirected Page" in result["text"], result["text"][:200]


@test("textTree omits content hidden by display, visibility, or opacity")
def test_hidden_content(client, base):
    client.ok("navigate_to_url", url=f"{base}/tree.html")
    tree = client.ok("get_page_content", format="textTree", region="document")
    for marker in ("HIDDEN_BY_DISPLAY", "HIDDEN_BY_VISIBILITY", "HIDDEN_BY_OPACITY"):
        assert marker not in tree, f"{marker} leaked into the tree"
    assert "Probe Page" in tree, "visible content missing"
    # display: contents generates no box but its children are rendered.
    assert "CONTENTS_WRAPPER_CHILD" in tree, "display: contents subtree dropped"


@test("textTree reaches into open shadow roots")
def test_shadow_extraction(client, base):
    client.ok("navigate_to_url", url=f"{base}/shadow.html")
    tree = client.ok("get_page_content", format="textTree", region="document")

    assert "INSIDE_OPEN_SHADOW" in tree, "open shadow content missing"
    assert re.search(r"button uid=\S+ 'Shadow Button'", tree), "shadow control has no handle"
    assert re.search(r"button uid=\S+ 'Nested Shadow Button'", tree), "shadow root inside a shadow root missing"

    # A closed root is invisible to page script, so it must simply be absent
    # rather than half-reported.
    assert "INSIDE_CLOSED_SHADOW" not in tree, "closed shadow content should be unreachable"
    assert "Closed Button" not in tree, "closed shadow control should be unreachable"

    # Slotted light DOM renders inside the shadow tree, once, in slot position.
    assert tree.count("SLOTTED_LIGHT_TEXT") == 1, f"slotted text emitted {tree.count('SLOTTED_LIGHT_TEXT')} times"
    assert tree.count("Slotted Button") == 1, "slotted control emitted more than once"
    order = [tree.index(m) for m in ("BEFORE_SLOT", "SLOTTED_LIGHT_TEXT", "AFTER_SLOT")]
    assert order == sorted(order), f"slotted content is out of flattened-tree order: {order}"


@test("shadow content is reachable by handle and text, and says why not by selector")
def test_shadow_targeting(client, base):
    client.ok("navigate_to_url", url=f"{base}/shadow.html")
    tree = client.ok("get_page_content", format="textTree", region="document")

    blocks, is_error = client.call("page_interactions",
                                   actions=[{"type": "click", "node": uid_of(tree, "Shadow Button")}])
    assert not is_error, f"handle targeting failed: {blocks[0]}"
    assert "SHADOW_BUTTON_CLICKED" in blocks[1], "the click did not reach the shadow button"

    client.ok("navigate_to_url", url=f"{base}/shadow.html")
    blocks, is_error = client.call("page_interactions", actions=[{"type": "click", "text": "Shadow Button"}])
    assert not is_error, f"text targeting failed: {blocks[0]}"
    assert "SHADOW_BUTTON_CLICKED" in blocks[1], "text targeting did not reach the shadow button"

    # CSS has no piercing combinator, so this cannot work; it should say so
    # rather than reporting a plain miss.
    blocks, is_error = client.call("page_interactions", actions=[{"type": "click", "selector": "#shadow-button"}])
    assert is_error, "a selector should not resolve into a shadow root"
    assert "shadow root" in blocks[0], f"unhelpful selector failure: {blocks[0]}"

    blocks, is_error = client.call("page_interactions", actions=[{"type": "click", "selector": "#no-such-thing"}])
    assert is_error and "shadow root" not in blocks[0], f"a real miss should stay plain: {blocks[0]}"


@test("list_tabs names the window holding each tab")
def test_list_tabs_windows(client, base):
    client.ok("navigate_to_url", url=f"{base}/tree.html")
    before = json.loads(client.ok("list_tabs"))
    created = json.loads(client.ok("create_tab"))

    tabs = json.loads(client.ok("list_tabs"))
    assert len(tabs) == len(before) + 1, f"expected one more tab than {len(before)}, got {len(tabs)}"
    for tab in tabs:
        handle = tab.get("window_handle")
        assert isinstance(handle, int) and handle > 0, f"bad window_handle in {tab}"

    # active is scoped to a window, so every window contributes exactly one.
    for handle in {tab["window_handle"] for tab in tabs}:
        actives = [t for t in tabs if t["window_handle"] == handle and t["active"]]
        assert len(actives) == 1, f"window {handle} has {len(actives)} active tabs"

    again = json.loads(client.ok("list_tabs"))
    assert [t["window_handle"] for t in again] == [t["window_handle"] for t in tabs], "window handles not stable"

    client.ok("close_tab", tab_handle=created["tab_handle"])


@test("markdown omits script bodies and hidden content")
def test_markdown_rendering(client, base):
    client.ok("navigate_to_url", url=f"{base}/tree.html")
    markdown = client.ok("get_page_content", format="markdown")
    for marker in ("SCRIPT_SOURCE_LEAKED", "HIDDEN_BY_DISPLAY", "HIDDEN_BY_VISIBILITY", "HIDDEN_BY_OPACITY"):
        assert marker not in markdown, f"{marker} leaked into the markdown"
    assert "1800px" not in markdown, "stylesheet text leaked into the markdown"
    # display: contents generates no box, but its children are still rendered.
    assert "CONTENTS_WRAPPER_CHILD" in markdown, "display: contents subtree dropped"
    assert "# Probe Page" in markdown, "heading missing"
    assert "[Heading With A Link](" in markdown, "link missing"
    assert "**brown fox**" in markdown, "emphasis missing"


@test("textTree describes controls with labels, values, and state")
def test_control_description(client, base):
    client.ok("navigate_to_url", url=f"{base}/tree.html")
    tree = client.ok("get_page_content", format="textTree", region="document")
    assert "label='Name'" in tree, "input label missing"
    assert "value='preset'" in tree, "input value missing"
    assert "checkbox" in tree and "checked" in tree, "checkbox state missing"
    assert "value='beta'" in tree, "selected option not reported"
    assert "disabled" in tree, "disabled button not marked"
    assert "label='Close dialog'" in tree, "aria-label dropped"
    # role="button" on a div should be reported as a button, not a div.
    assert re.search(r"button uid=\S+ 'Div As Button'", tree), "role not used as kind"


@test("textTree region selects viewport or whole document")
def test_region(client, base):
    client.ok("navigate_to_url", url=f"{base}/tree.html")
    viewport = client.ok("get_page_content", format="textTree", region="viewport")
    document = client.ok("get_page_content", format="textTree", region="document")
    assert "DEEP_MARKER_BELOW_FOLD" not in viewport, "below-fold content in viewport"
    assert "DEEP_MARKER_BELOW_FOLD" in document, "below-fold content missing from document"


@test("textTree handles stay valid across extractions and scrolling")
def test_handle_stability(client, base):
    client.ok("navigate_to_url", url=f"{base}/tree.html")
    first = client.ok("get_page_content", format="textTree", region="document")
    deep = uid_of(first, "Deep Button")

    again = client.ok("get_page_content", format="textTree", region="document")
    assert uid_of(again, "Deep Button") == deep, "handle changed between extractions"

    # Scrolling changes which nodes are visible; a handle must survive that.
    client.ok("page_interactions", return_content="none",
              actions=[{"type": "scroll", "y": 2200}])
    scrolled = client.ok("get_page_content", format="textTree", region="viewport")
    assert uid_of(scrolled, "Deep Button") == deep, "handle changed after scrolling"


@test("textTree reports truncation rather than silently cutting")
def test_tree_truncation(client, base):
    client.ok("navigate_to_url", url=f"{base}/tree.html")
    tree = client.ok("get_page_content", format="textTree", region="document", max_nodes=3)
    assert "truncated=nodes" in tree, f"no truncation notice:\n{tree}"
    assert "more remain" in tree, "truncation notice does not say more remain"


@test("page_interactions targets by handle, selector, and text")
def test_targeting(client, base):
    client.ok("navigate_to_url", url=f"{base}/tree.html")
    tree = client.ok("get_page_content", format="textTree", region="document")

    blocks, is_error = client.call("page_interactions", actions=[
        {"type": "type", "node": uid_of(tree, "label='Name'"), "value": "typed here"},
        {"type": "selectOption", "node": uid_of(tree, "value='beta'"), "value": "c"},
        {"type": "click", "node": uid_of(tree, "checkbox")},
    ])
    assert not is_error, blocks[0]
    assert len(blocks) == 2, f"expected outcomes and page, got {len(blocks)} block(s)"

    after = blocks[1]
    assert "value='typed here'" in after, f"type had no effect:\n{after}"
    assert "value='gamma'" in after, f"selectOption had no effect:\n{after}"
    assert "unchecked" in after, f"click did not toggle the checkbox:\n{after}"

    assert not client.call("page_interactions", actions=[
        {"type": "click", "selector": "#deep"}])[1], "selector targeting failed"
    # Deliberately not the submit button: a form navigation left in flight
    # would follow this test into the next one.
    assert not client.call("page_interactions", actions=[
        {"type": "click", "text": "Div As Button"}])[1], "text targeting failed"


@test("page_interactions distinguishes handle failures")
def test_handle_errors(client, base):
    client.ok("navigate_to_url", url=f"{base}/tree.html")
    tree = client.ok("get_page_content", format="textTree", region="document")
    epoch = uid_of(tree, "Deep Button").split(".")[0]

    cases = [
        (f"{epoch}.0.99999", "Unknown node"),
        ("99999.0.1", "stale"),
        ("not-a-handle", "Malformed"),
    ]
    for handle, expected in cases:
        blocks, is_error = client.call("page_interactions",
                                       actions=[{"type": "click", "node": handle}])
        assert is_error, f"{handle} was not reported as an error"
        assert expected in blocks[0], f"{handle}: expected {expected!r} in {blocks[0]!r}"


@test("page_interactions reports the page after a failed action too")
def test_failure_still_reports_page(client, base):
    client.ok("navigate_to_url", url=f"{base}/tree.html")
    blocks, is_error = client.call("page_interactions",
                                   actions=[{"type": "click", "selector": "#nope"}])
    assert is_error, "missing selector was not an error"
    assert len(blocks) == 2, "page state missing from a failed batch"
    assert "Probe Page" in blocks[1], "second block is not the page"

    blocks, _ = client.call("page_interactions", return_content="none",
                            actions=[{"type": "scroll", "y": 0}])
    assert len(blocks) == 1, "return_content=none still returned the page"


@test("console captures uncaught errors and unhandled rejections")
def test_console_sources(client, base):
    client.ok("navigate_to_url", url=f"{base}/console.html")

    def by_source():
        grouped = {}
        for message in client.json("browser_console_messages", limit=100)["messages"]:
            grouped.setdefault(message.get("source"), []).append(message)
        return grouped

    # The fixture throws from a timer, which can run after the load completes.
    def captured():
        grouped = by_source()
        return grouped if any("uncaught boom" in m["text"] for m in grouped.get("exception", [])) else None

    grouped = wait_until("the uncaught exception to reach the buffer", captured)

    assert "console" in grouped, "no console entries"
    assert any("unhandled rejection here" in m["text"] for m in grouped.get("rejection", [])), \
        f"unhandled rejection not captured: {grouped}"

    levels = {m["level"] for messages in grouped.values() for m in messages}
    assert {"debug", "log", "info", "warn", "error"} <= levels, f"levels lost: {levels}"


@test("console clear discards only what it returned")
def test_console_clear_scope(client, base):
    client.ok("navigate_to_url", url=f"{base}/console.html")

    # Messages the browser produces can keep arriving between the calls, so
    # compare which messages survived rather than how many there are.
    def key(message):
        return (message["source"], message["level"], message["text"], message["timestamp"])

    before = {key(m) for m in client.json("browser_console_messages", limit=1000)["messages"]}

    drained = client.json("browser_console_messages", level_filter=["debug"], clear=True)
    assert drained["returned"] >= 1, "no debug entries to drain"
    removed = {key(m) for m in drained["messages"]}

    after = {key(m) for m in client.json("browser_console_messages", limit=1000)["messages"]}
    assert not removed & after, f"cleared messages remain: {removed & after}"
    assert before - removed <= after, f"clear dropped messages it did not return: {before - removed - after}"


@test("console reports messages the browser produces itself")
def test_browser_console_messages(client, base):
    client.ok("navigate_to_url", url=f"{base}/browser-console.html")

    def failed_load():
        messages = client.json("browser_console_messages", limit=1000)["messages"]
        found = [m for m in messages if "definitely-not-here.png" in m.get("url", "") + m["text"]]
        return found or None

    found = wait_until("the failed image load to reach the buffer", failed_load)
    message = found[0]
    assert message["source"] == "network", f"unexpected source: {message}"
    assert message["level"] == "error", f"unexpected level: {message}"
    assert "id" not in message, f"internal id leaked: {message}"

    drained = client.json("browser_console_messages", level_filter=["error"], clear=True)
    assert drained["returned"] >= 1, "nothing to clear"
    assert "cleared" not in drained, "internal cleared list leaked"
    assert failed_load() is None, "browser message survived clear"


def load_net_fixture(client, base):
    """Load the page that issues fetches, and wait for them to be recorded.

    The buffer is drained first because it outlives navigations: a POST left by
    an earlier load would satisfy the wait below before this page had issued
    anything. The fixture exposes a promise for the same purpose, but
    evaluate_javascript cannot convert one, so the requests are waited for
    where they land instead.
    """
    client.json("list_network_requests", clear=True)
    client.ok("navigate_to_url", url=f"{base}/net.html")
    wait_until(
        "the fixture's fetches to be recorded",
        lambda: client.json("list_network_requests",
                            filter={"method": "post"})["matched"] >= 1,
    )


@test("network records survive a navigation and can be filtered")
def test_network(client, base):
    load_net_fixture(client, base)
    client.ok("navigate_to_url", url=f"{base}/tree.html")

    listed = client.json("list_network_requests")
    generations = {r["navigation"] for r in listed["requests"]}
    assert len(generations) >= 2, f"records did not survive the navigation: {generations}"
    assert any("net.html" in r["url"] for r in listed["requests"]), \
        "requests from the earlier page were dropped"

    posts = client.json("list_network_requests", filter={"method": "post"})
    assert posts["matched"] >= 1, "method filter matched nothing"
    assert all(r["method"] == "POST" for r in posts["requests"]), "method filter leaked"

    failures = client.json("list_network_requests",
                           filter={"status_min": 400, "status_max": 599})
    assert failures["matched"] >= 1, "status range matched nothing"
    assert all(400 <= r["status"] <= 599 for r in failures["requests"]), "status range leaked"

    # Case-insensitive substring, matching the documented behaviour.
    upper = client.json("list_network_requests", filter={"url_substring": "DATA.JSON"})
    assert upper["matched"] >= 1, "url_substring is not case-insensitive"


@test("network clear discards only what it returned")
def test_network_clear_scope(client, base):
    load_net_fixture(client, base)

    before = client.json("list_network_requests")["matched"]
    drained = client.json("list_network_requests", filter={"method": "post"}, clear=True)
    assert drained["returned"] >= 1, "no POST to drain"

    after = client.json("list_network_requests")["matched"]
    assert after == before - drained["returned"], \
        f"expected {before - drained['returned']} records left, found {after}"
    assert client.json("list_network_requests",
                       filter={"method": "post"})["matched"] == 0, "drained POSTs remain"


@test("tools/list advertises every tool with a usable schema")
def test_tools_list(client, base):
    tools = client.request("tools/list")["result"]["tools"]
    names = [tool["name"] for tool in tools]
    assert names == sorted(names), f"tools are not in a stable order: {names}"
    assert len(names) == len(set(names)), f"duplicate tool name in {names}"

    for tool in tools:
        name = tool["name"]
        assert re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", name), f"{name} is not a valid MCP tool name"
        assert tool.get("description"), f"{name} has no description"
        schema = tool.get("inputSchema")
        assert isinstance(schema, dict), f"{name} has no inputSchema object"
        assert schema.get("type") == "object", f"{name} inputSchema is not an object schema"
        assert schema.get("additionalProperties") is False, f"{name} accepts unknown arguments"


@test("oversized text is truncated on a character boundary")
def test_text_truncation(client, base):
    client.ok("navigate_to_url", url=f"{base}/big.html")
    html = client.ok("get_page_content", format="html")
    assert "[truncated:" in html, "large HTML was not truncated"
    # The fixture is entirely multi-byte, so a careless cut lands mid-character.
    html.encode("utf-8").decode("utf-8")
    assert len(html.encode("utf-8")) < 300 * 1024, f"{len(html.encode())} bytes returned"


@test("an oversized JSON result is refused rather than cut")
def test_json_result_refused(client, base):
    client.ok("navigate_to_url", url=f"{base}/tree.html")
    blocks, is_error = client.call("evaluate_javascript", script="'x'.repeat(300000)")
    assert is_error, "oversized script result was not refused"
    assert "over the" in blocks[0], f"unhelpful message: {blocks[0]!r}"


@test("tools reject arguments they do not accept")
def test_unknown_arguments(client, base):
    blocks, is_error = client.call("get_page_content", format="textTree", maxNodes=5)
    assert is_error, "misspelled argument was accepted"
    assert "does not accept" in blocks[0], blocks[0]
    assert "max_nodes" in blocks[0], "error does not list the accepted arguments"

    blocks, is_error = client.call("list_tabs", bogus=1)
    assert is_error, "a schema with no properties accepted an argument"


@test("every tool that acts on a tab requires tab_handle")
def test_tab_handle_required(client, base):
    for tool in client.request("tools/list")["result"]["tools"]:
        if "tab_handle" in tool["inputSchema"].get("properties", {}):
            assert "tab_handle" in tool["inputSchema"].get("required", []), f"{tool['name']} defaults the tab"

    result = client.request("tools/call", {"name": "page_info", "arguments": {}})["result"]
    assert result.get("isError"), "page_info ran without a tab_handle"
    assert "tab_handle" in result["content"][0]["text"], result["content"][0]["text"]

    blocks, is_error = client.call("page_info", tab_handle="1")
    assert is_error, "a string tab_handle was accepted"
    assert "Invalid tab_handle" in blocks[0], blocks[0]


@test("--mcp-stdio is refused while MCP is disabled in settings")
def test_disabled(client, base):
    profile = tempfile.mkdtemp(prefix="wig-mcp-e2e-disabled-")
    try:
        result = subprocess.run(
            ["dbus-run-session", "--", client.wig, "--mcp-stdio"],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=BOOT_TIMEOUT,
            env=profile_env(profile, enable_mcp=False),
        )
    finally:
        shutil.rmtree(profile, ignore_errors=True)

    assert result.returncode != 0, "a disabled MCP server still started"
    assert "MCP is disabled" in result.stderr, f"unhelpful message: {result.stderr!r}"


def main():
    if len(sys.argv) != 3:
        print("Bail out! usage: mcp-e2e.py <wig> <fixtures>")
        return 99

    wig, fixtures = sys.argv[1], sys.argv[2]
    if not os.access(wig, os.X_OK):
        print(f"Bail out! {wig} is not executable")
        return 99
    if not (os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY")):
        print("1..0 # SKIP no display available for the browser under test")
        return 0
    if not shutil.which("dbus-run-session"):
        print("1..0 # SKIP dbus-run-session is needed to isolate the session bus")
        return 0

    profile = tempfile.mkdtemp(prefix="wig-mcp-e2e-")
    staging = os.path.join(profile, "fixtures")
    shutil.copytree(fixtures, staging)
    paragraph = "<p>" + ("日本語テキスト—émoji—ünïcödé " * 40) + "</p>\n"
    with open(os.path.join(staging, "big.html"), "w", encoding="utf-8") as handle:
        handle.write('<!DOCTYPE html><html><head><meta charset="utf-8">'
                     "<title>Big</title></head><body>\n")
        handle.write(paragraph * 400)
        handle.write("</body></html>\n")

    server, base = serve(staging)
    failures = 0

    try:
        client = Client(wig, profile)
    except Exception as error:  # noqa: BLE001 - a boot failure must not look like a pass
        print(f"Bail out! could not start {wig}: {error}")
        server.shutdown()
        return 99

    print(f"1..{len(TESTS)}")
    try:
        for index, (name, fn) in enumerate(TESTS, start=1):
            try:
                fn(client, base)
                print(f"ok {index} - {name}")
            except Exception as error:  # noqa: BLE001 - report, do not abort the run
                failures += 1
                print(f"not ok {index} - {name}")
                for line in str(error).splitlines():
                    print(f"#   {line}")
    finally:
        client.close()
        server.shutdown()
        if failures:
            print(f"# browser log: {os.path.join(profile, 'wig.log')}")
        else:
            shutil.rmtree(profile, ignore_errors=True)

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
