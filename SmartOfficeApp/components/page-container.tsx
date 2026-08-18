import { PropsWithChildren, ReactNode } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { Colors } from '@/constants/theme';
import { useColorScheme } from '@/hooks/use-color-scheme';

type PageContainerProps = PropsWithChildren<{
  title: string;
  subtitle: string;
  headerAccessory?: ReactNode;
}>;

export function PageContainer({ title, subtitle, headerAccessory, children }: PageContainerProps) {
  const colorScheme = useColorScheme() ?? 'light';
  const colors = Colors[colorScheme];

  return (
    <SafeAreaView edges={['top']} style={[styles.safeArea, { backgroundColor: colors.background }]}>
      <ScrollView
        showsVerticalScrollIndicator={false}
        contentContainerStyle={styles.scrollContent}>
        <View style={styles.header}>
          <View style={styles.headerText}>
            <Text style={[styles.title, { color: colors.text }]}>{title}</Text>
            <Text style={[styles.subtitle, { color: colors.textSecondary }]}>{subtitle}</Text>
          </View>
          {headerAccessory}
        </View>
        {children}
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  safeArea: {
    flex: 1,
  },
  scrollContent: {
    paddingHorizontal: 18,
    paddingTop: 16,
    paddingBottom: 28,
    gap: 16,
  },
  header: {
    flexDirection: 'row',
    alignItems: 'flex-start',
    justifyContent: 'space-between',
    gap: 12,
    marginBottom: 4,
  },
  headerText: {
    flex: 1,
  },
  title: {
    fontSize: 30,
    lineHeight: 36,
    fontWeight: '800',
    letterSpacing: -0.5,
  },
  subtitle: {
    fontSize: 14,
    lineHeight: 20,
    marginTop: 2,
  },
});
