from contextlib import asynccontextmanager
from datetime import datetime, timezone
from typing import Annotated

from fastapi import FastAPI, HTTPException, Query
from sqlalchemy import and_, func, text
from sqlmodel import Session, select

from app.database import engine
from app.models import AccessLog, Area, Company, Permission, SystemState, User, UserAreaStatus
from app.schemas import (
    AccessLogListResponse,
    AccessLogResponse,
    AreaResponse,
    SystemStateResponse,
    UserAreaResponse,
    UserDetailsResponse,
    UserResponse,
)
from app.seed import initialize_database


FAILED_ATTEMPT_LIMIT = 3


def as_utc(value: datetime | None) -> datetime | None:
    """Return a database timestamp with explicit UTC timezone information."""
    if value is None:
        return None
    if value.tzinfo is None:
        return value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc)


@asynccontextmanager
async def lifespan(app: FastAPI):
    del app
    initialize_database()
    yield


app = FastAPI(title="Smart Office Backend", lifespan=lifespan)


@app.get("/")
def read_root() -> dict[str, str]:
    return {"message": "Smart Office Backend", "status": "running"}


@app.get("/api/health")
def read_health() -> dict[str, str]:
    try:
        with Session(engine) as session:
            session.exec(text("SELECT 1")).one()
    except Exception as exc:
        raise HTTPException(
            status_code=503,
            detail={"status": "error", "database": "unavailable"},
        ) from exc

    return {"status": "ok", "database": "connected"}


@app.get("/api/system-state", response_model=SystemStateResponse)
def read_system_state() -> SystemStateResponse:
    with Session(engine) as session:
        system_state = session.get(SystemState, 1)

        if system_state is None:
            raise HTTPException(status_code=404, detail="System state not found")

        return SystemStateResponse(
            system_active=system_state.system_active,
            lockdown_active=system_state.lockdown_active,
            failed_attempts=system_state.failed_attempts,
            failed_attempt_limit=FAILED_ATTEMPT_LIMIT,
            admin_mode=system_state.admin_mode,
            door_state=system_state.door_state,
            esp32_online=system_state.esp32_online,
            esp32_last_seen_at=as_utc(system_state.esp32_last_seen_at),
            last_updated_at=as_utc(system_state.last_updated_at),
        )


@app.get("/api/users", response_model=list[UserResponse])
def read_users() -> list[UserResponse]:
    with Session(engine) as session:
        rows = session.exec(
            select(User, Company)
            .join(Company, User.company_id == Company.company_id)
            .order_by(User.user_id)
        ).all()

        return [
            UserResponse(
                user_id=user.user_id,
                name=user.name,
                company=company.name,
                role=user.role,
                fingerprint_id=user.fingerprint_id,
                is_active=user.is_active,
            )
            for user, company in rows
        ]


@app.get("/api/users/{user_id}", response_model=UserDetailsResponse)
def read_user(user_id: int) -> UserDetailsResponse:
    with Session(engine) as session:
        user_row = session.exec(
            select(User, Company)
            .join(Company, User.company_id == Company.company_id)
            .where(User.user_id == user_id)
        ).first()

        if user_row is None:
            raise HTTPException(status_code=404, detail="User not found")

        user, company = user_row
        area_rows = session.exec(
            select(Area, Permission.allowed, UserAreaStatus.is_inside)
            .join(Permission, Permission.area_id == Area.area_id)
            .join(UserAreaStatus, UserAreaStatus.area_id == Area.area_id)
            .where(
                Permission.user_id == user_id,
                UserAreaStatus.user_id == user_id,
            )
            .order_by(Area.area_id)
        ).all()

        return UserDetailsResponse(
            user_id=user.user_id,
            name=user.name,
            company=company.name,
            role=user.role,
            fingerprint_id=user.fingerprint_id,
            is_active=user.is_active,
            areas=[
                UserAreaResponse(
                    area_id=area.area_id,
                    area_name=area.name,
                    allowed=allowed,
                    is_inside=is_inside,
                )
                for area, allowed, is_inside in area_rows
            ],
        )


@app.get("/api/areas", response_model=list[AreaResponse])
def read_areas() -> list[AreaResponse]:
    with Session(engine) as session:
        rows = session.exec(
            select(Area, func.count(UserAreaStatus.user_id))
            .outerjoin(
                UserAreaStatus,
                and_(
                    UserAreaStatus.area_id == Area.area_id,
                    UserAreaStatus.is_inside.is_(True),
                ),
            )
            .group_by(Area.area_id)
            .order_by(Area.area_id)
        ).all()

        return [
            AreaResponse(
                area_id=area.area_id,
                name=area.name,
                is_active=area.is_active,
                occupancy=occupancy,
            )
            for area, occupancy in rows
        ]


@app.get("/api/access-logs", response_model=AccessLogListResponse)
def read_access_logs(
    limit: Annotated[int, Query(ge=1, le=100)] = 50,
) -> AccessLogListResponse:
    with Session(engine) as session:
        rows = session.exec(
            select(AccessLog, User.name, Area.name)
            .join(Area, AccessLog.area_id == Area.area_id)
            .outerjoin(User, AccessLog.user_id == User.user_id)
            .order_by(
                AccessLog.event_timestamp.desc(),
                AccessLog.access_log_id.desc(),
            )
            .limit(limit)
        ).all()

        return AccessLogListResponse(
            items=[
                AccessLogResponse(
                    access_log_id=access_log.access_log_id,
                    user_id=access_log.user_id,
                    user_name=user_name or "Unknown Fingerprint",
                    area_id=access_log.area_id,
                    area_name=area_name,
                    direction=access_log.direction,
                    decision=access_log.decision,
                    denial_reason=access_log.denial_reason,
                    authentication_method=access_log.authentication_method,
                    event_timestamp=as_utc(access_log.event_timestamp),
                )
                for access_log, user_name, area_name in rows
            ]
        )
