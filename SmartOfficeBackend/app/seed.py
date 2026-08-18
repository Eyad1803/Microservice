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
    User(user_id=3, company_id=3, name="IT Admin", role="IT", fingerprint_id=3),
    User(user_id=4, company_id=4, name="Manager", role="Manager", fingerprint_id=4),
    User(user_id=5, company_id=5, name="Employee C", role="Employee", fingerprint_id=5),
    User(user_id=6, company_id=6, name="Employee D", role="Employee", fingerprint_id=6),
)

AREAS = (
    Area(area_id=1, name="Main Entrance"),
    Area(area_id=2, name="Company A"),
    Area(area_id=3, name="Company B"),
    Area(area_id=4, name="Server Room"),
    Area(area_id=5, name="Management / Admin"),
    Area(area_id=6, name="Company C"),
    Area(area_id=7, name="Company D"),
)

ALLOWED_AREA_IDS = {
    1: {1, 2},
    2: {1, 3},
    3: {1, 4},
    4: {1, 2, 3, 4, 5, 6, 7},
    5: {1, 6},
    6: {1, 7},
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
        area_id=4,
        direction="ENTRY",
        decision="DENIED",
        denial_reason="UNKNOWN_FINGERPRINT",
        authentication_method="FINGERPRINT",
        event_timestamp=datetime(2026, 1, 15, 17, 5, tzinfo=timezone.utc),
    ),
)


def seed_database() -> None:
    """Insert each required demo row only when that primary key is absent."""
    with Session(engine) as session:
        for company in COMPANIES:
            if session.get(Company, company.company_id) is None:
                session.add(company)
        session.commit()

        for user in USERS:
            if session.get(User, user.user_id) is None:
                session.add(user)
        for area in AREAS:
            if session.get(Area, area.area_id) is None:
                session.add(area)
        session.commit()

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
                if session.get(UserAreaStatus, key) is None:
                    session.add(
                        UserAreaStatus(
                            user_id=user_id,
                            area_id=area_id,
                            is_inside=False,
                            updated_at=seeded_at,
                        )
                    )

        for access_log in DEMO_ACCESS_LOGS:
            if session.get(AccessLog, access_log.access_log_id) is None:
                session.add(access_log)

        if session.get(SystemState, 1) is None:
            session.add(SystemState(system_state_id=1))

        session.commit()


def initialize_database() -> None:
    create_db_and_tables()
    seed_database()


if __name__ == "__main__":
    initialize_database()
    print("Smart Office database initialized successfully.")
