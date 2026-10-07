#!/usr/bin/env python3
"""Query the STC documentation MCP service.

The service speaks MCP over SSE: a GET on /mcp opens the event stream, and
JSON-RPC requests are POSTed to the message endpoint whose URL the server
announces on that stream. This is enough of a client to list the manuals and
search them, which is what we need to settle an open question about the
hardware.

Usage:
    stc_mcp.py list
    stc_mcp.py search "关键词" [maxResults]
    stc_mcp.py section FILE "章节标题"
    stc_mcp.py chapters FILE
"""

import json
import sys
import threading
import time
import urllib.request

# The manuals and their search results contain non-ASCII text, and the Windows
# console defaults to a code page that cannot represent all of it. Force UTF-8
# on the output streams so printing never raises.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

BASE = "https://help.stcaimcu.com"
SSE_URL = BASE + "/mcp"


class McpClient:
    def __init__(self, timeout=45):
        self.timeout = timeout
        self.message_url = None
        self.ready = threading.Event()
        self.responses = {}
        self.lock = threading.Lock()
        self.next_id = 1
        self.stream_thread = None
        self.stream_error = None
        self.raw_events = []

    # -- SSE stream ---------------------------------------------------------
    def _open_stream(self):
        try:
            req = urllib.request.Request(SSE_URL, headers={
                "Accept": "text/event-stream",
                "User-Agent": "newisp-mcp-probe/1.0",
            })
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                event_name = None
                data_lines = []
                for raw in resp:
                    line = raw.decode("utf-8", "replace").rstrip("\r\n")
                    if line == "":
                        if data_lines:
                            self._handle_event(event_name, "\n".join(data_lines))
                        event_name = None
                        data_lines = []
                        continue
                    if line.startswith(":"):
                        continue
                    if line.startswith("event:"):
                        event_name = line[6:].strip()
                    elif line.startswith("data:"):
                        data_lines.append(line[5:].lstrip())
        except Exception as exc:                      # noqa: BLE001
            self.stream_error = exc
            self.ready.set()

    def _handle_event(self, event_name, data):
        self.raw_events.append((event_name, data[:200]))

        # The server announces where to POST messages.
        if event_name == "endpoint" or (not self.message_url and
                                        data.startswith("/")):
            url = data.strip()
            if url.startswith("/"):
                self.message_url = BASE + url
                self.ready.set()
            return

        # Anything else may be a JSON-RPC reply.
        try:
            obj = json.loads(data)
        except Exception:                              # noqa: BLE001
            return
        if isinstance(obj, dict) and "id" in obj:
            with self.lock:
                self.responses[obj["id"]] = obj

    def start(self):
        self.stream_thread = threading.Thread(target=self._open_stream,
                                              daemon=True)
        self.stream_thread.start()
        self.ready.wait(timeout=20)
        return self.message_url is not None

    # -- JSON-RPC -----------------------------------------------------------
    def call(self, method, params=None):
        if not self.message_url:
            raise RuntimeError("no message endpoint announced by the server")
        req_id = self.next_id
        self.next_id += 1
        body = {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": method,
            "params": params or {},
        }
        data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(
            self.message_url, data=data,
            headers={"Content-Type": "application/json",
                     "User-Agent": "newisp-mcp-probe/1.0"},
            method="POST")
        with urllib.request.urlopen(req, timeout=self.timeout) as resp:
            resp.read()

        deadline = time.time() + 30
        while time.time() < deadline:
            with self.lock:
                if req_id in self.responses:
                    return self.responses.pop(req_id)
            time.sleep(0.1)
        raise TimeoutError(f"no reply to {method} within 30s")

    def initialize(self):
        return self.call("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "newisp-mcp-probe", "version": "1.0"},
        })

    def list_tools(self):
        return self.call("tools/list", {})

    def call_tool(self, name, arguments):
        return self.call("tools/call", {"name": name, "arguments": arguments})


def show(label, obj):
    print(f"--- {label} ---")
    print(json.dumps(obj, ensure_ascii=False, indent=2)[:6000])
    print()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    client = McpClient()
    print(f"opening SSE stream at {SSE_URL} ...")
    if not client.start():
        print("FAILED: the server did not announce a message endpoint")
        if client.stream_error:
            print(f"  stream error: {client.stream_error}")
        if client.raw_events:
            print("  raw events seen:")
            for name, data in client.raw_events[:5]:
                print(f"    event={name!r} data={data!r}")
        return 1
    print(f"  message endpoint: {client.message_url}\n")

    cmd = sys.argv[1]

    if cmd == "init":
        show("initialize", client.initialize())
        show("tools/list", client.list_tools())
        return 0

    client.initialize()

    if cmd == "list":
        show("list_files", client.call_tool("list_files", {}))
    elif cmd == "search":
        # The server's search_keyword wants a filename; there is no documented
        # global search, so callers pass the manual to search.
        if len(sys.argv) < 4:
            print("usage: search FILE KEYWORD [maxResults]")
            return 2
        args = {"filename": sys.argv[2], "keywords": sys.argv[3]}
        if len(sys.argv) > 4:
            args["maxResults"] = int(sys.argv[4])
        show(f"search_keyword {args}", client.call_tool("search_keyword", args))
    elif cmd == "chapters":
        show("list_chapters", client.call_tool("list_chapters",
                                               {"filename": sys.argv[2]}))
    elif cmd == "section":
        show("query_section", client.call_tool("query_section",
                                               {"filename": sys.argv[2],
                                                "title": sys.argv[3]}))
    else:
        print(f"unknown command: {cmd}")
        return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
