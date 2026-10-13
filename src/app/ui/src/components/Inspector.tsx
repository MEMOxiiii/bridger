import { useLayoutEffect, useRef, useState } from 'react'
import { send, type Doc, type Report } from '../bridge'
import { formatMeta, issueLabel, issueTotal } from '../formats'
import { Counter, dur, gsap, SmoothScroll, useGSAP } from '../motion'
import { setState, useStore } from '../store'
import { Icon } from './Icon'
import { showIssue } from '../actions'

type Tab = 'overview' | 'report' | 'view'

function Stat({ label, value, mono = true }: { label: string; value: React.ReactNode; mono?: boolean }) {
  return (
    <div className="stat">
      <dt className="overline">{label}</dt>
      <dd className={mono ? 'mono' : undefined}>{value}</dd>
    </div>
  )
}

function Overview({ doc }: { doc: Doc }) {
  const meta = formatMeta(doc.format)
  const issues = issueTotal(doc.report)
  return (
    <div className="tab-body">
      <dl className="stats-grid">
        {doc.kind === 'structure' && doc.size && (
          <Stat label="Size" value={`${doc.size[0]} × ${doc.size[1]} × ${doc.size[2]}`} />
        )}
        {doc.blocks !== undefined && <Stat label="Blocks" value={<Counter value={doc.blocks} />} />}
        {doc.block_entities !== undefined && <Stat label="Block entities" value={<Counter value={doc.block_entities} />} />}
        {doc.kind === 'world' && doc.chunks !== undefined && <Stat label="Chunks" value={<Counter value={doc.chunks} />} />}
        {doc.kind === 'world' && doc.spawn && <Stat label="Spawn" value={doc.spawn.join(', ')} />}
        {doc.data_version ? <Stat label="Data version" value={doc.data_version} /> : null}
      </dl>
      <div className={`format-card format-card--${meta.edition}`}>
        <div className="format-card-icon">
          <Icon name={doc.kind === 'world' ? 'globe' : 'cube'} />
        </div>
        <div>
          <div className="format-card-title">{meta.label}</div>
          <div className="format-card-sub">{doc.edition === 'java' ? 'Java Edition' : 'Bedrock Edition'} · {meta.short}</div>
        </div>
      </div>
      {doc.kind === 'structure' && (
        <div className={`callout ${issues === 0 ? 'callout-success' : 'callout-warning'}`}>
          <Icon name={issues === 0 ? 'check' : 'alert'} size={18} />
          <div>
            {issues === 0 ? (
              <>Everything in this file was read exactly.</>
            ) : (
              <>
                <strong>{issues.toLocaleString()}</strong> {issues === 1 ? 'thing' : 'things'} could not be shown exactly.{' '}
                <button type="button" className="link" data-tab="report">
                  See the report
                </button>
              </>
            )}
          </div>
        </div>
      )}
      {doc.warnings.length > 0 && (
        <div className="warnings">
          <div className="overline">Notes from the reader</div>
          <ul>
            {doc.warnings.slice(0, 20).map((w, i) => (
              <li key={i}>{w}</li>
            ))}
          </ul>
        </div>
      )}
      {doc.missing_models ? (
        <p className="muted small">{doc.missing_models} block types have no model in your resource packs and are not drawn.</p>
      ) : null}
    </div>
  )
}

export function ReportList({ report, emptyText }: { report: Report; emptyText: string }) {
  const focusKey = useStore((s) => s.focusKey)
  const kinds = Object.keys(report)
  if (kinds.length === 0)
    return (
      <div className="report-empty">
        <div className="report-empty-icon">
          <Icon name="check" size={22} />
        </div>
        <p>{emptyText}</p>
      </div>
    )
  const focus = showIssue
  return (
    <div className="report">
      {kinds.map((kind) => {
        const label = issueLabel(kind)
        const list = report[kind]
        const total = list.reduce((n, i) => n + i.count, 0)
        return (
          <section key={kind} className="report-group">
            <header>
              <span className={`dot dot-${label.tone}`} aria-hidden />
              <div>
                <h3>{label.title}</h3>
                <p>{label.hint}</p>
              </div>
              <span className={`badge badge-${label.tone}`}>{total.toLocaleString()}</span>
            </header>
            <ul>
              {list.map((issue) => {
                const key = `${kind}|${issue.subject}`
                const active = focusKey === key
                return (
                  <li key={key}>
                    <button
                      className={`issue ${active ? 'is-active' : ''}`}
                      onClick={() => focus(key, issue.examples)}
                      aria-pressed={active}
                      disabled={issue.examples.length === 0}
                      title={issue.examples.length ? 'Show where in the 3D view' : undefined}
                    >
                      <span className="issue-subject mono">{issue.subject}</span>
                      <span className="issue-count mono">×{issue.count.toLocaleString()}</span>
                      {issue.examples.length > 0 && <Icon name="frame" size={16} />}
                    </button>
                  </li>
                )
              })}
            </ul>
          </section>
        )
      })}
    </div>
  )
}

function ViewTab({ doc }: { doc: Doc }) {
  const map = useStore((s) => s.map)
  const fly = useStore((s) => s.fly)
  const slice = useStore((s) => s.slice)
  const settings = useStore((s) => s.settings)
  const [minY, maxY] = doc.kind === 'structure' && doc.size ? [0, doc.size[1]] : [-64, 320]
  const sliceValue = slice ?? maxY
  const setSlice = (v: number | null) => {
    setState({ slice: v })
    send({ type: 'view', slice: v })
  }
  const setMode = (mode: 'orbit' | 'fly' | 'map') => {
    if (mode === 'fly') return send({ type: 'fly', on: true })
    const m = mode === 'map'
    setState({ map: m })
    send({ type: 'view', map: m })
  }
  const mode = fly ? 'fly' : map ? 'map' : 'orbit'
  return (
    <div className="tab-body">
      <div className="field">
        <div className="field-label">Camera</div>
        <div className="segmented" role="radiogroup" aria-label="Camera">
          {(
            [
              ['orbit', 'Orbit', 'orbit'],
              ['fly', 'Fly', 'fly'],
              ['map', 'Map', 'map'],
            ] as const
          ).map(([id, label, icon]) => (
            <button key={id} role="radio" aria-checked={mode === id} className={mode === id ? 'is-active' : ''} onClick={() => setMode(id)}>
              <Icon name={icon} size={16} />
              {label}
            </button>
          ))}
        </div>
      </div>
      <div className="field">
        <div className="field-label">
          <span>Slice</span>
          <span className="mono muted">{slice === null ? 'off' : `below y ${Math.round(sliceValue)}`}</span>
        </div>
        <input
          type="range"
          className="range"
          min={minY}
          max={maxY}
          step={1}
          value={sliceValue}
          onChange={(e) => {
            const v = Number(e.target.value)
            setSlice(v >= maxY ? null : v)
          }}
          aria-label="Hide blocks above this height"
          style={{ '--fill': `${((sliceValue - minY) / Math.max(1, maxY - minY)) * 100}%` } as React.CSSProperties}
        />
        <p className="field-hint">Hide everything above a height to look inside.</p>
      </div>
      {doc.kind === 'world' && settings && (
        <div className="field">
          <div className="field-label">
            <span>Render distance</span>
            <span className="mono muted">{settings.render_distance} chunks</span>
          </div>
          <input
            type="range"
            className="range"
            min={4}
            max={32}
            step={1}
            value={settings.render_distance}
            onChange={(e) => {
              const v = Number(e.target.value)
              setState({ settings: { ...settings, render_distance: v } })
              send({ type: 'settings', render_distance: v })
            }}
            aria-label="Render distance in chunks"
            style={{ '--fill': `${((settings.render_distance - 4) / 28) * 100}%` } as React.CSSProperties}
          />
        </div>
      )}
      <div className="help">
        <div className="overline">Controls</div>
        <dl>
          <dt>Drag</dt>
          <dd>{map ? 'Pan the map' : 'Orbit'}</dd>
          <dt>Right-drag</dt>
          <dd>Pan</dd>
          <dt>Wheel</dt>
          <dd>Zoom</dd>
          <dt>Double-click / F</dt>
          <dd>Fly like a player</dd>
          <dt>M · R</dt>
          <dd>Map · reset view</dd>
        </dl>
      </div>
    </div>
  )
}

export function Inspector() {
  const doc = useStore((s) => s.doc)
  const [tab, setTab] = useState<Tab>('overview')
  const root = useRef<HTMLElement>(null)
  const indicator = useRef<HTMLSpanElement>(null)
  const tabsRef = useRef<HTMLDivElement>(null)
  const bodyRef = useRef<HTMLDivElement>(null)

  // Move the tab indicator under the active tab.
  useLayoutEffect(() => {
    const btn = tabsRef.current?.querySelector<HTMLButtonElement>(`[data-tab="${tab}"]`)
    if (!btn || !indicator.current) return
    gsap.to(indicator.current, { x: btn.offsetLeft, width: btn.offsetWidth, duration: dur(0.32), ease: 'power3.out' })
  }, [tab, doc])

  useGSAP(
    () => {
      if (!bodyRef.current) return
      gsap.fromTo(bodyRef.current, { autoAlpha: 0, y: 6 }, { autoAlpha: 1, y: 0, duration: dur(0.24) })
    },
    { dependencies: [tab, doc?.path], scope: root },
  )

  // "See the report" links inside the overview.
  const onClick = (e: React.MouseEvent) => {
    const t = (e.target as HTMLElement).closest<HTMLElement>('[data-tab]')
    if (t?.dataset.tab && t.tagName === 'BUTTON' && t.classList.contains('link')) setTab(t.dataset.tab as Tab)
  }

  if (!doc) return null
  const issues = issueTotal(doc.report)
  return (
    <aside className="panel inspector" ref={root} aria-label="Inspector" onClick={onClick}>
      <div className="tabs" role="tablist" ref={tabsRef}>
        <span className="tab-indicator" ref={indicator} aria-hidden />
        {(['overview', 'report', 'view'] as const).map((t) => (
          <button key={t} role="tab" data-tab={t} aria-selected={tab === t} className={tab === t ? 'is-active' : ''} onClick={() => setTab(t)}>
            {t === 'overview' ? 'Overview' : t === 'report' ? 'Report' : 'View'}
            {t === 'report' && issues > 0 && <span className="tab-count mono">{issues > 999 ? '999+' : issues}</span>}
          </button>
        ))}
      </div>
      <SmoothScroll className="panel-scroll">
        <div ref={bodyRef}>
          {tab === 'overview' && <Overview doc={doc} />}
          {tab === 'report' && (
            <div className="tab-body">
              <ReportList
                report={doc.report}
                emptyText={
                  doc.kind === 'world'
                    ? 'Worlds are checked while converting: the report appears when the conversion is done.'
                    : 'Nothing to report: every block was read exactly.'
                }
              />
            </div>
          )}
          {tab === 'view' && <ViewTab doc={doc} />}
        </div>
      </SmoothScroll>
    </aside>
  )
}
