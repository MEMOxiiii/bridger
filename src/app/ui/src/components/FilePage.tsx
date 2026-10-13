// The file page: what was opened (left) and what to do with it (right). Converting is the main
// job, so it gets the larger column; the 3D preview is one click away, never forced.
import { useEffect, useRef } from 'react'
import type { Doc } from '../bridge'
import { enterPreview } from '../actions'
import { formatMeta, issueTotal } from '../formats'
import { Counter, dur, EASE_IN, gsap, SmoothScroll, useGSAP } from '../motion'
import { setState, useStore } from '../store'
import { ConvertFlow } from './ConvertFlow'
import { Icon } from './Icon'
import { ReportList } from './Inspector'

// A small isometric stack in the file's edition colour (decorates the preview tile).
function PreviewArt({ edition }: { edition: 'java' | 'bedrock' }) {
  const [top, left, right] = edition === 'java' ? ['#ffd28a', '#f2b04a', '#b97a1f'] : ['#b6ffe6', '#6cf0c6', '#2aa889']
  const W = 16
  const H = 9
  const cubes = [
    [0, 0, 0],
    [1, 0, 0],
    [0, 0, 1],
    [1, 0, 1],
    [0, 1, 0],
    [1, 1, 0],
    [0, 2, 0],
  ].sort((a, b) => a[0] + a[2] - (b[0] + b[2]) || a[1] - b[1])
  return (
    <svg className="preview-art" viewBox="-40 -52 80 80" aria-hidden>
      {cubes.map(([x, y, z], i) => (
        <g key={i} transform={`translate(${(x - z) * W} ${(x + z) * H - y * 2 * H})`}>
          <path d={`M0 ${-H} L${W} 0 L0 ${H} L${-W} 0Z`} fill={top} />
          <path d={`M${-W} 0 L0 ${H} L0 ${3 * H} L${-W} ${2 * H}Z`} fill={left} />
          <path d={`M${W} 0 L0 ${H} L0 ${3 * H} L${W} ${2 * H}Z`} fill={right} />
        </g>
      ))}
    </svg>
  )
}

function Fact({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="fact">
      <dt>{label}</dt>
      <dd className="mono">{children}</dd>
    </div>
  )
}

function FileCard({ doc }: { doc: Doc }) {
  const previewReady = useStore((s) => s.previewReady)
  const assets = useStore((s) => s.assets)
  const missing = useStore((s) => s.missingModels)
  const meta = formatMeta(doc.format)
  const issues = issueTotal(doc.report)
  const notes = doc.warnings.length
  const noViewer = assets !== null && assets.viewer === false
  const noTextures = !noViewer && assets !== null && !assets.ok
  const blocked = noViewer || noTextures
  return (
    <section className="file-card" aria-label="Opened file">
      <div className="file-kicker">
        <span className={`edition edition--${doc.edition}`}>{doc.edition === 'java' ? 'Java' : 'Bedrock'}</span>
        <span className="file-format">{meta.label}</span>
      </div>
      <h1 className="file-name">{doc.name}</h1>
      <p className="file-path mono" title={doc.path}>
        {doc.path}
      </p>

      <dl className="facts">
        {doc.kind === 'structure' && doc.size && <Fact label="Size">{doc.size.join(' × ')}</Fact>}
        {doc.blocks !== undefined && (
          <Fact label="Blocks">
            <Counter value={doc.blocks} />
          </Fact>
        )}
        {doc.block_entities !== undefined && <Fact label="Block entities">{doc.block_entities.toLocaleString()}</Fact>}
        {doc.kind === 'world' && doc.chunks !== undefined && (
          <Fact label="Chunks">
            <Counter value={doc.chunks} />
          </Fact>
        )}
        {doc.kind === 'world' && doc.spawn && <Fact label="Spawn">{doc.spawn.join(', ')}</Fact>}
        {doc.data_version ? <Fact label="Data version">{doc.data_version}</Fact> : null}
      </dl>

      {doc.kind === 'structure' && (
        <button
          className={`health ${issues === 0 && notes === 0 ? 'health--ok' : 'health--warn'}`}
          onClick={() => (issues > 0 || notes > 0) && setState({ reportOpen: true })}
          disabled={issues === 0 && notes === 0}
        >
          <Icon name={issues === 0 && notes === 0 ? 'check' : 'alert'} size={18} />
          <span className="health-text">
            {issues === 0 && notes === 0 ? (
              'Read exactly — nothing was lost'
            ) : issues === 0 ? (
              <>
                Blocks read exactly, but <b>{notes}</b> {notes === 1 ? 'note' : 'notes'} about other data
              </>
            ) : (
              <>
                <b>{issues.toLocaleString()}</b> {issues === 1 ? 'item needs' : 'items need'} a look
              </>
            )}
          </span>
          {(issues > 0 || notes > 0) && <span className="health-cta">Review</span>}
        </button>
      )}

      <button className="preview-tile" onClick={enterPreview} disabled={!previewReady || blocked}>
        <PreviewArt edition={doc.edition} />
        <span className="preview-text">
          <span className="preview-title">
            {noViewer
              ? '3D preview unavailable here'
              : noTextures
                ? '3D preview needs textures'
                : previewReady
                  ? 'Open the 3D preview'
                  : 'Preparing the 3D preview…'}
          </span>
          <span className="preview-sub">
            {noViewer
              ? 'It needs a GPU with Vulkan 1.3. Converting works normally.'
              : noTextures
                ? 'Choose your Minecraft Java client jar in Settings.'
                : doc.kind === 'world'
                ? 'Fly over the world; chunks load around you.'
                : 'Orbit, fly through and slice it before converting.'}
          </span>
        </span>
        <span className="preview-go" aria-hidden>
          {blocked ? <Icon name="info" /> : previewReady ? <Icon name="arrow" /> : <span className="spinner" />}
        </span>
      </button>
      {missing > 0 && <p className="file-note">{missing} block types have no model in your resource packs and are not drawn.</p>}
    </section>
  )
}

// What could not be read exactly, over the page; entries jump to the 3D preview.
export function ReportSheet() {
  const doc = useStore((s) => s.doc)
  const open = useStore((s) => s.reportOpen)
  const root = useRef<HTMLDivElement>(null)
  useGSAP(
    () => {
      if (!open || !root.current) return
      gsap.fromTo(root.current.querySelector('.sheet'), { x: 40, autoAlpha: 0 }, { x: 0, autoAlpha: 1, duration: dur(0.4), ease: 'power3.out' })
      gsap.fromTo(root.current, { autoAlpha: 0 }, { autoAlpha: 1, duration: dur(0.2) })
    },
    { dependencies: [open] },
  )
  useEffect(() => {
    if (!open) return
    const key = (e: KeyboardEvent) => e.key === 'Escape' && close()
    window.addEventListener('keydown', key)
    return () => window.removeEventListener('keydown', key)
  })
  if (!open || !doc) return null
  const close = () => {
    if (!root.current) return setState({ reportOpen: false })
    gsap.to(root.current, { autoAlpha: 0, duration: dur(0.15), ease: EASE_IN, onComplete: () => setState({ reportOpen: false }) })
  }
  return (
    <div className="sheet-backdrop" ref={root} onMouseDown={(e) => e.target === e.currentTarget && close()}>
      <aside className="sheet" role="dialog" aria-modal aria-label="Report">
        <header className="sheet-head">
          <div>
            <div className="overline">Report</div>
            <h2>What could not be read exactly</h2>
            <p className="muted small">Click an entry to see those blocks in the 3D preview.</p>
          </div>
          <button className="icon-btn" onClick={close} aria-label="Close">
            <Icon name="close" />
          </button>
        </header>
        <SmoothScroll className="sheet-scroll">
          <div className="sheet-body">
            {issueTotal(doc.report) > 0 && <ReportList report={doc.report} emptyText="" />}
            {doc.warnings.length > 0 && (
              <div className="warnings">
                <div className="overline">Notes from the reader</div>
                <ul>
                  {doc.warnings.slice(0, 50).map((w, i) => (
                    <li key={i}>{w}</li>
                  ))}
                </ul>
              </div>
            )}
          </div>
        </SmoothScroll>
      </aside>
    </div>
  )
}

export function FilePage() {
  const doc = useStore((s) => s.doc)
  const root = useRef<HTMLDivElement>(null)
  useGSAP(
    () => {
      gsap
        .timeline({ defaults: { duration: dur(0.5), ease: 'power3.out' } })
        .from('.file-card > *', { y: 14, autoAlpha: 0, stagger: 0.04 })
        .from('.convert-card', { y: 20, autoAlpha: 0 }, '<0.1')
    },
    { dependencies: [doc?.path], scope: root },
  )
  if (!doc) return null
  return (
    <main className="page file-page" ref={root}>
      <div className="file-grid">
        <FileCard doc={doc} />
        <section className="convert-card" aria-label="Convert">
          <header className="convert-head">
            <div>
              <div className="overline">Convert</div>
              <h2>Make it work in {doc.edition === 'java' ? 'Bedrock — or another Java format' : 'Java Edition'}</h2>
            </div>
          </header>
          <SmoothScroll className="convert-scroll">
            <ConvertFlow />
          </SmoothScroll>
        </section>
      </div>
    </main>
  )
}
