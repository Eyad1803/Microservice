from datetime import datetime, timezone

from sqlmodel import Session

from app.database import create_db_and_tables, engine
from app.models import (
    AccessLog,
    Area,
    Company,
    Permission,
    SystemState,
    User,
    UserAreaStatus,
)


COMPANIES = (
    Company(company_id=1, name="Company A"),
    Company(company_id=2, name="Company B"),
    Company(company_id=3, name="IT"),
    Company(company_id=4, name="Management"),
    Company(company_id=5, name="Company C"),
    Company(company_id=6, name="Company D"),
)

USERS = (
    User(user_id=1, company_id=1, name="Employee A", role="Employee", fingerprint_id=1),
    User(user_id=2, company_id=2, name="Employee B", role="Employee", fingerprint_id=2),
    User(user_id=3, company_id=5, name="Employee C", role="Employee", fingerprint_id=3),
    User(user_id=4, company_id=6, name="Employee D", role="Employee", fingerprint_id=4),
    User(user_id=5, company_id=3, name="IT Admin", role="IT", fingerprint_id=5),
    User(user_id=6, company_id=4, name="Manager", role="Manager", fingerprint_id=6),
)

AREAS = (
    Area(area_id=1, name="Company A"),
    Area(area_id=2, name="Company B"),
    Area(area_id=3, name="Company C"),
    Area(area_id=4, name="Company D"),
    Area(area_id=5, name="Server Room"),
    Area(area_id=6, name="Management / Admin"),
    Area(area_id=7, name="Main Entrance"),
)

ALLOWED_AREA_IDS = {
    1: {1, 7},
    2: {2, 7},
    3: {3, 7},
    4: {4, 7},
    5: {5, 7},
    6: {1, 2, 3, 4, 5, 6, 7},
}

DEMO_ACCESS_LOGS = (
    AccessLog(
        access_log_id=1,
        user_id=1,
        area_id=1,
        direction="ENTRY",
        decision="GRANTED",
        authentication_method="FINGERPRINT",
        event_timestamp=datetime(2026, 1, 15, 8, 0, tzinfo=timezone.utc),
    ),
    AccessLog(
        access_log_id=2,
        user_id=1,
        area_id=1,
        direction="EXIT",
        decision="GRANTED",
        authentication_method="FINGERPRINT",
        event_timestamp=datetime(2026, 1, 15, 17, 0, tzinfo=timezone.utc),
    ),
    AccessLog(
        access_log_id=3,
        user_id=None,
        area_id=5,
        direction="ENTRY",
        decision="DENIED",
        denial_reason="UNKNOWN_FINGERPRINT",
        authentication_method="FINGERPRINT",
        event_timestamp=datetime(2026, 1, 15, 17, 5, tzinfo=timezone.utc),
    ),
)


def seed_database() -> None:
    """Synchronize the fixed demo mapping without duplicating seeded rows."""
    with Session(engine) as session:
        for company in COMPANIES:
            existing_company = session.get(Company, company.company_id)
            if existing_company is None:
                session.add(company)
            else:
                existing_company.name = company.name

        mapping_changed = False
        for user in USERS:
            existing_user = session.get(User, user.user_id)
            if existing_user is None:
                session.add(user)
                continue
            user_changed = (
                existing_user.company_id != user.company_id
                or existing_user.name != user.name
                or existing_user.role != user.role
                or existing_user.fingerprint_id != user.fingerprint_id
            )
            mapping_changed |= user_changed
            existing_user.company_id = user.company_id
            existing_user.name = user.name
            existing_user.role = user.role
            existing_user.fingerprint_id = user.fingerprint_id
            existing_user.is_active = True

        area_mapping_changed = any(
            (existing_area := session.get(Area, area.area_id)) is not None
            and existing_area.name != area.name
            for area in AREAS
        )
        mapping_changed |= area_mapping_changed

        if area_mapping_changed:
            # Area names are unique. Move all existing seeded IDs to unique
            # temporary names in the same transaction before assigning the
            # final reordered names, avoiding destructive delete/reseed logic.
            for area in AREAS:
                existing_area = session.get(Area, area.area_id)
                if existing_area is not None:
                    existing_area.name = f"__area_remap_{area.area_id}__"
            session.flush()

        for area in AREAS:
            existing_area = session.get(Area, area.area_id)
            if existing_area is None:
                session.add(area)
            else:
                existing_area.name = area.name
                existing_area.is_active = True

        seeded_at = datetime.now(timezone.utc)
        for user_id in range(1, 7):
            for area_id in range(1, 8):
                key = (user_id, area_id)
                if session.get(Permission, key) is None:
                    session.add(
                        Permission(
                            user_id=user_id,
                            area_id=area_id,
                            allowed=area_id in ALLOWED_AREA_IDS[user_id],
                        )
                    )
                else:
                    permission = session.get(Permission, key)
                    permission.allowed = area_id in ALLOWED_AREA_IDS[user_id]
                if session.get(UserAreaStatus, key) is None:
                    session.add(
                        UserAreaStatus(
                            user_id=user_id,
                            area_id=area_id,
                            is_inside=False,
                            updated_at=seeded_at,
                        )
                    )
                elif mapping_changed:
                    area_status = session.get(UserAreaStatus, key)
                    area_status.is_inside = False
                    area_status.updated_at = seeded_at

        for access_log in DEMO_ACCESS_LOGS:
            existing_log = session.get(AccessLog, access_log.access_log_id)
            if existing_log is None:
                session.add(access_log)
            else:
                existing_log.user_id = access_log.user_id
                existing_log.area_id = access_log.area_id
                existing_log.direction = access_log.direction
                existing_log.decision = access_log.decision
                existing_log.denial_reason = access_log.denial_reason
                existing_log.authentication_method = access_log.authentication_method
                existing_log.event_timestamp = access_log.event_timestamp

        if session.get(SystemState, 1) is None:
            session.add(SystemState(system_state_id=1))

        session.commit()


def initialize_database() -> None:
    create_db_and_tables()
    seed_database()


if __name__ == "__main__":
    initialize_database()
    print("Smart Office database initialized successfully.")
