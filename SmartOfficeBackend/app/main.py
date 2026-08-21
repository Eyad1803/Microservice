from __future__ import annotations

from contextlib import asynccontextmanager
from pathlib import Path
from typing import Annotated

from fastapi import FastAPI, HTTPException, Query
from sqlalchemy import and_, func, text
from sqlalchemy.engine import Engine
from sqlmodel import Session, select

from app.access_runtime import SingleStationRuntime
from app.access_service import AccessService, as_utc
from app.database import DATABASE_PATH, engine
from app.models import AccessLog, Area, Company, Permission, SystemState, User, UserAreaStatus
from app.schemas import (
    AccessCheckRequest,
    AccessCheckResponse,
    AccessCompleteRequest,
    AccessCompleteResponse,
    AccessLogListResponse,
    AccessLogResponse,
    AccessRequestAccepted,
    AccessRequestCreate,
    AccessRequestStatusResponse,
    AreaResponse,
    BootstrapResponse,
    HeartbeatRequest,
    HeartbeatResponse,
    SecurityEventRequest,
    SecurityEventResponse,
    SystemStateResponse,
    UserAreaResponse,
    UserDetailsResponse,
    UserResponse,
)
from app.seed import initialize_database


def create_app(
    *,
    target_engine: Engine = engine,
    database_path: Path = DATABASE_PATH,
    runtime: SingleStationRuntime | None = None,
    initialize_on_startup: bool = True,
) -> FastAPI:
    station_runtime = runtime or SingleStationRuntime()
    access_service = AccessService(target_engine, station_runtime)

    @asynccontextmanager
    async def lifespan(application: FastAPI):
        del application
        if initialize_on_startup:
            initialize_database(database_path, target_engine)
        yield

    application = FastAPI(title="Smart Office Backend", lifespan=lifespan)
    application.state.access_runtime = station_runtime
    application.state.access_service = access_service

    @application.get("/")
    def read_root() -> dict[str, str]:
        return {"message": "Smart Office Backend", "status": "running"}

    @application.get("/api/health")
    def read_health() -> dict[str, str]:
        try:
            with Session(target_engine) as session:
                session.exec(text("SELECT 1")).one()
        except Exception as exc:
            raise HTTPException(
                status_code=503,
                detail={"status": "error", "database": "unavailable"},
            ) from exc
        return {"status": "ok", "database": "connected"}

    @application.get("/api/system-state", response_model=SystemStateResponse)
    def read_system_state() -> SystemStateResponse:
        return access_service.system_state()

    @application.get("/api/users", response_model=list[UserResponse])
    def read_users() -> list[UserResponse]:
        with Session(target_engine) as session:
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

    @application.get("/api/users/{user_id}", response_model=UserDetailsResponse)
    def read_user(user_id: int) -> UserDetailsResponse:
        with Session(target_engine) as session:
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

    @application.get("/api/areas", response_model=list[AreaResponse])
    def read_areas() -> list[AreaResponse]:
        with Session(target_engine) as session:
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

    @application.get("/api/access-logs", response_model=AccessLogListResponse)
    def read_access_logs(
        limit: Annotated[int, Query(ge=1, le=100)] = 50,
    ) -> AccessLogListResponse:
        with Session(target_engine) as session:
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

    @application.post(
        "/api/access/requests",
        response_model=AccessRequestAccepted,
        status_code=202,
    )
    def create_access_request(payload: AccessRequestCreate) -> AccessRequestAccepted:
        return access_service.create_access_request(payload)

    @application.get(
        "/api/access/requests/{request_id}",
        response_model=AccessRequestStatusResponse,
    )
    def read_access_request(request_id: str) -> AccessRequestStatusResponse:
        return access_service.request_status(request_id)

    @application.post("/api/esp32/heartbeat", response_model=HeartbeatResponse)
    def receive_heartbeat(payload: HeartbeatRequest) -> HeartbeatResponse:
        return access_service.heartbeat(payload)

    @application.get("/api/esp32/bootstrap", response_model=BootstrapResponse)
    def read_bootstrap() -> BootstrapResponse:
        return access_service.bootstrap()

    @application.post("/api/access/check", response_model=AccessCheckResponse)
    def check_access(payload: AccessCheckRequest) -> AccessCheckResponse:
        return access_service.access_check(payload)

    @application.post("/api/access/complete", response_model=AccessCompleteResponse)
    def complete_access(payload: AccessCompleteRequest) -> AccessCompleteResponse:
        return access_service.complete(payload)

    @application.post(
        "/api/esp32/security-events",
        response_model=SecurityEventResponse,
    )
    def receive_security_event(payload: SecurityEventRequest) -> SecurityEventResponse:
        return access_service.security_event(payload)

    return application


app = create_app()
