import { ActivityIndicator, Pressable, StyleSheet, Text, View } from 'react-native';

import { Colors } from '@/constants/theme';
import { useColorScheme } from '@/hooks/use-color-scheme';

export function LoadingState({ message = 'Loading backend data…' }: { message?: string }) {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];

  return (
    <View style={[styles.card, { backgroundColor: colors.card, borderColor: colors.border }]}>
      <ActivityIndicator color={colors.tint} />
      <Text style={[styles.message, { color: colors.textSecondary }]}>{message}</Text>
    </View>
  );
}

export function ErrorState({ message, onRetry }: { message: string; onRetry: () => void }) {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];

  return (
    <View style={[styles.card, { backgroundColor: colors.card, borderColor: colors.danger }]}>
      <Text style={[styles.errorTitle, { color: colors.danger }]}>Unable to load data</Text>
      <Text style={[styles.message, { color: colors.textSecondary }]}>{message}</Text>
      <Pressable
        accessibilityRole="button"
        accessibilityLabel="Retry loading data"
        onPress={onRetry}
        style={({ pressed }) => [
          styles.retryButton,
          { backgroundColor: colors.tint },
          pressed && styles.pressed,
        ]}>
        <Text style={styles.retryText}>Retry</Text>
      </Pressable>
    </View>
  );
}

const styles = StyleSheet.create({
  card: {
    minHeight: 150,
    borderWidth: 1,
    borderRadius: 16,
    padding: 20,
    alignItems: 'center',
    justifyContent: 'center',
    gap: 12,
  },
  errorTitle: {
    fontSize: 16,
    fontWeight: '800',
  },
  message: {
    fontSize: 13,
    lineHeight: 19,
    textAlign: 'center',
  },
  retryButton: {
    minHeight: 40,
    paddingHorizontal: 20,
    borderRadius: 11,
    alignItems: 'center',
    justifyContent: 'center',
  },
  retryText: {
    color: '#FFFFFF',
    fontSize: 13,
    fontWeight: '800',
  },
  pressed: {
    opacity: 0.75,
  },
});
