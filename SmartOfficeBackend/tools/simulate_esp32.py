"""Development-only HTTP simulator for the future Smart Office ESP32 client."""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any
from uuid import uuid4


FINGERPRINT_RESULTS = (
    "MATCH",
    "NOT_RECOGNIZED",
    "TIMEOUT",
    "READ_ERROR",
    "UNAVAILABLE",
)
DOOR_RESULTS = ("DOOR_OPENED", "DOOR_OPEN_FAILED")
PRESENCE_VALUES = ("true", "false", "unavailable")


class SimulatorHttpError(RuntimeError):
    def __init__(self, status: int | None, message: str) -> None:
        super().__init__(message)
        self.status = status


@dataclass(frozen=True)
class SimulatorConfig:
    base_url: str
    boot_id: str
    fingerprint_result: str
    fingerprint_id: int
    presence: bool | None
    distance_cm: float | None
    door_result: str
    door_state: str
    admin_mode: bool
    heartbeat_seconds: float
    scan_delay_seconds: float
    door_delay_seconds: float

    @property
    def fingerprint_ready(self) -> bool:
        return self.fingerprint_result != "UNAVAILABLE"


def parse_presence(value: str) -> bool | None:
    return {"true": True, "false": False, "unavailable": None}[value]


def post_json(base_url: str, path: str, body: dict[str, Any]) -> dict[str, Any]:
    request = urllib.request.Request(
        f"{base_url.rstrip('/')}{path}",
        data=json.dumps(body).encode("utf-8"),
        headers={"Accept": "application/json", "Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raw_body = exc.read().decode("utf-8", errors="replace")
        try:
            error_body = json.loads(raw_body)
            detail = error_body.get("detail", error_body)
            message = json.dumps(detail, ensure_ascii=False)
        except json.JSONDecodeError:
            message = raw_body or exc.reason
        raise SimulatorHttpError(exc.code, message) from exc
    except (urllib.error.URLError, TimeoutError) as exc:
        raise SimulatorHttpError(None, f"Cannot connect to backend: {exc}") from exc


def heartbeat_body(config: SimulatorConfig, active_request_id: str | None) -> dict[str, Any]:
    return {
        "boot_id": config.boot_id,
        "door_state": config.door_state,
        "person_detected": config.presence,
        "distance_cm": config.distance_cm if config.presence is not None else None,
        "fingerprint_ready": config.fingerprint_ready,
        "admin_mode": config.admin_mode,
        "active_request_id": active_request_id,
    }


def check_body(config: SimulatorConfig, request_id: str) -> dict[str, Any]:
    body: dict[str, Any] = {
        "request_id": request_id,
        "precheck": {
            "door_state": config.door_state,
            "person_detected": config.presence,
            "distance_cm": config.distance_cm if config.presence is not None else None,
            "fingerprint_ready": config.fingerprint_ready,
        },
        "fingerprint_result": config.fingerprint_result,
    }
    if config.fingerprint_result == "MATCH":
        body["fingerprint_id"] = config.fingerprint_id
    return body


def process_command(config: SimulatorConfig, command: dict[str, Any]) -> None:
    request_id = str(command["request_id"])
    area_id = int(command["area_id"])
    direction = str(command["direction"])
    print(f"Command received: {request_id} area={area_id} direction={direction}")

    post_json(
        config.base_url,
        "/api/esp32/heartbeat",
        heartbeat_body(config, request_id),
    )
    print(f"Command acknowledged: {request_id}")
    time.sleep(config.scan_delay_seconds)

    checked = post_json(
        config.base_url,
        "/api/access/check",
        check_body(config, request_id),
    )
    print(
        "Access check: "
        f"status={checked.get('status')} reason={checked.get('reason_code')}"
    )

    if checked.get("status") != "AUTHORIZED_WAITING_DOOR":
        return

    time.sleep(config.door_delay_seconds)
    completed = post_json(
        config.base_url,
        "/api/access/complete",
        {"request_id": request_id, "door_result": config.door_result},
    )
    print(
        "Door completion: "
        f"status={completed.get('status')} reason={completed.get('reason_code')}"
    )


def run(config: SimulatorConfig) -> None:
    print(f"Simulated ESP32 boot ID: {config.boot_id}")
    print(f"Backend: {config.base_url}")
    print("Waiting for access commands. Press Ctrl+C to stop.")
    active_request_id: str | None = None

    while True:
        started_at = time.monotonic()
        try:
            response = post_json(
                config.base_url,
                "/api/esp32/heartbeat",
                heartbeat_body(config, active_request_id),
            )
            command = response.get("command")
            if command is not None:
                active_request_id = str(command["request_id"])
                try:
                    process_command(config, command)
                finally:
                    active_request_id = None
        except SimulatorHttpError as exc:
            status = f"HTTP {exc.status}" if exc.status is not None else "connection error"
            print(f"Simulator {status}: {exc}", file=sys.stderr)

        elapsed = time.monotonic() - started_at
        time.sleep(max(0.0, config.heartbeat_seconds - elapsed))


def parse_args() -> SimulatorConfig:
    parser = argparse.ArgumentParser(
        description="Simulate one ESP32 station through the real Phase 2 HTTP API."
    )
    parser.add_argument("--base-url", default="http://127.0.0.1:8000")
    parser.add_argument("--boot-id", default=f"phase3-sim-{uuid4().hex}")
    parser.add_argument(
        "--fingerprint-result",
        choices=FINGERPRINT_RESULTS,
        default="MATCH",
    )
    parser.add_argument("--fingerprint-id", type=int, choices=range(1, 7), default=1)
    parser.add_argument("--presence", choices=PRESENCE_VALUES, default="true")
    parser.add_argument("--distance-cm", type=float, default=11.8)
    parser.add_argument("--door-result", choices=DOOR_RESULTS, default="DOOR_OPENED")
    parser.add_argument("--door-state", choices=("OPEN", "CLOSED"), default="CLOSED")
    parser.add_argument(
        "--admin-mode",
        choices=("true", "false"),
        default="false",
        help="Mirror the simulator's local Admin Mode state in heartbeats.",
    )
    parser.add_argument("--heartbeat-seconds", type=float, default=2.0)
    parser.add_argument(
        "--scan-delay-seconds",
        type=float,
        default=1.5,
        help="Pause after command acknowledgement so IN_PROGRESS is visible.",
    )
    parser.add_argument(
        "--door-delay-seconds",
        type=float,
        default=1.5,
        help="Pause after authorization so AUTHORIZED_WAITING_DOOR is visible.",
    )
    args = parser.parse_args()

    if args.distance_cm < 0:
        parser.error("--distance-cm must be non-negative")
    if args.heartbeat_seconds <= 0:
        parser.error("--heartbeat-seconds must be greater than zero")
    if args.scan_delay_seconds < 0 or args.door_delay_seconds < 0:
        parser.error("simulated stage delays must be non-negative")

    return SimulatorConfig(
        base_url=args.base_url.rstrip("/"),
        boot_id=args.boot_id,
        fingerprint_result=args.fingerprint_result,
        fingerprint_id=args.fingerprint_id,
        presence=parse_presence(args.presence),
        distance_cm=args.distance_cm,
        door_result=args.door_result,
        door_state=args.door_state,
        admin_mode=args.admin_mode == "true",
        heartbeat_seconds=args.heartbeat_seconds,
        scan_delay_seconds=args.scan_delay_seconds,
        door_delay_seconds=args.door_delay_seconds,
    )


def main() -> int:
    config = parse_args()
    try:
        run(config)
    except KeyboardInterrupt:
        print("\nSimulator stopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
