import { useEffect, useRef, useState } from 'react'
import { send } from '../bridge'
import { basename } from '../formats'
import { dur, EASE_IN, gsap, SmoothScroll, useGSAP } from '../motion'
import { dismissToast, setState, useStore } from '../store'
import { Icon } from './Icon'
import { TexturesOffer } from './TexturesOffer'

// Mounted while `show`, with an entrance and a faster exit before unmounting.
function useAnimatedPresence(show: boolean, ref: React.RefObject<HTMLElement | null>, from: gsap.TweenVars, exit: gsap.TweenVars) {
  const [mounted, setMounted] = useState(show)
  useEffect(() => {
    if (show) setMounted(true)
    else if (mounted && ref.current) gsap.to(ref.current, { ...exit, duration: dur(0.16), ease: EASE_IN, onComplete: () => setMounted(false) })
    else setMounted(false)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [show])
  useGSAP(
    () => {
      if (mounted && show && ref.current) gsap.from(ref.current, { ...from, duration: dur(0.28) })
    },
    { dependencies: [mounted] },
  )
  return mounted
}

export function DropOverlay() {
  const dropping = useStore((s) => s.dropping)
  const ref = useRef<HTMLDivElement>(null)
  const mounted = useAnimatedPresence(dropping, ref, { autoAlpha: 0 }, { autoAlpha: 0 })
  useGSAP(
    () => {
      if (!mounted) return
      gsap.from('.drop-card', { scale: 0.92, duration: dur(0.35), ease: 'back.out(1.8)' })
      gsap.to('.drop-ring', { scale: 1.06, opacity: 0.4, duration: 0.9, ease: 'sine.inOut', yoyo: true, repeat: -1 })
    },
    { dependencies: [mounted], scope: ref },
  )
  if (!mounted) return null
  return (
    <div className="overlay drop-overlay" ref={ref} aria-live="polite">
      <div className="drop-card">
        <span className="drop-ring" aria-hidden />
        <Icon name="cube" size={36} />
        <div className="drop-title">Drop to open</div>
        <div className="drop-sub">Structures (.schem, .litematic, .nbt, .mcstructure) or worlds</div>
      </div>
    </div>
  )
}

export function LoadingOverlay() {
  const loading = useStore((s) => s.loading)
  const ref = useRef<HTMLDivElement>(null)
  const bar = useRef<HTMLDivElement>(null)
  const mounted = useAnimatedPresence(!!loading, ref, { autoAlpha: 0, y: 12 }, { autoAlpha: 0, y: -8 })
  useEffect(() => {
    if (bar.current && loading) gsap.to(bar.current, { scaleX: loading.fraction, duration: dur(0.4), ease: 'power2.out' })
  }, [loading])
  if (!mounted) return null
  return (
    <div className="loading" ref={ref} role="status" aria-live="polite">
      <div className="loading-name">{loading?.name}</div>
      <div className="loading-step">{loading?.step}…</div>
      <div className="progress">
        <div className="progress-fill" ref={bar} />
      </div>
    </div>
  )
}

export function FlyHint() {
  const fly = useStore((s) => s.fly)
  const ref = useRef<HTMLDivElement>(null)
  const mounted = useAnimatedPresence(fly, ref, { autoAlpha: 0, y: -16 }, { autoAlpha: 0, y: -10 })
  if (!mounted) return null
  const keys: [string, string][] = [
    ['W A S D', 'move'],
    ['Space · Shift', 'up · down'],
    ['Ctrl', 'sprint'],
    ['Wheel', 'speed'],
    ['Esc', 'back'],
  ]
  return (
    <div className="fly-hint" ref={ref} role="status">
      <span className="fly-badge">
        <Icon name="fly" size={16} /> Flying
      </span>
      {keys.map(([k, v]) => (
        <span key={k} className="fly-key">
          <kbd>{k}</kbd> {v}
        </span>
      ))}
    </div>
  )
}

export function Toasts() {
  const toasts = useStore((s) => s.toasts)
  const root = useRef<HTMLDivElement>(null)
  const seen = useRef(new Set<number>())
  useGSAP(
    () => {
      for (const t of toasts) {
        if (seen.current.has(t.id)) continue
        seen.current.add(t.id)
        gsap.from(`[data-toast="${t.id}"]`, { y: 16, autoAlpha: 0, duration: dur(0.3), ease: 'back.out(1.6)' })
      }
    },
    { dependencies: [toasts], scope: root },
  )
  return (
    <div className="toasts" ref={root} aria-live="polite">
      {toasts.map((t) => (
        <div key={t.id} data-toast={t.id} className={`toast toast-${t.kind}`} role={t.kind === 'error' ? 'alert' : 'status'}>
          <Icon name={t.kind === 'error' ? 'alert' : t.kind === 'success' ? 'check' : 'info'} size={18} />
          <span>{t.message}</span>
          <button className="icon-btn icon-btn--sm" onClick={() => dismissToast(t.id)} aria-label="Dismiss">
            <Icon name="close" size={14} />
          </button>
        </div>
      ))}
    </div>
  )
}

export function SettingsDialog() {
  const open = useStore((s) => s.settingsOpen)
  const settings = useStore((s) => s.settings)
  const assets = useStore((s) => s.assets)
  const version = useStore((s) => s.version)
  const gpu = useStore((s) => s.gpu)
  const update = useStore((s) => s.update)
  const ref = useRef<HTMLDivElement>(null)
  const card = useRef<HTMLDivElement>(null)
  const mounted = useAnimatedPresence(open, ref, { autoAlpha: 0 }, { autoAlpha: 0 })
  useGSAP(
    () => {
      if (mounted && card.current) {
        gsap.from(card.current, { y: 20, scale: 0.97, duration: dur(0.35), ease: 'power3.out' })
        card.current.querySelector<HTMLElement>('button, input')?.focus()
      }
    },
    { dependencies: [mounted] },
  )
  if (!mounted || !settings) return null
  const close = () => setState({ settingsOpen: false })
  const setPacks = (packs: string[]) => send({ type: 'settings', packs })
  const move = (i: number, d: number) => {
    const p = [...settings.packs]
    const j = i + d
    if (j < 0 || j >= p.length) return
    ;[p[i], p[j]] = [p[j], p[i]]
    setPacks(p)
  }
  // Focus trap: Tab cycles inside the dialog.
  const onKey = (e: React.KeyboardEvent) => {
    if (e.key !== 'Tab' || !card.current) return
    const items = [...card.current.querySelectorAll<HTMLElement>('button:not(:disabled), input')]
    if (items.length === 0) return
    const first = items[0]
    const last = items[items.length - 1]
    if (e.shiftKey && document.activeElement === first) {
      e.preventDefault()
      last.focus()
    } else if (!e.shiftKey && document.activeElement === last) {
      e.preventDefault()
      first.focus()
    }
  }
  return (
    <div className="overlay modal-backdrop" ref={ref} onMouseDown={(e) => e.target === e.currentTarget && close()} onKeyDown={onKey}>
      <div className="modal" ref={card} role="dialog" aria-modal aria-labelledby="settings-title">
        <header className="modal-header">
          <h2 id="settings-title">Settings</h2>
          <button className="icon-btn" onClick={close} aria-label="Close">
            <Icon name="close" />
          </button>
        </header>
        <SmoothScroll className="modal-scroll">
          <section className="modal-section">
            <h3>Textures</h3>
            <p className="muted small">
              The 3D preview draws blocks with the textures of your own Minecraft Java {assets?.java_version} installation. Bridger never
              ships Mojang's assets.
            </p>
            <div className={`asset-status ${assets?.ok ? 'is-ok' : 'is-missing'}`}>
              <Icon name={assets?.ok ? 'check' : 'alert'} size={18} />
              <div>
                <div className="asset-title">{assets?.ok ? 'Textures loaded' : 'Textures not found'}</div>
                <div className="asset-path mono">{assets?.ok ? assets.jar : assets?.error}</div>
                {assets?.ok && assets.downloaded && <div className="asset-path">Downloaded from Mojang by Bridger.</div>}
              </div>
            </div>
            <TexturesOffer />
            <div className="row">
              <button className="btn btn-secondary" onClick={() => send({ type: 'pick_client_jar' })}>
                Choose a client jar…
              </button>
              {settings.client_jar && (
                <button className="btn btn-ghost" onClick={() => send({ type: 'settings', client_jar: '' })}>
                  Use the launcher's
                </button>
              )}
            </div>
          </section>
          <section className="modal-section">
            <h3>Resource packs</h3>
            <p className="muted small">Applied on top of the game's textures. The last one wins.</p>
            {settings.packs.length === 0 ? (
              <p className="empty-line">No resource packs.</p>
            ) : (
              <ol className="pack-list">
                {settings.packs.map((p, i) => (
                  <li key={p} className="pack">
                    <span className="pack-name" title={p}>
                      {basename(p)}
                    </span>
                    <button className="icon-btn icon-btn--sm" onClick={() => move(i, -1)} disabled={i === 0} aria-label="Move up">
                      <Icon name="up" size={14} />
                    </button>
                    <button className="icon-btn icon-btn--sm" onClick={() => move(i, 1)} disabled={i === settings.packs.length - 1} aria-label="Move down">
                      <Icon name="down" size={14} />
                    </button>
                    <button className="icon-btn icon-btn--sm" onClick={() => setPacks(settings.packs.filter((x) => x !== p))} aria-label="Remove">
                      <Icon name="trash" size={14} />
                    </button>
                  </li>
                ))}
              </ol>
            )}
            <button className="btn btn-secondary" onClick={() => send({ type: 'add_pack' })}>
              <Icon name="plus" size={16} />
              Add a resource pack…
            </button>
          </section>
          <section className="modal-section">
            <h3>Updates</h3>
            <label className="check">
              <input type="checkbox" checked={settings.check_updates} onChange={(e) => send({ type: 'settings', check_updates: e.target.checked })} />
              <span>Check for a new version once a day</span>
            </label>
            <p className="muted small">
              Bridger asks GitHub for the latest release; it downloads nothing until you agree, and checks the download's SHA-256.
            </p>
            <div className="textures-row">
              <span className="muted small" aria-live="polite">
                {update.phase === 'none' && `Bridger ${update.current} is the latest version.`}
                {update.phase === 'checking' && 'Checking…'}
                {['available', 'running', 'ready'].includes(update.phase) && `Bridger ${update.version} is available.`}
                {update.phase === 'failed' && update.error}
              </span>
              <button
                className="btn btn-ghost btn-sm"
                disabled={update.phase === 'checking' || update.phase === 'running'}
                onClick={() => {
                  setState({ update: { ...update, phase: 'checking', dismissed: false } })
                  send({ type: 'update_check' })
                }}
              >
                Check now
              </button>
            </div>
          </section>
          <section className="modal-section about">
            <h3>About</h3>
            <dl className="about-grid mono">
              <dt>Bridger</dt>
              <dd>{version}</dd>
              <dt>Java</dt>
              <dd>{assets?.java_version}</dd>
              <dt>Bedrock</dt>
              <dd>{assets?.bedrock_version}</dd>
              <dt>GPU</dt>
              <dd>{gpu}</dd>
            </dl>
          </section>
        </SmoothScroll>
      </div>
    </div>
  )
}
