# Smart Office App

React Native application for the Smart Office Building Access System, built with Expo SDK 54, TypeScript, and Expo Router.

## Implemented screens

- Dashboard
- Users
- User details
- Areas
- Access logs

All live screen data comes from the FastAPI REST API. `data/mockData.ts` is retained only as historical development data and is not imported by the running application. The app does not fall back to mock data when the backend is unavailable.

## Current architecture

```text
React Native App
        |
        v
FastAPI REST API
        |
        v
SQLite
```

The ESP32 currently runs as a standalone hardware controller. ESP32 Wi-Fi/HTTP communication, `/api/access/check`, `/api/esp32/heartbeat`, and ESP32/database synchronization are intentionally not implemented yet.

## Local setup

Install dependencies:

```powershell
npm install
```

Copy `.env.example` to `.env` and replace the placeholder with the active LAN IPv4 address of the PC running FastAPI:

```dotenv
EXPO_PUBLIC_API_BASE_URL=http://YOUR_PC_LOCAL_IP:8000
```

The API base URL is read centrally in `services/api.ts`. Do not duplicate it in screen files. On the current development network the configured address is `http://10.0.0.12:8000`.

Start Expo:

```powershell
npx expo start
```

For a physical phone, keep the phone and PC on the same local network and start FastAPI on `0.0.0.0:8000`.

## Validation

```powershell
npx tsc --noEmit
npm run lint
npx expo-doctor
npx expo install --check
```

Expo web additionally requires the FastAPI server to allow the web development origin through CORS. Native iOS and Android requests are not subject to browser CORS enforcement.
