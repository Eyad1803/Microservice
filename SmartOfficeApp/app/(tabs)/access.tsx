import { useFocusEffect } from 'expo-router';
import { useCallback, useRef, useState } from 'react';
import { ActivityIndicator, Pressable, StyleSheet, Text, View } from 'react-native';

import { ErrorState, LoadingState } from '@/components/data-state';
import { PageContainer } from '@/components/page-container';
import { StatusBadge } from '@/components/status-badge';
import { Colors } from '@/constants/theme';
import { useColorScheme } from '@/hooks/use-color-scheme';
import {
  ApiError,
  createAccessRequest,
  getAccessRequest,
  getAreas,
  getSystemState,
} from '@/services/api';
import type {
  AccessDirection,
  AccessReasonCode,
  AccessRequest,
  AccessRequestLifecycle,
  AccessRequestResult,
  SmartOfficeArea,
  SmartOfficeSystemState,
  StatusTone,
} from '@/types/smart-office';

const terminalStatuses: AccessRequestLifecycle[] = ['GRANTED', 'DENIED', 'FAILED', 'EXPIRED'];

const reasonMessages: Partial<Record<AccessReasonCode, string>> = {
  NO_PERMISSION: 'No permission for this area.',
  ALREADY_INSIDE: 'User is already inside this area.',
  ALREADY_OUTSIDE: 'User is already outside this area.',
  UNKNOWN_FINGERPRINT: 'Fingerprint was not recognized.',
  USER_INACTIVE: 'The identified user is inactive.',
  AREA_INACTIVE: 'The selected area is inactive.',
  FINGERPRINT_TIMEOUT: 'Fingerprint scan timed out.',
  FINGERPRINT_READ_ERROR: 'Fingerprint could not be read.',
  FINGERPRINT_UNAVAILABLE: 'Fingerprint sensor is unavailable.',
  LOCKDOWN_ACTIVE: 'System is in Lockdown.',
  SYSTEM_INACTIVE: 'The access system is inactive.',
  PERSON_NOT_DETECTED: 'Person presence is required for entry.',
  ULTRASONIC_UNAVAILABLE: 'Person-presence sensor is unavailable.',
  DOOR_ALREADY_OPEN: 'The door is already open.',
  DOOR_OPEN_FAILED: 'Door could not be opened.',
  ESP32_OFFLINE: 'The ESP32 station is offline.',
  REQUEST_IN_PROGRESS: 'Another access request is in progress.',
  REQUEST_EXPIRED: 'The access attempt was not completed.',
};

function presentReason(reasonCode: AccessReasonCode | null | undefined, fallback?: string | null) {
  if (reasonCode && reasonMessages[reasonCode]) {
    return reasonMessages[reasonCode];
  }
  return fallback ?? 'The access request could not be completed.';
}

function resultPresentation(status: AccessRequestLifecycle | null): {
  title: string;
  description: string;
  tone: StatusTone;
} {
  switch (status) {
    case 'QUEUED':
      return {
        title: 'WAITING FOR ESP32',
        description: 'Request sent. Do not place your finger until the station acknowledges it.',
        tone: 'warning',
      };
    case 'IN_PROGRESS':
      return {
        title: 'PLACE FINGER',
        description: 'ESP32 acknowledged the request. Place your finger on the sensor now.',
        tone: 'warning',
      };
    case 'AUTHORIZED_WAITING_DOOR':
      return {
        title: 'ACCESS AUTHORIZED',
        description: 'Waiting for door result.',
        tone: 'success',
      };
    case 'GRANTED':
      return { title: 'ACCESS GRANTED', description: 'The access event was completed.', tone: 'success' };
    case 'DENIED':
      return { title: 'ACCESS DENIED', description: 'The access request was denied.', tone: 'danger' };
    case 'FAILED':
      return { title: 'ACCESS FAILED', description: 'Door could not be opened.', tone: 'danger' };
    case 'EXPIRED':
      return {
        title: 'REQUEST EXPIRED',
        description: 'The access attempt was not completed.',
        tone: 'neutral',
      };
    default:
      return {
        title: 'No access attempt yet',
        description: 'Select an area and start a fingerprint scan.',
        tone: 'neutral',
      };
  }
}

export default function AccessScreen() {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];
  const [areas, setAreas] = useState<SmartOfficeArea[] | null>(null);
  const [areasError, setAreasError] = useState<Error | null>(null);
  const [systemState, setSystemState] = useState<SmartOfficeSystemState | null>(null);
  const [backendError, setBackendError] = useState<Error | null>(null);
  const [selectedMode, setSelectedMode] = useState<AccessDirection>('ENTRY');
  const [selectedAreaId, setSelectedAreaId] = useState<number | null>(null);
  const [ownedRequest, setOwnedRequest] = useState<AccessRequest | null>(null);
  const [requestResult, setRequestResult] = useState<AccessRequestResult | null>(null);
  const [requestError, setRequestError] = useState<ApiError | null>(null);
  const [isCreatingRequest, setIsCreatingRequest] = useState(false);
  const stationRefreshRef = useRef<() => void>(() => undefined);
  const areasRefreshRef = useRef<() => void>(() => undefined);

  useFocusEffect(
    useCallback(() => {
      let active = true;
      let inFlight = false;
      let rerunImmediately = false;
      let timer: ReturnType<typeof setTimeout> | null = null;

      const poll = async () => {
        if (!active || inFlight) {
          rerunImmediately = true;
          return;
        }
        inFlight = true;
        try {
          const nextState = await getSystemState();
          if (active) {
            setSystemState(nextState);
            setBackendError(null);
          }
        } catch (error) {
          if (active) {
            setBackendError(
              error instanceof Error ? error : new Error('Cannot connect to Smart Office backend.'),
            );
          }
        } finally {
          inFlight = false;
          if (active) {
            const delay = rerunImmediately ? 0 : 1_000;
            rerunImmediately = false;
            timer = setTimeout(() => void poll(), delay);
          }
        }
      };

      stationRefreshRef.current = () => {
        rerunImmediately = true;
        if (!inFlight) {
          if (timer) clearTimeout(timer);
          timer = null;
          void poll();
        }
      };
      void poll();
      return () => {
        active = false;
        stationRefreshRef.current = () => undefined;
        if (timer) clearTimeout(timer);
      };
    }, []),
  );

  useFocusEffect(
    useCallback(() => {
      let active = true;
      let inFlight = false;

      const loadAreas = async () => {
        if (inFlight) return;
        inFlight = true;
        try {
          const nextAreas = await getAreas();
          if (active) {
            setAreas(nextAreas);
            setAreasError(null);
          }
        } catch (error) {
          if (active) {
            setAreasError(error instanceof Error ? error : new Error('Unable to load areas.'));
          }
        } finally {
          inFlight = false;
        }
      };

      areasRefreshRef.current = () => void loadAreas();
      void loadAreas();
      return () => {
        active = false;
        areasRefreshRef.current = () => undefined;
      };
    }, []),
  );

  useFocusEffect(
    useCallback(() => {
      if (!ownedRequest) return undefined;
      let active = true;
      let timer: ReturnType<typeof setTimeout> | null = null;

      const pollRequest = async () => {
        let terminal = false;
        try {
          const nextResult = await getAccessRequest(ownedRequest.requestId);
          if (!active) return;
          setRequestResult(nextResult);
          setRequestError(null);
          terminal = terminalStatuses.includes(nextResult.status);
          if (terminal) {
            setOwnedRequest(null);
            stationRefreshRef.current();
          }
        } catch (error) {
          if (!active) return;
          const apiError =
            error instanceof ApiError
              ? error
              : new ApiError('Unable to read the access request status.');
          if (apiError.reasonCode === 'REQUEST_EXPIRED') {
            terminal = true;
            setRequestResult({
              ...ownedRequest,
              status: 'EXPIRED',
              updatedAt: ownedRequest.createdAt,
              user: null,
              reasonCode: 'REQUEST_EXPIRED',
              message: apiError.message,
            });
            setRequestError(null);
            setOwnedRequest(null);
            stationRefreshRef.current();
          } else {
            setRequestError(apiError);
          }
        } finally {
          if (active && !terminal) {
            timer = setTimeout(() => void pollRequest(), 1_000);
          }
        }
      };

      void pollRequest();
      return () => {
        active = false;
        if (timer) clearTimeout(timer);
      };
    }, [ownedRequest]),
  );

  const selectedArea = areas?.find((area) => area.id === selectedAreaId) ?? null;
  const isEntry = selectedMode === 'ENTRY';
  const backendConnected = systemState !== null && backendError === null;
  const esp32Online = backendConnected && systemState.esp32Online;
  const personDetected = esp32Online ? systemState.personDetected : null;
  const fingerprintReady = esp32Online && systemState.fingerprintReady === true;
  const currentRequest = requestResult ?? ownedRequest;
  const currentStatus = currentRequest?.status ?? null;
  const anotherRequestActive =
    systemState?.activeAccessRequest !== null &&
    systemState?.activeAccessRequest.requestId !== ownedRequest?.requestId;
  const scanEnabled = Boolean(
    backendConnected &&
      systemState.systemActive &&
      esp32Online &&
      fingerprintReady &&
      selectedArea?.isActive &&
      !systemState.lockdownActive &&
      systemState.doorState === 'CLOSED' &&
      !ownedRequest &&
      !isCreatingRequest &&
      systemState.activeAccessRequest === null &&
      (!isEntry || personDetected === true),
  );
  const initialLoading = !areas && !systemState && !areasError && !backendError;

  const clearTerminalResult = () => {
    if (!ownedRequest) {
      setRequestResult(null);
      setRequestError(null);
    }
  };
  const chooseMode = (mode: AccessDirection) => {
    if (ownedRequest) return;
    setSelectedMode(mode);
    clearTerminalResult();
  };
  const chooseArea = (areaId: number) => {
    if (ownedRequest) return;
    setSelectedAreaId(areaId);
    clearTerminalResult();
  };
  const startAccessRequest = async () => {
    if (!scanEnabled || !selectedArea) return;
    setIsCreatingRequest(true);
    setRequestResult(null);
    setRequestError(null);
    try {
      setOwnedRequest(await createAccessRequest(selectedArea.id, selectedMode));
    } catch (error) {
      setRequestError(
        error instanceof ApiError ? error : new ApiError('Unable to create an access request.'),
      );
      stationRefreshRef.current();
    } finally {
      setIsCreatingRequest(false);
    }
  };
  const retryConnections = () => {
    stationRefreshRef.current();
    areasRefreshRef.current();
  };

  const presenceLabel =
    personDetected === true
      ? 'PERSON DETECTED'
      : personDetected === false
        ? 'NO PERSON'
        : 'UNAVAILABLE';
  const presenceTone: StatusTone =
    personDetected === true ? 'success' : personDetected === false ? 'warning' : 'neutral';
  const presentation = isCreatingRequest
    ? {
        title: 'SENDING REQUEST',
        description: 'Sending the access request to the Backend…',
        tone: 'warning' as const,
      }
    : resultPresentation(currentStatus);
  const resultArea = currentRequest
    ? areas?.find((area) => area.id === currentRequest.areaId) ?? null
    : null;
  const displayReason =
    requestResult?.status === 'DENIED' || requestResult?.status === 'FAILED'
      ? presentReason(requestResult.reasonCode, requestResult.message)
      : requestResult?.status === 'EXPIRED'
        ? presentReason('REQUEST_EXPIRED', requestResult.message)
        : presentation.description;
  const resultPalette = {
    success: { soft: colors.successSoft, strong: colors.success },
    danger: { soft: colors.dangerSoft, strong: colors.danger },
    warning: { soft: colors.warningSoft, strong: colors.warning },
    neutral: { soft: colors.neutralSoft, strong: colors.textSecondary },
  }[presentation.tone];

  let actionExplanation = scanEnabled
    ? 'Ready to send an access request.'
    : 'Complete the operational requirements above to start a scan.';
  if (backendError) actionExplanation = 'Backend disconnected. Retry or wait for reconnection.';
  else if (ownedRequest && currentStatus === 'QUEUED') {
    actionExplanation = 'Request sent. Wait for ESP32 acknowledgement before placing your finger.';
  } else if (ownedRequest && currentStatus === 'IN_PROGRESS') {
    actionExplanation = 'ESP32 acknowledged the request. Place your finger on the sensor now.';
  } else if (ownedRequest) actionExplanation = 'This access request is currently in progress.';
  else if (anotherRequestActive) actionExplanation = 'Another access request is in progress.';
  else if (systemState?.lockdownActive) actionExplanation = 'Scanning is disabled while Lockdown is active.';

  return (
    <PageContainer
      title="Access Control"
      subtitle="Physical access station"
      headerAccessory={
        <StatusBadge
          label={backendConnected ? 'API CONNECTED' : 'API DISCONNECTED'}
          tone={backendConnected ? 'success' : backendError ? 'danger' : 'neutral'}
        />
      }>
      {initialLoading ? <LoadingState message="Loading access station…" /> : null}
      {backendError || areasError ? (
        <ErrorState
          message={(backendError ?? areasError)?.message ?? 'Unable to load access data.'}
          onRetry={retryConnections}
        />
      ) : null}

      {systemState || areas ? (
        <>
          <Text style={[styles.sectionTitle, { color: colors.text }]}>Station Status</Text>
          <View style={styles.statusGrid}>
            <View style={[styles.statusCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
              <Text style={[styles.cardLabel, { color: colors.textSecondary }]}>ESP32</Text>
              <StatusBadge label={esp32Online ? 'ONLINE' : 'OFFLINE'} tone={esp32Online ? 'success' : 'neutral'} />
              <Text style={[styles.cardDescription, { color: colors.textSecondary }]}>
                {backendError
                  ? 'Backend connection unavailable.'
                  : esp32Online
                    ? 'Heartbeat is current.'
                    : 'Waiting for a current heartbeat.'}
              </Text>
            </View>
            <View style={[styles.statusCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
              <Text style={[styles.cardLabel, { color: colors.textSecondary }]}>Presence</Text>
              <StatusBadge label={presenceLabel} tone={presenceTone} />
              <Text style={[styles.cardDescription, { color: colors.textSecondary }]}>
                {personDetected === null
                  ? 'Current sensor data is unavailable.'
                  : systemState?.distanceCm === null
                    ? 'No distance measurement.'
                    : `Distance: ${systemState?.distanceCm.toFixed(1)} cm`}
              </Text>
            </View>
          </View>

          <Text style={[styles.sectionTitle, { color: colors.text }]}>Access Mode</Text>
          <View style={[styles.modeSelector, { backgroundColor: colors.card, borderColor: colors.border }]}>
            {(['ENTRY', 'EXIT'] as AccessDirection[]).map((mode) => {
              const isSelected = selectedMode === mode;
              return (
                <Pressable
                  key={mode}
                  accessibilityRole="button"
                  accessibilityState={{ selected: isSelected, disabled: Boolean(ownedRequest) }}
                  disabled={Boolean(ownedRequest)}
                  onPress={() => chooseMode(mode)}
                  style={({ pressed }) => [
                    styles.modeButton,
                    { backgroundColor: isSelected ? colors.tint : 'transparent' },
                    ownedRequest && styles.disabledControl,
                    pressed && styles.pressed,
                  ]}>
                  <Text style={[styles.modeText, { color: isSelected ? '#FFFFFF' : colors.textSecondary }]}>
                    {mode}
                  </Text>
                </Pressable>
              );
            })}
          </View>

          <View style={[styles.helperCard, { backgroundColor: colors.neutralSoft }]}>
            <Text style={[styles.helperTitle, { color: colors.text }]}>
              {isEntry ? 'Entry Requirements' : 'Exit Requirements'}
            </Text>
            <Text style={[styles.helperText, { color: colors.textSecondary }]}>
              {isEntry
                ? 'ESP32 online • Person detected • Area selected • Fingerprint ready'
                : 'ESP32 online • Area selected • Fingerprint ready'}
            </Text>
            <Text style={[styles.helperEmphasis, { color: colors.tint }]}>
              {isEntry ? 'Person presence is required for entry.' : 'Presence is not required for exit.'}
            </Text>
          </View>

          <View style={styles.sectionHeader}>
            <Text style={[styles.sectionTitle, { color: colors.text }]}>Select Area</Text>
            <Text style={[styles.sectionHint, { color: colors.textSecondary }]}>Backend data</Text>
          </View>
          <View style={styles.areaGrid}>
            {(areas ?? []).map((area) => {
              const isSelected = selectedAreaId === area.id;
              const disabled = !area.isActive || Boolean(ownedRequest);
              return (
                <Pressable
                  key={area.id}
                  accessibilityRole="button"
                  accessibilityLabel={`Select ${area.name}`}
                  accessibilityState={{ selected: isSelected, disabled }}
                  disabled={disabled}
                  onPress={() => chooseArea(area.id)}
                  style={({ pressed }) => [
                    styles.areaButton,
                    {
                      backgroundColor: isSelected ? colors.tint : colors.card,
                      borderColor: isSelected ? colors.tint : colors.border,
                    },
                    disabled && styles.disabledArea,
                    pressed && styles.pressed,
                  ]}>
                  <Text style={[styles.areaNumber, { color: isSelected ? '#EAF1FF' : colors.textSecondary }]}>
                    AREA {area.id}
                  </Text>
                  <Text style={[styles.areaName, { color: isSelected ? '#FFFFFF' : colors.text }]}>
                    {area.name}
                  </Text>
                  {!area.isActive ? (
                    <Text style={[styles.inactiveText, { color: colors.textSecondary }]}>INACTIVE</Text>
                  ) : null}
                </Pressable>
              );
            })}
          </View>

          <Text style={[styles.sectionTitle, { color: colors.text }]}>Selected Area</Text>
          <View style={[styles.selectionCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
            {selectedArea ? (
              <>
                <View style={styles.selectionText}>
                  <Text style={[styles.selectedAreaName, { color: colors.text }]}>{selectedArea.name}</Text>
                  <Text style={[styles.selectedAreaNumber, { color: colors.textSecondary }]}>Area {selectedArea.id}</Text>
                </View>
                <StatusBadge label="SELECTED" tone="success" />
              </>
            ) : (
              <Text style={[styles.noSelectionText, { color: colors.textSecondary }]}>No area selected</Text>
            )}
          </View>

          <View style={[styles.actionCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
            <Text style={[styles.actionTitle, { color: colors.text }]}>Fingerprint Action</Text>
            <Pressable
              accessibilityRole="button"
              accessibilityState={{ disabled: !scanEnabled }}
              disabled={!scanEnabled}
              onPress={() => void startAccessRequest()}
              style={({ pressed }) => [
                styles.actionButton,
                { backgroundColor: scanEnabled ? colors.tint : colors.neutralSoft },
                pressed && styles.pressed,
              ]}>
              {isCreatingRequest ? (
                <ActivityIndicator color="#FFFFFF" />
              ) : (
                <Text style={[styles.actionButtonText, { color: scanEnabled ? '#FFFFFF' : colors.textSecondary }]}>
                  {isEntry ? 'Scan Fingerprint' : 'Scan for Exit'}
                </Text>
              )}
            </Pressable>
            <Text style={[styles.disabledExplanation, { color: colors.textSecondary }]}>
              {actionExplanation}
            </Text>
            {requestError ? (
              <Text style={[styles.requestError, { color: colors.danger }]}>
                {presentReason(requestError.reasonCode, requestError.message)}
              </Text>
            ) : null}
          </View>

          <Text style={[styles.sectionTitle, { color: colors.text }]}>Access Result</Text>
          <View style={[styles.resultCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
            <View style={[styles.resultIcon, { backgroundColor: resultPalette.soft }]}>
              <View style={[styles.resultDot, { backgroundColor: resultPalette.strong }]} />
            </View>
            <View style={styles.resultText}>
              <Text style={[styles.resultTitle, { color: resultPalette.strong }]}>{presentation.title}</Text>
              {requestResult?.user ? (
                <Text style={[styles.resultDetail, { color: colors.text }]}>{requestResult.user.name}</Text>
              ) : requestResult?.reasonCode === 'UNKNOWN_FINGERPRINT' ? (
                <Text style={[styles.resultDetail, { color: colors.text }]}>Unknown Fingerprint</Text>
              ) : null}
              {currentRequest ? (
                <Text style={[styles.resultDetail, { color: colors.textSecondary }]}>
                  {resultArea?.name ?? `Area ${currentRequest.areaId}`} • {currentRequest.direction}
                </Text>
              ) : null}
              <Text style={[styles.resultDescription, { color: colors.textSecondary }]}>{displayReason}</Text>
            </View>
          </View>
        </>
      ) : null}
    </PageContainer>
  );
}

const styles = StyleSheet.create({
  sectionHeader: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  sectionTitle: { fontSize: 19, fontWeight: '700' },
  sectionHint: { fontSize: 12, fontWeight: '500' },
  statusGrid: { flexDirection: 'row', gap: 10 },
  statusCard: { flex: 1, minHeight: 148, borderWidth: 1, borderRadius: 16, padding: 14, alignItems: 'flex-start', gap: 10 },
  cardLabel: { fontSize: 13, fontWeight: '600' },
  cardDescription: { fontSize: 12, lineHeight: 17 },
  modeSelector: { borderWidth: 1, borderRadius: 15, padding: 5, flexDirection: 'row', gap: 5 },
  modeButton: { flex: 1, minHeight: 48, borderRadius: 11, alignItems: 'center', justifyContent: 'center' },
  modeText: { fontSize: 13, fontWeight: '800', letterSpacing: 0.6 },
  helperCard: { borderRadius: 14, padding: 14, gap: 6 },
  helperTitle: { fontSize: 13, fontWeight: '700' },
  helperText: { fontSize: 12, lineHeight: 18 },
  helperEmphasis: { fontSize: 12, lineHeight: 18, fontWeight: '700' },
  areaGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 10 },
  areaButton: { width: '48.5%', minHeight: 88, borderWidth: 1, borderRadius: 15, padding: 13, justifyContent: 'center', gap: 5 },
  disabledArea: { opacity: 0.5 },
  disabledControl: { opacity: 0.7 },
  areaNumber: { fontSize: 10, fontWeight: '800', letterSpacing: 0.5 },
  areaName: { fontSize: 14, lineHeight: 18, fontWeight: '700' },
  inactiveText: { fontSize: 9, fontWeight: '800' },
  selectionCard: { minHeight: 76, borderWidth: 1, borderRadius: 16, padding: 15, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 12 },
  selectionText: { flex: 1 },
  selectedAreaName: { fontSize: 16, fontWeight: '700' },
  selectedAreaNumber: { fontSize: 12, marginTop: 3 },
  noSelectionText: { fontSize: 14, fontWeight: '600' },
  actionCard: { borderWidth: 1, borderRadius: 16, padding: 15, gap: 12 },
  actionTitle: { fontSize: 15, fontWeight: '700' },
  actionButton: { minHeight: 52, borderRadius: 13, alignItems: 'center', justifyContent: 'center' },
  actionButtonText: { fontSize: 15, fontWeight: '800' },
  disabledExplanation: { fontSize: 12, lineHeight: 18, textAlign: 'center' },
  requestError: { fontSize: 12, lineHeight: 18, fontWeight: '700', textAlign: 'center' },
  resultCard: { minHeight: 104, borderWidth: 1, borderRadius: 16, padding: 15, flexDirection: 'row', alignItems: 'center', gap: 13 },
  resultIcon: { width: 42, height: 42, borderRadius: 13, alignItems: 'center', justifyContent: 'center' },
  resultDot: { width: 10, height: 10, borderRadius: 5 },
  resultText: { flex: 1 },
  resultTitle: { fontSize: 15, fontWeight: '800' },
  resultDetail: { fontSize: 13, lineHeight: 18, marginTop: 3, fontWeight: '600' },
  resultDescription: { fontSize: 12, lineHeight: 17, marginTop: 4 },
  pressed: { opacity: 0.72 },
});
