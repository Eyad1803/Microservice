import { router } from 'expo-router';
import { Pressable, StyleSheet, Text, View } from 'react-native';

import { ErrorState, LoadingState } from '@/components/data-state';
import { PageContainer } from '@/components/page-container';
import { IconSymbol } from '@/components/ui/icon-symbol';
import { Colors } from '@/constants/theme';
import { useApiData } from '@/hooks/use-api-data';
import { useColorScheme } from '@/hooks/use-color-scheme';
import { getUsers } from '@/services/api';

export default function UsersScreen() {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];
  const { data: users, error, isLoading, retry } = useApiData(getUsers);

  return (
    <PageContainer title="Users" subtitle="Configured fingerprint users">
      {isLoading ? <LoadingState message="Loading users…" /> : null}
      {!isLoading && error ? (
        <ErrorState message={error.message} onRetry={() => void retry()} />
      ) : null}
      {!isLoading && !error && users ? (
        <>
      <View style={styles.summaryRow}>
        <Text style={[styles.summaryText, { color: colors.textSecondary }]}>
          {users.length} configured users
        </Text>
        <Text style={[styles.summaryText, { color: colors.textSecondary }]}>Tap to view details</Text>
      </View>

      <View style={styles.list}>
        {users.map((user) => (
          <Pressable
            key={user.id}
            accessibilityRole="button"
            accessibilityLabel={`View details for ${user.name}`}
            onPress={() =>
              router.push({ pathname: '/users/[id]', params: { id: String(user.id) } })
            }
            style={({ pressed }) => [
              styles.card,
              { backgroundColor: colors.card, borderColor: colors.border },
              pressed && styles.cardPressed,
            ]}>
            <View style={[styles.avatar, { backgroundColor: colors.neutralSoft }]}>
              <Text style={[styles.avatarText, { color: colors.tint }]}>
                {user.name
                  .split(' ')
                  .map((part) => part[0])
                  .join('')}
              </Text>
            </View>
            <View style={styles.details}>
              <Text style={[styles.name, { color: colors.text }]}>{user.name}</Text>
              <Text style={[styles.company, { color: colors.textSecondary }]}>{user.company}</Text>
              <View style={styles.metaRow}>
                <View style={[styles.metaChip, { backgroundColor: colors.neutralSoft }]}>
                  <Text style={[styles.metaText, { color: colors.textSecondary }]}>{user.role}</Text>
                </View>
                <View style={[styles.metaChip, { backgroundColor: colors.neutralSoft }]}>
                  <Text style={[styles.metaText, { color: colors.textSecondary }]}>
                    Fingerprint ID {user.fingerprintId}
                  </Text>
                </View>
              </View>
            </View>
            <View style={[styles.chevron, { backgroundColor: colors.neutralSoft }]}>
              <IconSymbol name="chevron.right" size={20} color={colors.icon} />
            </View>
          </Pressable>
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
    flexDirection: 'row',
    alignItems: 'center',
    gap: 13,
  },
  cardPressed: {
    opacity: 0.7,
  },
  avatar: {
    width: 48,
    height: 48,
    borderRadius: 14,
    alignItems: 'center',
    justifyContent: 'center',
  },
  avatarText: {
    fontSize: 16,
    fontWeight: '800',
  },
  details: {
    flex: 1,
  },
  name: {
    fontSize: 17,
    fontWeight: '700',
  },
  company: {
    fontSize: 13,
    marginTop: 2,
  },
  metaRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 7,
    marginTop: 10,
  },
  metaChip: {
    paddingHorizontal: 9,
    paddingVertical: 5,
    borderRadius: 8,
  },
  metaText: {
    fontSize: 11,
    fontWeight: '600',
  },
  chevron: {
    width: 30,
    height: 30,
    borderRadius: 10,
    alignItems: 'center',
    justifyContent: 'center',
  },
});
