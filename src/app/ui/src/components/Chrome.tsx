// Window chrome over the 3D view: the top bar (brand + document), the bottom dock (camera
// tools + Convert), and the stats HUD.
import { send } from '../bridge'
import { formatMeta } from '../formats'
import { resetConvert, setState, useStore } from '../store'
import { Icon, Logo } from './Icon'

export function TopBar() {
  const doc = useStore((s) => s.doc)
  const meta = doc ? formatMeta(doc.format) : null
  return (
    <header className="topbar">
      <div className="brand">
        <Logo />
        <span className="brand-name">Bridger</span>
      </div>
      {doc && meta && (
        <div className="doc-chip" title={doc.path}>
          <span className={`edition-dot edition-dot--${doc.edition}`} aria-hidden />
          <span className="doc-name">{doc.name}</span>
          <span className={`chip chip-${meta.edition}`}>{meta.short === 'folder' ? 'Java world' : meta.short}</span>
        </div>
      )}
      <div className="topbar-actions">
        {doc && (
          <button className="btn btn-ghost" onClick={() => send({ type: 'open_dialog', folder: false })} title="Open another file (Ctrl+O)">
            <Icon name="folder" />
            <span>Open</span>
          </button>
        )}
        <button className="icon-btn" onClick={() => setState({ settingsOpen: true })} aria-label="Settings" title="Settings">
          <Icon name="settings" />
        </button>
      </div>
    </header>
  )
}

export function Dock() {
  const doc = useStore((s) => s.doc)
  const map = useStore((s) => s.map)
  const fly = useStore((s) => s.fly)
  const slice = useStore((s) => s.slice)
  const converting = useStore((s) => s.convert.phase === 'running')
  if (!doc) return null
  const toggleMap = () => {
    setState({ map: !map })
    send({ type: 'view', map: !map })
  }
  return (
    <div className="dock-wrap">
    <nav className="dock" aria-label="View tools">
      <div className="dock-group">
        <button className={`dock-btn ${!map && !fly ? 'is-active' : ''}`} onClick={() => map && toggleMap()} aria-pressed={!map && !fly} title="Orbit">
          <Icon name="orbit" />
          <span className="dock-label">Orbit</span>
        </button>
        <button className={`dock-btn ${fly ? 'is-active' : ''}`} onClick={() => send({ type: 'fly', on: true })} aria-pressed={fly} title="Fly (F)">
          <Icon name="fly" />
          <span className="dock-label">Fly</span>
        </button>
        <button className={`dock-btn ${map ? 'is-active' : ''}`} onClick={toggleMap} aria-pressed={map} title="Top-down map (M)">
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
          title="Slice: hide the top half (fine-tune in View)"
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
      <button className="btn btn-primary dock-convert" onClick={() => (converting ? setState((s) => ({ convert: { ...s.convert, open: true } })) : resetConvert(true))}>
        <Icon name="sparkle" />
        {converting ? 'Converting…' : 'Convert'}
        <Icon name="arrow" size={18} />
      </button>
    </nav>
    </div>
  )
}

export function Hud() {
  const stats = useStore((s) => s.stats)
  const doc = useStore((s) => s.doc)
  if (!stats || !doc) return null
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
