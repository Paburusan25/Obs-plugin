#!/usr/bin/env python3
"""Servidor falso compatible con /v1/chat/completions para probar el motor sin internet.
Modos (variable MOCK_MODE): ok | error500 | fence | truncated
Escribe cada petición recibida en el archivo MOCK_LOG (una línea JSON por petición)."""
import json, os, sys
from http.server import BaseHTTPRequestHandler, HTTPServer

MODE = os.environ.get("MOCK_MODE", "ok")
LOG = os.environ.get("MOCK_LOG", "/dev/null")


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n))
        with open(LOG, "a") as f:
            f.write(json.dumps({"auth": self.headers.get("Authorization"), "path": self.path, "body": body}) + "\n")
        user = body["messages"][1]["content"]
        doc = user.split("DOCUMENTO ACTUAL:\n<<<\n", 1)[1].split("\n>>>", 1)[0]
        new = user.split("NUEVA TRANSCRIPCIÓN:\n<<<\n", 1)[1].split("\n>>>", 1)[0]

        if MODE == "error500":
            self.send_response(500)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"error": {"message": "boom"}}).encode())
            return

        if MODE == "truncated":
            out = "# solo"
        else:
            out = doc.replace("## Sin clasificar",
                              "## Unidad de competencia: Prueba\n### Criterios de desempeño\n- " + new.replace("\n", " ") +
                              "\n\n## Sin clasificar")
        if MODE == "fence":
            out = "```markdown\n" + out + "\n```"
        resp = {"choices": [{"message": {"role": "assistant", "content": out}}]}
        data = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
