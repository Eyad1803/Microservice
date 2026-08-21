from __future__ import annotations

from datetime import datetime
from enum import Enum

from pydantic import model_validator
from sqlmodel import Field, SQLModel


class Direction(str, Enum):
    ENTRY = "ENTRY"
    EXIT = "EXIT"


class DoorState(str, Enum):
    OPEN = "OPEN"
    CLOSED = "CLOSED"


class RequestStatus(str, Enum):
    QUEUED = "QUEUED"
    IN_PROGRESS = "IN_PROGRESS"
    AUTHORIZED_WAITING_DOOR = "AUTHORIZED_WAITING_DOOR"
    GRANTED = "GRANTED"
    DENIED = "DENIED"
    FAILED = "FAILED"
    EXPIRED = "EXPIRED"


class FingerprintResult(str, Enum):
    MATCH = "MATCH"
    NOT_RECOGNIZED = "NOT_RECOGNIZED"
    TIMEOUT = "TIMEOUT"
    READ_ERROR = "READ_ERROR"
    UNAVAILABLE = "UNAVAILABLE"
    NOT_SCANNED = "NOT_SCANNED"


class DoorResult(str, Enum):
    DOOR_OPENED = "DOOR_OPENED"
    DOOR_OPEN_FAILED = "DOOR_OPEN_FAILED"


class SecurityEventType(str, Enum):
    ADMIN_RFID_ACCEPTED = "ADMIN_RFID_ACCEPTED"
    UNKNOWN_RFID = "UNKNOWN_RFID"


class ReasonCode(str, Enum):
    AUTHORIZED = "AUTHORIZED"
    UNKNOWN_FINGERPRINT = "UNKNOWN_FINGERPRINT"
    USER_INACTIVE = "USER_INACTIVE"
    NO_PERMISSION = "NO_PERMISSION"
    ALREADY_INSIDE = "ALREADY_INSIDE"
    ALREADY_OUTSIDE = "ALREADY_OUTSIDE"
    LOCKDOWN_ACTIVE = "LOCKDOWN_ACTIVE"
    SYSTEM_INACTIVE = "SYSTEM_INACTIVE"
    AREA_NOT_FOUND = "AREA_NOT_FOUND"
    AREA_INACTIVE = "AREA_INACTIVE"
    PERSON_NOT_DETECTED = "PERSON_NOT_DETECTED"
    ULTRASONIC_UNAVAILABLE = "ULTRASONIC_UNAVAILABLE"
    FINGERPRINT_UNAVAILABLE = "FINGERPRINT_UNAVAILABLE"
    FINGERPRINT_TIMEOUT = "FINGERPRINT_TIMEOUT"
    FINGERPRINT_READ_ERROR = "FINGERPRINT_READ_ERROR"
    DOOR_ALREADY_OPEN = "DOOR_ALREADY_OPEN"
    DOOR_OPEN_FAILED = "DOOR_OPEN_FAILED"
    ESP32_OFFLINE = "ESP32_OFFLINE"
    REQUEST_IN_PROGRESS = "REQUEST_IN_PROGRESS"
    REQUEST_EXPIRED = "REQUEST_EXPIRED"
    REQUEST_OUTCOME_CONFLICT = "REQUEST_OUTCOME_CONFLICT"
    SECURITY_EVENT_CONFLICT = "SECURITY_EVENT_CONFLICT"


class ActiveAccessRequestResponse(SQLModel):
    request_id: str
    status: RequestStatus
    area_id: int
    direction: Direction


class SystemStateResponse(SQLModel):
    system_active: bool
    lockdown_active: bool
    failed_attempts: int
    failed_attempt_limit: int
    admin_mode: bool
    door_state: DoorState
    esp32_online: bool
    esp32_last_seen_at: datetime | None
    last_updated_at: datetime
    person_detected: bool | None = None
    distance_cm: float | None = None
    fingerprint_ready: bool | None = None
    active_access_request: ActiveAccessRequestResponse | None = None


class UserResponse(SQLModel):
    user_id: int
    name: str
    company: str
    role: str
    fingerprint_id: int
    is_active: bool


class UserAreaResponse(SQLModel):
    area_id: int
    area_name: str
    allowed: bool
    is_inside: bool


class UserDetailsResponse(UserResponse):
    areas: list[UserAreaResponse]


class AreaResponse(SQLModel):
    area_id: int
    name: str
    is_active: bool
    occupancy: int


class AccessLogResponse(SQLModel):
    access_log_id: int
    user_id: int | None
    user_name: str
    area_id: int
    area_name: str
    direction: Direction
    decision: str
    denial_reason: str | None
    authentication_method: str
    event_timestamp: datetime


class AccessLogListResponse(SQLModel):
    items: list[AccessLogResponse]


class AccessRequestCreate(SQLModel):
    area_id: int
    direction: Direction


class AccessRequestAccepted(SQLModel):
    request_id: str
    status: RequestStatus
    area_id: int
    direction: Direction
    created_at: datetime
    expires_at: datetime


class RequestUserResponse(SQLModel):
    user_id: int
    name: str


class AccessRequestStatusResponse(SQLModel):
    request_id: str
    status: RequestStatus
    area_id: int
    direction: Direction
    user: RequestUserResponse | None = None
    reason_code: ReasonCode | None = None
    message: str | None = None
    created_at: datetime
    updated_at: datetime
    expires_at: datetime | None = None


class HeartbeatRequest(SQLModel):
    boot_id: str = Field(min_length=1)
    door_state: DoorState
    person_detected: bool | None
    distance_cm: float | None = Field(default=None, ge=0)
    fingerprint_ready: bool
    admin_mode: bool
    active_request_id: str | None = None


class AccessCommandResponse(SQLModel):
    request_id: str
    area_id: int
    direction: Direction
    expires_at: datetime


class HeartbeatResponse(SQLModel):
    server_time: datetime
    system_active: bool
    failed_attempts: int
    lockdown_active: bool
    admin_mode: bool
    pending_request_id: str | None
    command: AccessCommandResponse | None


class AttendanceResponse(SQLModel):
    user_id: int
    area_id: int
    is_inside: bool


class PendingAuthorizationResponse(SQLModel):
    request_id: str
    user_id: int
    area_id: int
    direction: Direction
    created_at: datetime
    authorized_at: datetime
    expires_at: datetime


class BootstrapResponse(SQLModel):
    system_active: bool
    failed_attempts: int
    lockdown_active: bool
    admin_mode: bool
    attendance: list[AttendanceResponse]
    pending_authorization: PendingAuthorizationResponse | None


class AccessPrecheck(SQLModel):
    door_state: DoorState
    person_detected: bool | None
    distance_cm: float | None = Field(default=None, ge=0)
    fingerprint_ready: bool


class AccessCheckRequest(SQLModel):
    request_id: str = Field(min_length=1)
    precheck: AccessPrecheck
    fingerprint_result: FingerprintResult
    fingerprint_id: int | None = None
    confidence: int | None = Field(default=None, ge=0)

    @model_validator(mode="after")
    def require_fingerprint_id_for_match(self) -> "AccessCheckRequest":
        if self.fingerprint_result == FingerprintResult.MATCH and self.fingerprint_id is None:
            raise ValueError("fingerprint_id is required when fingerprint_result is MATCH")
        return self


class AccessCheckResponse(SQLModel):
    request_id: str
    status: RequestStatus
    reason_code: ReasonCode
    message: str
    failed_attempts: int
    lockdown_active: bool
    user: RequestUserResponse | None = None
    idempotent_replay: bool = False


class AccessCompleteRequest(SQLModel):
    request_id: str = Field(min_length=1)
    door_result: DoorResult


class AccessCompleteResponse(SQLModel):
    request_id: str
    status: RequestStatus
    reason_code: ReasonCode
    message: str
    failed_attempts: int
    lockdown_active: bool
    idempotent_replay: bool = False


class SecurityEventRequest(SQLModel):
    boot_id: str = Field(min_length=1)
    event_id: str = Field(min_length=1)
    event_type: SecurityEventType


class SecurityEventResponse(SQLModel):
    event_id: str
    accepted: bool
    failed_attempts: int
    lockdown_active: bool
    admin_mode: bool
    idempotent_replay: bool = False
