import { Platform } from 'react-native';

export const Colors = {
  light: {
    text: '#172033',
    textSecondary: '#667085',
    background: '#F4F7FB',
    card: '#FFFFFF',
    border: '#E4EAF2',
    tint: '#246BFD',
    icon: '#667085',
    tabIconDefault: '#8B95A7',
    tabIconSelected: '#246BFD',
    success: '#168A50',
    successSoft: '#E7F7EF',
    danger: '#C43636',
    dangerSoft: '#FDECEC',
    warning: '#A15C00',
    warningSoft: '#FFF2DA',
    neutralSoft: '#EEF2F7',
  },
  dark: {
    text: '#F2F5FA',
    textSecondary: '#A5B0C1',
    background: '#0F1621',
    card: '#182231',
    border: '#29364A',
    tint: '#77A7FF',
    icon: '#A5B0C1',
    tabIconDefault: '#7F8BA0',
    tabIconSelected: '#77A7FF',
    success: '#56D391',
    successSoft: '#173B2B',
    danger: '#FF8585',
    dangerSoft: '#472529',
    warning: '#F6BC62',
    warningSoft: '#49371F',
    neutralSoft: '#263244',
  },
};

export const Fonts = Platform.select({
  ios: {
    /** iOS `UIFontDescriptorSystemDesignDefault` */
    sans: 'system-ui',
    /** iOS `UIFontDescriptorSystemDesignSerif` */
    serif: 'ui-serif',
    /** iOS `UIFontDescriptorSystemDesignRounded` */
    rounded: 'ui-rounded',
    /** iOS `UIFontDescriptorSystemDesignMonospaced` */
    mono: 'ui-monospace',
  },
  default: {
    sans: 'normal',
    serif: 'serif',
    rounded: 'normal',
    mono: 'monospace',
  },
  web: {
    sans: "system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif",
    serif: "Georgia, 'Times New Roman', serif",
    rounded: "'SF Pro Rounded', 'Hiragino Maru Gothic ProN', Meiryo, 'MS PGothic', sans-serif",
    mono: "SFMono-Regular, Menlo, Monaco, Consolas, 'Liberation Mono', 'Courier New', monospace",
  },
});
