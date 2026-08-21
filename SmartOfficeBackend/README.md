# Smart Office Backend

A small synchronous FastAPI backend for the Smart Office university project. It uses SQLModel and one local SQLite database.

## Setup (Windows PowerShell)

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
```

## Run

From the `SmartOfficeBackend` folder:

```powershell
./start_backend.cmd
```

The helper requires the project `.venv`, refuses to start a second listener when port 8000 is occupied, and listens on `0.0.0.0` so a phone on the same LAN can reach the API. It does not stop or kill existing processes. Use `check_backend.cmd` to inspect port 8000.

For loopback-only development after activating `.venv`, `python -m uvicorn app.main:app --reload` is also available.

Open:

- `http://127.0.0.1:8000/`
- `http://127.0.0.1:8000/api/health`
- `http://127.0.0.1:8000/api/system-state`
- `http://127.0.0.1:8000/api/users`
- `http://127.0.0.1:8000/api/users/1`
- `http://127.0.0.1:8000/api/areas`
- `http://127.0.0.1:8000/api/access-logs?limit=3`
- `http://127.0.0.1:8000/docs` for FastAPI's generated documentation

The server creates and seeds `smart_office.db` on startup. Seeding is idempotent, so restarting the server does not duplicate data.
The seed also synchronizes the fixed development mapping in place and resets
per-area attendance only when it detects a user/area mapping migration.

The database can also be initialized without starting the server:

```powershell
python -m app.seed
```

## Seeded access mapping

| Fingerprint/User ID | User | Allowed area IDs |
| ---: | --- | --- |
| 1 | Employee A | 1, 7 |
| 2 | Employee B | 2, 7 |
| 3 | Employee C | 3, 7 |
| 4 | Employee D | 4, 7 |
| 5 | IT Admin | 5, 7 |
| 6 | Manager | 1–7 |

| Area ID | Area |
| ---: | --- |
| 1 | Company A |
| 2 | Company B |
| 3 | Company C |
| 4 | Company D |
| 5 | Server Room |
| 6 | Management / Admin |
| 7 | Main Entrance |

## Current API scope

The API currently provides the root and health checks plus read-only system-state,
user, area, and access-log endpoints for the mobile application. Access-control
and ESP32 write endpoints are intentionally reserved for later tasks.

The implemented path is `React Native App -> FastAPI -> SQLite`. The ESP32 remains a standalone controller; ESP32 Wi-Fi/HTTP, `/api/access/check`, `/api/esp32/heartbeat`, and ESP32/database synchronization are not implemented yet.
