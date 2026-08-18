import { router, useLocalSearchParams } from 'expo-router';
import { useCallback } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { ErrorState, LoadingState } from '@/components/data-state';
import { StatusBadge } from '@/components/status-badge';
import { IconSymbol } from '@/components/ui/icon-symbol';
import { Colors } from '@/constants/theme';
import { useApiData } from '@/hooks/use-api-data';
import { useColorScheme } from '@/hooks/use-color-scheme';
import { ApiError, getUserById } from '@/services/api';

export default function UserDetailsScreen() {
  const { id } = useLocalSearchParams<{ id: string | string[] }>();
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];
  const routeId = Array.isArray(id) ? id[0] : id;
  const userId = Number(routeId);
  const loadUser = useCallback(() => {
    if (!Number.isInteger(userId) || userId <= 0) {
      return Promise.reject(new ApiError('User not found', 404));
    }
    return getUserById(userId);
  }, [userId]);
  const { data: user, error, isLoading, retry } = useApiData(loadUser);
  const isNotFound = error instanceof ApiError && error.status === 404;
  const initials = user
    ? user.name
        .split(' ')
        .map((part) => part[0])
        .join('')
    : '';

  return (
    <SafeAreaView
      edges={['top', 'bottom']}
      style={[styles.safeArea, { backgroundColor: colors.background }]}>
      <ScrollView
        showsVerticalScrollIndicator={false}
        contentContainerStyle={styles.scrollContent}>
        <Pressable
          accessibilityRole="button"
          accessibilityLabel="Back to users"
          onPress={() => router.back()}
          style={({ pressed }) => [
            styles.backButton,
            { backgroundColor: colors.neutralSoft },
            pressed && styles.pressed,
          ]}>
          <IconSymbol name="chevron.left" size={21} color={colors.tint} />
          <Text style={[styles.backText, { color: colors.tint }]}>Users</Text>
        </Pressable>

        {isLoading ? <LoadingState message="Loading user details…" /> : null}
        {!isLoading && isNotFound ? (
          <View style={styles.notFoundContainer}>
            <Text style={[styles.notFoundTitle, { color: colors.text }]}>User not found</Text>
            <Text style={[styles.notFoundMessage, { color: colors.textSecondary }]}>
              No configured user exists for this ID.
            </Text>
          </View>
        ) : null}
        {!isLoading && error && !isNotFound ? (
          <ErrorState message={error.message} onRetry={() => void retry()} />
        ) : null}
        {!isLoading && !error && user ? (
          <>
            <View
              style={[
                styles.profileCard,
                { backgroundColor: colors.card, borderColor: colors.border },
              ]}>
              <View style={styles.profileTopRow}>
                <View style={[styles.avatar, { backgroundColor: colors.neutralSoft }]}>
                  <Text style={[styles.avatarText, { color: colors.tint }]}>{initials}</Text>
                </View>
                <View style={styles.profileIdentity}>
                  <Text style={[styles.userName, { color: colors.text }]}>{user.name}</Text>
                  <Text style={[styles.userCompany, { color: colors.textSecondary }]}>
                    {user.company}
                  </Text>
                </View>
                <StatusBadge
                  label={user.isActive ? 'ACTIVE' : 'INACTIVE'}
                  tone={user.isActive ? 'success' : 'neutral'}
                />
              </View>

              <View style={[styles.profileDetails, { borderTopColor: colors.border }]}>
                <InfoRow label="Company" value={user.company} />
                <InfoRow label="Role" value={user.role} />
                <InfoRow label="Fingerprint ID" value={String(user.fingerprintId)} />
              </View>
            </View>

            <Text style={[styles.sectionTitle, { color: colors.text }]}>Access Permissions</Text>
            <View
              style={[
                styles.listCard,
                { backgroundColor: colors.card, borderColor: colors.border },
              ]}>
              {user.areas.map((area, index) => (
                <View
                  key={area.id}
                  style={[
                    styles.detailRow,
                    index < user.areas.length - 1 && {
                      borderBottomColor: colors.border,
                      borderBottomWidth: 1,
                    },
                  ]}>
                  <Text style={[styles.areaName, { color: colors.text }]}>{area.name}</Text>
                  <View
                    style={[
                      styles.permissionBadge,
                      {
                        backgroundColor: area.allowed ? colors.successSoft : colors.dangerSoft,
                      },
                    ]}>
                    <Text
                      style={[
                        styles.permissionText,
                        { color: area.allowed ? colors.success : colors.danger },
                      ]}>
                      {area.allowed ? '✓ ALLOWED' : '× NOT ALLOWED'}
                    </Text>
                  </View>
                </View>
              ))}
            </View>

            <Text style={[styles.sectionTitle, { color: colors.text }]}>Current Area Status</Text>
            <Text style={[styles.sectionDescription, { color: colors.textSecondary }]}>
              Tracked independently for this user in each area.
            </Text>
            <View
              style={[
                styles.listCard,
                { backgroundColor: colors.card, borderColor: colors.border },
              ]}>
              {user.areas.map((area, index) => (
                <View
                  key={area.id}
                  style={[
                    styles.detailRow,
                    index < user.areas.length - 1 && {
                      borderBottomColor: colors.border,
                      borderBottomWidth: 1,
                    },
                  ]}>
                  <Text style={[styles.areaName, { color: colors.text }]}>{area.name}</Text>
                  <View
                    style={[
                      {
                        backgroundColor: area.isInside
                          ? colors.successSoft
                          : colors.neutralSoft,
                      },
                      styles.areaStatusBadge,
                    ]}>
                    <View
                      style={[
                        styles.statusDot,
                        {
                          backgroundColor: area.isInside ? colors.success : colors.textSecondary,
                        },
                      ]}
                    />
                    <Text
                      style={[
                        styles.areaStatusText,
                        { color: area.isInside ? colors.success : colors.textSecondary },
                      ]}>
                      {area.isInside ? 'INSIDE' : 'OUTSIDE'}
                    </Text>
                  </View>
                </View>
              ))}
            </View>
          </>
        ) : null}
      </ScrollView>
    </SafeAreaView>
  );
}

function InfoRow({ label, value }: { label: string; value: string }) {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];

  return (
    <View style={styles.infoRow}>
      <Text style={[styles.infoLabel, { color: colors.textSecondary }]}>{label}</Text>
      <Text style={[styles.infoValue, { color: colors.text }]}>{value}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  safeArea: {
    flex: 1,
  },
  scrollContent: {
    paddingHorizontal: 18,
    paddingTop: 12,
    paddingBottom: 30,
    gap: 15,
  },
  backButton: {
    minHeight: 38,
    paddingHorizontal: 10,
    borderRadius: 12,
    flexDirection: 'row',
    alignItems: 'center',
    alignSelf: 'flex-start',
    gap: 2,
  },
  backText: {
    fontSize: 15,
    fontWeight: '700',
  },
  pressed: {
    opacity: 0.7,
  },
  profileCard: {
    borderWidth: 1,
    borderRadius: 18,
    padding: 16,
  },
  profileTopRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
  },
  avatar: {
    width: 56,
    height: 56,
    borderRadius: 16,
    alignItems: 'center',
    justifyContent: 'center',
  },
  avatarText: {
    fontSize: 18,
    fontWeight: '800',
  },
  profileIdentity: {
    flex: 1,
  },
  userName: {
    fontSize: 21,
    lineHeight: 26,
    fontWeight: '800',
  },
  userCompany: {
    fontSize: 13,
    marginTop: 3,
  },
  profileDetails: {
    borderTopWidth: 1,
    marginTop: 16,
    paddingTop: 13,
    gap: 10,
  },
  infoRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    gap: 16,
  },
  infoLabel: {
    fontSize: 13,
  },
  infoValue: {
    fontSize: 13,
    fontWeight: '700',
    textAlign: 'right',
  },
  sectionTitle: {
    fontSize: 19,
    fontWeight: '700',
    marginTop: 5,
  },
  sectionDescription: {
    fontSize: 12,
    lineHeight: 17,
    marginTop: -10,
  },
  listCard: {
    borderWidth: 1,
    borderRadius: 16,
    overflow: 'hidden',
    paddingHorizontal: 14,
  },
  detailRow: {
    minHeight: 59,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 10,
  },
  areaName: {
    flex: 1,
    fontSize: 14,
    fontWeight: '600',
  },
  permissionBadge: {
    minHeight: 28,
    minWidth: 98,
    paddingHorizontal: 9,
    borderRadius: 9,
    alignItems: 'center',
    justifyContent: 'center',
  },
  permissionText: {
    fontSize: 10,
    fontWeight: '800',
    letterSpacing: 0.2,
  },
  areaStatusBadge: {
    minHeight: 28,
    minWidth: 83,
    paddingHorizontal: 9,
    borderRadius: 9,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    gap: 6,
  },
  statusDot: {
    width: 7,
    height: 7,
    borderRadius: 4,
  },
  areaStatusText: {
    fontSize: 10,
    fontWeight: '800',
    letterSpacing: 0.3,
  },
  notFoundContainer: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    padding: 24,
    gap: 16,
  },
  notFoundTitle: {
    fontSize: 22,
    fontWeight: '800',
  },
  notFoundMessage: {
    fontSize: 13,
    textAlign: 'center',
  },
});
