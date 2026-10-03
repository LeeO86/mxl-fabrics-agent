#!/usr/bin/env python3
"""Minimal IS-04 Query/Registration stand-in plus one IS-05 receiver.

The agent's observer polls the Query API. Registration requests are accepted so
nmos-cpp can heartbeat without a full registry. Receiver state is mutable:

  curl -X PUT -d '{"master_enable":true,"mxl_domain_id":"...","mxl_flow_id":"..."}' \
       http://127.0.0.1:8870/control
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
QUERY_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8871
REG_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8870
NODE_ID = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
DEVICE_ID = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
RECEIVER_ID = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"

state = {
    "master_enable": False,
    "mxl_domain_id": None,
    "mxl_flow_id": None,
}
registered_nodes = {}
deleted = []


def active():
    return {
        "master_enable": state["master_enable"],
        "sender_id": None,
        "activation": {"mode": None, "requested_time": None, "activation_time": None},
        "transport_params": [
            {"mxl_domain_id": state["mxl_domain_id"], "mxl_flow_id": state["mxl_flow_id"]}
        ],
    }


def nodes():
    found = [
        {
            "id": NODE_ID,
            "version": "0:0",
            "label": "fake-receiver",
            "hostname": "localhost",
            "api": {"versions": ["v1.3"], "endpoints": [{"host": HOST, "port": QUERY_PORT, "protocol": "http"}]},
            "caps": {},
            "services": [],
            "clocks": [],
            "interfaces": [],
        }
    ]
    found.extend(registered_nodes.values())
    return found


def devices():
    return [
        {
            "id": DEVICE_ID,
            "version": "0:0",
            "label": "fake-device",
            "node_id": NODE_ID,
            "senders": [],
            "receivers": [RECEIVER_ID],
            "controls": [
                {
                    "href": f"http://{HOST}:{QUERY_PORT}/x-nmos/connection/v1.2",
                    "type": "urn:x-nmos:control:sr-ctrl/v1.2",
                }
            ],
        }
    ]


def receivers():
    return [
        {
            "id": RECEIVER_ID,
            "version": "0:0",
            "label": "fake mxl receiver",
            "device_id": DEVICE_ID,
            "transport": "urn:x-nmos:transport:mxl",
            "format": "urn:x-nmos:format:data",
            "caps": {},
            "subscription": {"sender_id": None, "active": state["master_enable"]},
        }
    ]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return

    def _json(self, code, body):
        raw = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path.endswith("/nodes"):
            return self._json(200, nodes())
        if path.endswith("/devices"):
            return self._json(200, devices())
        if path.endswith("/receivers"):
            return self._json(200, receivers())
        if path.endswith("/active"):
            return self._json(200, active())
        if path == "/control":
            return self._json(200, state)
        if path == "/deleted":
            return self._json(200, deleted)
        if path == "/x-nmos/registration/v1.3/" or path == "/x-nmos/query/v1.3/":
            return self._json(200, ["v1.3"])
        return self._json(200, [])

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        path = self.path.split("?", 1)[0]
        if path.rstrip("/").endswith("/resource"):
            try:
                body = json.loads(raw.decode() or "{}")
            except json.JSONDecodeError:
                body = {}
            if isinstance(body, dict) and body.get("type") == "node" and isinstance(body.get("data"), dict):
                data = body["data"]
                if data.get("id"):
                    registered_nodes[data["id"]] = data
            return self._json(201, {})
        self._json(200, {})

    def do_PUT(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        body = json.loads(raw.decode() or "{}")
        if self.path.split("?", 1)[0] == "/control":
            state["master_enable"] = bool(body.get("master_enable", False))
            state["mxl_domain_id"] = body.get("mxl_domain_id")
            state["mxl_flow_id"] = body.get("mxl_flow_id")
            return self._json(200, state)
        self._json(200, {})

    def do_PATCH(self):
        self.do_PUT()

    def do_DELETE(self):
        path = self.path.split("?", 1)[0]
        deleted.append(path)
        if "/nodes/" in path:
            registered_nodes.pop(path.rstrip("/").split("/")[-1], None)
        self._json(204, {})


def serve(port):
    httpd = ThreadingHTTPServer((HOST, port), Handler)
    httpd.serve_forever()


if __name__ == "__main__":
    import threading

    threading.Thread(target=serve, args=(REG_PORT,), daemon=True).start()
    serve(QUERY_PORT)
