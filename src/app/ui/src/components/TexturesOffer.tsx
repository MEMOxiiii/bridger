// Offer to fetch the textures for the 3D preview from Mojang, only when the user asks. It says what
// is downloaded, from where, how big it is and what it is for, before anything happens.
import { useEffect, useRef } from 'react'
import { send } from '../bridge'
import { dur, gsap } from '../motion'
import { useStore } from '../store'
import { Icon } from './Icon'

export function TexturesOffer({ compact = false }: { compact?: boolean }) {
  const assets = useStore((s) => s.assets)
  const t = useStore((s) => s.textures)
  const bar = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (bar.current) gsap.to(bar.current, { scaleX: t.fraction, duration: dur(0.3), ease: 'power2.out' })
  }, [t.fraction])
  if (!assets || assets.ok || !assets.can_download) return null
  const version = assets.java_version ?? '26.3'

  if (t.phase === 'running')
    return (
      <div className={`textures-offer ${compact ? 'is-compact' : ''}`} aria-live="polite">
        <div className="textures-head">
          <span className="spinner" />
          <span className="textures-title">Downloading the textures from Mojang…</span>
        </div>
        <div className="progress">
          <div className="progress-fill" ref={bar} />
        </div>
        <div className="textures-row">
          <span className="muted small">
            {t.step} · {Math.round(t.fraction * 100)}%
          </span>
          <button className="btn btn-ghost btn-sm" onClick={() => send({ type: 'cancel_textures' })}>
            Cancel
          </button>
        </div>
      </div>
    )

  return (
    <div className={`textures-offer ${compact ? 'is-compact' : ''}`}>
      <div className="textures-head">
        <Icon name="download" size={18} />
        <span className="textures-title">Get the textures for the 3D preview</span>
      </div>
      {!compact && (
        <p className="textures-text">
          Minecraft Java {version} was not found on this computer. Bridger can download its client file (about 40 MB) from Mojang's
          official servers. It is only read for block textures and models, checked against Mojang's published fingerprint, and kept in
          Bridger's own folder. Converting works without it.
        </p>
      )}
      {t.phase === 'failed' && <p className="field-error">{t.error}</p>}
      <div className="textures-row">
        <button className="btn btn-primary btn-sm" onClick={() => send({ type: 'download_textures' })}>
          <Icon name="download" size={16} />
          {t.phase === 'failed' ? 'Try again' : 'Download from Mojang'}
        </button>
        <button className="btn btn-ghost btn-sm" onClick={() => send({ type: 'pick_client_jar' })}>
          I have a client jar…
        </button>
      </div>
    </div>
  )
}
