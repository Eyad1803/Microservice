import { StyleSheet, Text, View } from 'react-native';

import { AccessLogCard } from '@/components/access-log-card';
import { ErrorState, LoadingState } from '@/components/data-state';
import { PageContainer } from '@/components/page-container';
import { Colors } from '@/constants/theme';
import { useApiData } from '@/hooks/use-api-data';
import { useColorScheme } from '@/hooks/use-color-scheme';
import { getAccessLogs } from '@/services/api';

export default function LogsScreen() {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];
  const { data: accessLogs, error, isLoading, retry } = useApiData(getAccessLogs);

  return (
    <PageContainer title="Access Logs" subtitle="Recent physical access attempts">
      {isLoading ? <LoadingState message="Loading access logs…" /> : null}
      {!isLoading && error ? (
        <ErrorState message={error.message} onRetry={() => void retry()} />
      ) : null}
      {!isLoading && !error && accessLogs ? (
        <>
      <View
        style={[
          styles.notice,
          { backgroundColor: colors.neutralSoft, borderColor: colors.border },
        ]}>
        <View style={[styles.noticeDot, { backgroundColor: colors.tint }]} />
        <Text style={[styles.noticeText, { color: colors.textSecondary }]}>Read-only access activity from the Smart Office backend.</Text>
      </View>

      <View style={styles.list}>
        {accessLogs.map((log) => (
          <AccessLogCard key={log.id} log={log} />
        ))}
      </View>
        </>
      ) : null}
    </PageContainer>
  );
}

const styles = StyleSheet.create({
  notice: {
    borderWidth: 1,
    borderRadius: 14,
    padding: 12,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 9,
  },
  noticeDot: {
    width: 8,
    height: 8,
    borderRadius: 4,
  },
  noticeText: {
    flex: 1,
    fontSize: 12,
    lineHeight: 17,
    fontWeight: '500',
  },
  list: {
    gap: 11,
  },
});
