"""Transactional business rules for Smart Office access integration."""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
from threading import RLock
from uuid import uuid4

from fastapi import HTTPException
from sqlalchemy import text
from sqlalchemy.engine import Engine
from sqlmodel import Session, select

from app.access_runtime import (
    ACCESS_REQUEST_TTL_SECONDS,
    ESP32_OFFLINE_TIMEOUT_SECONDS,
    RuntimeRequest,
    RuntimeRequestConflict,
    SingleStationRuntime,
)
from app.models import AccessLog, Area, Permission, SystemState, User, UserAreaStatus, utc_now
from app.schemas import (
    AccessCheckRequest,
    AccessCheckResponse,
    AccessCommandResponse,
    AccessCompleteRequest,
    AccessCompleteResponse,
    AccessRequestAccepted,
    AccessRequestCreate,
    AccessRequestStatusResponse,
    ActiveAccessRequestResponse,
    AttendanceResponse,
    BootstrapResponse,
    Direction,
    DoorResult,
    DoorState,
    FingerprintResult,
    HeartbeatRequest,
    HeartbeatResponse,
    PendingAuthorizationResponse,
    ReasonCode,
    RequestStatus,
    RequestUserResponse,
    SecurityEventRequest,
    SecurityEventResponse,
    SecurityEventType,
    SystemStateResponse,
)


FAILED_ATTEMPT_LIMIT = 3

PENDING_FIELDS = (
    "pending_request_id",
    "pending_user_id",
    "pending_area_id",
    "pending_direction",
    "pending_created_at",
    "pending_authorized_at",
    "pending_expires_at",
)

MESSAGES = {
    ReasonCode.AUTHORIZED: "Access was authorized.",
    ReasonCode.UNKNOWN_FINGERPRINT: "The fingerprint is not assigned to an active known user.",
    ReasonCode.USER_INACTIVE: "The identified user is inactive.",
    ReasonCode.NO_PERMISSION: "The user does not have permission for this area.",
    ReasonCode.ALREADY_INSIDE: "The user is already marked inside this area.",
    ReasonCode.ALREADY_OUTSIDE: "The user is already marked outside this area.",
    ReasonCode.LOCKDOWN_ACTIVE: "The access system is in Lockdown.",
    ReasonCode.SYSTEM_INACTIVE: "The access system is inactive.",
    ReasonCode.AREA_NOT_FOUND: "The requested area does not exist.",
    ReasonCode.AREA_INACTIVE: "The requested area is inactive.",
    ReasonCode.PERSON_NOT_DETECTED: "No person was detected for Entry.",
    ReasonCode.ULTRASONIC_UNAVAILABLE: "The presence sensor result is unavailable.",
    ReasonCode.FINGERPRINT_UNAVAILABLE: "The fingerprint sensor is unavailable.",
    ReasonCode.FINGERPRINT_TIMEOUT: "The fingerprint scan timed out.",
    ReasonCode.FINGERPRINT_READ_ERROR: "The fingerprint sensor could not read the finger.",
    ReasonCode.DOOR_ALREADY_OPEN: "The door is already open.",
    ReasonCode.DOOR_OPEN_FAILED: "Authorization succeeded, but the door did not open.",
    ReasonCode.ESP32_OFFLINE: "The ESP32 station is offline.",
    ReasonCode.REQUEST_IN_PROGRESS: "Another access request is already in progress.",
    ReasonCode.REQUEST_EXPIRED: "The access request expired or is no longer available.",
    ReasonCode.REQUEST_OUTCOME_CONFLICT: "The reported door result conflicts with the committed result.",
    ReasonCode.SECURITY_EVENT_CONFLICT: "The security event identity was reused with another type.",
}


def as_utc(value: datetime | None) -> datetime | None:
    if value is None:
        return None
    if value.tzinfo is None:
        return value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc)


def api_error(status_code: int, reason_code: ReasonCode, message: str | None = None) -> HTTPException:
    return HTTPException(
        status_code=status_code,
        detail={
            "reason_code": reason_code.value,
            "message": message or MESSAGES[reason_code],
        },
    )


class AccessService:
    """Coordinates short runtime locks with serialized SQLite write transactions."""

    def __init__(self, target_engine: Engine, runtime: SingleStationRuntime) -> None:
        self.engine = target_engine
        self.runtime = runtime
        self._write_lock = RLock()

    @staticmethod
    def _begin_immediate(session: Session) -> None:
        session.exec(text("BEGIN IMMEDIATE"))

    @staticmethod
    def _system_state(session: Session) -> SystemState:
        state = session.get(SystemState, 1)
        if state is None:
            raise RuntimeError("SystemState singleton row is missing")
        return state

    @staticmethod
    def _pending_values(state: SystemState) -> list[object | None]:
        return [getattr(state, field) for field in PENDING_FIELDS]

    @classmethod
    def _has_pending(cls, state: SystemState) -> bool:
        values = cls._pending_values(state)
        if any(value is None for value in values) and not all(value is None for value in values):
            raise RuntimeError("SystemState pending authorization is partially populated")
        return all(value is not None for value in values)

    @staticmethod
    def _clear_pending(state: SystemState) -> None:
        for field in PENDING_FIELDS:
            setattr(state, field, None)

    @staticmethod
    def _online(last_seen: datetime | None, now: datetime) -> bool:
        timestamp = as_utc(last_seen)
        return timestamp is not None and now - timestamp <= timedelta(
            seconds=ESP32_OFFLINE_TIMEOUT_SECONDS
        )

    @staticmethod
    def _increment_failure(state: SystemState) -> None:
        state.failed_attempts = min(FAILED_ATTEMPT_LIMIT, state.failed_attempts + 1)
        if state.failed_attempts >= FAILED_ATTEMPT_LIMIT:
            state.lockdown_active = True

    @staticmethod
    def _request_user(user: User | None) -> RequestUserResponse | None:
        if user is None:
            return None
        return RequestUserResponse(user_id=user.user_id, name=user.name)

    def system_state(self) -> SystemStateResponse:
        now = self.runtime.now()
        station = self.runtime.station_snapshot()
        with Session(self.engine) as session:
            state = self._system_state(session)
            online = self._online(state.esp32_last_seen_at, now)
            runtime_fresh = (
                station.latest_heartbeat_at is not None
                and now - station.latest_heartbeat_at
                <= timedelta(seconds=ESP32_OFFLINE_TIMEOUT_SECONDS)
            )
            active: ActiveAccessRequestResponse | None = None
            if self._has_pending(state):
                active = ActiveAccessRequestResponse(
                    request_id=state.pending_request_id,
                    status=RequestStatus.AUTHORIZED_WAITING_DOOR,
                    area_id=state.pending_area_id,
                    direction=Direction(state.pending_direction),
                )
            elif station.active_request is not None:
                request = station.active_request
                active = ActiveAccessRequestResponse(
                    request_id=request.request_id,
                    status=request.status,
                    area_id=request.area_id,
                    direction=request.direction,
                )
            return SystemStateResponse(
                system_active=state.system_active,
                lockdown_active=state.lockdown_active,
                failed_attempts=state.failed_attempts,
                failed_attempt_limit=FAILED_ATTEMPT_LIMIT,
                admin_mode=state.admin_mode,
                door_state=DoorState(state.door_state),
                esp32_online=online,
                esp32_last_seen_at=as_utc(state.esp32_last_seen_at),
                last_updated_at=as_utc(state.last_updated_at),
                person_detected=station.person_detected if online and runtime_fresh else None,
                distance_cm=station.distance_cm if online and runtime_fresh else None,
                fingerprint_ready=station.fingerprint_ready if online and runtime_fresh else None,
                active_access_request=active,
            )

    def create_access_request(self, payload: AccessRequestCreate) -> AccessRequestAccepted:
        now = self.runtime.now()
        station = self.runtime.station_snapshot()
        with Session(self.engine) as session:
            state = self._system_state(session)
            if not state.system_active:
                raise api_error(409, ReasonCode.SYSTEM_INACTIVE)
            area = session.get(Area, payload.area_id)
            if area is None:
                raise api_error(404, ReasonCode.AREA_NOT_FOUND)
            if not area.is_active:
                raise api_error(409, ReasonCode.AREA_INACTIVE)
            if self._has_pending(state):
                raise api_error(409, ReasonCode.REQUEST_IN_PROGRESS)
            if state.lockdown_active:
                raise api_error(409, ReasonCode.LOCKDOWN_ACTIVE)
            if not self._online(state.esp32_last_seen_at, now):
                raise api_error(503, ReasonCode.ESP32_OFFLINE)
            if station.fingerprint_ready is not True:
                raise api_error(503, ReasonCode.FINGERPRINT_UNAVAILABLE)
            if state.door_state != DoorState.CLOSED.value:
                raise api_error(409, ReasonCode.DOOR_ALREADY_OPEN)
            if payload.direction == Direction.ENTRY and station.person_detected is not True:
                raise api_error(409, ReasonCode.PERSON_NOT_DETECTED)

        request_id = f"req_{uuid4().hex}"
        try:
            request = self.runtime.create_request(
                request_id=request_id,
                area_id=payload.area_id,
                direction=payload.direction,
            )
        except RuntimeRequestConflict as exc:
            raise api_error(409, ReasonCode.REQUEST_IN_PROGRESS) from exc
        return AccessRequestAccepted(
            request_id=request.request_id,
            status=request.status,
            area_id=request.area_id,
            direction=request.direction,
            created_at=request.created_at,
            expires_at=request.expires_at,
        )

    def heartbeat(self, payload: HeartbeatRequest) -> HeartbeatResponse:
        now = self.runtime.now()
        with self._write_lock, Session(self.engine) as session:
            self._begin_immediate(session)
            state = self._system_state(session)
            state.door_state = payload.door_state.value
            state.esp32_online = True
            state.esp32_last_seen_at = now
            if state.admin_mode and not payload.admin_mode:
                state.admin_mode = False
            state.last_updated_at = now
            session.add(state)
            session.commit()
            canonical = (
                state.system_active,
                state.failed_attempts,
                state.lockdown_active,
                state.admin_mode,
                state.pending_request_id,
            )

        self.runtime.update_heartbeat(
            boot_id=payload.boot_id,
            person_detected=payload.person_detected,
            distance_cm=payload.distance_cm,
            fingerprint_ready=payload.fingerprint_ready,
            active_request_id=payload.active_request_id,
            received_at=now,
        )
        command: AccessCommandResponse | None = None
        if canonical[4] is None:
            queued = self.runtime.queued_command()
            if queued is not None:
                command = AccessCommandResponse(
                    request_id=queued.request_id,
                    area_id=queued.area_id,
                    direction=queued.direction,
                    expires_at=queued.expires_at,
                )
        return HeartbeatResponse(
            server_time=now,
            system_active=canonical[0],
            failed_attempts=canonical[1],
            lockdown_active=canonical[2],
            admin_mode=canonical[3],
            pending_request_id=canonical[4],
            command=command,
        )

    def bootstrap(self) -> BootstrapResponse:
        with Session(self.engine) as session:
            state = self._system_state(session)
            attendance = session.exec(
                select(UserAreaStatus).order_by(UserAreaStatus.user_id, UserAreaStatus.area_id)
            ).all()
            pending = None
            if self._has_pending(state):
                pending = PendingAuthorizationResponse(
                    request_id=state.pending_request_id,
                    user_id=state.pending_user_id,
                    area_id=state.pending_area_id,
                    direction=Direction(state.pending_direction),
                    created_at=as_utc(state.pending_created_at),
                    authorized_at=as_utc(state.pending_authorized_at),
                    expires_at=as_utc(state.pending_expires_at),
                )
            return BootstrapResponse(
                system_active=state.system_active,
                failed_attempts=state.failed_attempts,
                lockdown_active=state.lockdown_active,
                admin_mode=state.admin_mode,
                attendance=[
                    AttendanceResponse(
                        user_id=row.user_id,
                        area_id=row.area_id,
                        is_inside=row.is_inside,
                    )
                    for row in attendance
                ],
                pending_authorization=pending,
            )

    def _status_from_log(
        self,
        session: Session,
        log: AccessLog,
    ) -> AccessRequestStatusResponse:
        if log.decision == "GRANTED":
            status = RequestStatus.GRANTED
            reason = ReasonCode.AUTHORIZED
        elif log.denial_reason == ReasonCode.DOOR_OPEN_FAILED.value:
            status = RequestStatus.FAILED
            reason = ReasonCode.DOOR_OPEN_FAILED
        else:
            status = RequestStatus.DENIED
            reason = ReasonCode(log.denial_reason)
        user = session.get(User, log.user_id) if log.user_id is not None else None
        created_at = as_utc(log.request_created_at) or as_utc(log.event_timestamp)
        return AccessRequestStatusResponse(
            request_id=log.request_id,
            status=status,
            area_id=log.area_id,
            direction=Direction(log.direction),
            user=self._request_user(user),
            reason_code=reason,
            message=MESSAGES[reason],
            created_at=created_at,
            updated_at=as_utc(log.event_timestamp),
            expires_at=None,
        )

    def request_status(self, request_id: str) -> AccessRequestStatusResponse:
        with Session(self.engine) as session:
            log = session.exec(
                select(AccessLog).where(AccessLog.request_id == request_id)
            ).first()
            if log is not None:
                return self._status_from_log(session, log)
            state = self._system_state(session)
            if self._has_pending(state) and state.pending_request_id == request_id:
                user = session.get(User, state.pending_user_id)
                return AccessRequestStatusResponse(
                    request_id=request_id,
                    status=RequestStatus.AUTHORIZED_WAITING_DOOR,
                    area_id=state.pending_area_id,
                    direction=Direction(state.pending_direction),
                    user=self._request_user(user),
                    reason_code=ReasonCode.AUTHORIZED,
                    message="Authorization is committed and waiting for the door result.",
                    created_at=as_utc(state.pending_created_at),
                    updated_at=as_utc(state.pending_authorized_at),
                    expires_at=as_utc(state.pending_expires_at),
                )

        request = self.runtime.get_request(request_id)
        if request is None or request.status == RequestStatus.EXPIRED:
            raise api_error(410, ReasonCode.REQUEST_EXPIRED)
        return AccessRequestStatusResponse(
            request_id=request.request_id,
            status=request.status,
            area_id=request.area_id,
            direction=request.direction,
            user=(
                RequestUserResponse(user_id=request.user_id, name=request.user_name)
                if request.user_id is not None and request.user_name is not None
                else None
            ),
            reason_code=request.reason_code,
            message=request.message,
            created_at=request.created_at,
            updated_at=request.updated_at,
            expires_at=request.expires_at,
        )

    def _check_response_from_log(
        self,
        session: Session,
        log: AccessLog,
        state: SystemState,
    ) -> AccessCheckResponse:
        status_response = self._status_from_log(session, log)
        return AccessCheckResponse(
            request_id=log.request_id,
            status=status_response.status,
            reason_code=status_response.reason_code,
            message=status_response.message,
            failed_attempts=state.failed_attempts,
            lockdown_active=state.lockdown_active,
            user=status_response.user,
            idempotent_replay=True,
        )

    def _terminal_without_log(
        self,
        request: RuntimeRequest,
        state: SystemState,
        reason: ReasonCode,
    ) -> AccessCheckResponse:
        message = MESSAGES[reason]
        self.runtime.mark_request(
            request.request_id,
            RequestStatus.DENIED,
            reason_code=reason,
            message=message,
        )
        return AccessCheckResponse(
            request_id=request.request_id,
            status=RequestStatus.DENIED,
            reason_code=reason,
            message=message,
            failed_attempts=state.failed_attempts,
            lockdown_active=state.lockdown_active,
        )

    def _deny_with_log(
        self,
        session: Session,
        state: SystemState,
        request: RuntimeRequest,
        reason: ReasonCode,
        *,
        user: User | None,
        increment: bool,
        now: datetime,
    ) -> AccessCheckResponse:
        if increment:
            self._increment_failure(state)
        state.last_updated_at = now
        log = AccessLog(
            user_id=user.user_id if user else None,
            area_id=request.area_id,
            direction=request.direction.value,
            decision="DENIED",
            denial_reason=reason.value,
            authentication_method="FINGERPRINT",
            event_timestamp=now,
            request_id=request.request_id,
            request_created_at=request.created_at,
        )
        session.add(state)
        session.add(log)
        session.commit()
        message = MESSAGES[reason]
        self.runtime.mark_request(
            request.request_id,
            RequestStatus.DENIED,
            user_id=user.user_id if user else None,
            user_name=user.name if user else None,
            reason_code=reason,
            message=message,
        )
        return AccessCheckResponse(
            request_id=request.request_id,
            status=RequestStatus.DENIED,
            reason_code=reason,
            message=message,
            failed_attempts=state.failed_attempts,
            lockdown_active=state.lockdown_active,
            user=self._request_user(user),
        )

    def access_check(self, payload: AccessCheckRequest) -> AccessCheckResponse:
        now = self.runtime.now()
        with self._write_lock, Session(self.engine) as session:
            self._begin_immediate(session)
            state = self._system_state(session)
            final_log = session.exec(
                select(AccessLog).where(AccessLog.request_id == payload.request_id)
            ).first()
            if final_log is not None:
                response = self._check_response_from_log(session, final_log, state)
                session.rollback()
                return response

            if self._has_pending(state):
                if state.pending_request_id == payload.request_id:
                    user = session.get(User, state.pending_user_id)
                    response = AccessCheckResponse(
                        request_id=payload.request_id,
                        status=RequestStatus.AUTHORIZED_WAITING_DOOR,
                        reason_code=ReasonCode.AUTHORIZED,
                        message="Open the door and report the result.",
                        failed_attempts=state.failed_attempts,
                        lockdown_active=state.lockdown_active,
                        user=self._request_user(user),
                        idempotent_replay=True,
                    )
                    session.rollback()
                    return response
                session.rollback()
                raise api_error(409, ReasonCode.REQUEST_IN_PROGRESS)

            request = self.runtime.get_request(payload.request_id)
            if request is None or request.status not in {
                RequestStatus.QUEUED,
                RequestStatus.IN_PROGRESS,
            }:
                session.rollback()
                raise api_error(410, ReasonCode.REQUEST_EXPIRED)

            precheck_reason: ReasonCode | None = None
            if state.lockdown_active and payload.fingerprint_result == FingerprintResult.NOT_SCANNED:
                precheck_reason = ReasonCode.LOCKDOWN_ACTIVE
            elif payload.precheck.door_state == DoorState.OPEN:
                precheck_reason = ReasonCode.DOOR_ALREADY_OPEN
            elif not payload.precheck.fingerprint_ready:
                precheck_reason = ReasonCode.FINGERPRINT_UNAVAILABLE
            elif request.direction == Direction.ENTRY and payload.precheck.person_detected is None:
                precheck_reason = ReasonCode.ULTRASONIC_UNAVAILABLE
            elif request.direction == Direction.ENTRY and payload.precheck.person_detected is False:
                precheck_reason = ReasonCode.PERSON_NOT_DETECTED
            elif payload.fingerprint_result in {
                FingerprintResult.UNAVAILABLE,
                FingerprintResult.NOT_SCANNED,
            }:
                precheck_reason = ReasonCode.FINGERPRINT_UNAVAILABLE

            if precheck_reason is not None:
                session.rollback()
                return self._terminal_without_log(request, state, precheck_reason)

            if not state.system_active:
                session.rollback()
                return self._terminal_without_log(request, state, ReasonCode.SYSTEM_INACTIVE)

            if payload.fingerprint_result == FingerprintResult.TIMEOUT:
                return self._deny_with_log(
                    session,
                    state,
                    request,
                    ReasonCode.FINGERPRINT_TIMEOUT,
                    user=None,
                    increment=True,
                    now=now,
                )
            if payload.fingerprint_result == FingerprintResult.READ_ERROR:
                return self._deny_with_log(
                    session,
                    state,
                    request,
                    ReasonCode.FINGERPRINT_READ_ERROR,
                    user=None,
                    increment=True,
                    now=now,
                )
            if payload.fingerprint_result == FingerprintResult.NOT_RECOGNIZED:
                return self._deny_with_log(
                    session,
                    state,
                    request,
                    ReasonCode.UNKNOWN_FINGERPRINT,
                    user=None,
                    increment=True,
                    now=now,
                )

            user = session.exec(
                select(User).where(User.fingerprint_id == payload.fingerprint_id)
            ).first()
            if user is None:
                return self._deny_with_log(
                    session,
                    state,
                    request,
                    ReasonCode.UNKNOWN_FINGERPRINT,
                    user=None,
                    increment=True,
                    now=now,
                )
            if not user.is_active:
                return self._deny_with_log(
                    session,
                    state,
                    request,
                    ReasonCode.USER_INACTIVE,
                    user=user,
                    increment=False,
                    now=now,
                )

            area = session.get(Area, request.area_id)
            if area is None or not area.is_active:
                return self._deny_with_log(
                    session,
                    state,
                    request,
                    ReasonCode.AREA_INACTIVE,
                    user=user,
                    increment=False,
                    now=now,
                )

            if state.lockdown_active:
                return self._deny_with_log(
                    session,
                    state,
                    request,
                    ReasonCode.LOCKDOWN_ACTIVE,
                    user=user,
                    increment=False,
                    now=now,
                )

            permission = session.get(Permission, (user.user_id, request.area_id))
            attendance = session.get(UserAreaStatus, (user.user_id, request.area_id))
            if permission is None or attendance is None:
                session.rollback()
                raise RuntimeError("Canonical permission or attendance row is missing")

            if request.direction == Direction.ENTRY:
                if not permission.allowed:
                    return self._deny_with_log(
                        session, state, request, ReasonCode.NO_PERMISSION,
                        user=user, increment=True, now=now,
                    )
                if attendance.is_inside:
                    return self._deny_with_log(
                        session, state, request, ReasonCode.ALREADY_INSIDE,
                        user=user, increment=True, now=now,
                    )
            else:
                if not attendance.is_inside:
                    return self._deny_with_log(
                        session, state, request, ReasonCode.ALREADY_OUTSIDE,
                        user=user, increment=False, now=now,
                    )
                if not permission.allowed:
                    return self._deny_with_log(
                        session, state, request, ReasonCode.NO_PERMISSION,
                        user=user, increment=True, now=now,
                    )

            state.pending_request_id = request.request_id
            state.pending_user_id = user.user_id
            state.pending_area_id = request.area_id
            state.pending_direction = request.direction.value
            state.pending_created_at = request.created_at
            state.pending_authorized_at = now
            state.pending_expires_at = request.expires_at
            state.last_updated_at = now
            session.add(state)
            authorized_user_id = user.user_id
            authorized_user_name = user.name
            authorized_user = self._request_user(user)
            session.commit()
            failed_attempts = state.failed_attempts
            lockdown_active = state.lockdown_active

        message = "Open the door and report the result."
        self.runtime.mark_request(
            request.request_id,
            RequestStatus.AUTHORIZED_WAITING_DOOR,
            user_id=authorized_user_id,
            user_name=authorized_user_name,
            reason_code=ReasonCode.AUTHORIZED,
            message=message,
        )
        return AccessCheckResponse(
            request_id=request.request_id,
            status=RequestStatus.AUTHORIZED_WAITING_DOOR,
            reason_code=ReasonCode.AUTHORIZED,
            message=message,
            failed_attempts=failed_attempts,
            lockdown_active=lockdown_active,
            user=authorized_user,
        )

    def _complete_response_from_log(
        self,
        log: AccessLog,
        state: SystemState,
        claimed_result: DoorResult,
    ) -> AccessCompleteResponse:
        if log.decision == "GRANTED":
            committed_result = DoorResult.DOOR_OPENED
            status = RequestStatus.GRANTED
            reason = ReasonCode.AUTHORIZED
        elif log.denial_reason == ReasonCode.DOOR_OPEN_FAILED.value:
            committed_result = DoorResult.DOOR_OPEN_FAILED
            status = RequestStatus.FAILED
            reason = ReasonCode.DOOR_OPEN_FAILED
        else:
            raise api_error(409, ReasonCode.REQUEST_OUTCOME_CONFLICT)
        if committed_result != claimed_result:
            raise api_error(409, ReasonCode.REQUEST_OUTCOME_CONFLICT)
        return AccessCompleteResponse(
            request_id=log.request_id,
            status=status,
            reason_code=reason,
            message=MESSAGES[reason],
            failed_attempts=state.failed_attempts,
            lockdown_active=state.lockdown_active,
            idempotent_replay=True,
        )

    def complete(self, payload: AccessCompleteRequest) -> AccessCompleteResponse:
        now = self.runtime.now()
        with self._write_lock, Session(self.engine) as session:
            self._begin_immediate(session)
            state = self._system_state(session)
            final_log = session.exec(
                select(AccessLog).where(AccessLog.request_id == payload.request_id)
            ).first()
            if final_log is not None:
                response = self._complete_response_from_log(
                    final_log, state, payload.door_result
                )
                session.rollback()
                return response

            if not self._has_pending(state):
                session.rollback()
                raise api_error(410, ReasonCode.REQUEST_EXPIRED)
            if state.pending_request_id != payload.request_id:
                session.rollback()
                raise api_error(409, ReasonCode.REQUEST_IN_PROGRESS)

            attendance = session.get(
                UserAreaStatus,
                (state.pending_user_id, state.pending_area_id),
            )
            if attendance is None:
                session.rollback()
                raise RuntimeError("Pending authorization attendance row is missing")

            if payload.door_result == DoorResult.DOOR_OPENED:
                attendance.is_inside = state.pending_direction == Direction.ENTRY.value
                attendance.updated_at = now
                decision = "GRANTED"
                denial_reason = None
                status = RequestStatus.GRANTED
                reason = ReasonCode.AUTHORIZED
                state.door_state = DoorState.OPEN.value
                if not state.lockdown_active:
                    state.failed_attempts = 0
                session.add(attendance)
            else:
                decision = "DENIED"
                denial_reason = ReasonCode.DOOR_OPEN_FAILED.value
                status = RequestStatus.FAILED
                reason = ReasonCode.DOOR_OPEN_FAILED
                state.door_state = DoorState.CLOSED.value

            log = AccessLog(
                user_id=state.pending_user_id,
                area_id=state.pending_area_id,
                direction=state.pending_direction,
                decision=decision,
                denial_reason=denial_reason,
                authentication_method="FINGERPRINT",
                event_timestamp=now,
                request_id=state.pending_request_id,
                request_created_at=state.pending_created_at,
            )
            self._clear_pending(state)
            state.last_updated_at = now
            session.add(state)
            session.add(log)
            session.commit()
            failed_attempts = state.failed_attempts
            lockdown_active = state.lockdown_active

        self.runtime.mark_request(
            payload.request_id,
            status,
            reason_code=reason,
            message=MESSAGES[reason],
        )
        return AccessCompleteResponse(
            request_id=payload.request_id,
            status=status,
            reason_code=reason,
            message=MESSAGES[reason],
            failed_attempts=failed_attempts,
            lockdown_active=lockdown_active,
        )

    def security_event(self, payload: SecurityEventRequest) -> SecurityEventResponse:
        now = self.runtime.now()
        with self._write_lock, Session(self.engine) as session:
            self._begin_immediate(session)
            state = self._system_state(session)
            same_identity = (
                state.last_security_boot_id == payload.boot_id
                and state.last_security_event_id == payload.event_id
            )
            if same_identity:
                if state.last_security_event_type != payload.event_type.value:
                    session.rollback()
                    raise api_error(409, ReasonCode.SECURITY_EVENT_CONFLICT)
                response = SecurityEventResponse(
                    event_id=payload.event_id,
                    accepted=True,
                    failed_attempts=state.failed_attempts,
                    lockdown_active=state.lockdown_active,
                    admin_mode=state.admin_mode,
                    idempotent_replay=True,
                )
                session.rollback()
                return response

            if payload.event_type == SecurityEventType.UNKNOWN_RFID:
                self._increment_failure(state)
            else:
                state.failed_attempts = 0
                state.lockdown_active = False
                state.admin_mode = True

            state.last_security_boot_id = payload.boot_id
            state.last_security_event_id = payload.event_id
            state.last_security_event_type = payload.event_type.value
            state.last_security_event_at = now
            state.last_updated_at = now
            session.add(state)
            session.commit()
            return SecurityEventResponse(
                event_id=payload.event_id,
                accepted=True,
                failed_attempts=state.failed_attempts,
                lockdown_active=state.lockdown_active,
                admin_mode=state.admin_mode,
            )
