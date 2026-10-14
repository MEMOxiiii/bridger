// 20 x 20 stroke icons (1.7px, round joins), drawn for this app. Decorative unless given a label.

// A gear outline built from geometry (8 teeth), so it is symmetric at any size.
function gearPath() {
  const teeth = 8
  const cx = 10
  const cy = 10
  const outer = 8.2
  const inner = 6.3
  const pts: string[] = []
  for (let i = 0; i < teeth; ++i) {
    const a = (i / teeth) * Math.PI * 2
    const half = (Math.PI / teeth) * 0.42 // tooth half-width
    const bevel = (Math.PI / teeth) * 0.62
    const at = (r: number, t: number) => `${(cx + r * Math.cos(t)).toFixed(2)} ${(cy + r * Math.sin(t)).toFixed(2)}`
    pts.push(at(inner, a - bevel), at(outer, a - half), at(outer, a + half), at(inner, a + bevel))
  }
  return `M${pts.join('L')}Z M12.7 10a2.7 2.7 0 1 1-5.4 0 2.7 2.7 0 1 1 5.4 0Z`
}

const paths: Record<string, string> = {
  folder: 'M2.5 6.2a1.7 1.7 0 0 1 1.7-1.7h3.2l1.9 1.9h6.5a1.7 1.7 0 0 1 1.7 1.7v6.7a1.7 1.7 0 0 1-1.7 1.7H4.2a1.7 1.7 0 0 1-1.7-1.7z',
  file: 'M5.5 2.5h5.8L15 6.2v10a1.3 1.3 0 0 1-1.3 1.3H5.5a1.3 1.3 0 0 1-1.3-1.3V3.8a1.3 1.3 0 0 1 1.3-1.3zM11.2 2.5v3.8H15',
  cube: 'M10 2.5l6.5 3.75v7.5L10 17.5l-6.5-3.75v-7.5zM10 10l6.5-3.75M10 10v7.5M10 10L3.5 6.25',
  orbit: 'M10 12.6a2.6 2.6 0 1 0 0-5.2 2.6 2.6 0 0 0 0 5.2zM17.5 10c0 1.9-3.4 3.4-7.5 3.4S2.5 11.9 2.5 10 5.9 6.6 10 6.6s7.5 1.5 7.5 3.4z',
  fly: 'M3 11.5l14-6-4.6 12-2.6-4.8zM9.8 12.7l7.2-7.2',
  map: 'M2.5 5l5-2 5 2 5-2v12l-5 2-5-2-5 2zM7.5 3v12M12.5 5v12',
  layers: 'M10 2.5l7.5 4-7.5 4-7.5-4zM2.5 10l7.5 4 7.5-4M2.5 13.5l7.5 4 7.5-4',
  frame: 'M3 7V4a1 1 0 0 1 1-1h3M13 3h3a1 1 0 0 1 1 1v3M17 13v3a1 1 0 0 1-1 1h-3M7 17H4a1 1 0 0 1-1-1v-3',
  settings: gearPath(),
  arrow: 'M4 10h12M11 5l5 5-5 5',
  back: 'M16 10H4M9 5l-5 5 5 5',
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
  globe: 'M10 17.5a7.5 7.5 0 1 0 0-15 7.5 7.5 0 0 0 0 15zM2.5 10h15M10 2.5c2 2.2 3 4.7 3 7.5s-1 5.3-3 7.5c-2-2.2-3-4.7-3-7.5s1-5.3 3-7.5z',
  stop: 'M6 6h8v8H6z',
  eye: 'M1.8 10S4.8 4.5 10 4.5 18.2 10 18.2 10 15.2 15.5 10 15.5 1.8 10 1.8 10zM10 12.5a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5z',
  swap: 'M4 7h11l-3-3M16 13H5l3 3',
  upload: 'M10 13V3M6 7l4-4 4 4M3.5 13v2.5a1 1 0 0 0 1 1h11a1 1 0 0 0 1-1V13',
  download: 'M10 3v10M6 9l4 4 4-4M3.5 13v2.5a1 1 0 0 0 1 1h11a1 1 0 0 0 1-1V13',
  clock: 'M10 17.5a7.5 7.5 0 1 0 0-15 7.5 7.5 0 0 0 0 15zM10 6v4l2.5 2',
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
      strokeWidth={1.7}
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
