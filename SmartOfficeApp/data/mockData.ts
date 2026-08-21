export type StatusTone = 'success' | 'danger' | 'warning' | 'neutral';

export type AreaId = 1 | 2 | 3 | 4 | 5 | 6 | 7;

export type SmartOfficeUser = {
  id: number;
  name: string;
  company: string;
  role: string;
  fingerprintId: number;
  status: 'Active';
  allowedAreaIds: AreaId[];
  areaStatus: Record<AreaId, boolean>;
};

export type SmartOfficeArea = {
  id: AreaId;
  name: string;
  occupancy: number;
  status: 'Active';
};

export type AccessResult = 'ENTRY GRANTED' | 'EXIT GRANTED' | 'ACCESS DENIED';

export type AccessLog = {
  id: string;
  user: string;
  area: string;
  result: AccessResult;
  timestamp: string;
  reason?: string;
};

function createAreaStatus(...insideAreaIds: AreaId[]): Record<AreaId, boolean> {
  return {
    1: insideAreaIds.includes(1),
    2: insideAreaIds.includes(2),
    3: insideAreaIds.includes(3),
    4: insideAreaIds.includes(4),
    5: insideAreaIds.includes(5),
    6: insideAreaIds.includes(6),
    7: insideAreaIds.includes(7),
  };
}

export const users: SmartOfficeUser[] = [
  {
    id: 1,
    name: 'Employee A',
    company: 'Company A',
    role: 'Employee',
    fingerprintId: 1,
    status: 'Active',
    allowedAreaIds: [1, 7],
    areaStatus: createAreaStatus(),
  },
  {
    id: 2,
    name: 'Employee B',
    company: 'Company B',
    role: 'Employee',
    fingerprintId: 2,
    status: 'Active',
    allowedAreaIds: [2, 7],
    areaStatus: createAreaStatus(),
  },
  {
    id: 3,
    name: 'Employee C',
    company: 'Company C',
    role: 'Employee',
    fingerprintId: 3,
    status: 'Active',
    allowedAreaIds: [3, 7],
    areaStatus: createAreaStatus(),
  },
  {
    id: 4,
    name: 'Employee D',
    company: 'Company D',
    role: 'Employee',
    fingerprintId: 4,
    status: 'Active',
    allowedAreaIds: [4, 7],
    areaStatus: createAreaStatus(),
  },
  {
    id: 5,
    name: 'IT Admin',
    company: 'IT',
    role: 'IT',
    fingerprintId: 5,
    status: 'Active',
    allowedAreaIds: [5, 7],
    areaStatus: createAreaStatus(),
  },
  {
    id: 6,
    name: 'Manager',
    company: 'Management',
    role: 'Manager',
    fingerprintId: 6,
    status: 'Active',
    allowedAreaIds: [1, 2, 3, 4, 5, 6, 7],
    areaStatus: createAreaStatus(),
  },
];

export const areas: SmartOfficeArea[] = [
  { id: 1, name: 'Company A', occupancy: 0, status: 'Active' },
  { id: 2, name: 'Company B', occupancy: 0, status: 'Active' },
  { id: 3, name: 'Company C', occupancy: 0, status: 'Active' },
  { id: 4, name: 'Company D', occupancy: 0, status: 'Active' },
  { id: 5, name: 'Server Room', occupancy: 0, status: 'Active' },
  { id: 6, name: 'Management / Admin', occupancy: 0, status: 'Active' },
  { id: 7, name: 'Main Entrance', occupancy: 0, status: 'Active' },
];

export const accessLogs: AccessLog[] = [
  {
    id: 'log-1',
    user: 'Employee B',
    area: 'Company B',
    result: 'EXIT GRANTED',
    timestamp: 'Today, 10:42 AM',
  },
  {
    id: 'log-2',
    user: 'Unknown Finger',
    area: 'Server Room',
    result: 'ACCESS DENIED',
    timestamp: 'Today, 10:31 AM',
  },
  {
    id: 'log-3',
    user: 'IT Admin',
    area: 'Server Room',
    result: 'ENTRY GRANTED',
    timestamp: 'Today, 10:18 AM',
  },
  {
    id: 'log-4',
    user: 'Employee A',
    area: 'Company B',
    result: 'ACCESS DENIED',
    reason: 'No Permission',
    timestamp: 'Today, 9:56 AM',
  },
  {
    id: 'log-5',
    user: 'Employee A',
    area: 'Company A',
    result: 'ENTRY GRANTED',
    timestamp: 'Today, 9:42 AM',
  },
];

export const systemStatus: {
  label: string;
  value: string;
  detail?: string;
  tone: StatusTone;
}[] = [
  { label: 'System', value: 'ACTIVE', tone: 'success' },
  { label: 'ESP32', value: 'OFFLINE', detail: 'Demo Mode', tone: 'neutral' },
  { label: 'Door', value: 'CLOSED', tone: 'neutral' },
  { label: 'Lockdown', value: 'OFF', tone: 'success' },
  { label: 'Failed Attempts', value: '0 / 3', tone: 'success' },
];

export const occupancySummary = [
  { label: 'Total Users Inside', count: 5 },
  { label: 'Company A', count: 1 },
  { label: 'Company B', count: 1 },
  { label: 'Company C', count: 1 },
  { label: 'Company D', count: 0 },
  { label: 'Server Room', count: 1 },
  { label: 'Management', count: 1 },
];
