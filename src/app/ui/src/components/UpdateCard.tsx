// A new Bridger release: offered in a card at the bottom right, downloaded only when the user agrees, then installed
// by restarting (or when Bridger closes). docs/app/updates.md.
import { useEffect, useRef } from 'react'
import { send } from '../bridge'
import { dur, gsap } from '../motion'
import { setState, useStore } from '../store'
import { Icon } from './Icon'

function megabytes(bytes: number) {
  return bytes > 0 ? `${(bytes / (1024 * 1024)).toFixed(0)} MB` : ''
}

// The first lines of the release notes (without headings and list marks), for a short "what's new".
function summary(notes: string) {
  return notes
    .split('\n')
    .filter((l) => !/^\s*#/.test(l)) // section headings ("### Added")
    .map((l) => l.replace(/^[-*]\s+/, '').trim())
    .filter((l) => l.length > 0 && !/^\[.*\]:/.test(l))
    .slice(0, 4)
}

export function UpdateCard() {
  const u = useStore((s) => s.update)
  const converting = useStore((s) => s.convert.phase === 'running')
  const bar = useRef<HTMLDivElement>(null)
  const card = useRef<HTMLDivElement>(null)
  const visible = !u.dismissed && ['available', 'running', 'ready', 'failed'].includes(u.phase) && u.version !== ''
  useEffect(() => {
    if (bar.current) gsap.to(bar.current, { scaleX: u.fraction, duration: dur(0.3), ease: 'power2.out' })
  }, [u.fraction])
  useEffect(() => {
    if (visible && card.current) gsap.from(card.current, { y: 24, autoAlpha: 0, duration: dur(0.4), ease: 'power3.out' })
  }, [visible])
  if (!visible) return null
  const later = () => setState({ update: { ...u, dismissed: true } })

  return (
    <aside className="update-card textures-offer" ref={card} aria-live="polite" aria-labelledby="update-title">
      <div className="textures-head">
        {u.phase === 'running' ? <span className="spinner" /> : <Icon name={u.phase === 'ready' ? 'check' : 'download'} size={18} />}
        <span className="textures-title" id="update-title">
          {u.phase === 'ready'
            ? `Bridger ${u.version} is ready`
            : u.phase === 'running'
              ? `Downloading Bridger ${u.version}…`
              : `Bridger ${u.version} is available`}
        </span>
        {u.phase !== 'running' && (
          <button className="icon-btn update-close" onClick={later} aria-label="Close">
            <Icon name="close" size={16} />
          </button>
        )}
      </div>

      {u.phase === 'available' && (
        <>
          <p className="textures-text">
            You have {u.current}. {summary(u.notes).length > 0 ? 'What is new:' : ''}
          </p>
          {summary(u.notes).length > 0 && (
            <ul className="update-notes">
              {summary(u.notes).map((l, i) => (
                <li key={i}>{l}</li>
              ))}
            </ul>
          )}
          <p className="textures-text small">
            The download {megabytes(u.size) && `(${megabytes(u.size)}) `}comes from the project's GitHub releases and is checked against
            its published SHA-256 before anything is replaced.
          </p>
        </>
      )}

      {u.phase === 'running' && (
        <>
          <div className="progress">
            <div className="progress-fill" ref={bar} />
          </div>
          <div className="textures-row">
            <span className="muted small">
              {u.step} · {Math.round(u.fraction * 100)}%
            </span>
            <button className="btn btn-ghost btn-sm" onClick={() => send({ type: 'update_cancel' })}>
              Cancel
            </button>
          </div>
        </>
      )}

      {u.phase === 'ready' && (
        <p className="textures-text">
          Restart to use it now, or keep working: it is installed when you close Bridger.
          {converting && ' Bridger can restart after the conversion finishes.'}
        </p>
      )}

      {u.phase === 'failed' && <p className="field-error">{u.error}</p>}

      <div className="textures-row">
        {u.phase === 'available' && u.installable && (
          <button className="btn btn-primary btn-sm" onClick={() => send({ type: 'update_install' })}>
            <Icon name="download" size={16} />
            Download and install
          </button>
        )}
        {(u.phase === 'failed' || (u.phase === 'available' && !u.installable)) && (
          <button className="btn btn-primary btn-sm" onClick={() => send({ type: 'update_page' })}>
            Open the release page
          </button>
        )}
        {u.phase === 'ready' && (
          <button className="btn btn-primary btn-sm" disabled={converting} onClick={() => send({ type: 'update_restart' })}>
            Restart now
          </button>
        )}
        {u.phase === 'available' && (
          <button
            className="btn btn-ghost btn-sm"
            onClick={() => {
              send({ type: 'update_skip' })
              later()
            }}
          >
            Skip this version
          </button>
        )}
        {u.phase === 'ready' && (
          <button className="btn btn-ghost btn-sm" onClick={later}>
            Later
          </button>
        )}
      </div>
    </aside>
  )
}
