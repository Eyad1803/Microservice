"""Tiny standard-library ASGI test client used to avoid extra test dependencies."""

from __future__ import annotations

import asyncio
import json
from dataclasses import dataclass
from typing import Any
from urllib.parse import urlsplit

from fastapi import FastAPI


@dataclass(frozen=True)
class ASGIResponse:
    status_code: int
    body: bytes
    headers: dict[str, str]

    def json(self) -> Any:
        return json.loads(self.body.decode("utf-8"))


async def _request(
    app: FastAPI,
    method: str,
    target: str,
    json_body: Any | None,
    request_headers: dict[str, str] | None,
) -> ASGIResponse:
    parsed = urlsplit(target)
    body = b"" if json_body is None else json.dumps(json_body).encode("utf-8")
    request_sent = False
    messages: list[dict[str, Any]] = []

    async def receive() -> dict[str, Any]:
        nonlocal request_sent
        if not request_sent:
            request_sent = True
            return {"type": "http.request", "body": body, "more_body": False}
        return {"type": "http.disconnect"}

    async def send(message: dict[str, Any]) -> None:
        messages.append(message)

    headers = [(b"accept", b"application/json")]
    if json_body is not None:
        headers.append((b"content-type", b"application/json"))
    if request_headers:
        headers.extend(
            (key.lower().encode("latin-1"), value.encode("latin-1"))
            for key, value in request_headers.items()
        )
    scope = {
        "type": "http",
        "asgi": {"version": "3.0", "spec_version": "2.3"},
        "http_version": "1.1",
        "method": method.upper(),
        "scheme": "http",
        "path": parsed.path,
        "raw_path": parsed.path.encode("ascii"),
        "query_string": parsed.query.encode("ascii"),
        "root_path": "",
        "headers": headers,
        "client": ("testclient", 50000),
        "server": ("testserver", 80),
    }
    await app(scope, receive, send)
    start = next(message for message in messages if message["type"] == "http.response.start")
    response_body = b"".join(
        message.get("body", b"")
        for message in messages
        if message["type"] == "http.response.body"
    )
    response_headers = {
        key.decode("latin-1"): value.decode("latin-1")
        for key, value in start.get("headers", [])
    }
    return ASGIResponse(start["status"], response_body, response_headers)


def request(
    app: FastAPI,
    method: str,
    target: str,
    json_body: Any | None = None,
    headers: dict[str, str] | None = None,
) -> ASGIResponse:
    return asyncio.run(_request(app, method, target, json_body, headers))
