from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from app.database import create_sqlite_engine
from app.main import create_app
from app.seed import initialize_database
from tests.asgi_client import request


class ApiAuthenticationTests(unittest.TestCase):
    TOKEN = "test-token-with-at-least-32-characters"

    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.database_path = Path(self.temp_dir.name) / "auth.db"
        self.engine = create_sqlite_engine(self.database_path)
        initialize_database(self.database_path, self.engine)
        self.app = create_app(
            target_engine=self.engine,
            database_path=self.database_path,
            initialize_on_startup=False,
            require_api_auth=True,
            api_token=self.TOKEN,
        )

    def tearDown(self) -> None:
        self.engine.dispose()
        self.temp_dir.cleanup()

    def test_health_remains_public(self) -> None:
        response = request(self.app, "GET", "/api/health")
        self.assertEqual(response.status_code, 200)

    def test_protected_endpoint_rejects_missing_and_wrong_tokens(self) -> None:
        missing = request(self.app, "GET", "/api/system-state")
        wrong = request(
            self.app,
            "GET",
            "/api/system-state",
            headers={"Authorization": "Bearer wrong-token"},
        )
        self.assertEqual(missing.status_code, 401)
        self.assertEqual(wrong.status_code, 401)
        self.assertEqual(missing.headers.get("www-authenticate"), "Bearer")

    def test_valid_bearer_token_reaches_existing_endpoint(self) -> None:
        response = request(
            self.app,
            "GET",
            "/api/system-state",
            headers={"Authorization": f"Bearer {self.TOKEN}"},
        )
        self.assertEqual(response.status_code, 200)

    def test_missing_server_token_fails_closed(self) -> None:
        app = create_app(
            target_engine=self.engine,
            database_path=self.database_path,
            initialize_on_startup=False,
            require_api_auth=True,
        )
        self.assertEqual(request(app, "GET", "/api/system-state").status_code, 503)


if __name__ == "__main__":
    unittest.main()
