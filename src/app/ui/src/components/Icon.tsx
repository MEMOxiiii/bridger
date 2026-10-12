// 20x20 stroke icons (1.6px, round caps). Decorative by default (aria-hidden).
const paths: Record<string, string> = {
  folder: 'M2.5 6.5a2 2 0 0 1 2-2h3l2 2h6a2 2 0 0 1 2 2v6a2 2 0 0 1-2 2h-11a2 2 0 0 1-2-2z',
  file: 'M5.5 2.5h6l3.5 3.5v10a1.5 1.5 0 0 1-1.5 1.5h-8A1.5 1.5 0 0 1 4 16V4a1.5 1.5 0 0 1 1.5-1.5zM11.5 2.5V6H15',
  cube: 'M10 2.5l6.5 3.75v7.5L10 17.5l-6.5-3.75v-7.5zM10 10l6.5-3.75M10 10v7.5M10 10L3.5 6.25',
  orbit: 'M10 13.5a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7zM2.5 10c0-2 3.4-3.6 7.5-3.6s7.5 1.6 7.5 3.6-3.4 3.6-7.5 3.6M6.5 12.9L5 14.6',
  fly: 'M10 3l2 4.5 4.5.5-3.4 3 1 4.5L10 13.2 5.9 15.5l1-4.5L3.5 8l4.5-.5z',
  map: 'M2.5 5l5-2 5 2 5-2v12l-5 2-5-2-5 2zM7.5 3v12M12.5 5v12',
  layers: 'M10 2.5l7.5 4-7.5 4-7.5-4zM2.5 10l7.5 4 7.5-4M2.5 13.5l7.5 4 7.5-4',
  frame: 'M3 7V4a1 1 0 0 1 1-1h3M13 3h3a1 1 0 0 1 1 1v3M17 13v3a1 1 0 0 1-1 1h-3M7 17H4a1 1 0 0 1-1-1v-3M10 7.5v5M7.5 10h5',
  settings:
    'M10 12.5a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5zM16.2 12.3l1.1.9-1.6 2.8-1.4-.5a6 6 0 0 1-1.6.9L12.5 18h-3.2l-.3-1.6a6 6 0 0 1-1.6-.9l-1.4.5-1.6-2.8 1.1-.9a6 6 0 0 1 0-1.9l-1.1-.9 1.6-2.8 1.4.5a6 6 0 0 1 1.6-.9L9.3 2h3.2l.2 1.6a6 6 0 0 1 1.6.9l1.4-.5 1.6 2.8-1.1.9a6 6 0 0 1 0 1.9z',
  arrow: 'M4 10h12M11 5l5 5-5 5',
  close: 'M5 5l10 10M15 5L5 15',
  check: 'M4 10.5l4 4 8-9',
  alert: 'M10 3l7.5 13.5h-15zM10 8v3.5M10 14v.01',
  info: 'M10 17.5a7.5 7.5 0 1 0 0-15 7.5 7.5 0 0 0 0 15zM10 9v5M10 6v.01',
  reveal: 'M11 3h6v6M17 3l-7 7M14.5 11.5V16a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V6.5a1 1 0 0 1 1-1h4.5',
  plus: 'M10 4v12M4 10h12',
  trash: 'M4 6h12M8 6V4h4v2M5.5 6l.7 10a1 1 0 0 0 1 1h5.6a1 1 0 0 0 1-1l.7-10',
  up: 'M10 15V5M5.5 9.5L10 5l4.5 4.5',
  down: 'M10 5v10M5.5 10.5L10 15l4.5-4.5',
  chevron: 'M8 5l5 5-5 5',
  sparkle: 'M10 2.5l1.6 4.4 4.4 1.6-4.4 1.6L10 14.5l-1.6-4.4L4 8.5l4.4-1.6zM15.5 13l.7 1.8 1.8.7-1.8.7-.7 1.8-.7-1.8-1.8-.7 1.8-.7z',
  globe: 'M10 17.5a7.5 7.5 0 1 0 0-15 7.5 7.5 0 0 0 0 15zM2.5 10h15M10 2.5c2 2.2 3 4.7 3 7.5s-1 5.3-3 7.5c-2-2.2-3-4.7-3-7.5s1-5.3 3-7.5z',
  stop: 'M6 6h8v8H6z',
  keyboard: 'M2.5 5.5h15v9h-15zM5.5 8.5h.01M8.5 8.5h.01M11.5 8.5h.01M14.5 8.5h.01M6.5 11.5h7',
}

export type IconName = keyof typeof paths

export function Icon({ name, size = 20, label }: { name: IconName; size?: number; label?: string }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 20 20"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.6}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden={label ? undefined : true}
      role={label ? 'img' : undefined}
      aria-label={label}
    >
      <path d={paths[name]} />
    </svg>
  )
}

// The Bridger mark: two blocks, Java amber and Bedrock teal, joined by a bridge.
export function Logo({ size = 28 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 32 32" aria-hidden>
      <defs>
        <linearGradient id="brg" x1="0" x2="1" y1="0" y2="0">
          <stop offset="0" stopColor="oklch(80% 0.14 70)" />
          <stop offset="1" stopColor="oklch(78% 0.11 182)" />
        </linearGradient>
      </defs>
      <path d="M3 13l6-3.5 6 3.5v7l-6 3.5-6-3.5z" fill="oklch(80% 0.14 70)" />
      <path d="M3 13l6 3.5 6-3.5-6-3.5z" fill="oklch(88% 0.1 75)" />
      <path d="M17 13l6-3.5 6 3.5v7l-6 3.5-6-3.5z" fill="oklch(78% 0.11 182)" />
      <path d="M17 13l6 3.5 6-3.5-6-3.5z" fill="oklch(87% 0.08 182)" />
      <path d="M9 9.5c2-3.2 4.3-4.8 7-4.8s5 1.6 7 4.8" fill="none" stroke="url(#brg)" strokeWidth="2.2" strokeLinecap="round" />
    </svg>
  )
}
