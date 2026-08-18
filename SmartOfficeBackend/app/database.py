from pathlib import Path

from sqlalchemy import event
from sqlmodel import Session, SQLModel, create_engine


DATABASE_PATH = Path(__file__).resolve().parent.parent / "smart_office.db"
DATABASE_URL = f"sqlite:///{DATABASE_PATH.as_posix()}"

engine = create_engine(
    DATABASE_URL,
    connect_args={"check_same_thread": False},
)


@event.listens_for(engine, "connect")
def enable_sqlite_foreign_keys(dbapi_connection, connection_record) -> None:
    """Enable foreign-key enforcement for every SQLite connection."""
    del connection_record
    cursor = dbapi_connection.cursor()
    cursor.execute("PRAGMA foreign_keys=ON")
    cursor.close()


def create_db_and_tables() -> None:
    # Importing the models registers exactly the seven tables in the metadata.
    from app import models  # noqa: F401

    SQLModel.metadata.create_all(engine)


def get_session():
    with Session(engine) as session:
        yield session
