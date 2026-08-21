import { useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';

import { ErrorState, LoadingState } from '@/components/data-state';
import { PageContainer } from '@/components/page-container';
import { StatusBadge } from '@/components/status-badge';
import { Colors } from '@/constants/theme';
import { useApiData } from '@/hooks/use-api-data';
import { useColorScheme } from '@/hooks/use-color-scheme';
import { getAreas, getSystemState } from '@/services/api';

type AccessMode = 'ENTRY' | 'EXIT';

async function getAccessScreenData() {
  const [systemState, areas] = await Promise.all([getSystemState(), getAreas()]);
  return { systemState, areas };
}

export default function AccessScreen() {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];
  const [selectedMode, setSelectedMode] = useState<AccessMode>('ENTRY');
  const [selectedAreaId, setSelectedAreaId] = useState<number | null>(null);
  const { data, error, isLoading, retry } = useApiData(getAccessScreenData);
  const selectedArea = data?.areas.find((area) => area.id === selectedAreaId) ?? null;
  const isEntry = selectedMode === 'ENTRY';

  return (
    <PageContainer
      title="Access Control"
      subtitle="Physical access station"
      headerAccessory={
        <StatusBadge
          label={data ? 'API CONNECTED' : 'BACKEND'}
          tone={error ? 'danger' : data ? 'success' : 'neutral'}
        />
      }>
      {isLoading ? <LoadingState message="Loading access station…" /> : null}
      {!isLoading && error ? (
        <ErrorState message={error.message} onRetry={() => void retry()} />
      ) : null}

      {!isLoading && !error && data ? (
        <>
          <Text style={[styles.sectionTitle, { color: colors.text }]}>Station Status</Text>
          <View style={styles.statusGrid}>
            <View
              style={[
                styles.statusCard,
                { backgroundColor: colors.card, borderColor: colors.border },
              ]}>
              <Text style={[styles.cardLabel, { color: colors.textSecondary }]}>ESP32</Text>
              <StatusBadge
                label={data.systemState.esp32Online ? 'ONLINE' : 'OFFLINE'}
                tone={data.systemState.esp32Online ? 'success' : 'neutral'}
              />
              <Text style={[styles.cardDescription, { color: colors.textSecondary }]}>
                {data.systemState.esp32Online
                  ? 'Device status received from backend.'
                  : 'No heartbeat received.'}
              </Text>
            </View>

            <View
              style={[
                styles.statusCard,
                { backgroundColor: colors.card, borderColor: colors.border },
              ]}>
              <Text style={[styles.cardLabel, { color: colors.textSecondary }]}>Presence</Text>
              <StatusBadge label="UNAVAILABLE" tone="neutral" />
              <Text style={[styles.cardDescription, { color: colors.textSecondary }]}>
                Waiting for ESP32 integration.
              </Text>
            </View>
          </View>

          <Text style={[styles.sectionTitle, { color: colors.text }]}>Access Mode</Text>
          <View
            style={[
              styles.modeSelector,
              { backgroundColor: colors.card, borderColor: colors.border },
            ]}>
            {(['ENTRY', 'EXIT'] as AccessMode[]).map((mode) => {
              const isSelected = selectedMode === mode;

              return (
                <Pressable
                  key={mode}
                  accessibilityRole="button"
                  accessibilityState={{ selected: isSelected }}
                  onPress={() => setSelectedMode(mode)}
                  style={({ pressed }) => [
                    styles.modeButton,
                    { backgroundColor: isSelected ? colors.tint : 'transparent' },
                    pressed && styles.pressed,
                  ]}>
                  <Text
                    style={[
                      styles.modeText,
                      { color: isSelected ? '#FFFFFF' : colors.textSecondary },
                    ]}>
                    {mode}
                  </Text>
                </Pressable>
              );
            })}
          </View>

          <View style={[styles.helperCard, { backgroundColor: colors.neutralSoft }]}>
            <Text style={[styles.helperTitle, { color: colors.text }]}>Future requirements</Text>
            <Text style={[styles.helperText, { color: colors.textSecondary }]}>
              {isEntry
                ? 'ESP32 online • Person presence • Area selected • Fingerprint scan'
                : 'ESP32 online • Area selected • Fingerprint scan'}
            </Text>
            <Text style={[styles.helperEmphasis, { color: colors.tint }]}>
              {isEntry
                ? 'Person presence is required for entry.'
                : 'Presence is not required for exit.'}
            </Text>
          </View>

          <View style={styles.sectionHeader}>
            <Text style={[styles.sectionTitle, { color: colors.text }]}>Select Area</Text>
            <Text style={[styles.sectionHint, { color: colors.textSecondary }]}>Backend data</Text>
          </View>
          <View style={styles.areaGrid}>
            {data.areas.map((area) => {
              const isSelected = selectedAreaId === area.id;

              return (
                <Pressable
                  key={area.id}
                  accessibilityRole="button"
                  accessibilityLabel={`Select ${area.name}`}
                  accessibilityState={{ selected: isSelected, disabled: !area.isActive }}
                  disabled={!area.isActive}
                  onPress={() => setSelectedAreaId(area.id)}
                  style={({ pressed }) => [
                    styles.areaButton,
                    {
                      backgroundColor: isSelected ? colors.tint : colors.card,
                      borderColor: isSelected ? colors.tint : colors.border,
                    },
                    !area.isActive && styles.disabledArea,
                    pressed && styles.pressed,
                  ]}>
                  <Text
                    style={[
                      styles.areaNumber,
                      { color: isSelected ? '#EAF1FF' : colors.textSecondary },
                    ]}>
                    AREA {area.id}
                  </Text>
                  <Text
                    style={[
                      styles.areaName,
                      { color: isSelected ? '#FFFFFF' : colors.text },
                    ]}>
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
          <View
            style={[
              styles.selectionCard,
              { backgroundColor: colors.card, borderColor: colors.border },
            ]}>
            {selectedArea ? (
              <>
                <View style={styles.selectionText}>
                  <Text style={[styles.selectedAreaName, { color: colors.text }]}>
                    {selectedArea.name}
                  </Text>
                  <Text style={[styles.selectedAreaNumber, { color: colors.textSecondary }]}>
                    Area {selectedArea.id}
                  </Text>
                </View>
                <StatusBadge label="SELECTED" tone="success" />
              </>
            ) : (
              <Text style={[styles.noSelectionText, { color: colors.textSecondary }]}>
                No area selected
              </Text>
            )}
          </View>

          <View
            style={[
              styles.actionCard,
              { backgroundColor: colors.card, borderColor: colors.border },
            ]}>
            <Text style={[styles.actionTitle, { color: colors.text }]}>Fingerprint Action</Text>
            <Pressable
              accessibilityRole="button"
              accessibilityState={{ disabled: true }}
              disabled
              style={[styles.actionButton, { backgroundColor: colors.neutralSoft }]}>
              <Text style={[styles.actionButtonText, { color: colors.textSecondary }]}>
                {isEntry ? 'Scan Fingerprint' : 'Scan for Exit'}
              </Text>
            </Pressable>
            <Text style={[styles.disabledExplanation, { color: colors.textSecondary }]}>
              Hardware control is not connected yet. Available after ESP32 integration.
            </Text>
          </View>

          <Text style={[styles.sectionTitle, { color: colors.text }]}>Access Result</Text>
          <View
            style={[
              styles.resultCard,
              { backgroundColor: colors.card, borderColor: colors.border },
            ]}>
            <View style={[styles.resultIcon, { backgroundColor: colors.neutralSoft }]}>
              <View style={[styles.resultDot, { backgroundColor: colors.textSecondary }]} />
            </View>
            <View style={styles.resultText}>
              <Text style={[styles.resultTitle, { color: colors.text }]}>No access attempt yet</Text>
              <Text style={[styles.resultDescription, { color: colors.textSecondary }]}>
                A real hardware result will appear here after integration.
              </Text>
            </View>
          </View>
        </>
      ) : null}
    </PageContainer>
  );
}

const styles = StyleSheet.create({
  sectionHeader: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  sectionTitle: {
    fontSize: 19,
    fontWeight: '700',
  },
  sectionHint: {
    fontSize: 12,
    fontWeight: '500',
  },
  statusGrid: {
    flexDirection: 'row',
    gap: 10,
  },
  statusCard: {
    flex: 1,
    minHeight: 148,
    borderWidth: 1,
    borderRadius: 16,
    padding: 14,
    alignItems: 'flex-start',
    gap: 10,
  },
  cardLabel: {
    fontSize: 13,
    fontWeight: '600',
  },
  cardDescription: {
    fontSize: 12,
    lineHeight: 17,
  },
  modeSelector: {
    borderWidth: 1,
    borderRadius: 15,
    padding: 5,
    flexDirection: 'row',
    gap: 5,
  },
  modeButton: {
    flex: 1,
    minHeight: 48,
    borderRadius: 11,
    alignItems: 'center',
    justifyContent: 'center',
  },
  modeText: {
    fontSize: 13,
    fontWeight: '800',
    letterSpacing: 0.6,
  },
  helperCard: {
    borderRadius: 14,
    padding: 14,
    gap: 6,
  },
  helperTitle: {
    fontSize: 13,
    fontWeight: '700',
  },
  helperText: {
    fontSize: 12,
    lineHeight: 18,
  },
  helperEmphasis: {
    fontSize: 12,
    lineHeight: 18,
    fontWeight: '700',
  },
  areaGrid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 10,
  },
  areaButton: {
    width: '48.5%',
    minHeight: 88,
    borderWidth: 1,
    borderRadius: 15,
    padding: 13,
    justifyContent: 'center',
    gap: 5,
  },
  disabledArea: {
    opacity: 0.5,
  },
  areaNumber: {
    fontSize: 10,
    fontWeight: '800',
    letterSpacing: 0.5,
  },
  areaName: {
    fontSize: 14,
    lineHeight: 18,
    fontWeight: '700',
  },
  inactiveText: {
    fontSize: 9,
    fontWeight: '800',
  },
  selectionCard: {
    minHeight: 76,
    borderWidth: 1,
    borderRadius: 16,
    padding: 15,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 12,
  },
  selectionText: {
    flex: 1,
  },
  selectedAreaName: {
    fontSize: 16,
    fontWeight: '700',
  },
  selectedAreaNumber: {
    fontSize: 12,
    marginTop: 3,
  },
  noSelectionText: {
    fontSize: 14,
    fontWeight: '600',
  },
  actionCard: {
    borderWidth: 1,
    borderRadius: 16,
    padding: 15,
    gap: 12,
  },
  actionTitle: {
    fontSize: 15,
    fontWeight: '700',
  },
  actionButton: {
    minHeight: 52,
    borderRadius: 13,
    alignItems: 'center',
    justifyContent: 'center',
  },
  actionButtonText: {
    fontSize: 15,
    fontWeight: '800',
  },
  disabledExplanation: {
    fontSize: 12,
    lineHeight: 18,
    textAlign: 'center',
  },
  resultCard: {
    minHeight: 104,
    borderWidth: 1,
    borderRadius: 16,
    padding: 15,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 13,
  },
  resultIcon: {
    width: 42,
    height: 42,
    borderRadius: 13,
    alignItems: 'center',
    justifyContent: 'center',
  },
  resultDot: {
    width: 10,
    height: 10,
    borderRadius: 5,
  },
  resultText: {
    flex: 1,
  },
  resultTitle: {
    fontSize: 15,
    fontWeight: '700',
  },
  resultDescription: {
    fontSize: 12,
    lineHeight: 17,
    marginTop: 4,
  },
  pressed: {
    opacity: 0.72,
  },
});
