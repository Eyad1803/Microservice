import { StyleSheet, Text, View } from 'react-native';

import { ErrorState, LoadingState } from '@/components/data-state';
import { PageContainer } from '@/components/page-container';
import { StatusBadge } from '@/components/status-badge';
import { Colors } from '@/constants/theme';
import { useApiData } from '@/hooks/use-api-data';
import { useColorScheme } from '@/hooks/use-color-scheme';
import { getAreas } from '@/services/api';

export default function AreasScreen() {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];
  const { data: areas, error, isLoading, retry } = useApiData(getAreas);
  const activeAreaCount = areas?.filter((area) => area.isActive).length ?? 0;

  return (
    <PageContainer title="Areas" subtitle="Building access zones">
      {isLoading ? <LoadingState message="Loading areas…" /> : null}
      {!isLoading && error ? (
        <ErrorState message={error.message} onRetry={() => void retry()} />
      ) : null}
      {!isLoading && !error && areas ? (
        <>
      <View style={styles.summaryRow}>
        <Text style={[styles.summaryText, { color: colors.textSecondary }]}>
          {areas.length} configured areas
        </Text>
        <Text style={[styles.summaryText, { color: colors.success }]}>
          {activeAreaCount === areas.length ? 'All active' : `${activeAreaCount} active`}
        </Text>
      </View>

      <View style={styles.list}>
        {areas.map((area) => (
          <View
            key={area.id}
            style={[styles.card, { backgroundColor: colors.card, borderColor: colors.border }]}>
            <View style={styles.cardTopRow}>
              <View style={styles.areaHeading}>
                <View style={[styles.areaNumber, { backgroundColor: colors.neutralSoft }]}>
                  <Text style={[styles.areaNumberText, { color: colors.tint }]}>{area.id}</Text>
                </View>
                <Text style={[styles.areaName, { color: colors.text }]}>{area.name}</Text>
              </View>
              <StatusBadge
                label={area.isActive ? 'ACTIVE' : 'INACTIVE'}
                tone={area.isActive ? 'success' : 'neutral'}
              />
            </View>
            <View style={[styles.occupancyRow, { borderTopColor: colors.border }]}>
              <Text style={[styles.occupancyLabel, { color: colors.textSecondary }]}>Occupancy</Text>
              <Text style={[styles.occupancyValue, { color: colors.text }]}>
                {area.occupancy} {area.occupancy === 1 ? 'person' : 'people'}
              </Text>
            </View>
          </View>
        ))}
      </View>
        </>
      ) : null}
    </PageContainer>
  );
}

const styles = StyleSheet.create({
  summaryRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
  },
  summaryText: {
    fontSize: 12,
    fontWeight: '600',
  },
  list: {
    gap: 11,
  },
  card: {
    borderWidth: 1,
    borderRadius: 16,
    padding: 15,
    gap: 14,
  },
  cardTopRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 10,
  },
  areaHeading: {
    flex: 1,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 11,
  },
  areaNumber: {
    width: 38,
    height: 38,
    borderRadius: 11,
    alignItems: 'center',
    justifyContent: 'center',
  },
  areaNumberText: {
    fontSize: 15,
    fontWeight: '800',
  },
  areaName: {
    flex: 1,
    fontSize: 16,
    fontWeight: '700',
  },
  occupancyRow: {
    borderTopWidth: 1,
    paddingTop: 12,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  occupancyLabel: {
    fontSize: 13,
  },
  occupancyValue: {
    fontSize: 13,
    fontWeight: '700',
  },
});
