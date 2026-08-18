import { StyleSheet, Text, View } from 'react-native';

import { AccessLogCard } from '@/components/access-log-card';
import { ErrorState, LoadingState } from '@/components/data-state';
import { PageContainer } from '@/components/page-container';
import { StatusBadge } from '@/components/status-badge';
import { Colors } from '@/constants/theme';
import { useApiData } from '@/hooks/use-api-data';
import { useColorScheme } from '@/hooks/use-color-scheme';
import { getDashboardData } from '@/services/api';
import type { StatusTone } from '@/types/smart-office';

export default function DashboardScreen() {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];
  const { data, error, isLoading, retry } = useApiData(getDashboardData);
  const systemStatus: { label: string; value: string; detail?: string; tone: StatusTone }[] = data
    ? [
        {
          label: 'System',
          value: data.systemState.systemActive ? 'ACTIVE' : 'INACTIVE',
          tone: data.systemState.systemActive ? 'success' : 'danger',
        },
        {
          label: 'ESP32',
          value: data.systemState.esp32Online ? 'ONLINE' : 'OFFLINE',
          tone: data.systemState.esp32Online ? 'success' : 'neutral',
        },
        {
          label: 'Door',
          value: data.systemState.doorState,
          tone: data.systemState.doorState === 'OPEN' ? 'warning' : 'neutral',
        },
        {
          label: 'Lockdown',
          value: data.systemState.lockdownActive ? 'ON' : 'OFF',
          tone: data.systemState.lockdownActive ? 'danger' : 'success',
        },
        {
          label: 'Failed Attempts',
          value: `${data.systemState.failedAttempts} / ${data.systemState.failedAttemptLimit}`,
          tone:
            data.systemState.failedAttempts >= data.systemState.failedAttemptLimit
              ? 'danger'
              : data.systemState.failedAttempts > 0
                ? 'warning'
                : 'success',
        },
      ]
    : [];
  const occupancySummary = data
    ? [
        { label: 'Total Users Inside', count: data.totalUsersInside },
        ...data.areas.map((area) => ({ label: area.name, count: area.occupancy })),
      ]
    : [];

  return (
    <PageContainer
      title="Smart Office"
      subtitle="Building Access System"
      headerAccessory={
        <StatusBadge
          label={data ? 'API CONNECTED' : 'BACKEND'}
          tone={error ? 'danger' : data ? 'success' : 'neutral'}
        />
      }>
      {isLoading ? <LoadingState message="Loading dashboard…" /> : null}
      {!isLoading && error ? (
        <ErrorState message={error.message} onRetry={() => void retry()} />
      ) : null}
      {!isLoading && !error && data ? (
        <>
      <View style={styles.sectionHeader}>
        <Text style={[styles.sectionTitle, { color: colors.text }]}>System Status</Text>
        <Text style={[styles.sectionHint, { color: colors.textSecondary }]}>Backend data</Text>
      </View>

      <View style={styles.statusGrid}>
        {systemStatus.map((item) => (
          <View
            key={item.label}
            style={[
              styles.statusCard,
              { backgroundColor: colors.card, borderColor: colors.border },
            ]}>
            <Text style={[styles.cardLabel, { color: colors.textSecondary }]}>{item.label}</Text>
            <StatusBadge label={item.value} tone={item.tone} />
            {item.detail ? (
              <Text style={[styles.cardDetail, { color: colors.textSecondary }]}>{item.detail}</Text>
            ) : null}
          </View>
        ))}
      </View>

      <Text style={[styles.sectionTitle, { color: colors.text }]}>Occupancy</Text>
      <View style={styles.occupancyGrid}>
        {occupancySummary.map((item, index) => (
          <View
            key={item.label}
            style={[
              styles.occupancyCard,
              index === 0 && styles.totalOccupancyCard,
              {
                backgroundColor: index === 0 ? colors.tint : colors.card,
                borderColor: index === 0 ? colors.tint : colors.border,
              },
            ]}>
            <Text
              style={[
                styles.occupancyCount,
                { color: index === 0 ? '#FFFFFF' : colors.text },
              ]}>
              {item.count}
            </Text>
            <Text
              style={[
                styles.occupancyLabel,
                { color: index === 0 ? '#EAF1FF' : colors.textSecondary },
              ]}>
              {item.label}
            </Text>
          </View>
        ))}
      </View>

      <View style={styles.sectionHeader}>
        <Text style={[styles.sectionTitle, { color: colors.text }]}>Recent Activity</Text>
        <Text style={[styles.sectionHint, { color: colors.textSecondary }]}>Latest events</Text>
      </View>
      <View style={styles.activityList}>
        {data.accessLogs.map((log) => (
          <AccessLogCard key={log.id} log={log} compact />
        ))}
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
    marginTop: 4,
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
    flexWrap: 'wrap',
    gap: 10,
  },
  statusCard: {
    width: '48.5%',
    minHeight: 112,
    borderWidth: 1,
    borderRadius: 16,
    padding: 14,
    alignItems: 'flex-start',
    gap: 9,
  },
  cardLabel: {
    fontSize: 13,
    fontWeight: '600',
  },
  cardDetail: {
    fontSize: 12,
    marginTop: -4,
  },
  occupancyGrid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 10,
  },
  occupancyCard: {
    width: '31.4%',
    minHeight: 92,
    borderWidth: 1,
    borderRadius: 16,
    padding: 13,
    justifyContent: 'center',
  },
  totalOccupancyCard: {
    width: '100%',
    minHeight: 104,
  },
  occupancyCount: {
    fontSize: 29,
    fontWeight: '800',
    lineHeight: 34,
  },
  occupancyLabel: {
    fontSize: 12,
    fontWeight: '600',
    lineHeight: 16,
    marginTop: 3,
  },
  activityList: {
    gap: 10,
  },
});
