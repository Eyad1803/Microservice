from __future__ import annotations

import os
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from contextlib import closing
from pathlib import Path

from app.database import DEFAULT_DATABASE_PATH, DATABASE_PATH_ENV_VAR, resolve_database_path
from app.database_validation import connect_read_only, validate_existing_database
from tools.prepare_phase3_simulation import (
    SIMULATION_DATABASE_NAME,
    prepare_simulation_database,
    validate_simulation_target,
)


class DatabaseOverrideTests(unittest.TestCase):
    def test_default_path_remains_live_database(self) -> None:
        original = os.environ.pop(DATABASE_PATH_ENV_VAR, None)
        try:
            self.assertEqual(resolve_database_path(), DEFAULT_DATABASE_PATH.resolve())
        finally:
            if original is not None:
                os.environ[DATABASE_PATH_ENV_VAR] = original

    def test_explicit_environment_override_controls_imported_database_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            target = Path(temp_dir) / SIMULATION_DATABASE_NAME
            environment = os.environ.copy()
            environment[DATABASE_PATH_ENV_VAR] = str(target)
            output = subprocess.check_output(
                [
                    sys.executable,
                    "-c",
                    "from app.database import DATABASE_PATH; print(DATABASE_PATH)",
                ],
                cwd=DEFAULT_DATABASE_PATH.parent,
                env=environment,
                text=True,
            ).strip()
            self.assertEqual(Path(output), target.resolve())


class SimulationDatabaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.database_path = Path(self.temp_dir.name) / SIMULATION_DATABASE_NAME

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_creation_has_exact_clean_canonical_baseline(self) -> None:
        result = prepare_simulation_database(self.database_path)
        self.assertEqual(result, "NEW_INITIALIZED")
        validate_existing_database(self.database_path)

        with connect_read_only(self.database_path) as connection:
            counts = {
                table: connection.execute(f'SELECT COUNT(*) FROM "{table}"').fetchone()[0]
                for table in (
                    "Companies",
                    "Users",
                    "Areas",
                    "Permissions",
                    "UserAreaStatus",
                    "SystemState",
                    "AccessLogs",
                )
            }
            self.assertEqual(
                counts,
                {
                    "Companies": 6,
                    "Users": 6,
                    "Areas": 7,
                    "Permissions": 42,
                    "UserAreaStatus": 42,
                    "SystemState": 1,
                    "AccessLogs": 0,
                },
            )
            self.assertEqual(
                connection.execute(
                    "SELECT COUNT(*) FROM UserAreaStatus WHERE is_inside=0"
                ).fetchone()[0],
                42,
            )

    def test_reset_recreates_only_clean_simulation_database(self) -> None:
        prepare_simulation_database(self.database_path)
        with closing(sqlite3.connect(self.database_path)) as connection, connection:
            connection.execute("UPDATE SystemState SET failed_attempts=2")
            connection.execute(
                "UPDATE UserAreaStatus SET is_inside=1 WHERE user_id=1 AND area_id=1"
            )

        result = prepare_simulation_database(self.database_path, reset=True)
        self.assertEqual(result, "NEW_INITIALIZED")
        with connect_read_only(self.database_path) as connection:
            self.assertEqual(
                tuple(
                    connection.execute(
                        "SELECT failed_attempts,lockdown_active,pending_request_id "
                        "FROM SystemState"
                    ).fetchone()
                ),
                (0, 0, None),
            )
            self.assertEqual(
                connection.execute(
                    "SELECT COUNT(*) FROM UserAreaStatus WHERE is_inside=1"
                ).fetchone()[0],
                0,
            )
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM AccessLogs").fetchone()[0], 0)

    def test_guard_refuses_live_backup_and_other_filenames(self) -> None:
        protected_backup = (
            DEFAULT_DATABASE_PATH.parent
            / "smart_office.pre_final_integration.20260821_073233.db"
        )
        for unsafe in (
            DEFAULT_DATABASE_PATH,
            protected_backup,
            DEFAULT_DATABASE_PATH.parent / "unrelated.db",
        ):
            with self.subTest(unsafe=unsafe), self.assertRaises(ValueError):
                validate_simulation_target(unsafe)


if __name__ == "__main__":
    unittest.main()
