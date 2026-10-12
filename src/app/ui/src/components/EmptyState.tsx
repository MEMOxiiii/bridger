import { useRef } from 'react'
import { send } from '../bridge'
import { basename, dirname, formatMeta } from '../formats'
import { dur, EASE_OUT, gsap, useGSAP } from '../motion'
import { useStore } from '../store'
import { Icon } from './Icon'

const INPUTS = ['schem', 'litematic', 'nbt', 'schematic', 'mcstructure', 'mcworld', 'java-world']

// Isometric hero: a Java tower (amber) and a Bedrock tower (teal) joined by a bridge of blocks.
// Grid -> screen: x right-down, z left-down, y up; painted back to front.
type Hue = 'java' | 'bedrock' | 'mid'
const SHADES: Record<Hue, [string, string, string]> = {
  java: ['oklch(87% 0.12 74)', 'oklch(76% 0.14 66)', 'oklch(64% 0.13 58)'],
  bedrock: ['oklch(87% 0.08 182)', 'oklch(72% 0.11 184)', 'oklch(60% 0.1 188)'],
  mid: ['oklch(88% 0.09 120)', 'oklch(75% 0.11 122)', 'oklch(63% 0.1 125)'],
}
const CUBES: { x: number; y: number; z: number; hue: Hue; part: string }[] = (() => {
  const out: { x: number; y: number; z: number; hue: Hue; part: string }[] = []
  // Towers: 2 x 2 footprints, 3 high. The bridge runs along x = -z, which is horizontal on screen.
  for (const [x0, z0, hue, part] of [
    [0, 0, 'java', 'hero-stack--java'],
    [5, -5, 'bedrock', 'hero-stack--bedrock'],
  ] as const)
    for (let y = 0; y < 3; ++y) for (let x = 0; x < 2; ++x) for (let z = 0; z < 2; ++z) out.push({ x: x0 + x, y, z: z0 + z, hue, part })
  for (let i = 1; i <= 3; ++i) out.push({ x: 1 + i, y: 2 + (i === 2 ? 0.3 : 0.12), z: -i, hue: 'mid', part: 'hero-bridge' })
  return out.sort((p, q) => p.x + p.z - (q.x + q.z) || p.y - q.y)
})()

function HeroArt() {
  const W = 22
  const H = 12.5
  return (
    <svg className="hero-art" viewBox="-70 -96 360 180" aria-hidden>
      <ellipse cx="110" cy="64" rx="160" ry="16" fill="oklch(0% 0 0 / 0.32)" />
      {CUBES.map((c, i) => {
        const sx = (c.x - c.z) * W
        const sy = (c.x + c.z) * H - c.y * 2 * H
        const [top, left, right] = SHADES[c.hue]
        return (
          <g key={i} className={`hero-cube ${c.part}`} transform={`translate(${sx} ${sy})`}>
            <path d={`M0 ${-H} L${W} 0 L0 ${H} L${-W} 0 Z`} fill={top} />
            <path d={`M${-W} 0 L0 ${H} L0 ${3 * H} L${-W} ${2 * H} Z`} fill={left} />
            <path d={`M${W} 0 L0 ${H} L0 ${3 * H} L${W} ${2 * H} Z`} fill={right} />
          </g>
        )
      })}
    </svg>
  )
}

export function EmptyState() {
  const root = useRef<HTMLDivElement>(null)
  const recent = useStore((s) => s.settings?.recent)
  const assets = useStore((s) => s.assets)

  useGSAP(
    () => {
      const mm = gsap.matchMedia()
      mm.add({ reduce: '(prefers-reduced-motion: reduce)', full: '(prefers-reduced-motion: no-preference)' }, (ctx) => {
        const reduce = ctx.conditions?.reduce
        const tl = gsap.timeline({ defaults: { ease: EASE_OUT, duration: dur(0.6) } })
        if (reduce) {
          tl.from('.empty > *', { autoAlpha: 0, duration: 0.2 })
          return
        }
        tl.from('.hero-stack--java', { y: '-=40', autoAlpha: 0, stagger: 0.04, ease: 'back.out(1.6)' })
          .from('.hero-stack--bedrock', { y: '-=40', autoAlpha: 0, stagger: 0.04, ease: 'back.out(1.6)' }, '<0.1')
          .from('.hero-bridge', { y: '-=16', autoAlpha: 0, stagger: 0.07, ease: 'back.out(2)' }, '-=0.2')
          .from('.empty-copy > *', { y: 16, autoAlpha: 0, stagger: 0.06 }, '-=0.45')
          .from('.empty-actions > *, .empty-formats, .empty-recent', { y: 12, autoAlpha: 0, stagger: 0.05 }, '-=0.4')
        // Idle: the bridge breathes gently.
        gsap.to('.hero-bridge', { y: '-=4', duration: 2.4, ease: 'sine.inOut', yoyo: true, repeat: -1, delay: 1.6 })
      })
    },
    { scope: root },
  )

  return (
    <div className="empty" ref={root}>
      <HeroArt />
      <div className="empty-copy">
        <h1>
          Bring your builds <span className="gradient-text">across editions.</span>
        </h1>
        <p>
          Drop a Java or Bedrock structure or world anywhere on this window to see it in 3D, check what converts exactly, and save it for
          the other edition.
        </p>
      </div>
      <div className="empty-actions">
        <button className="btn btn-primary btn-lg" onClick={() => send({ type: 'open_dialog', folder: false })}>
          <Icon name="file" />
          Open a file
          <kbd>Ctrl O</kbd>
        </button>
        <button className="btn btn-secondary btn-lg" onClick={() => send({ type: 'open_dialog', folder: true })}>
          <Icon name="folder" />
          Open a Java world folder
        </button>
      </div>
      <ul className="empty-formats" aria-label="Supported formats">
        {INPUTS.map((id) => {
          const m = formatMeta(id)
          return (
            <li key={id} className={`chip chip-${m.edition}`}>
              {m.short === 'folder' ? 'Java world' : m.short}
            </li>
          )
        })}
      </ul>
      {recent && recent.length > 0 && (
        <div className="empty-recent">
          <div className="overline">Recent</div>
          <ul>
            {recent.map((path) => (
              <li key={path}>
                <button className="recent-item" onClick={() => send({ type: 'open', path })} title={path}>
                  <Icon name={/\.[a-z]+$/i.test(path) ? 'file' : 'folder'} size={16} />
                  <span className="recent-name">{basename(path)}</span>
                  <span className="recent-dir">{dirname(path)}</span>
                </button>
              </li>
            ))}
          </ul>
        </div>
      )}
      {assets && !assets.ok && (
        <p className="empty-note">
          <Icon name="info" size={16} />
          Converting works without it, but the 3D preview uses the textures of Minecraft Java {assets.java_version}. Install it with the
          launcher, or pick its client jar in Settings.
        </p>
      )}
    </div>
  )
}
