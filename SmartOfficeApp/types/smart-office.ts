export type StatusTone = 'success' | 'danger' | 'warning' | 'neutral';

export type SmartOfficeSystemState = {
  systemActive: boolean;
  lockdownActive: boolean;
  failedAttempts: number;
  failedAttemptLimit: number;
  adminMode: boolean;
  doorState: 'OPEN' | 'CLOSED';
  esp32Online: boolean;
  esp32LastSeenAt: string | null;
  lastUpdatedAt: string;
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
  direction: 'ENTRY' | 'EXIT';
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
