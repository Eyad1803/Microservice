# Smart Office Building Access System

University hardware project with three deliberately separated parts:

- `SmartOfficeAccessStep1`: standalone ESP32/Arduino access controller.
- `SmartOfficeBackend`: FastAPI, SQLModel, and SQLite read API.
- `SmartOfficeApp`: React Native application using Expo SDK 54 and Expo Router.

## Implemented now

```text
React Native App -> FastAPI -> SQLite

ESP32 standalone hardware logic
```

The mobile application reads real data from FastAPI and SQLite. The ESP32 independently implements RFID administration, fingerprint access, permissions, per-user/per-area anti-passback state, lockdown, LCD feedback, servo control, and ultrasonic doorway checks.

## Final stage not implemented yet

```text
ESP32 -> FastAPI -> SQLite
```

The remaining final integration includes ESP32 Wi-Fi/HTTP communication, `POST /api/access/check`, `POST /api/esp32/heartbeat`, and synchronization of access decisions and state with the database. Those features must not be described as complete until that stage is implemented and tested.

See each project folder's README for setup, wiring, and runtime instructions.
