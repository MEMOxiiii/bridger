// Chrome around the pages: the top bar (all screens), and in the 3D preview the camera dock and the
// stats HUD.
import { useEffect, useRef, useState } from 'react'
import { send } from '../bridge'
import { exitPreview, openFile, openFolder } from '../actions'
import { formatMeta } from '../formats'
import { setState, useStore } from '../store'
import { Wordmark } from './Brand'
import { Icon } from './Icon'

// "Open another": a file or a Java world folder (a world is a folder, so it needs its own dialog).
function OpenMenu() {
  const [open, setOpen] = useState(false)
  const root = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!open) return
    const close = (e: MouseEvent | KeyboardEvent) => {
      if (e instanceof KeyboardEvent ? e.key === 'Escape' : !root.current?.contains(e.target as Node)) setOpen(false)
    }
    window.addEventListener('mousedown', close)
    window.addEventListener('keydown', close)
    return () => {
      window.removeEventListener('mousedown', close)
      window.removeEventListener('keydown', close)
    }
  }, [open])
  const pick = (f: () => void) => {
    setOpen(false)
    f()
  }
  return (
    <div className="open-menu" ref={root}>
      <button className="btn btn-secondary btn-sm" onClick={() => setOpen((o) => !o)} aria-haspopup="menu" aria-expanded={open}>
        <Icon name="upload" size={18} />
        Open another
        <span className={`open-menu-caret ${open ? 'is-open' : ''}`} aria-hidden>
          <Icon name="chevron" size={14} />
        </span>
      </button>
      {open && (
        <div className="open-menu-list" role="menu">
          <button role="menuitem" onClick={() => pick(openFile)}>
            <Icon name="file" size={18} />
            <span>
              <span className="open-menu-title">A file</span>
              <span className="open-menu-sub">.schem, .litematic, .nbt, .schematic, .mcstructure, .mcworld</span>
            </span>
            <kbd>Ctrl O</kbd>
          </button>
          <button role="menuitem" onClick={() => pick(openFolder)}>
            <Icon name="folder" size={18} />
            <span>
              <span className="open-menu-title">A Java world folder</span>
              <span className="open-menu-sub">The world's folder in .minecraft/saves</span>
            </span>
            <kbd>Ctrl Shift O</kbd>
          </button>
        </div>
      )}
    </div>
  )
}

export function TopBar() {
  const doc = useStore((s) => s.doc)
  const view = useStore((s) => s.view)
  const converting = useStore((s) => s.convert.phase === 'running')
  const preview = !!doc && view === 'preview'
  return (
    <header className={`topbar ${preview ? 'topbar--preview' : ''}`}>
      {preview ? (
        <button className="btn btn-primary btn-sm back-btn" onClick={exitPreview} title="Back to converting (Esc)">
          <Icon name="back" size={18} />
          {converting ? 'Back — converting…' : 'Back to convert'}
        </button>
      ) : (
        <div className="topbar-brand">
          <Wordmark height={26} />
        </div>
      )}
      {doc && (
        <div className="crumb" title={doc.path}>
          <span className={`edition-dot edition-dot--${doc.edition}`} aria-hidden />
          <span className="crumb-name">{doc.name}</span>
          <span className="crumb-format mono">{formatMeta(doc.format).short === 'folder' ? 'world' : formatMeta(doc.format).short}</span>
        </div>
      )}
      <div className="topbar-actions">
        {doc && !preview && (
          <OpenMenu />
        )}
        <button className="icon-btn" onClick={() => setState({ settingsOpen: true })} aria-label="Settings" title="Settings">
          <Icon name="settings" />
        </button>
      </div>
    </header>
  )
}

export function Dock() {
  const map = useStore((s) => s.map)
  const fly = useStore((s) => s.fly)
  const slice = useStore((s) => s.slice)
  const doc = useStore((s) => s.doc)
  if (!doc) return null
  const setMap = (on: boolean) => {
    setState({ map: on })
    send({ type: 'view', map: on })
  }
  return (
    <div className="dock-wrap">
      <nav className="dock" aria-label="View tools">
        <div className="dock-group">
          <button className={`dock-btn ${!map && !fly ? 'is-active' : ''}`} onClick={() => setMap(false)} aria-pressed={!map && !fly} title="Orbit">
            <Icon name="orbit" />
            <span className="dock-label">Orbit</span>
          </button>
          <button className={`dock-btn ${fly ? 'is-active' : ''}`} onClick={() => send({ type: 'fly', on: true })} aria-pressed={fly} title="Fly (F)">
            <Icon name="fly" />
            <span className="dock-label">Fly</span>
          </button>
          <button className={`dock-btn ${map ? 'is-active' : ''}`} onClick={() => setMap(!map)} aria-pressed={map} title="Top-down map (M)">
            <Icon name="map" />
            <span className="dock-label">Map</span>
          </button>
        </div>
        <span className="dock-sep" aria-hidden />
        <div className="dock-group">
          <button
            className={`dock-btn ${slice !== null ? 'is-active' : ''}`}
            onClick={() => {
              const next = slice === null ? (doc.size ? Math.ceil(doc.size[1] / 2) : 64) : null
              setState({ slice: next })
              send({ type: 'view', slice: next })
            }}
            aria-pressed={slice !== null}
            title="Slice: hide the top (fine-tune in View)"
          >
            <Icon name="layers" />
            <span className="dock-label">Slice</span>
          </button>
          <button className="dock-btn" onClick={() => send({ type: 'reset_camera' })} title="Reset view (R)">
            <Icon name="frame" />
            <span className="dock-label">Frame</span>
          </button>
        </div>
        <span className="dock-sep" aria-hidden />
        <button className="btn btn-primary dock-convert" onClick={exitPreview}>
          <Icon name="swap" size={18} />
          Convert
        </button>
      </nav>
    </div>
  )
}

export function Hud() {
  const stats = useStore((s) => s.stats)
  const doc = useStore((s) => s.doc)
  const view = useStore((s) => s.view)
  if (!stats || !doc || view !== 'preview') return null
  const loading = stats.chunks_total !== undefined && (stats.chunks_pending ?? 0) > 0
  return (
    <div className="hud mono" aria-hidden>
      <span>
        <b>{Math.round(stats.fps)}</b> fps
      </span>
      <span>{stats.quads >= 1e6 ? `${(stats.quads / 1e6).toFixed(1)}M` : stats.quads >= 1e4 ? `${Math.round(stats.quads / 1000)}k` : stats.quads.toLocaleString()} quads</span>
      {stats.chunks_total !== undefined && (
        <span className={loading ? 'hud-loading' : undefined}>
          {stats.chunks_loaded?.toLocaleString()} chunks{loading ? ' …' : ''}
        </span>
      )}
    </div>
  )
}
