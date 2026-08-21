"""Thread-safe, single-station transient state for access integration."""

from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass, replace
from datetime import datetime, timedelta
from threading import RLock
from typing import Callable

from app.models import utc_now
from app.schemas import Direction, ReasonCode, RequestStatus


HEARTBEAT_EXPECTED_SECONDS = 2
ESP32_OFFLINE_TIMEOUT_SECONDS = 6
ACCESS_REQUEST_TTL_SECONDS = 30
RECENT_TERMINAL_LIMIT = 32


class RuntimeRequestConflict(RuntimeError):
    """Raised when the one physical station already has a request."""


@dataclass
class RuntimeRequest:
    request_id: str
    status: RequestStatus
    area_id: int
    direction: Direction
    created_at: datetime
    updated_at: datetime
    expires_at: datetime
    user_id: int | None = None
    user_name: str | None = None
    reason_code: ReasonCode | None = None
    message: str | None = None


@dataclass(frozen=True)
class StationSnapshot:
    boot_id: str | None
    latest_heartbeat_at: datetime | None
    person_detected: bool | None
    distance_cm: float | None
    fingerprint_ready: bool | None
    active_request: RuntimeRequest | None


class SingleStationRuntime:
    """Holds transient state for one ESP32 and one active access request."""

    def __init__(self, clock: Callable[[], datetime] = utc_now) -> None:
        self._clock = clock
        self._lock = RLock()
        self._recent: OrderedDict[str, RuntimeRequest] = OrderedDict()
        self.boot_id: str | None = None
        self.latest_heartbeat_at: datetime | None = None
        self.person_detected: bool | None = None
        self.distance_cm: float | None = None
        self.fingerprint_ready: bool | None = None
        self.active_request: RuntimeRequest | None = None

    def now(self) -> datetime:
        return self._clock()

    def _remember(self, request: RuntimeRequest) -> None:
        self._recent[request.request_id] = replace(request)
        self._recent.move_to_end(request.request_id)
        while len(self._recent) > RECENT_TERMINAL_LIMIT:
            self._recent.popitem(last=False)

    def _expire_if_needed(self, now: datetime) -> None:
        request = self.active_request
        if (
            request is not None
            and request.status in {RequestStatus.QUEUED, RequestStatus.IN_PROGRESS}
            and now >= request.expires_at
        ):
            request.status = RequestStatus.EXPIRED
            request.updated_at = now
            request.reason_code = ReasonCode.REQUEST_EXPIRED
            request.message = "The access request expired before authorization."
            self._remember(request)
            self.active_request = None

    def create_request(
        self,
        request_id: str,
        area_id: int,
        direction: Direction,
    ) -> RuntimeRequest:
        now = self.now()
        with self._lock:
            self._expire_if_needed(now)
            if self.active_request is not None:
                raise RuntimeRequestConflict("The physical station already has an active request")
            request = RuntimeRequest(
                request_id=request_id,
                status=RequestStatus.QUEUED,
                area_id=area_id,
                direction=direction,
                created_at=now,
                updated_at=now,
                expires_at=now + timedelta(seconds=ACCESS_REQUEST_TTL_SECONDS),
            )
            self.active_request = request
            return replace(request)

    def update_heartbeat(
        self,
        *,
        boot_id: str,
        person_detected: bool | None,
        distance_cm: float | None,
        fingerprint_ready: bool,
        active_request_id: str | None,
        received_at: datetime,
    ) -> None:
        with self._lock:
            self._expire_if_needed(received_at)
            boot_changed = self.boot_id is not None and self.boot_id != boot_id
            if boot_changed:
                request = self.active_request
                if request is not None and request.status in {
                    RequestStatus.QUEUED,
                    RequestStatus.IN_PROGRESS,
                }:
                    request.status = RequestStatus.EXPIRED
                    request.updated_at = received_at
                    request.reason_code = ReasonCode.REQUEST_EXPIRED
                    request.message = "The ESP32 restarted before authorization."
                    self._remember(request)
                    self.active_request = None
                self.person_detected = None
                self.distance_cm = None
                self.fingerprint_ready = None

            self.boot_id = boot_id
            self.latest_heartbeat_at = received_at
            self.person_detected = person_detected
            self.distance_cm = distance_cm
            self.fingerprint_ready = fingerprint_ready

            request = self.active_request
            if (
                request is not None
                and request.status == RequestStatus.QUEUED
                and active_request_id == request.request_id
            ):
                request.status = RequestStatus.IN_PROGRESS
                request.updated_at = received_at

    def station_snapshot(self) -> StationSnapshot:
        now = self.now()
        with self._lock:
            self._expire_if_needed(now)
            return StationSnapshot(
                boot_id=self.boot_id,
                latest_heartbeat_at=self.latest_heartbeat_at,
                person_detected=self.person_detected,
                distance_cm=self.distance_cm,
                fingerprint_ready=self.fingerprint_ready,
                active_request=replace(self.active_request) if self.active_request else None,
            )

    def get_request(self, request_id: str) -> RuntimeRequest | None:
        now = self.now()
        with self._lock:
            self._expire_if_needed(now)
            if self.active_request and self.active_request.request_id == request_id:
                return replace(self.active_request)
            recent = self._recent.get(request_id)
            return replace(recent) if recent else None

    def queued_command(self) -> RuntimeRequest | None:
        now = self.now()
        with self._lock:
            self._expire_if_needed(now)
            if self.active_request and self.active_request.status == RequestStatus.QUEUED:
                return replace(self.active_request)
            return None

    def mark_request(
        self,
        request_id: str,
        status: RequestStatus,
        *,
        user_id: int | None = None,
        user_name: str | None = None,
        reason_code: ReasonCode | None = None,
        message: str | None = None,
    ) -> None:
        now = self.now()
        with self._lock:
            request = self.active_request
            if request is None or request.request_id != request_id:
                return
            request.status = status
            request.updated_at = now
            request.user_id = user_id
            request.user_name = user_name
            request.reason_code = reason_code
            request.message = message
            if status in {
                RequestStatus.GRANTED,
                RequestStatus.DENIED,
                RequestStatus.FAILED,
                RequestStatus.EXPIRED,
            }:
                self._remember(request)
                self.active_request = None

    def reset_for_test(self) -> None:
        """Clear only in-memory state; no production HTTP endpoint exposes this."""
        with self._lock:
            self._recent.clear()
            self.boot_id = None
            self.latest_heartbeat_at = None
            self.person_detected = None
            self.distance_cm = None
            self.fingerprint_ready = None
            self.active_request = None
