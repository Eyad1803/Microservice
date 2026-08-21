export type StatusTone = 'success' | 'danger' | 'warning' | 'neutral';

export type AccessDirection = 'ENTRY' | 'EXIT';

export type DoorState = 'OPEN' | 'CLOSED';

export type AccessRequestLifecycle =
  | 'QUEUED'
  | 'IN_PROGRESS'
  | 'AUTHORIZED_WAITING_DOOR'
  | 'GRANTED'
  | 'DENIED'
  | 'FAILED'
  | 'EXPIRED';

export type AccessReasonCode =
  | 'AUTHORIZED'
  | 'UNKNOWN_FINGERPRINT'
  | 'USER_INACTIVE'
  | 'NO_PERMISSION'
  | 'ALREADY_INSIDE'
  | 'ALREADY_OUTSIDE'
  | 'LOCKDOWN_ACTIVE'
  | 'SYSTEM_INACTIVE'
  | 'AREA_NOT_FOUND'
  | 'AREA_INACTIVE'
  | 'PERSON_NOT_DETECTED'
  | 'ULTRASONIC_UNAVAILABLE'
  | 'FINGERPRINT_UNAVAILABLE'
  | 'FINGERPRINT_TIMEOUT'
  | 'FINGERPRINT_READ_ERROR'
  | 'DOOR_ALREADY_OPEN'
  | 'DOOR_OPEN_FAILED'
  | 'ESP32_OFFLINE'
  | 'REQUEST_IN_PROGRESS'
  | 'REQUEST_EXPIRED'
  | 'REQUEST_OUTCOME_CONFLICT'
  | 'SECURITY_EVENT_CONFLICT';

export type ActiveAccessRequest = {
  requestId: string;
  status: AccessRequestLifecycle;
  areaId: number;
  direction: AccessDirection;
};

export type StationState = {
  esp32Online: boolean;
  personDetected: boolean | null;
  distanceCm: number | null;
  fingerprintReady: boolean | null;
  activeAccessRequest: ActiveAccessRequest | null;
};

export type SmartOfficeSystemState = {
  systemActive: boolean;
  lockdownActive: boolean;
  failedAttempts: number;
  failedAttemptLimit: number;
  adminMode: boolean;
  doorState: DoorState;
  esp32LastSeenAt: string | null;
  lastUpdatedAt: string;
} & StationState;

export type AccessUserResult = {
  id: number;
  name: string;
};

export type AccessRequest = {
  requestId: string;
  status: AccessRequestLifecycle;
  areaId: number;
  direction: AccessDirection;
  createdAt: string;
  expiresAt: string | null;
};

export type AccessRequestResult = AccessRequest & {
  updatedAt: string;
  user: AccessUserResult | null;
  reasonCode: AccessReasonCode | null;
  message: string | null;
};

export type SmartOfficeUser = {
  id: number;
  name: string;
  company: string;
  role: string;
  fingerprintId: number;
  isActive: boolean;
};

export type UserArea = {
  id: number;
  name: string;
  allowed: boolean;
  isInside: boolean;
};

export type SmartOfficeUserDetails = SmartOfficeUser & {
  areas: UserArea[];
};

export type SmartOfficeArea = {
  id: number;
  name: string;
  occupancy: number;
  isActive: boolean;
};

export type AccessLog = {
  id: number;
  userId: number | null;
  user: string;
  areaId: number;
  area: string;
  direction: AccessDirection;
  decision: 'GRANTED' | 'DENIED';
  reason: string | null;
  authenticationMethod: 'FINGERPRINT' | 'RFID';
  eventTimestamp: string;
};

export type DashboardData = {
  systemState: SmartOfficeSystemState;
  areas: SmartOfficeArea[];
  accessLogs: AccessLog[];
  totalUsersInside: number;
};
