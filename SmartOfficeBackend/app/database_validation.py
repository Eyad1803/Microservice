"""Read-only validation for an existing Smart Office database."""

from __future__ import annotations

import sqlite3
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

from app.reference_data import (
    APPLICATION_TABLES,
    AREAS,
    COMPANIES,
    PERMISSIONS,
    STATUS_KEYS,
    USERS,
)


class DatabaseValidationError(RuntimeError):
    """Raised when an existing database does not match the approved contract."""


FINAL_COLUMNS = {
    "Companies": (
        ("company_id", "INTEGER", 1, 1),
        ("name", "VARCHAR", 1, 0),
    ),
    "Users": (
        ("user_id", "INTEGER", 1, 1),
        ("company_id", "INTEGER", 1, 0),
        ("name", "VARCHAR", 1, 0),
        ("role", "VARCHAR", 1, 0),
        ("fingerprint_id", "INTEGER", 1, 0),
        ("is_active", "BOOLEAN", 1, 0),
    ),
    "Areas": (
        ("area_id", "INTEGER", 1, 1),
        ("name", "VARCHAR", 1, 0),
        ("is_active", "BOOLEAN", 1, 0),
    ),
    "Permissions": (
        ("user_id", "INTEGER", 1, 1),
        ("area_id", "INTEGER", 1, 2),
        ("allowed", "BOOLEAN", 1, 0),
    ),
    "UserAreaStatus": (
        ("user_id", "INTEGER", 1, 1),
        ("area_id", "INTEGER", 1, 2),
        ("is_inside", "BOOLEAN", 1, 0),
        ("updated_at", "DATETIME", 1, 0),
    ),
    "AccessLogs": (
        ("access_log_id", "INTEGER", 1, 1),
        ("user_id", "INTEGER", 0, 0),
        ("area_id", "INTEGER", 1, 0),
        ("direction", "VARCHAR", 1, 0),
        ("decision", "VARCHAR", 1, 0),
        ("denial_reason", "VARCHAR", 0, 0),
        ("authentication_method", "VARCHAR", 1, 0),
        ("event_timestamp", "DATETIME", 1, 0),
        ("request_id", "VARCHAR", 0, 0),
        ("request_created_at", "DATETIME", 0, 0),
    ),
    "SystemState": (
        ("system_state_id", "INTEGER", 1, 1),
        ("system_active", "BOOLEAN", 1, 0),
        ("lockdown_active", "BOOLEAN", 1, 0),
        ("failed_attempts", "INTEGER", 1, 0),
        ("admin_mode", "BOOLEAN", 1, 0),
        ("door_state", "VARCHAR", 1, 0),
        ("esp32_online", "BOOLEAN", 1, 0),
        ("esp32_last_seen_at", "DATETIME", 0, 0),
        ("last_updated_at", "DATETIME", 1, 0),
        ("pending_request_id", "VARCHAR", 0, 0),
        ("pending_user_id", "INTEGER", 0, 0),
        ("pending_area_id", "INTEGER", 0, 0),
        ("pending_direction", "VARCHAR", 0, 0),
        ("pending_created_at", "DATETIME", 0, 0),
        ("pending_authorized_at", "DATETIME", 0, 0),
        ("pending_expires_at", "DATETIME", 0, 0),
        ("last_security_boot_id", "VARCHAR", 0, 0),
        ("last_security_event_id", "VARCHAR", 0, 0),
        ("last_security_event_type", "VARCHAR", 0, 0),
        ("last_security_event_at", "DATETIME", 0, 0),
    ),
}

EXPECTED_FOREIGN_KEYS = {
    "Companies": set(),
    "Users": {("company_id", "Companies", "company_id")},
    "Areas": set(),
    "Permissions": {
        ("user_id", "Users", "user_id"),
        ("area_id", "Areas", "area_id"),
    },
    "UserAreaStatus": {
        ("user_id", "Users", "user_id"),
        ("area_id", "Areas", "area_id"),
    },
    "AccessLogs": {
        ("user_id", "Users", "user_id"),
        ("area_id", "Areas", "area_id"),
    },
    "SystemState": {
        ("pending_user_id", "Users", "user_id"),
        ("pending_area_id", "Areas", "area_id"),
    },
}

PENDING_FIELDS = (
    "pending_request_id",
    "pending_user_id",
    "pending_area_id",
    "pending_direction",
    "pending_created_at",
    "pending_authorized_at",
    "pending_expires_at",
)


@contextmanager
def connect_read_only(database_path: Path) -> Iterator[sqlite3.Connection]:
    connection = sqlite3.connect(database_path.resolve().as_uri() + "?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys=ON")
    try:
        yield connection
    finally:
        connection.close()


def logical_tables(connection: sqlite3.Connection) -> set[str]:
    return {
        row[0]
        for row in connection.execute(
            "SELECT name FROM sqlite_master "
            "WHERE type='table' AND name NOT LIKE 'sqlite_%'"
        )
    }


def column_signature(connection: sqlite3.Connection, table: str) -> tuple[tuple[Any, ...], ...]:
    return tuple(
        (row[1], row[2].upper(), row[3], row[5])
        for row in connection.execute(f'PRAGMA table_info("{table}")')
    )


def _foreign_key_signature(connection: sqlite3.Connection, table: str) -> set[tuple[str, str, str]]:
    return {
        (row[3], row[2], row[4])
        for row in connection.execute(f'PRAGMA foreign_key_list("{table}")')
    }


def _unique_indexes(connection: sqlite3.Connection, table: str) -> set[tuple[str, ...]]:
    result: set[tuple[str, ...]] = set()
    for row in connection.execute(f'PRAGMA index_list("{table}")'):
        if not row[2]:
            continue
        columns = tuple(
            column[2]
            for column in connection.execute(f'PRAGMA index_info("{row[1]}")')
        )
        result.add(columns)
    return result


def validate_final_schema(connection: sqlite3.Connection) -> None:
    tables = logical_tables(connection)
    if tables != APPLICATION_TABLES:
        raise DatabaseValidationError(
            f"Expected exactly seven application tables {sorted(APPLICATION_TABLES)}, "
            f"found {sorted(tables)}"
        )

    for table, expected in FINAL_COLUMNS.items():
        actual = column_signature(connection, table)
        if actual != expected:
            raise DatabaseValidationError(
                f"{table} column definition mismatch. Expected {expected}, found {actual}"
            )
        foreign_keys = _foreign_key_signature(connection, table)
        if foreign_keys != EXPECTED_FOREIGN_KEYS[table]:
            raise DatabaseValidationError(
                f"{table} foreign-key mismatch. Expected "
                f"{sorted(EXPECTED_FOREIGN_KEYS[table])}, found {sorted(foreign_keys)}"
            )

    expected_unique_indexes = {
        "Companies": {("name",)},
        "Users": {("fingerprint_id",)},
        "Areas": {("name",)},
        "Permissions": {("user_id", "area_id")},
        "UserAreaStatus": {("user_id", "area_id")},
    }
    for table, required in expected_unique_indexes.items():
        actual = _unique_indexes(connection, table)
        if not required.issubset(actual):
            raise DatabaseValidationError(
                f"{table} unique constraints mismatch. Required {required}, found {actual}"
            )

    index_row = connection.execute(
        "SELECT sql FROM sqlite_master WHERE type='index' "
        "AND name='ux_AccessLogs_request_id_nonnull'"
    ).fetchone()
    if index_row is None:
        raise DatabaseValidationError(
            "Missing unique partial index ux_AccessLogs_request_id_nonnull"
        )
    index_list_row = next(
        (
            row
            for row in connection.execute('PRAGMA index_list("AccessLogs")')
            if row[1] == "ux_AccessLogs_request_id_nonnull"
        ),
        None,
    )
    index_columns = tuple(
        row[2]
        for row in connection.execute(
            'PRAGMA index_info("ux_AccessLogs_request_id_nonnull")'
        )
    )
    normalized_sql = " ".join(index_row[0].upper().replace('"', "").split())
    if (
        index_list_row is None
        or index_list_row[2] != 1
        or index_list_row[4] != 1
        or index_columns != ("request_id",)
        or "WHERE REQUEST_ID IS NOT NULL" not in normalized_sql
    ):
        raise DatabaseValidationError(
            "ux_AccessLogs_request_id_nonnull is not the approved unique partial index"
        )

    create_sql = {
        row[0]: " ".join(row[1].upper().split())
        for row in connection.execute(
            "SELECT name, sql FROM sqlite_master WHERE type='table'"
        )
        if row[1]
    }
    required_checks = {
        "AccessLogs": (
            "DIRECTION IN ('ENTRY', 'EXIT')",
            "DECISION IN ('GRANTED', 'DENIED')",
            "AUTHENTICATION_METHOD IN ('FINGERPRINT', 'RFID')",
        ),
        "SystemState": (
            "SYSTEM_STATE_ID = 1",
            "DOOR_STATE IN ('OPEN', 'CLOSED')",
        ),
    }
    for table, fragments in required_checks.items():
        for fragment in fragments:
            if fragment not in create_sql[table]:
                raise DatabaseValidationError(f"{table} is missing CHECK constraint: {fragment}")


def validate_reference_data(connection: sqlite3.Connection) -> None:
    companies = tuple(
        tuple(row)
        for row in connection.execute(
            "SELECT company_id, name FROM Companies ORDER BY company_id"
        )
    )
    if companies != COMPANIES:
        raise DatabaseValidationError(
            f"Companies mapping mismatch. Expected {COMPANIES}, found {companies}"
        )

    users = tuple(
        tuple(row)
        for row in connection.execute(
            "SELECT user_id, company_id, name, role, fingerprint_id, is_active "
            "FROM Users ORDER BY user_id"
        )
    )
    if users != USERS:
        raise DatabaseValidationError(f"Users mapping mismatch. Expected {USERS}, found {users}")

    areas = tuple(
        tuple(row)
        for row in connection.execute(
            "SELECT area_id, name, is_active FROM Areas ORDER BY area_id"
        )
    )
    if areas != AREAS:
        raise DatabaseValidationError(f"Areas mapping mismatch. Expected {AREAS}, found {areas}")

    permissions = tuple(
        tuple(row)
        for row in connection.execute(
            "SELECT user_id, area_id, allowed FROM Permissions ORDER BY user_id, area_id"
        )
    )
    if permissions != PERMISSIONS:
        raise DatabaseValidationError(
            f"Permissions mismatch. Expected {PERMISSIONS}, found {permissions}"
        )

    status_keys = tuple(
        tuple(row)
        for row in connection.execute(
            "SELECT user_id, area_id FROM UserAreaStatus ORDER BY user_id, area_id"
        )
    )
    if status_keys != STATUS_KEYS:
        raise DatabaseValidationError(
            f"UserAreaStatus key mismatch. Expected {STATUS_KEYS}, found {status_keys}"
        )


def validate_system_state(connection: sqlite3.Connection) -> None:
    state_rows = connection.execute("SELECT * FROM SystemState").fetchall()
    if len(state_rows) != 1 or state_rows[0]["system_state_id"] != 1:
        raise DatabaseValidationError("SystemState must contain exactly the singleton row with ID 1")
    state = state_rows[0]
    if state["door_state"] not in {"OPEN", "CLOSED"}:
        raise DatabaseValidationError(f"Invalid SystemState door_state: {state['door_state']}")
    if not 0 <= state["failed_attempts"] <= 3:
        raise DatabaseValidationError(
            f"SystemState failed_attempts must be between 0 and 3, found {state['failed_attempts']}"
        )
    pending_values = [state[field] for field in PENDING_FIELDS]
    if any(value is None for value in pending_values) and not all(
        value is None for value in pending_values
    ):
        raise DatabaseValidationError(
            "SystemState pending authorization is partially populated"
        )
    if all(value is not None for value in pending_values) and state["pending_direction"] not in {
        "ENTRY",
        "EXIT",
    }:
        raise DatabaseValidationError(
            f"Invalid pending_direction: {state['pending_direction']}"
        )


def validate_integrity(connection: sqlite3.Connection) -> None:
    foreign_key_violations = connection.execute("PRAGMA foreign_key_check").fetchall()
    if foreign_key_violations:
        raise DatabaseValidationError(
            f"Foreign-key violations found: {[tuple(row) for row in foreign_key_violations]}"
        )
    integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
    if integrity != "ok":
        raise DatabaseValidationError(f"SQLite integrity_check failed: {integrity}")


def validate_connection(connection: sqlite3.Connection) -> None:
    validate_final_schema(connection)
    validate_reference_data(connection)
    validate_system_state(connection)
    validate_integrity(connection)


def validate_existing_database(database_path: Path) -> None:
    with connect_read_only(database_path) as connection:
        validate_connection(connection)


def business_snapshot(connection: sqlite3.Connection) -> dict[str, tuple[tuple[Any, ...], ...]]:
    snapshot: dict[str, tuple[tuple[Any, ...], ...]] = {}
    for table, order_by in (
        ("Companies", "company_id"),
        ("Users", "user_id"),
        ("Areas", "area_id"),
        ("Permissions", "user_id, area_id"),
        ("UserAreaStatus", "user_id, area_id"),
        ("AccessLogs", "access_log_id"),
        ("SystemState", "system_state_id"),
    ):
        snapshot[table] = tuple(
            tuple(row)
            for row in connection.execute(f'SELECT * FROM "{table}" ORDER BY {order_by}')
        )
    return snapshot
