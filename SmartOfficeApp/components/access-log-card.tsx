import { StyleSheet, Text, View } from 'react-native';

import { StatusBadge } from '@/components/status-badge';
import { Colors } from '@/constants/theme';
import { useColorScheme } from '@/hooks/use-color-scheme';
import type { AccessLog } from '@/types/smart-office';

type AccessLogCardProps = {
  log: AccessLog;
  compact?: boolean;
};

export function AccessLogCard({ log, compact = false }: AccessLogCardProps) {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];
  const isDenied = log.decision === 'DENIED';
  const resultLabel = `${log.direction} ${log.decision}`;
  const eventDate = new Date(log.eventTimestamp);
  const timestamp = Number.isNaN(eventDate.getTime())
    ? log.eventTimestamp
    : eventDate.toLocaleString();
  const reason = log.reason?.replace(/_/g, ' ');

  return (
    <View
      style={[
        styles.card,
        compact && styles.compactCard,
        {
          backgroundColor: colors.card,
          borderColor: colors.border,
          borderLeftColor: isDenied ? colors.danger : colors.success,
        },
      ]}>
      <View style={styles.topRow}>
        <View style={styles.identity}>
          <Text style={[styles.user, { color: colors.text }]}>{log.user}</Text>
          <Text style={[styles.area, { color: colors.textSecondary }]}>{log.area}</Text>
        </View>
        <StatusBadge label={resultLabel} tone={isDenied ? 'danger' : 'success'} />
      </View>
      <View style={styles.bottomRow}>
        <Text style={[styles.timestamp, { color: colors.textSecondary }]}>{timestamp}</Text>
        {reason ? <Text style={[styles.reason, { color: colors.danger }]}>{reason}</Text> : null}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  card: {
    borderWidth: 1,
    borderLeftWidth: 4,
    borderRadius: 16,
    padding: 15,
    gap: 12,
  },
  compactCard: {
    padding: 13,
  },
  topRow: {
    flexDirection: 'row',
    alignItems: 'flex-start',
    justifyContent: 'space-between',
    gap: 10,
  },
  identity: {
    flex: 1,
  },
  user: {
    fontSize: 16,
    fontWeight: '700',
  },
  area: {
    fontSize: 13,
    marginTop: 3,
  },
  bottomRow: {
    minHeight: 18,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 12,
  },
  timestamp: {
    fontSize: 12,
  },
  reason: {
    fontSize: 12,
    fontWeight: '700',
  },
});
