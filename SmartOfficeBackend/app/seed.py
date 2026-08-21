"""Safe new-database initialization and validation-only existing startup."""

from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path

from sqlalchemy.engine import Engine
from sqlmodel import Session

from app.database import (
    DATABASE_PATH,
    create_db_and_tables,
    create_sqlite_engine,
    engine,
)
from app.database_validation import (
    DatabaseValidationError,
    connect_read_only,
    logical_tables,
    validate_existing_database,
)
from app.models import Area, Company, Permission, SystemState, User, UserAreaStatus
from app.reference_data import AREAS, COMPANIES, PERMISSIONS, STATUS_KEYS, USERS


def create_new_database_reference_data(target_engine: Engine) -> None:
    """Insert canonical data once after creating a genuinely new database."""
    created_at = datetime.now(timezone.utc)
    with Session(target_engine) as session:
        session.add_all(
            [Company(company_id=company_id, name=name) for company_id, name in COMPANIES]
        )
        session.add_all(
            [
                User(
                    user_id=user_id,
                    company_id=company_id,
                    name=name,
                    role=role,
                    fingerprint_id=fingerprint_id,
                    is_active=is_active,
                )
                for user_id, company_id, name, role, fingerprint_id, is_active in USERS
            ]
        )
        session.add_all(
            [
                Area(area_id=area_id, name=name, is_active=is_active)
                for area_id, name, is_active in AREAS
            ]
        )
        session.add_all(
            [
                Permission(user_id=user_id, area_id=area_id, allowed=allowed)
                for user_id, area_id, allowed in PERMISSIONS
            ]
        )
        session.add_all(
            [
                UserAreaStatus(
                    user_id=user_id,
                    area_id=area_id,
                    is_inside=False,
                    updated_at=created_at,
                )
                for user_id, area_id in STATUS_KEYS
            ]
        )
        session.add(SystemState(system_state_id=1))
        session.commit()


def _choose_engine(database_path: Path, target_engine: Engine | None) -> Engine:
    if target_engine is not None:
        return target_engine
    if database_path.resolve() == DATABASE_PATH.resolve():
        return engine
    return create_sqlite_engine(database_path)


def _existing_logical_tables(database_path: Path) -> set[str]:
    try:
        with connect_read_only(database_path) as connection:
            return logical_tables(connection)
    except Exception as exc:
        raise DatabaseValidationError(
            f"Could not inspect existing database {database_path}: {exc}"
        ) from exc


def initialize_database(
    database_path: Path = DATABASE_PATH,
    target_engine: Engine | None = None,
) -> str:
    """Create a genuinely new database or validate an existing one without writes."""
    database_path = database_path.resolve()
    database_exists = database_path.exists()
    tables = _existing_logical_tables(database_path) if database_exists else set()

    if database_exists and tables:
        validate_existing_database(database_path)
        return "EXISTING_VALID"

    selected_engine = _choose_engine(database_path, target_engine)
    create_db_and_tables(selected_engine)
    create_new_database_reference_data(selected_engine)
    validate_existing_database(database_path)
    return "NEW_INITIALIZED"


if __name__ == "__main__":
    result = initialize_database()
    print(f"Smart Office database initialization result: {result}")
