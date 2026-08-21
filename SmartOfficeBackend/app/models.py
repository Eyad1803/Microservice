from datetime import datetime, timezone

from sqlalchemy import CheckConstraint, Index, text
from sqlmodel import Field, Relationship, SQLModel


def utc_now() -> datetime:
    """Return the current time in UTC."""
    return datetime.now(timezone.utc)


class Company(SQLModel, table=True):
    __tablename__ = "Companies"

    company_id: int = Field(primary_key=True)
    name: str = Field(unique=True)

    users: list["User"] = Relationship(back_populates="company")


class User(SQLModel, table=True):
    __tablename__ = "Users"

    user_id: int = Field(primary_key=True)
    company_id: int = Field(foreign_key="Companies.company_id")
    name: str
    role: str
    fingerprint_id: int = Field(unique=True)
    is_active: bool = Field(default=True)

    company: Company = Relationship(back_populates="users")
    permissions: list["Permission"] = Relationship(back_populates="user")
    area_statuses: list["UserAreaStatus"] = Relationship(back_populates="user")
    access_logs: list["AccessLog"] = Relationship(back_populates="user")


class Area(SQLModel, table=True):
    __tablename__ = "Areas"

    area_id: int = Field(primary_key=True)
    name: str = Field(unique=True)
    is_active: bool = Field(default=True)

    permissions: list["Permission"] = Relationship(back_populates="area")
    user_statuses: list["UserAreaStatus"] = Relationship(back_populates="area")
    access_logs: list["AccessLog"] = Relationship(back_populates="area")


class Permission(SQLModel, table=True):
    __tablename__ = "Permissions"

    user_id: int = Field(foreign_key="Users.user_id", primary_key=True)
    area_id: int = Field(foreign_key="Areas.area_id", primary_key=True)
    allowed: bool = Field(default=False)

    user: User = Relationship(back_populates="permissions")
    area: Area = Relationship(back_populates="permissions")


class UserAreaStatus(SQLModel, table=True):
    __tablename__ = "UserAreaStatus"

    user_id: int = Field(foreign_key="Users.user_id", primary_key=True)
    area_id: int = Field(foreign_key="Areas.area_id", primary_key=True)
    is_inside: bool = Field(default=False)
    updated_at: datetime = Field(default_factory=utc_now)

    user: User = Relationship(back_populates="area_statuses")
    area: Area = Relationship(back_populates="user_statuses")


class AccessLog(SQLModel, table=True):
    __tablename__ = "AccessLogs"
    __table_args__ = (
        CheckConstraint("direction IN ('ENTRY', 'EXIT')"),
        CheckConstraint("decision IN ('GRANTED', 'DENIED')"),
        CheckConstraint("authentication_method IN ('FINGERPRINT', 'RFID')"),
        Index(
            "ux_AccessLogs_request_id_nonnull",
            "request_id",
            unique=True,
            sqlite_where=text("request_id IS NOT NULL"),
        ),
    )

    access_log_id: int | None = Field(default=None, primary_key=True)
    user_id: int | None = Field(default=None, foreign_key="Users.user_id")
    area_id: int = Field(foreign_key="Areas.area_id")
    direction: str
    decision: str
    denial_reason: str | None = Field(default=None)
    authentication_method: str
    event_timestamp: datetime = Field(default_factory=utc_now)
    request_id: str | None = Field(default=None)
    request_created_at: datetime | None = Field(default=None)

    user: User | None = Relationship(back_populates="access_logs")
    area: Area = Relationship(back_populates="access_logs")


class SystemState(SQLModel, table=True):
    __tablename__ = "SystemState"
    __table_args__ = (
        CheckConstraint("system_state_id = 1"),
        CheckConstraint("door_state IN ('OPEN', 'CLOSED')"),
    )

    system_state_id: int = Field(default=1, primary_key=True)
    system_active: bool = Field(default=True)
    lockdown_active: bool = Field(default=False)
    failed_attempts: int = Field(default=0)
    admin_mode: bool = Field(default=False)
    door_state: str = Field(default="CLOSED")
    esp32_online: bool = Field(default=False)
    esp32_last_seen_at: datetime | None = Field(default=None)
    last_updated_at: datetime = Field(default_factory=utc_now)
    pending_request_id: str | None = Field(default=None)
    pending_user_id: int | None = Field(default=None, foreign_key="Users.user_id")
    pending_area_id: int | None = Field(default=None, foreign_key="Areas.area_id")
    pending_direction: str | None = Field(default=None)
    pending_created_at: datetime | None = Field(default=None)
    pending_authorized_at: datetime | None = Field(default=None)
    pending_expires_at: datetime | None = Field(default=None)
    last_security_boot_id: str | None = Field(default=None)
    last_security_event_id: str | None = Field(default=None)
    last_security_event_type: str | None = Field(default=None)
    last_security_event_at: datetime | None = Field(default=None)
