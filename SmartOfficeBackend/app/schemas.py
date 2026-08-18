from datetime import datetime

from sqlmodel import SQLModel


class SystemStateResponse(SQLModel):
    system_active: bool
    lockdown_active: bool
    failed_attempts: int
    failed_attempt_limit: int
    admin_mode: bool
    door_state: str
    esp32_online: bool
    esp32_last_seen_at: datetime | None
    last_updated_at: datetime


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
    direction: str
    decision: str
    denial_reason: str | None
    authentication_method: str
    event_timestamp: datetime


class AccessLogListResponse(SQLModel):
    items: list[AccessLogResponse]
