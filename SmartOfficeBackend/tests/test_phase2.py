from __future__ import annotations

import sqlite3
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
from datetime import datetime, timedelta, timezone
from pathlib import Path
from threading import Barrier, Lock

from app.access_runtime import SingleStationRuntime
from app.database import create_sqlite_engine
from app.database_validation import business_snapshot, connect_read_only
from app.main import create_app
from app.seed import initialize_database
from tests.asgi_client import ASGIResponse, request


class MutableClock:
    def __init__(self) -> None:
        self._value = datetime(2026, 8, 21, 8, 0, tzinfo=timezone.utc)
        self._lock = Lock()

    def __call__(self) -> datetime:
        with self._lock:
            return self._value

    def advance(self, seconds: int) -> None:
        with self._lock:
            self._value += timedelta(seconds=seconds)


class Phase2TestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.database_path = Path(self.temp_dir.name) / "phase2.db"
        self.engine = create_sqlite_engine(self.database_path)
        initialize_database(self.database_path, self.engine)
        self.clock = MutableClock()
        self.runtime = SingleStationRuntime(self.clock)
        self.app = create_app(
            target_engine=self.engine,
            database_path=self.database_path,
            runtime=self.runtime,
            initialize_on_startup=False,
        )

    def tearDown(self) -> None:
        self.engine.dispose()
        self.temp_dir.cleanup()

    def api(self, method: str, path: str, body: object | None = None) -> ASGIResponse:
        return request(self.app, method, path, body)

    def heartbeat(
        self,
        *,
        boot_id: str = "boot-1",
        door_state: str = "CLOSED",
        person_detected: bool | None = True,
        distance_cm: float | None = 11.8,
        fingerprint_ready: bool = True,
        admin_mode: bool = False,
        active_request_id: str | None = None,
    ) -> ASGIResponse:
        return self.api(
            "POST",
            "/api/esp32/heartbeat",
            {
                "boot_id": boot_id,
                "door_state": door_state,
                "person_detected": person_detected,
                "distance_cm": distance_cm,
                "fingerprint_ready": fingerprint_ready,
                "admin_mode": admin_mode,
                "active_request_id": active_request_id,
            },
        )

    def create_request(self, area_id: int = 1, direction: str = "ENTRY") -> ASGIResponse:
        return self.api(
            "POST",
            "/api/access/requests",
            {"area_id": area_id, "direction": direction},
        )

    def check(
        self,
        request_id: str,
        *,
        fingerprint_result: str = "MATCH",
        fingerprint_id: int | None = 1,
        door_state: str = "CLOSED",
        person_detected: bool | None = True,
        distance_cm: float | None = 11.8,
        fingerprint_ready: bool = True,
    ) -> ASGIResponse:
        body = {
            "request_id": request_id,
            "precheck": {
                "door_state": door_state,
                "person_detected": person_detected,
                "distance_cm": distance_cm,
                "fingerprint_ready": fingerprint_ready,
            },
            "fingerprint_result": fingerprint_result,
            "confidence": 126,
        }
        if fingerprint_id is not None:
            body["fingerprint_id"] = fingerprint_id
        return self.api("POST", "/api/access/check", body)

    def authorize(
        self,
        *,
        area_id: int = 1,
        direction: str = "ENTRY",
        fingerprint_id: int = 1,
    ) -> str:
        self.assertEqual(self.heartbeat(person_detected=True).status_code, 200)
        created = self.create_request(area_id, direction)
        self.assertEqual(created.status_code, 202, created.body)
        request_id = created.json()["request_id"]
        checked = self.check(request_id, fingerprint_id=fingerprint_id)
        self.assertEqual(checked.status_code, 200, checked.body)
        self.assertEqual(checked.json()["status"], "AUTHORIZED_WAITING_DOOR")
        return request_id

    def execute(self, sql: str, parameters: tuple[object, ...] = ()) -> None:
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            connection.execute(sql, parameters)

    def scalar(self, sql: str, parameters: tuple[object, ...] = ()) -> object:
        with closing(sqlite3.connect(self.database_path)) as connection:
            return connection.execute(sql, parameters).fetchone()[0]

    def rows(self, sql: str, parameters: tuple[object, ...] = ()) -> list[tuple[object, ...]]:
        with closing(sqlite3.connect(self.database_path)) as connection:
            return list(connection.execute(sql, parameters))

    def snapshot(self) -> dict[str, tuple[tuple[object, ...], ...]]:
        with connect_read_only(self.database_path) as connection:
            return business_snapshot(connection)


class RequestAndHeartbeatTests(Phase2TestCase):
    def test_request_creation_preconditions(self) -> None:
        offline = self.create_request()
        self.assertEqual(offline.status_code, 503)
        self.assertEqual(offline.json()["detail"]["reason_code"], "ESP32_OFFLINE")

        self.assertEqual(self.heartbeat().status_code, 200)
        self.execute("UPDATE SystemState SET system_active=0")
        self.assertEqual(self.create_request().json()["detail"]["reason_code"], "SYSTEM_INACTIVE")
        self.execute("UPDATE SystemState SET system_active=1")

        unknown = self.create_request(99)
        self.assertEqual(unknown.status_code, 404)
        self.assertEqual(unknown.json()["detail"]["reason_code"], "AREA_NOT_FOUND")

        self.execute("UPDATE Areas SET is_active=0 WHERE area_id=1")
        self.assertEqual(self.create_request().json()["detail"]["reason_code"], "AREA_INACTIVE")
        self.execute("UPDATE Areas SET is_active=1 WHERE area_id=1")

        self.assertEqual(self.heartbeat(fingerprint_ready=False).status_code, 200)
        unavailable = self.create_request()
        self.assertEqual(unavailable.status_code, 503)
        self.assertEqual(unavailable.json()["detail"]["reason_code"], "FINGERPRINT_UNAVAILABLE")

        self.assertEqual(self.heartbeat(fingerprint_ready=True, door_state="OPEN").status_code, 200)
        self.assertEqual(self.create_request().json()["detail"]["reason_code"], "DOOR_ALREADY_OPEN")

        self.assertEqual(self.heartbeat(door_state="CLOSED").status_code, 200)
        self.execute("UPDATE SystemState SET lockdown_active=1, failed_attempts=3")
        self.assertEqual(self.create_request().json()["detail"]["reason_code"], "LOCKDOWN_ACTIVE")

    def test_request_body_validation_uses_fastapi_422(self) -> None:
        invalid_direction = self.api(
            "POST",
            "/api/access/requests",
            {"area_id": 1, "direction": "SIDEWAYS"},
        )
        self.assertEqual(invalid_direction.status_code, 422)

        missing_match_id = self.api(
            "POST",
            "/api/access/check",
            {
                "request_id": "req_validation",
                "precheck": {
                    "door_state": "CLOSED",
                    "person_detected": True,
                    "distance_cm": 10,
                    "fingerprint_ready": True,
                },
                "fingerprint_result": "MATCH",
            },
        )
        self.assertEqual(missing_match_id.status_code, 422)

    def test_entry_requires_presence_but_exit_does_not(self) -> None:
        self.heartbeat(person_detected=False)
        entry = self.create_request(direction="ENTRY")
        self.assertEqual(entry.status_code, 409)
        self.assertEqual(entry.json()["detail"]["reason_code"], "PERSON_NOT_DETECTED")
        exit_request = self.create_request(direction="EXIT")
        self.assertEqual(exit_request.status_code, 202)
        self.assertEqual(exit_request.json()["direction"], "EXIT")

    def test_request_id_ownership_one_active_and_expiry(self) -> None:
        self.heartbeat()
        created = self.create_request()
        self.assertEqual(created.status_code, 202)
        request_id = created.json()["request_id"]
        self.assertTrue(request_id.startswith("req_"))
        self.assertEqual(len(request_id), 36)
        second = self.create_request()
        self.assertEqual(second.status_code, 409)
        self.assertEqual(second.json()["detail"]["reason_code"], "REQUEST_IN_PROGRESS")
        before = self.snapshot()
        self.clock.advance(59)
        self.assertEqual(
            self.api("GET", f"/api/access/requests/{request_id}").status_code,
            200,
        )
        self.clock.advance(2)
        expired = self.api("GET", f"/api/access/requests/{request_id}")
        self.assertEqual(expired.status_code, 410)
        self.assertEqual(expired.json()["detail"]["reason_code"], "REQUEST_EXPIRED")
        self.assertEqual(before, self.snapshot())

    def test_heartbeat_transient_state_and_derived_offline(self) -> None:
        heartbeat = self.heartbeat(person_detected=True, distance_cm=9.5)
        self.assertEqual(heartbeat.status_code, 200)
        state = self.api("GET", "/api/system-state").json()
        self.assertTrue(state["esp32_online"])
        self.assertTrue(state["person_detected"])
        self.assertEqual(state["distance_cm"], 9.5)
        self.assertTrue(state["fingerprint_ready"])
        self.clock.advance(7)
        stale = self.api("GET", "/api/system-state").json()
        self.assertFalse(stale["esp32_online"])
        self.assertIsNone(stale["person_detected"])
        self.assertIsNone(stale["distance_cm"])
        self.assertIsNone(stale["fingerprint_ready"])

    def test_late_delivery_gets_one_finite_in_progress_deadline(self) -> None:
        self.heartbeat()
        created = self.create_request().json()
        request_id = created["request_id"]

        self.clock.advance(59)
        self.assertEqual(
            self.heartbeat(active_request_id=request_id).status_code,
            200,
        )
        acknowledged = self.api(
            "GET", f"/api/access/requests/{request_id}"
        ).json()
        self.assertEqual(acknowledged["status"], "IN_PROGRESS")
        acknowledged_at = datetime.fromisoformat(acknowledged["updated_at"])
        processing_deadline = datetime.fromisoformat(acknowledged["expires_at"])
        self.assertEqual(
            processing_deadline - acknowledged_at,
            timedelta(seconds=45),
        )

        self.clock.advance(40)
        polled = self.api("GET", f"/api/access/requests/{request_id}")
        self.assertEqual(polled.status_code, 200)
        self.assertEqual(polled.json()["expires_at"], acknowledged["expires_at"])

        self.assertEqual(
            self.heartbeat(active_request_id=request_id).status_code,
            200,
        )
        after_heartbeat = self.api(
            "GET", f"/api/access/requests/{request_id}"
        )
        self.assertEqual(after_heartbeat.status_code, 200)
        self.assertEqual(
            after_heartbeat.json()["expires_at"], acknowledged["expires_at"]
        )

        self.clock.advance(6)
        expired = self.api("GET", f"/api/access/requests/{request_id}")
        self.assertEqual(expired.status_code, 410)
        self.assertEqual(expired.json()["detail"]["reason_code"], "REQUEST_EXPIRED")
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 0)
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 0)

    def test_command_redelivery_acknowledgement_and_duplicate_heartbeat(self) -> None:
        self.heartbeat()
        request_id = self.create_request().json()["request_id"]
        first = self.heartbeat().json()
        repeated = self.heartbeat().json()
        self.assertEqual(first["command"]["request_id"], request_id)
        self.assertEqual(repeated["command"]["request_id"], request_id)
        acknowledged = self.heartbeat(active_request_id=request_id).json()
        self.assertIsNone(acknowledged["command"])
        status = self.api("GET", f"/api/access/requests/{request_id}").json()
        self.assertEqual(status["status"], "IN_PROGRESS")
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 0)

    def test_system_state_projects_runtime_and_persistent_active_request(self) -> None:
        self.heartbeat()
        request_id = self.create_request().json()["request_id"]
        active = self.api("GET", "/api/system-state").json()["active_access_request"]
        self.assertEqual(
            active,
            {
                "request_id": request_id,
                "status": "QUEUED",
                "area_id": 1,
                "direction": "ENTRY",
            },
        )
        self.check(request_id)
        pending = self.api("GET", "/api/system-state").json()["active_access_request"]
        self.assertEqual(pending["status"], "AUTHORIZED_WAITING_DOOR")
        self.assertEqual(pending["request_id"], request_id)

    def test_new_boot_expires_runtime_request_but_preserves_pending(self) -> None:
        self.heartbeat(boot_id="boot-1")
        queued_id = self.create_request().json()["request_id"]
        self.heartbeat(boot_id="boot-2")
        self.assertEqual(
            self.api("GET", f"/api/access/requests/{queued_id}").status_code,
            410,
        )

        pending_id = self.authorize()
        self.heartbeat(boot_id="boot-3")
        pending = self.api("GET", f"/api/access/requests/{pending_id}")
        self.assertEqual(pending.status_code, 200)
        self.assertEqual(pending.json()["status"], "AUTHORIZED_WAITING_DOOR")

    def test_heartbeat_security_authority_and_admin_mirroring(self) -> None:
        self.execute(
            "UPDATE SystemState SET failed_attempts=3, lockdown_active=1, admin_mode=1"
        )
        response = self.heartbeat(admin_mode=False).json()
        self.assertEqual(response["failed_attempts"], 3)
        self.assertTrue(response["lockdown_active"])
        self.assertFalse(response["admin_mode"])
        self.execute("UPDATE SystemState SET admin_mode=0")
        response = self.heartbeat(admin_mode=True).json()
        self.assertFalse(response["admin_mode"])


class AuthorizationTests(Phase2TestCase):
    def test_employee_a_authorization_persists_pending_only_and_replays(self) -> None:
        self.execute("UPDATE SystemState SET failed_attempts=2")
        request_id = self.authorize()
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 0)
        self.assertEqual(
            self.scalar("SELECT is_inside FROM UserAreaStatus WHERE user_id=1 AND area_id=1"),
            0,
        )
        state = self.rows(
            "SELECT pending_request_id,pending_user_id,pending_area_id,pending_direction,"
            "pending_created_at,pending_authorized_at,pending_expires_at,failed_attempts "
            "FROM SystemState"
        )[0]
        self.assertEqual(state[:4], (request_id, 1, 1, "ENTRY"))
        self.assertTrue(all(value is not None for value in state[4:7]))
        self.assertEqual(state[7], 2)
        replay = self.check(request_id)
        self.assertEqual(replay.json()["status"], "AUTHORIZED_WAITING_DOOR")
        self.assertTrue(replay.json()["idempotent_replay"])
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 0)

    def test_no_permission_denial_is_idempotent(self) -> None:
        self.heartbeat()
        request_id = self.create_request(2).json()["request_id"]
        first = self.check(request_id)
        second = self.check(request_id)
        self.assertEqual(first.json()["reason_code"], "NO_PERMISSION")
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 1)
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 1)
        self.assertTrue(second.json()["idempotent_replay"])

    def test_anti_passback_rules(self) -> None:
        self.execute(
            "UPDATE UserAreaStatus SET is_inside=1 WHERE user_id=1 AND area_id=1"
        )
        self.heartbeat()
        entry_id = self.create_request(1, "ENTRY").json()["request_id"]
        entry = self.check(entry_id)
        self.assertEqual(entry.json()["reason_code"], "ALREADY_INSIDE")
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 1)

        self.execute(
            "UPDATE UserAreaStatus SET is_inside=0 WHERE user_id=1 AND area_id=1"
        )
        exit_id = self.create_request(1, "EXIT").json()["request_id"]
        outside = self.check(exit_id)
        self.assertEqual(outside.json()["reason_code"], "ALREADY_OUTSIDE")
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 1)

    def test_unknown_timeout_and_read_error_rules(self) -> None:
        cases = [
            ("NOT_RECOGNIZED", None, "UNKNOWN_FINGERPRINT"),
            ("MATCH", 99, "UNKNOWN_FINGERPRINT"),
            ("TIMEOUT", None, "FINGERPRINT_TIMEOUT"),
        ]
        self.heartbeat()
        for expected_count, (result, fingerprint_id, reason) in enumerate(cases, start=1):
            request_id = self.create_request().json()["request_id"]
            response = self.check(
                request_id,
                fingerprint_result=result,
                fingerprint_id=fingerprint_id,
            )
            self.assertEqual(response.json()["reason_code"], reason)
            self.assertEqual(
                self.scalar("SELECT failed_attempts FROM SystemState"),
                expected_count,
            )
        self.assertEqual(self.scalar("SELECT lockdown_active FROM SystemState"), 1)
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 3)

    def test_read_error_increments_once(self) -> None:
        self.heartbeat()
        request_id = self.create_request().json()["request_id"]
        response = self.check(
            request_id,
            fingerprint_result="READ_ERROR",
            fingerprint_id=None,
        )
        self.assertEqual(response.json()["reason_code"], "FINGERPRINT_READ_ERROR")
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 1)

    def test_inactive_user_and_area_changed_after_scan_do_not_increment(self) -> None:
        self.heartbeat()
        inactive_id = self.create_request().json()["request_id"]
        self.execute("UPDATE Users SET is_active=0 WHERE user_id=1")
        inactive = self.check(inactive_id)
        self.assertEqual(inactive.json()["reason_code"], "USER_INACTIVE")
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 0)
        self.execute("UPDATE Users SET is_active=1 WHERE user_id=1")

        area_id = self.create_request().json()["request_id"]
        self.execute("UPDATE Areas SET is_active=0 WHERE area_id=1")
        area = self.check(area_id)
        self.assertEqual(area.json()["reason_code"], "AREA_INACTIVE")
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 0)
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 2)

    def test_pre_scan_failures_create_no_log_or_counter(self) -> None:
        self.heartbeat()
        cases = [
            ({"person_detected": False}, "PERSON_NOT_DETECTED"),
            ({"person_detected": None}, "ULTRASONIC_UNAVAILABLE"),
            ({"fingerprint_ready": False, "fingerprint_result": "NOT_SCANNED"}, "FINGERPRINT_UNAVAILABLE"),
        ]
        for overrides, reason in cases:
            request_id = self.create_request().json()["request_id"]
            result = overrides.pop("fingerprint_result", "NOT_SCANNED")
            response = self.check(
                request_id,
                fingerprint_result=result,
                fingerprint_id=None,
                **overrides,
            )
            self.assertEqual(response.json()["reason_code"], reason)
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 0)
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 0)

    def test_pre_scan_lockdown_creates_no_log_and_does_not_increment(self) -> None:
        self.heartbeat()
        request_id = self.create_request().json()["request_id"]
        self.execute("UPDATE SystemState SET failed_attempts=3, lockdown_active=1")
        response = self.check(
            request_id,
            fingerprint_result="NOT_SCANNED",
            fingerprint_id=None,
        )
        self.assertEqual(response.json()["reason_code"], "LOCKDOWN_ACTIVE")
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 3)
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 0)

    def test_lockdown_race_after_real_scan_logs_without_increment(self) -> None:
        self.heartbeat()
        request_id = self.create_request().json()["request_id"]
        self.execute("UPDATE SystemState SET failed_attempts=3, lockdown_active=1")
        response = self.check(request_id)
        self.assertEqual(response.json()["reason_code"], "LOCKDOWN_ACTIVE")
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 3)
        log = self.rows("SELECT user_id,denial_reason FROM AccessLogs")[0]
        self.assertEqual(log, (1, "LOCKDOWN_ACTIVE"))


class CompletionAndStatusTests(Phase2TestCase):
    def test_door_opened_commits_once_and_status_reconstructs_after_restart(self) -> None:
        self.execute("UPDATE SystemState SET failed_attempts=2")
        request_id = self.authorize()
        first = self.api(
            "POST",
            "/api/access/complete",
            {"request_id": request_id, "door_result": "DOOR_OPENED"},
        )
        self.assertEqual(first.json()["status"], "GRANTED")
        self.assertEqual(
            self.scalar("SELECT is_inside FROM UserAreaStatus WHERE user_id=1 AND area_id=1"),
            1,
        )
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 1)
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 0)
        self.assertIsNone(self.scalar("SELECT pending_request_id FROM SystemState"))
        request_created_at, event_timestamp = self.rows(
            "SELECT request_created_at,event_timestamp FROM AccessLogs WHERE request_id=?",
            (request_id,),
        )[0]
        self.assertEqual(request_created_at, "2026-08-21 08:00:00.000000")
        self.assertEqual(event_timestamp, "2026-08-21 08:00:00.000000")
        replay = self.api(
            "POST",
            "/api/access/complete",
            {"request_id": request_id, "door_result": "DOOR_OPENED"},
        )
        self.assertTrue(replay.json()["idempotent_replay"])
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 1)

        restarted = create_app(
            target_engine=self.engine,
            database_path=self.database_path,
            runtime=SingleStationRuntime(self.clock),
            initialize_on_startup=False,
        )
        reconstructed = request(restarted, "GET", f"/api/access/requests/{request_id}")
        self.assertEqual(reconstructed.json()["status"], "GRANTED")

    def test_door_open_failed_preserves_state_and_conflicting_retry(self) -> None:
        self.execute("UPDATE SystemState SET failed_attempts=2")
        request_id = self.authorize()
        before = self.scalar(
            "SELECT is_inside FROM UserAreaStatus WHERE user_id=1 AND area_id=1"
        )
        failed = self.api(
            "POST",
            "/api/access/complete",
            {"request_id": request_id, "door_result": "DOOR_OPEN_FAILED"},
        )
        self.assertEqual(failed.json()["status"], "FAILED")
        self.assertEqual(
            self.scalar("SELECT is_inside FROM UserAreaStatus WHERE user_id=1 AND area_id=1"),
            before,
        )
        self.assertEqual(
            self.scalar("SELECT denial_reason FROM AccessLogs"),
            "DOOR_OPEN_FAILED",
        )
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 2)
        replay = self.api(
            "POST",
            "/api/access/complete",
            {"request_id": request_id, "door_result": "DOOR_OPEN_FAILED"},
        )
        self.assertTrue(replay.json()["idempotent_replay"])
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 1)
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 2)
        status = self.api("GET", f"/api/access/requests/{request_id}").json()
        self.assertEqual(status["status"], "FAILED")
        conflict = self.api(
            "POST",
            "/api/access/complete",
            {"request_id": request_id, "door_result": "DOOR_OPENED"},
        )
        self.assertEqual(conflict.status_code, 409)
        self.assertEqual(
            conflict.json()["detail"]["reason_code"],
            "REQUEST_OUTCOME_CONFLICT",
        )

    def test_granted_then_failed_conflict(self) -> None:
        request_id = self.authorize()
        self.api(
            "POST",
            "/api/access/complete",
            {"request_id": request_id, "door_result": "DOOR_OPENED"},
        )
        conflict = self.api(
            "POST",
            "/api/access/complete",
            {"request_id": request_id, "door_result": "DOOR_OPEN_FAILED"},
        )
        self.assertEqual(conflict.status_code, 409)

    def test_pending_survives_runtime_restart_and_never_ttl_expires(self) -> None:
        request_id = self.authorize()
        self.clock.advance(1000)
        restarted = create_app(
            target_engine=self.engine,
            database_path=self.database_path,
            runtime=SingleStationRuntime(self.clock),
            initialize_on_startup=False,
        )
        status = request(restarted, "GET", f"/api/access/requests/{request_id}")
        self.assertEqual(status.json()["status"], "AUTHORIZED_WAITING_DOOR")
        blocked = request(
            restarted,
            "POST",
            "/api/access/requests",
            {"area_id": 1, "direction": "ENTRY"},
        )
        self.assertEqual(blocked.status_code, 409)
        self.assertEqual(blocked.json()["detail"]["reason_code"], "REQUEST_IN_PROGRESS")

    def test_runtime_queued_in_progress_and_denied_status(self) -> None:
        self.heartbeat()
        request_id = self.create_request(2).json()["request_id"]
        self.assertEqual(
            self.api("GET", f"/api/access/requests/{request_id}").json()["status"],
            "QUEUED",
        )
        self.heartbeat(active_request_id=request_id)
        self.assertEqual(
            self.api("GET", f"/api/access/requests/{request_id}").json()["status"],
            "IN_PROGRESS",
        )
        self.check(request_id)
        restarted = create_app(
            target_engine=self.engine,
            database_path=self.database_path,
            runtime=SingleStationRuntime(self.clock),
            initialize_on_startup=False,
        )
        denied = request(restarted, "GET", f"/api/access/requests/{request_id}")
        self.assertEqual(denied.json()["status"], "DENIED")
        self.assertEqual(denied.json()["reason_code"], "NO_PERMISSION")


class BootstrapAndSecurityTests(Phase2TestCase):
    def test_bootstrap_returns_canonical_state_without_mutation(self) -> None:
        before = self.snapshot()
        response = self.api("GET", "/api/esp32/bootstrap")
        after = self.snapshot()
        body = response.json()
        self.assertEqual(len(body["attendance"]), 42)
        self.assertEqual(body["failed_attempts"], 0)
        self.assertFalse(body["lockdown_active"])
        self.assertFalse(body["admin_mode"])
        self.assertTrue(body["system_active"])
        self.assertIsNone(body["pending_authorization"])
        self.assertEqual(before, after)

    def test_bootstrap_returns_exact_pending_authorization(self) -> None:
        request_id = self.authorize()
        before = self.snapshot()
        pending = self.api("GET", "/api/esp32/bootstrap").json()["pending_authorization"]
        self.assertEqual(pending["request_id"], request_id)
        self.assertEqual(pending["user_id"], 1)
        self.assertEqual(pending["area_id"], 1)
        self.assertEqual(pending["direction"], "ENTRY")
        self.assertIsNotNone(pending["created_at"])
        self.assertIsNotNone(pending["authorized_at"])
        self.assertEqual(before, self.snapshot())

    def test_unknown_rfid_idempotency_threshold_and_conflict(self) -> None:
        first = self.api(
            "POST",
            "/api/esp32/security-events",
            {"boot_id": "boot-1", "event_id": "event-1", "event_type": "UNKNOWN_RFID"},
        )
        replay = self.api(
            "POST",
            "/api/esp32/security-events",
            {"boot_id": "boot-1", "event_id": "event-1", "event_type": "UNKNOWN_RFID"},
        )
        self.assertEqual(first.json()["failed_attempts"], 1)
        self.assertEqual(replay.json()["failed_attempts"], 1)
        self.assertTrue(replay.json()["idempotent_replay"])
        conflict = self.api(
            "POST",
            "/api/esp32/security-events",
            {"boot_id": "boot-1", "event_id": "event-1", "event_type": "ADMIN_RFID_ACCEPTED"},
        )
        self.assertEqual(conflict.status_code, 409)
        for event_id in ("event-2", "event-3"):
            result = self.api(
                "POST",
                "/api/esp32/security-events",
                {"boot_id": "boot-1", "event_id": event_id, "event_type": "UNKNOWN_RFID"},
            )
        self.assertEqual(result.json()["failed_attempts"], 3)
        self.assertTrue(result.json()["lockdown_active"])
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 0)

    def test_admin_rfid_is_only_unlock_and_restart_dedupe_persists(self) -> None:
        self.execute("UPDATE SystemState SET failed_attempts=3, lockdown_active=1")
        event = {"boot_id": "boot-1", "event_id": "admin-1", "event_type": "ADMIN_RFID_ACCEPTED"}
        accepted = self.api("POST", "/api/esp32/security-events", event).json()
        self.assertEqual(accepted["failed_attempts"], 0)
        self.assertFalse(accepted["lockdown_active"])
        self.assertTrue(accepted["admin_mode"])
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 0)
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM UserAreaStatus WHERE is_inside=1"), 0)

        restarted = create_app(
            target_engine=self.engine,
            database_path=self.database_path,
            runtime=SingleStationRuntime(self.clock),
            initialize_on_startup=False,
        )
        replay = request(restarted, "POST", "/api/esp32/security-events", event).json()
        self.assertTrue(replay["idempotent_replay"])
        self.assertEqual(replay["failed_attempts"], 0)

    def test_lockdown_during_pending_authorization(self) -> None:
        request_id = self.authorize()
        for event_id in ("rfid-1", "rfid-2", "rfid-3"):
            self.api(
                "POST",
                "/api/esp32/security-events",
                {"boot_id": "boot-1", "event_id": event_id, "event_type": "UNKNOWN_RFID"},
            )
        self.assertEqual(self.scalar("SELECT lockdown_active FROM SystemState"), 1)
        self.assertEqual(self.scalar("SELECT pending_request_id FROM SystemState"), request_id)
        blocked = self.create_request()
        self.assertEqual(blocked.status_code, 409)
        replay = self.check(request_id)
        self.assertEqual(replay.json()["status"], "AUTHORIZED_WAITING_DOOR")
        completed = self.api(
            "POST",
            "/api/access/complete",
            {"request_id": request_id, "door_result": "DOOR_OPENED"},
        )
        self.assertEqual(completed.json()["status"], "GRANTED")
        self.assertTrue(completed.json()["lockdown_active"])
        self.assertEqual(completed.json()["failed_attempts"], 3)
        self.assertEqual(self.scalar("SELECT is_inside FROM UserAreaStatus WHERE user_id=1 AND area_id=1"), 1)
        admin = self.api(
            "POST",
            "/api/esp32/security-events",
            {"boot_id": "boot-1", "event_id": "admin", "event_type": "ADMIN_RFID_ACCEPTED"},
        )
        self.assertFalse(admin.json()["lockdown_active"])
        self.assertEqual(admin.json()["failed_attempts"], 0)


class ConcurrencyTests(Phase2TestCase):
    def run_concurrently(self, first, second) -> tuple[ASGIResponse, ASGIResponse]:
        barrier = Barrier(3)

        def wrapped(callable_):
            barrier.wait()
            return callable_()

        with ThreadPoolExecutor(max_workers=2) as executor:
            future_one = executor.submit(wrapped, first)
            future_two = executor.submit(wrapped, second)
            barrier.wait()
            return future_one.result(), future_two.result()

    def test_two_simultaneous_request_creations_accept_only_one(self) -> None:
        self.heartbeat()
        responses = self.run_concurrently(self.create_request, self.create_request)
        self.assertEqual(sorted(response.status_code for response in responses), [202, 409])

    def test_two_simultaneous_denial_retries_mutate_once(self) -> None:
        self.heartbeat()
        request_id = self.create_request(2).json()["request_id"]
        responses = self.run_concurrently(
            lambda: self.check(request_id),
            lambda: self.check(request_id),
        )
        self.assertEqual([response.status_code for response in responses], [200, 200])
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 1)
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 1)

    def test_two_simultaneous_completions_commit_once(self) -> None:
        request_id = self.authorize()
        call = lambda: self.api(
            "POST",
            "/api/access/complete",
            {"request_id": request_id, "door_result": "DOOR_OPENED"},
        )
        responses = self.run_concurrently(call, call)
        self.assertEqual([response.status_code for response in responses], [200, 200])
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM AccessLogs"), 1)
        self.assertEqual(self.scalar("SELECT is_inside FROM UserAreaStatus WHERE user_id=1 AND area_id=1"), 1)

    def test_two_simultaneous_unknown_rfid_retries_increment_once(self) -> None:
        event = {"boot_id": "boot-1", "event_id": "event-1", "event_type": "UNKNOWN_RFID"}
        call = lambda: self.api("POST", "/api/esp32/security-events", event)
        responses = self.run_concurrently(call, call)
        self.assertEqual([response.status_code for response in responses], [200, 200])
        self.assertEqual(self.scalar("SELECT failed_attempts FROM SystemState"), 1)


if __name__ == "__main__":
    unittest.main()
