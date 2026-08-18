import { StyleSheet, Text, View } from 'react-native';

import { Colors } from '@/constants/theme';
import { useColorScheme } from '@/hooks/use-color-scheme';
import type { StatusTone } from '@/types/smart-office';

type StatusBadgeProps = {
  label: string;
  tone: StatusTone;
};

export function StatusBadge({ label, tone }: StatusBadgeProps) {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];
  const palette = {
    success: { background: colors.successSoft, foreground: colors.success },
    danger: { background: colors.dangerSoft, foreground: colors.danger },
    warning: { background: colors.warningSoft, foreground: colors.warning },
    neutral: { background: colors.neutralSoft, foreground: colors.textSecondary },
  }[tone];

  return (
    <View style={[styles.badge, { backgroundColor: palette.background }]}>
      <View style={[styles.dot, { backgroundColor: palette.foreground }]} />
      <Text style={[styles.label, { color: palette.foreground }]}>{label}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  badge: {
    minHeight: 27,
    paddingHorizontal: 9,
    borderRadius: 999,
    flexDirection: 'row',
    alignItems: 'center',
    alignSelf: 'flex-start',
    gap: 6,
  },
  dot: {
    width: 7,
    height: 7,
    borderRadius: 4,
  },
  label: {
    fontSize: 11,
    fontWeight: '800',
    letterSpacing: 0.3,
  },
});
