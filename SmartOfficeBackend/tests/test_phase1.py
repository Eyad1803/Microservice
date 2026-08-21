from __future__ import annotations

import sqlite3
import tempfile
import unittest
from contextlib import closing
from datetime import datetime, timezone
from pathlib import Path

from sqlmodel import Session

from app.database import create_sqlite_engine
from app.database_validation import (
    DatabaseValidationError,
    business_snapshot,
    connect_read_only,
    validate_existing_database,
)
from app.models import AccessLog
from app.reference_data import AREAS, COMPANIES, PERMISSIONS, STATUS_KEYS, USERS
from app.seed import initialize_database
from migrations.phase1_safe_migration import MigrationError, migrate_database


OLD_SCHEMA_SQL = '''
CREATE TABLE "Companies" (
    company_id INTEGER NOT NULL,
    name VARCHAR NOT NULL,
    PRIMARY KEY (company_id),
    UNIQUE (name)
);
CREATE TABLE "Users" (
    user_id INTEGER NOT NULL,
    company_id INTEGER NOT NULL,
    name VARCHAR NOT NULL,
    role VARCHAR NOT NULL,
    fingerprint_id INTEGER NOT NULL,
    is_active BOOLEAN NOT NULL,
    PRIMARY KEY (user_id),
    FOREIGN KEY(company_id) REFERENCES "Companies" (company_id),
    UNIQUE (fingerprint_id)
);
CREATE TABLE "Areas" (
    area_id INTEGER NOT NULL,
    name VARCHAR NOT NULL,
    is_active BOOLEAN NOT NULL,
    PRIMARY KEY (area_id),
    UNIQUE (name)
);
CREATE TABLE "Permissions" (
    user_id INTEGER NOT NULL,
    area_id INTEGER NOT NULL,
    allowed BOOLEAN NOT NULL,
    PRIMARY KEY (user_id, area_id),
    FOREIGN KEY(user_id) REFERENCES "Users" (user_id),
    FOREIGN KEY(area_id) REFERENCES "Areas" (area_id)
);
CREATE TABLE "UserAreaStatus" (
    user_id INTEGER NOT NULL,
    area_id INTEGER NOT NULL,
    is_inside BOOLEAN NOT NULL,
    updated_at DATETIME NOT NULL,
    PRIMARY KEY (user_id, area_id),
    FOREIGN KEY(user_id) REFERENCES "Users" (user_id),
    FOREIGN KEY(area_id) REFERENCES "Areas" (area_id)
);
CREATE TABLE "AccessLogs" (
    access_log_id INTEGER NOT NULL,
    user_id INTEGER,
    area_id INTEGER NOT NULL,
    direction VARCHAR NOT NULL,
    decision VARCHAR NOT NULL,
    denial_reason VARCHAR,
    authentication_method VARCHAR NOT NULL,
    event_timestamp DATETIME NOT NULL,
    PRIMARY KEY (access_log_id),
    CHECK (direction IN ('ENTRY', 'EXIT')),
    CHECK (decision IN ('GRANTED', 'DENIED')),
    CHECK (authentication_method IN ('FINGERPRINT', 'RFID')),
    FOREIGN KEY(user_id) REFERENCES "Users" (user_id),
    FOREIGN KEY(area_id) REFERENCES "Areas" (area_id)
);
CREATE TABLE "SystemState" (
    system_state_id INTEGER NOT NULL,
    system_active BOOLEAN NOT NULL,
    lockdown_active BOOLEAN NOT NULL,
    failed_attempts INTEGER NOT NULL,
    admin_mode BOOLEAN NOT NULL,
    door_state VARCHAR NOT NULL,
    esp32_online BOOLEAN NOT NULL,
    esp32_last_seen_at DATETIME,
    last_updated_at DATETIME NOT NULL,
    PRIMARY KEY (system_state_id),
    CHECK (system_state_id = 1),
    CHECK (door_state IN ('OPEN', 'CLOSED'))
);
'''

STATUS_TIMESTAMP = "2026-08-21 02:48:58.296869"
SYSTEM_TIMESTAMP = "2026-08-18 02:15:41.159332"


def create_old_database(path: Path) -> None:
    connection = sqlite3.connect(path)
    try:
        connection.execute("PRAGMA foreign_keys=ON")
        connection.executescript(OLD_SCHEMA_SQL)
        connection.executemany("INSERT INTO Companies VALUES (?, ?)", COMPANIES)
        connection.executemany("INSERT INTO Users VALUES (?, ?, ?, ?, ?, ?)", USERS)
        connection.executemany("INSERT INTO Areas VALUES (?, ?, ?)", AREAS)
        connection.executemany("INSERT INTO Permissions VALUES (?, ?, ?)", PERMISSIONS)
        connection.executemany(
            "INSERT INTO UserAreaStatus VALUES (?, ?, ?, ?)",
            [(user_id, area_id, False, STATUS_TIMESTAMP) for user_id, area_id in STATUS_KEYS],
        )
        connection.execute(
            "INSERT INTO SystemState VALUES (1, 1, 0, 0, 0, 'CLOSED', 0, NULL, ?)",
            (SYSTEM_TIMESTAMP,),
        )
        connection.executemany(
            "INSERT INTO AccessLogs VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            [
                (1, 1, 1, "ENTRY", "GRANTED", None, "FINGERPRINT", "2026-01-15 08:00:00.000000"),
                (2, 1, 1, "EXIT", "GRANTED", None, "FINGERPRINT", "2026-01-15 17:00:00.000000"),
                (3, None, 5, "ENTRY", "DENIED", "UNKNOWN_FINGERPRINT", "FINGERPRINT", "2026-01-15 17:05:00.000000"),
            ],
        )
        connection.commit()
    finally:
        connection.close()


def snapshot(path: Path) -> dict[str, tuple[tuple[object, ...], ...]]:
    with connect_read_only(path) as connection:
        return business_snapshot(connection)


class TemporaryDatabaseTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.database_path = Path(self.temp_dir.name) / "test.db"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def create_new_database(self) -> None:
        target_engine = create_sqlite_engine(self.database_path)
        try:
            self.assertEqual(
                initialize_database(self.database_path, target_engine),
                "NEW_INITIALIZED",
            )
        finally:
            target_engine.dispose()


class MigrationTests(TemporaryDatabaseTestCase):
    def setUp(self) -> None:
        super().setUp()
        create_old_database(self.database_path)

    def test_exact_old_schema_migrates_and_removes_demo_logs(self) -> None:
        result = migrate_database(self.database_path)
        self.assertEqual(result.status, "MIGRATED")
        self.assertEqual(result.demo_logs_removed, 3)
        validate_existing_database(self.database_path)
        with connect_read_only(self.database_path) as connection:
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM AccessLogs").fetchone()[0], 0)

    def test_final_schema_reports_already_migrated_without_changes(self) -> None:
        migrate_database(self.database_path)
        before = snapshot(self.database_path)
        result = migrate_database(self.database_path)
        after = snapshot(self.database_path)
        self.assertEqual(result.status, "ALREADY_MIGRATED_VALID")
        self.assertEqual(result.demo_logs_removed, 0)
        self.assertEqual(before, after)

    def test_partial_schema_fails(self) -> None:
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            connection.execute("ALTER TABLE SystemState ADD COLUMN pending_request_id VARCHAR")
        with self.assertRaisesRegex(MigrationError, "PARTIAL OR MIXED SCHEMA"):
            migrate_database(self.database_path)

    def test_canonical_mapping_mismatch_fails_before_schema_change(self) -> None:
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            connection.execute("UPDATE Areas SET name='Wrong' WHERE area_id=1")
        with self.assertRaisesRegex(MigrationError, "Areas mapping mismatch"):
            migrate_database(self.database_path)
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            columns = [row[1] for row in connection.execute("PRAGMA table_info(SystemState)")]
        self.assertNotIn("pending_request_id", columns)

    def test_altered_demo_signature_rolls_back_all_schema_changes(self) -> None:
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            connection.execute("UPDATE AccessLogs SET direction='EXIT' WHERE access_log_id=1")
        with self.assertRaisesRegex(MigrationError, "complete signature matched 0 rows"):
            migrate_database(self.database_path)
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            system_columns = [row[1] for row in connection.execute("PRAGMA table_info(SystemState)")]
            access_columns = [row[1] for row in connection.execute("PRAGMA table_info(AccessLogs)")]
            count = connection.execute("SELECT COUNT(*) FROM AccessLogs").fetchone()[0]
        self.assertNotIn("pending_request_id", system_columns)
        self.assertNotIn("request_id", access_columns)
        self.assertEqual(count, 3)

    def test_status_and_system_business_state_are_preserved(self) -> None:
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            connection.execute(
                "UPDATE UserAreaStatus SET is_inside=1, updated_at='2026-08-21 05:00:00' "
                "WHERE user_id=1 AND area_id=1"
            )
            connection.execute(
                "UPDATE SystemState SET failed_attempts=2, last_updated_at='2026-08-21 05:01:00'"
            )
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            status_before = tuple(connection.execute(
                "SELECT * FROM UserAreaStatus ORDER BY user_id, area_id"
            ))
            system_before = tuple(connection.execute("SELECT * FROM SystemState"))[0]
        migrate_database(self.database_path)
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            status_after = tuple(connection.execute(
                "SELECT user_id, area_id, is_inside, updated_at FROM UserAreaStatus "
                "ORDER BY user_id, area_id"
            ))
            system_after = tuple(connection.execute(
                "SELECT system_state_id, system_active, lockdown_active, failed_attempts, "
                "admin_mode, door_state, esp32_online, esp32_last_seen_at, last_updated_at "
                "FROM SystemState"
            ))[0]
            new_values = tuple(connection.execute(
                "SELECT pending_request_id, pending_user_id, pending_area_id, pending_direction, "
                "pending_created_at, pending_authorized_at, pending_expires_at, "
                "last_security_boot_id, last_security_event_id, last_security_event_type, "
                "last_security_event_at FROM SystemState"
            ))[0]
        self.assertEqual(status_before, status_after)
        self.assertEqual(system_before, system_after)
        self.assertEqual(new_values, (None,) * 11)

    def test_partial_unique_index_allows_null_and_rejects_duplicate_value(self) -> None:
        migrate_database(self.database_path)
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            base = (None, 1, "ENTRY", "DENIED", "TEST", "FINGERPRINT", "2026-08-21 06:00:00")
            connection.execute(
                "INSERT INTO AccessLogs "
                "(user_id, area_id, direction, decision, denial_reason, authentication_method, event_timestamp) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                base,
            )
            connection.execute(
                "INSERT INTO AccessLogs "
                "(user_id, area_id, direction, decision, denial_reason, authentication_method, event_timestamp) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                base,
            )
            values = (*base, "req-1")
            connection.execute(
                "INSERT INTO AccessLogs "
                "(user_id, area_id, direction, decision, denial_reason, authentication_method, "
                "event_timestamp, request_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                values,
            )
            with self.assertRaises(sqlite3.IntegrityError):
                connection.execute(
                    "INSERT INTO AccessLogs "
                    "(user_id, area_id, direction, decision, denial_reason, authentication_method, "
                    "event_timestamp, request_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                    values,
                )

    def test_foreign_keys_and_integrity_remain_valid(self) -> None:
        migrate_database(self.database_path)
        with connect_read_only(self.database_path) as connection:
            self.assertEqual(connection.execute("PRAGMA foreign_key_check").fetchall(), [])
            self.assertEqual(connection.execute("PRAGMA integrity_check").fetchone()[0], "ok")


class NewDatabaseTests(TemporaryDatabaseTestCase):
    def test_new_database_has_canonical_empty_live_state(self) -> None:
        self.create_new_database()
        validate_existing_database(self.database_path)
        with connect_read_only(self.database_path) as connection:
            tables = {
                row[0]
                for row in connection.execute(
                    "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"
                )
            }
            counts = {
                table: connection.execute(f'SELECT COUNT(*) FROM "{table}"').fetchone()[0]
                for table in tables
            }
            outside = connection.execute(
                "SELECT COUNT(*) FROM UserAreaStatus WHERE is_inside=0"
            ).fetchone()[0]
            index = connection.execute(
                "SELECT sql FROM sqlite_master WHERE type='index' "
                "AND name='ux_AccessLogs_request_id_nonnull'"
            ).fetchone()
        self.assertEqual(len(tables), 7)
        self.assertEqual(counts["Companies"], 6)
        self.assertEqual(counts["Users"], 6)
        self.assertEqual(counts["Areas"], 7)
        self.assertEqual(counts["Permissions"], 42)
        self.assertEqual(counts["UserAreaStatus"], 42)
        self.assertEqual(counts["SystemState"], 1)
        self.assertEqual(counts["AccessLogs"], 0)
        self.assertEqual(outside, 42)
        self.assertIsNotNone(index)

    def test_repeated_existing_startup_is_business_read_only(self) -> None:
        self.create_new_database()
        before = snapshot(self.database_path)
        self.assertEqual(initialize_database(self.database_path), "EXISTING_VALID")
        middle = snapshot(self.database_path)
        self.assertEqual(initialize_database(self.database_path), "EXISTING_VALID")
        after = snapshot(self.database_path)
        self.assertEqual(before, middle)
        self.assertEqual(middle, after)

    def _expect_validation_failure(self, sql: str, message: str) -> None:
        self.create_new_database()
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            connection.execute("PRAGMA foreign_keys=OFF")
            connection.execute(sql)
        with self.assertRaisesRegex(DatabaseValidationError, message):
            initialize_database(self.database_path)

    def test_missing_user_fails(self) -> None:
        self._expect_validation_failure("DELETE FROM Users WHERE user_id=1", "Users mapping mismatch")

    def test_missing_area_fails(self) -> None:
        self._expect_validation_failure("DELETE FROM Areas WHERE area_id=1", "Areas mapping mismatch")

    def test_missing_permission_fails(self) -> None:
        self._expect_validation_failure(
            "DELETE FROM Permissions WHERE user_id=1 AND area_id=1",
            "Permissions mismatch",
        )

    def test_changed_fingerprint_mapping_fails(self) -> None:
        self._expect_validation_failure(
            "UPDATE Users SET fingerprint_id=99 WHERE user_id=1",
            "Users mapping mismatch",
        )

    def test_changed_permission_fails(self) -> None:
        self._expect_validation_failure(
            "UPDATE Permissions SET allowed=0 WHERE user_id=1 AND area_id=1",
            "Permissions mismatch",
        )

    def test_partially_populated_pending_state_fails(self) -> None:
        self._expect_validation_failure(
            "UPDATE SystemState SET pending_request_id='req-1'",
            "partially populated",
        )

    def test_invalid_pending_direction_fails(self) -> None:
        self._expect_validation_failure(
            "UPDATE SystemState SET pending_request_id='req-1', pending_user_id=1, "
            "pending_area_id=1, pending_direction='SIDEWAYS', "
            "pending_created_at='2026-08-21 07:00:00', "
            "pending_authorized_at='2026-08-21 07:00:01', "
            "pending_expires_at='2026-08-21 07:00:30'",
            "Invalid pending_direction",
        )

    def test_access_log_model_uses_sqlite_generated_integer_id(self) -> None:
        self.create_new_database()
        target_engine = create_sqlite_engine(self.database_path)
        try:
            with Session(target_engine) as session:
                access_log = AccessLog(
                    user_id=1,
                    area_id=1,
                    direction="ENTRY",
                    decision="GRANTED",
                    authentication_method="FINGERPRINT",
                    event_timestamp=datetime.now(timezone.utc),
                )
                session.add(access_log)
                session.commit()
                session.refresh(access_log)
                self.assertIsInstance(access_log.access_log_id, int)
        finally:
            target_engine.dispose()


if __name__ == "__main__":
    unittest.main()
