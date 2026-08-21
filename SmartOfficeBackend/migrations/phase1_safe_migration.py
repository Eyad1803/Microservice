"""One-time Phase 1 migration for the Smart Office SQLite database."""

from __future__ import annotations

import argparse
import sqlite3
from dataclasses import dataclass
from pathlib import Path

from app.database_validation import (
    DatabaseValidationError,
    EXPECTED_FOREIGN_KEYS,
    FINAL_COLUMNS,
    column_signature,
    logical_tables,
    validate_connection,
    validate_integrity,
    validate_reference_data,
)
from app.reference_data import APPLICATION_TABLES


class MigrationError(RuntimeError):
    """Raised when the database is unsafe to migrate."""


@dataclass(frozen=True)
class MigrationResult:
    status: str
    demo_logs_removed: int


PRE_MIGRATION_COLUMNS = dict(FINAL_COLUMNS)
PRE_MIGRATION_COLUMNS["AccessLogs"] = FINAL_COLUMNS["AccessLogs"][:8]
PRE_MIGRATION_COLUMNS["SystemState"] = FINAL_COLUMNS["SystemState"][:9]

SYSTEM_STATE_ADDITIONS = (
    'ALTER TABLE "SystemState" ADD COLUMN pending_request_id VARCHAR',
    'ALTER TABLE "SystemState" ADD COLUMN pending_user_id INTEGER REFERENCES "Users"("user_id")',
    'ALTER TABLE "SystemState" ADD COLUMN pending_area_id INTEGER REFERENCES "Areas"("area_id")',
    'ALTER TABLE "SystemState" ADD COLUMN pending_direction VARCHAR',
    'ALTER TABLE "SystemState" ADD COLUMN pending_created_at DATETIME',
    'ALTER TABLE "SystemState" ADD COLUMN pending_authorized_at DATETIME',
    'ALTER TABLE "SystemState" ADD COLUMN pending_expires_at DATETIME',
    'ALTER TABLE "SystemState" ADD COLUMN last_security_boot_id VARCHAR',
    'ALTER TABLE "SystemState" ADD COLUMN last_security_event_id VARCHAR',
    'ALTER TABLE "SystemState" ADD COLUMN last_security_event_type VARCHAR',
    'ALTER TABLE "SystemState" ADD COLUMN last_security_event_at DATETIME',
)

ACCESS_LOG_ADDITIONS = (
    'ALTER TABLE "AccessLogs" ADD COLUMN request_id VARCHAR',
    'ALTER TABLE "AccessLogs" ADD COLUMN request_created_at DATETIME',
)

REQUEST_ID_INDEX_SQL = (
    'CREATE UNIQUE INDEX "ux_AccessLogs_request_id_nonnull" '
    'ON "AccessLogs" ("request_id") WHERE "request_id" IS NOT NULL'
)

DEMO_LOGS = (
    {
        "access_log_id": 1,
        "user_id": 1,
        "area_id": 1,
        "direction": "ENTRY",
        "decision": "GRANTED",
        "denial_reason": None,
        "authentication_method": "FINGERPRINT",
        "event_timestamp": "2026-01-15 08:00:00.000000",
        "user_name": "Employee A",
        "area_name": "Company A",
    },
    {
        "access_log_id": 2,
        "user_id": 1,
        "area_id": 1,
        "direction": "EXIT",
        "decision": "GRANTED",
        "denial_reason": None,
        "authentication_method": "FINGERPRINT",
        "event_timestamp": "2026-01-15 17:00:00.000000",
        "user_name": "Employee A",
        "area_name": "Company A",
    },
    {
        "access_log_id": 3,
        "user_id": None,
        "area_id": 5,
        "direction": "ENTRY",
        "decision": "DENIED",
        "denial_reason": "UNKNOWN_FINGERPRINT",
        "authentication_method": "FINGERPRINT",
        "event_timestamp": "2026-01-15 17:05:00.000000",
        "user_name": None,
        "area_name": "Server Room",
    },
)

OLD_SYSTEM_STATE_FIELDS = tuple(column[0] for column in PRE_MIGRATION_COLUMNS["SystemState"])
OLD_ACCESS_LOG_FIELDS = tuple(column[0] for column in PRE_MIGRATION_COLUMNS["AccessLogs"])
NEW_NULL_FIELDS = tuple(column[0] for column in FINAL_COLUMNS["SystemState"][9:])


def _connect(database_path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(database_path)
    connection.row_factory = sqlite3.Row
    connection.isolation_level = None
    connection.execute("PRAGMA foreign_keys=ON")
    connection.execute("PRAGMA busy_timeout=5000")
    return connection


def _foreign_keys(connection: sqlite3.Connection, table: str) -> set[tuple[str, str, str]]:
    return {
        (row[3], row[2], row[4])
        for row in connection.execute(f'PRAGMA foreign_key_list("{table}")')
    }


def _validate_pre_migration_schema(connection: sqlite3.Connection) -> None:
    if logical_tables(connection) != APPLICATION_TABLES:
        raise MigrationError("Pre-migration database does not contain exactly seven tables")
    for table, expected_columns in PRE_MIGRATION_COLUMNS.items():
        actual_columns = column_signature(connection, table)
        if actual_columns != expected_columns:
            raise MigrationError(
                f"Pre-migration {table} columns mismatch. "
                f"Expected {expected_columns}, found {actual_columns}"
            )
        expected_foreign_keys = (
            set() if table == "SystemState" else EXPECTED_FOREIGN_KEYS[table]
        )
        actual_foreign_keys = _foreign_keys(connection, table)
        if actual_foreign_keys != expected_foreign_keys:
            raise MigrationError(
                f"Pre-migration {table} foreign keys mismatch. "
                f"Expected {expected_foreign_keys}, found {actual_foreign_keys}"
            )

    if connection.execute(
        "SELECT 1 FROM sqlite_master WHERE type='index' "
        "AND name='ux_AccessLogs_request_id_nonnull'"
    ).fetchone():
        raise MigrationError("Pre-migration schema unexpectedly contains the final request index")

    create_sql = {
        row[0]: " ".join(row[1].upper().split())
        for row in connection.execute(
            "SELECT name, sql FROM sqlite_master WHERE type='table'"
        )
        if row[1]
    }
    for table, fragments in {
        "AccessLogs": (
            "DIRECTION IN ('ENTRY', 'EXIT')",
            "DECISION IN ('GRANTED', 'DENIED')",
            "AUTHENTICATION_METHOD IN ('FINGERPRINT', 'RFID')",
        ),
        "SystemState": (
            "SYSTEM_STATE_ID = 1",
            "DOOR_STATE IN ('OPEN', 'CLOSED')",
        ),
    }.items():
        for fragment in fragments:
            if fragment not in create_sql[table]:
                raise MigrationError(f"Pre-migration {table} is missing CHECK: {fragment}")


def classify_schema(connection: sqlite3.Connection) -> str:
    if logical_tables(connection) != APPLICATION_TABLES:
        raise MigrationError(
            "PARTIAL OR UNKNOWN SCHEMA: expected exactly the seven Smart Office tables"
        )
    signatures = {
        table: column_signature(connection, table) for table in APPLICATION_TABLES
    }
    if all(signatures[table] == PRE_MIGRATION_COLUMNS[table] for table in APPLICATION_TABLES):
        _validate_pre_migration_schema(connection)
        return "PRE_MIGRATION"
    if all(signatures[table] == FINAL_COLUMNS[table] for table in APPLICATION_TABLES):
        try:
            validate_connection(connection)
        except DatabaseValidationError as exc:
            raise MigrationError(f"PARTIAL OR INVALID FINAL SCHEMA: {exc}") from exc
        return "FINAL"
    differences = {
        table: [column[0] for column in signatures[table]]
        for table in APPLICATION_TABLES
        if signatures[table]
        not in {PRE_MIGRATION_COLUMNS[table], FINAL_COLUMNS[table]}
    }
    raise MigrationError(f"PARTIAL OR MIXED SCHEMA: {differences}")


def _rows(connection: sqlite3.Connection, sql: str) -> tuple[tuple[object, ...], ...]:
    return tuple(tuple(row) for row in connection.execute(sql))


def _validate_pre_migration_data(connection: sqlite3.Connection) -> None:
    try:
        validate_reference_data(connection)
        validate_integrity(connection)
    except DatabaseValidationError as exc:
        raise MigrationError(str(exc)) from exc

    state = connection.execute("SELECT * FROM SystemState").fetchall()
    if len(state) != 1 or state[0]["system_state_id"] != 1:
        raise MigrationError("SystemState must contain exactly singleton ID 1")
    if state[0]["door_state"] not in {"OPEN", "CLOSED"}:
        raise MigrationError(f"Invalid door_state: {state[0]['door_state']}")
    if not 0 <= state[0]["failed_attempts"] <= 3:
        raise MigrationError(
            f"failed_attempts must be between 0 and 3, found {state[0]['failed_attempts']}"
        )


def _demo_predicate(demo: dict[str, object]) -> tuple[str, list[object]]:
    user_condition = "user_id IS NULL" if demo["user_id"] is None else "user_id = ?"
    denial_condition = (
        "denial_reason IS NULL"
        if demo["denial_reason"] is None
        else "denial_reason = ?"
    )
    parameters: list[object] = [demo["access_log_id"]]
    if demo["user_id"] is not None:
        parameters.append(demo["user_id"])
    parameters.extend(
        [
            demo["area_id"],
            demo["direction"],
            demo["decision"],
        ]
    )
    if demo["denial_reason"] is not None:
        parameters.append(demo["denial_reason"])
    parameters.extend(
        [
            demo["authentication_method"],
            demo["event_timestamp"],
            demo["area_id"],
            demo["area_name"],
        ]
    )
    user_exists = ""
    if demo["user_id"] is not None:
        user_exists = (
            " AND EXISTS (SELECT 1 FROM Users "
            "WHERE user_id = ? AND name = ?)"
        )
        parameters.extend([demo["user_id"], demo["user_name"]])
    predicate = (
        "access_log_id = ? AND "
        f"{user_condition} AND area_id = ? AND direction = ? AND decision = ? AND "
        f"{denial_condition} AND authentication_method = ? AND event_timestamp = ? AND "
        "request_id IS NULL AND request_created_at IS NULL AND "
        "EXISTS (SELECT 1 FROM Areas WHERE area_id = ? AND name = ?)"
        f"{user_exists}"
    )
    return predicate, parameters


def _delete_demo_logs(connection: sqlite3.Connection) -> int:
    removed = 0
    for demo in DEMO_LOGS:
        predicate, parameters = _demo_predicate(demo)
        matches = connection.execute(
            f'SELECT COUNT(*) FROM "AccessLogs" WHERE {predicate}', parameters
        ).fetchone()[0]
        if matches != 1:
            raise MigrationError(
                f"Demo log {demo['access_log_id']} complete signature matched {matches} rows; expected 1"
            )
        cursor = connection.execute(
            f'DELETE FROM "AccessLogs" WHERE {predicate}', parameters
        )
        if cursor.rowcount != 1:
            raise MigrationError(
                f"Demo log {demo['access_log_id']} deleted {cursor.rowcount} rows; expected 1"
            )
        removed += cursor.rowcount
    return removed


def migrate_database(database_path: Path) -> MigrationResult:
    database_path = database_path.resolve()
    if not database_path.is_file():
        raise MigrationError(f"Database does not exist: {database_path}")

    connection = _connect(database_path)
    try:
        state = classify_schema(connection)
        if state == "FINAL":
            return MigrationResult(status="ALREADY_MIGRATED_VALID", demo_logs_removed=0)

        _validate_pre_migration_data(connection)
        reference_before = {
            table: _rows(connection, f'SELECT * FROM "{table}" ORDER BY {order_by}')
            for table, order_by in (
                ("Companies", "company_id"),
                ("Users", "user_id"),
                ("Areas", "area_id"),
                ("Permissions", "user_id, area_id"),
            )
        }
        status_before = _rows(
            connection,
            'SELECT * FROM "UserAreaStatus" ORDER BY user_id, area_id',
        )
        system_before = _rows(
            connection,
            "SELECT " + ", ".join(OLD_SYSTEM_STATE_FIELDS) + " FROM SystemState",
        )
        non_demo_logs_before = _rows(
            connection,
            "SELECT "
            + ", ".join(OLD_ACCESS_LOG_FIELDS)
            + " FROM AccessLogs WHERE access_log_id NOT IN (1, 2, 3) ORDER BY access_log_id",
        )

        connection.execute("BEGIN IMMEDIATE")
        try:
            for statement in SYSTEM_STATE_ADDITIONS + ACCESS_LOG_ADDITIONS:
                connection.execute(statement)
            connection.execute(REQUEST_ID_INDEX_SQL)

            for field in NEW_NULL_FIELDS:
                non_null = connection.execute(
                    f'SELECT COUNT(*) FROM SystemState WHERE "{field}" IS NOT NULL'
                ).fetchone()[0]
                if non_null:
                    raise MigrationError(f"New SystemState field {field} was not initialized NULL")
            for field in ("request_id", "request_created_at"):
                non_null = connection.execute(
                    f'SELECT COUNT(*) FROM AccessLogs WHERE "{field}" IS NOT NULL'
                ).fetchone()[0]
                if non_null:
                    raise MigrationError(f"New AccessLogs field {field} was not initialized NULL")

            removed = _delete_demo_logs(connection)

            for table, before in reference_before.items():
                order_by = "user_id, area_id" if table == "Permissions" else {
                    "Companies": "company_id",
                    "Users": "user_id",
                    "Areas": "area_id",
                }[table]
                after = _rows(connection, f'SELECT * FROM "{table}" ORDER BY {order_by}')
                if after != before:
                    raise MigrationError(f"Migration changed {table} business data")
            if _rows(
                connection,
                'SELECT * FROM "UserAreaStatus" ORDER BY user_id, area_id',
            ) != status_before:
                raise MigrationError("Migration changed UserAreaStatus")
            if _rows(
                connection,
                "SELECT " + ", ".join(OLD_SYSTEM_STATE_FIELDS) + " FROM SystemState",
            ) != system_before:
                raise MigrationError("Migration changed existing SystemState fields")
            if _rows(
                connection,
                "SELECT "
                + ", ".join(OLD_ACCESS_LOG_FIELDS)
                + " FROM AccessLogs ORDER BY access_log_id",
            ) != non_demo_logs_before:
                raise MigrationError("Migration changed or removed a non-demo AccessLog")

            validate_connection(connection)
            connection.commit()
        except Exception:
            connection.rollback()
            raise

        validate_connection(connection)
        return MigrationResult(status="MIGRATED", demo_logs_removed=removed)
    except (sqlite3.Error, DatabaseValidationError) as exc:
        raise MigrationError(str(exc)) from exc
    finally:
        connection.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="SQLite database to migrate")
    args = parser.parse_args()
    try:
        result = migrate_database(args.database)
    except MigrationError as exc:
        parser.exit(1, f"MIGRATION BLOCKED: {exc}\n")
    print(f"{result.status}; demo_logs_removed={result.demo_logs_removed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
