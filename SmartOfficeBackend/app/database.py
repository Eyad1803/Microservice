import os
from pathlib import Path

from sqlalchemy import event
from sqlalchemy.engine import Engine
from sqlmodel import Session, SQLModel, create_engine


BACKEND_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DATABASE_PATH = BACKEND_ROOT / "smart_office.db"
DATABASE_PATH_ENV_VAR = "SMART_OFFICE_DATABASE_PATH"


def resolve_database_path(override: str | None = None) -> Path:
    """Resolve an explicit database override or retain the live default."""
    configured_path = override if override is not None else os.environ.get(DATABASE_PATH_ENV_VAR)
    if configured_path and configured_path.strip():
        return Path(configured_path.strip()).expanduser().resolve()
    return DEFAULT_DATABASE_PATH.resolve()


DATABASE_PATH = resolve_database_path()


def enable_sqlite_foreign_keys(dbapi_connection, connection_record) -> None:
    """Enable foreign-key enforcement for every SQLite connection."""
    del connection_record
    cursor = dbapi_connection.cursor()
    cursor.execute("PRAGMA foreign_keys=ON")
    cursor.close()


def create_sqlite_engine(database_path: Path) -> Engine:
    database_url = f"sqlite:///{database_path.resolve().as_posix()}"
    target_engine = create_engine(
        database_url,
        connect_args={"check_same_thread": False},
    )
    event.listen(target_engine, "connect", enable_sqlite_foreign_keys)
    return target_engine


engine = create_sqlite_engine(DATABASE_PATH)


def create_db_and_tables(target_engine: Engine = engine) -> None:
    # Importing the models registers exactly the seven tables in the metadata.
    from app import models  # noqa: F401

    SQLModel.metadata.create_all(target_engine)


def get_session():
    with Session(engine) as session:
        yield session
