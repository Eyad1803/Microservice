import type {
  AccessDirection,
  AccessLog,
  AccessReasonCode,
  AccessRequest,
  AccessRequestLifecycle,
  AccessRequestResult,
  DashboardData,
  SmartOfficeArea,
  SmartOfficeSystemState,
  SmartOfficeUser,
  SmartOfficeUserDetails,
} from '../types/smart-office';

const configuredApiBaseUrl = process.env.EXPO_PUBLIC_API_BASE_URL;
const configuredApiToken = process.env.EXPO_PUBLIC_SMART_OFFICE_API_TOKEN;
const requestTimeoutMs = 10_000;

type SystemStateDto = {
  system_active: boolean;
  lockdown_active: boolean;
  failed_attempts: number;
  failed_attempt_limit: number;
  admin_mode: boolean;
  door_state: 'OPEN' | 'CLOSED';
  esp32_online: boolean;
  esp32_last_seen_at: string | null;
  last_updated_at: string;
  person_detected: boolean | null;
  distance_cm: number | null;
  fingerprint_ready: boolean | null;
  active_access_request: ActiveAccessRequestDto | null;
};

type ActiveAccessRequestDto = {
  request_id: string;
  status: AccessRequestLifecycle;
  area_id: number;
  direction: AccessDirection;
};

type AccessRequestDto = {
  request_id: string;
  status: AccessRequestLifecycle;
  area_id: number;
  direction: AccessDirection;
  created_at: string;
  expires_at: string | null;
};

type AccessRequestAcceptedDto = AccessRequestDto & {
  expires_at: string;
};

type AccessRequestStatusDto = AccessRequestDto & {
  updated_at: string;
  user: { user_id: number; name: string } | null;
  reason_code: AccessReasonCode | null;
  message: string | null;
};

type UserDto = {
  user_id: number;
  name: string;
  company: string;
  role: string;
  fingerprint_id: number;
  is_active: boolean;
};

type UserDetailsDto = UserDto & {
  areas: {
    area_id: number;
    area_name: string;
    allowed: boolean;
    is_inside: boolean;
  }[];
};

type AreaDto = {
  area_id: number;
  name: string;
  is_active: boolean;
  occupancy: number;
};

type AccessLogDto = {
  access_log_id: number;
  user_id: number | null;
  user_name: string;
  area_id: number;
  area_name: string;
  direction: 'ENTRY' | 'EXIT';
  decision: 'GRANTED' | 'DENIED';
  denial_reason: string | null;
  authentication_method: 'FINGERPRINT' | 'RFID';
  event_timestamp: string;
};

type AccessLogsDto = {
  items: AccessLogDto[];
};

type ErrorResponseDto = {
  detail?: unknown;
};

type StructuredErrorDetailDto = {
  reason_code?: unknown;
  message?: unknown;
};

export class ApiError extends Error {
  status?: number;
  reasonCode?: AccessReasonCode;

  constructor(message: string, status?: number, reasonCode?: AccessReasonCode) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.reasonCode = reasonCode;
  }
}

function getApiBaseUrl(): string {
  const apiBaseUrl = configuredApiBaseUrl?.trim().replace(/\/+$/, '');

  if (!apiBaseUrl) {
    throw new ApiError(
      'Smart Office API is not configured. Set EXPO_PUBLIC_API_BASE_URL and reload the app.',
    );
  }

  return apiBaseUrl;
}

function getApiToken(): string {
  const apiToken = configuredApiToken?.trim();

  if (!apiToken) {
    throw new ApiError(
      'Smart Office API authentication is not configured. Set EXPO_PUBLIC_SMART_OFFICE_API_TOKEN and reload the app.',
    );
  }

  return apiToken;
}

function parseErrorDetail(body: ErrorResponseDto): {
  message: string | null;
  reasonCode?: AccessReasonCode;
} {
  if (typeof body.detail === 'string') {
    return { message: body.detail };
  }

  if (body.detail && typeof body.detail === 'object') {
    const detail = body.detail as StructuredErrorDetailDto;
    return {
      message: typeof detail.message === 'string' ? detail.message : null,
      reasonCode:
        typeof detail.reason_code === 'string'
          ? (detail.reason_code as AccessReasonCode)
          : undefined,
    };
  }

  return { message: null };
}

async function request<T>(
  path: string,
  options: { method?: 'GET' | 'POST'; body?: object } = {},
): Promise<T> {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), requestTimeoutMs);
  let response: Response;

  try {
    response = await fetch(`${getApiBaseUrl()}${path}`, {
      method: options.method ?? 'GET',
      headers: {
        Accept: 'application/json',
        Authorization: `Bearer ${getApiToken()}`,
        ...(options.body ? { 'Content-Type': 'application/json' } : {}),
      },
      body: options.body ? JSON.stringify(options.body) : undefined,
      signal: controller.signal,
    });
  } catch (error) {
    if (error instanceof ApiError) {
      throw error;
    }

    throw new ApiError('Cannot connect to Smart Office backend.');
  } finally {
    clearTimeout(timeout);
  }

  if (!response.ok) {
    let detail: ReturnType<typeof parseErrorDetail> = { message: null };

    try {
      detail = parseErrorDetail((await response.json()) as ErrorResponseDto);
    } catch {
      // The status code still provides a clear error if the body is not JSON.
    }

    throw new ApiError(
      detail.message ?? `Smart Office backend returned HTTP ${response.status}.`,
      response.status,
      detail.reasonCode,
    );
  }

  return (await response.json()) as T;
}

function mapUser(user: UserDto): SmartOfficeUser {
  return {
    id: user.user_id,
    name: user.name,
    company: user.company,
    role: user.role,
    fingerprintId: user.fingerprint_id,
    isActive: user.is_active,
  };
}

function mapAccessLog(log: AccessLogDto): AccessLog {
  return {
    id: log.access_log_id,
    userId: log.user_id,
    user: log.user_name,
    areaId: log.area_id,
    area: log.area_name,
    direction: log.direction,
    decision: log.decision,
    reason: log.denial_reason,
    authenticationMethod: log.authentication_method,
    eventTimestamp: log.event_timestamp,
  };
}

export async function getSystemState(): Promise<SmartOfficeSystemState> {
  const state = await request<SystemStateDto>('/api/system-state');

  return {
    systemActive: state.system_active,
    lockdownActive: state.lockdown_active,
    failedAttempts: state.failed_attempts,
    failedAttemptLimit: state.failed_attempt_limit,
    adminMode: state.admin_mode,
    doorState: state.door_state,
    esp32Online: state.esp32_online,
    esp32LastSeenAt: state.esp32_last_seen_at,
    lastUpdatedAt: state.last_updated_at,
    personDetected: state.person_detected,
    distanceCm: state.distance_cm,
    fingerprintReady: state.fingerprint_ready,
    activeAccessRequest: state.active_access_request
      ? {
          requestId: state.active_access_request.request_id,
          status: state.active_access_request.status,
          areaId: state.active_access_request.area_id,
          direction: state.active_access_request.direction,
        }
      : null,
  };
}

function mapAccessRequest(requestData: AccessRequestDto): AccessRequest {
  return {
    requestId: requestData.request_id,
    status: requestData.status,
    areaId: requestData.area_id,
    direction: requestData.direction,
    createdAt: requestData.created_at,
    expiresAt: requestData.expires_at,
  };
}

export async function createAccessRequest(
  areaId: number,
  direction: AccessDirection,
): Promise<AccessRequest> {
  const response = await request<AccessRequestAcceptedDto>('/api/access/requests', {
    method: 'POST',
    body: { area_id: areaId, direction },
  });
  return mapAccessRequest(response);
}

export async function getAccessRequest(requestId: string): Promise<AccessRequestResult> {
  const response = await request<AccessRequestStatusDto>(
    `/api/access/requests/${encodeURIComponent(requestId)}`,
  );
  return {
    ...mapAccessRequest(response),
    updatedAt: response.updated_at,
    user: response.user ? { id: response.user.user_id, name: response.user.name } : null,
    reasonCode: response.reason_code,
    message: response.message,
  };
}

export async function getUsers(): Promise<SmartOfficeUser[]> {
  const users = await request<UserDto[]>('/api/users');
  return users.map(mapUser);
}

export async function getUserById(userId: number): Promise<SmartOfficeUserDetails> {
  const user = await request<UserDetailsDto>(`/api/users/${userId}`);

  return {
    ...mapUser(user),
    areas: user.areas.map((area) => ({
      id: area.area_id,
      name: area.area_name,
      allowed: area.allowed,
      isInside: area.is_inside,
    })),
  };
}

export async function getAreas(): Promise<SmartOfficeArea[]> {
  const areas = await request<AreaDto[]>('/api/areas');

  return areas.map((area) => ({
    id: area.area_id,
    name: area.name,
    occupancy: area.occupancy,
    isActive: area.is_active,
  }));
}

export async function getAccessLogs(limit?: number): Promise<AccessLog[]> {
  const query = limit === undefined ? '' : `?limit=${encodeURIComponent(String(limit))}`;
  const response = await request<AccessLogsDto>(`/api/access-logs${query}`);
  return response.items.map(mapAccessLog);
}

export async function getDashboardData(): Promise<DashboardData> {
  const [systemState, areas, accessLogs, users] = await Promise.all([
    getSystemState(),
    getAreas(),
    getAccessLogs(3),
    getUsers(),
  ]);
  const userDetails = await Promise.all(users.map((user) => getUserById(user.id)));
  const totalUsersInside = userDetails.filter((user) =>
    user.areas.some((area) => area.isInside),
  ).length;

  return { systemState, areas, accessLogs, totalUsersInside };
}
