import { useEffect, useRef, useState } from 'react'
import { send, type ConvertRequest, type Doc, type EntityMode, type Fallback, type Target } from '../bridge'
import { VersionPicker } from './VersionPicker'
import { basename, dirname, formatMeta, issueTotal } from '../formats'
import { dur, gsap, useGSAP } from '../motion'
import { setConvert, useStore, type ConvertState } from '../store'
import { Icon } from './Icon'
import { ReportList } from './Inspector'

function stem(name: string) {
  return name.replace(/\.[^.]+$/, '')
}

function TargetCard({ t, selected, onSelect }: { t: Target; selected: boolean; onSelect: () => void }) {
  const meta = formatMeta(t.id)
  return (
    <button className={`target target--${t.edition} ${selected ? 'is-selected' : ''}`} onClick={onSelect} role="radio" aria-checked={selected}>
      <span className="target-head">
        <span className="target-label">{meta.label}</span>
        <span className="target-ext mono">{meta.short}</span>
      </span>
      <span className="target-blurb">{meta.blurb}</span>
      <span className="target-check" aria-hidden>
        <Icon name="check" size={14} />
      </span>
    </button>
  )
}

function Segmented<T extends string>({ value, options, onChange, label }: { value: T; options: [T, string][]; onChange: (v: T) => void; label: string }) {
  return (
    <div className="segmented segmented--sm" role="radiogroup" aria-label={label}>
      {options.map(([v, text]) => (
        <button key={v} role="radio" aria-checked={value === v} className={value === v ? 'is-active' : ''} onClick={() => onChange(v)}>
          {text}
        </button>
      ))}
    </div>
  )
}

// "Java 1.20.1 → Bedrock 26.50": what the conversion does, in one line.
function Route({ doc, edition, version }: { doc: Doc; edition: Target['edition']; version: string }) {
  const src = doc.source_version
  const from = src ? `${src.edition === 'java' ? 'Java' : 'Bedrock'} ${src.name || '(version not recorded)'}` : null
  const to = `${edition === 'java' ? 'Java' : 'Bedrock'} ${version}`
  return (
    <p className="route" aria-label={from ? `From ${from} to ${to}` : `To ${to}`}>
      {from && (
        <>
          <span className={`route-end route-end--${src!.edition}`}>{from}</span>
          <Icon name="arrow" size={16} />
        </>
      )}
      <span className={`route-end route-end--${edition}`}>{to}</span>
    </p>
  )
}

function Setup({ doc, convert }: { doc: Doc; convert: ConvertState }) {
  const targets = doc.targets
  const target = convert.target ?? targets[0]?.id ?? null
  const meta = target ? formatMeta(target) : null
  const targetInfo = targets.find((t) => t.id === target) ?? null
  const versions = targetInfo?.versions ?? []
  const version = versions.find((v) => v.name === convert.targetVersion)?.name ?? versions[0]?.name ?? null
  const [name, setName] = useState(stem(doc.kind === 'world' ? doc.name : doc.name))
  const [placement, setPlacement] = useState<'new' | 'into'>('new')
  const [air, setAir] = useState<'keep' | 'replace'>('keep')
  const [height, setHeight] = useState<'fail' | 'clip' | 'shift'>('fail')
  const [clearEntities, setClearEntities] = useState(false)
  const [pos, setPos] = useState<[string, string, string]>(['0', '64', '0'])
  const [advanced, setAdvanced] = useState(false)
  // Advanced options, remembered in the settings.
  const settings = useStore((s) => s.settings)
  const [entities, setEntitiesState] = useState<EntityMode>(settings?.convert_entities ?? 'decorations')
  const [player, setPlayerState] = useState(settings?.convert_player ?? true)
  const [fallback, setFallbackState] = useState<Fallback>(settings?.convert_fallback ?? 'air')
  const setEntities = (v: EntityMode) => {
    setEntitiesState(v)
    send({ type: 'settings', convert_entities: v })
  }
  const setPlayer = (v: boolean) => {
    setPlayerState(v)
    send({ type: 'settings', convert_player: v })
  }
  const setFallback = (v: Fallback) => {
    setFallbackState(v)
    send({ type: 'settings', convert_fallback: v })
  }
  const isWorld = doc.kind === 'world'
  const structureToWorld = doc.kind === 'structure' && target === 'mcworld'
  const into = structureToWorld && placement === 'into'
  const posValid = pos.every((p) => /^-?\d+$/.test(p.trim()))
  const ready = !!target && !!convert.output && (!into || (!!convert.intoWorld && posValid))

  const start = () => {
    if (!target || !convert.output) return
    const req: ConvertRequest = {
      output: convert.output,
      format: target,
      level_name: name.trim(),
      air,
      height,
      clear_entities: into && clearEntities,
      entities,
      player,
      fallback,
      overwrite: true, // the save dialog already asked before replacing a file
    }
    if (version && !into) req.target_version = version // placing into a world writes for that world's game
    if (into && convert.intoWorld) {
      req.into_world = convert.intoWorld
      req.place_at = pos.map((p) => parseInt(p, 10)) as [number, number, number]
    }
    setConvert({ phase: 'running', fraction: 0, step: 'Starting', error: null, summary: null })
    send({ type: 'convert', ...req })
  }

  const groups: [string, Target[]][] = [
    ['Bedrock Edition', targets.filter((t) => t.edition === 'bedrock')],
    ['Java Edition', targets.filter((t) => t.edition === 'java')],
  ]
  return (
    <div className="flow-step">
      <section className="flow-section">
        <h3 className="section-title">
          <span className="step-num">01</span> Format
        </h3>
        {groups.map(([label, list]) =>
          list.length === 0 ? null : (
            <div key={label} className="target-group">
              <div className="overline">{label}</div>
              <div className="targets" role="radiogroup" aria-label={label}>
                {list.map((t) => (
                  <TargetCard
                    key={t.id}
                    t={t}
                    selected={t.id === target}
                    onSelect={() => target !== t.id && setConvert({ target: t.id, targetVersion: null, output: null })}
                  />
                ))}
              </div>
            </div>
          ),
        )}
        {targetInfo && version && !into && (
          <div className="field version-field">
            <span className="field-label">Game version</span>
            <VersionPicker edition={targetInfo.edition} versions={versions} value={version} onChange={(v) => setConvert({ targetVersion: v })} />
            <Route doc={doc} edition={targetInfo.edition} version={version} />
          </div>
        )}
        {into && <p className="field-hint version-field">The build is written for the game version of the world you place it into.</p>}
      </section>

      <section className="flow-section">
        <h3 className="section-title">
          <span className="step-num">02</span> Options
        </h3>
        <label className="field">
          <span className="field-label">{target === 'mcworld' || target === 'java-world' ? 'World name' : 'Name'}</span>
          <input className="input" value={name} onChange={(e) => setName(e.target.value)} spellCheck={false} />
        </label>
        {structureToWorld && (
          <div className="field">
            <span className="field-label">Where to put it</span>
            <div className="choice-list" role="radiogroup" aria-label="Where to put it">
              <button className={`choice ${placement === 'new' ? 'is-active' : ''}`} role="radio" aria-checked={placement === 'new'} onClick={() => setPlacement('new')}>
                <span className="choice-radio" aria-hidden />
                <span>
                  <span className="choice-title">A new flat world</span>
                  <span className="choice-sub">The build stands on a superflat world, with you spawning in front of it.</span>
                </span>
              </button>
              <button className={`choice ${placement === 'into' ? 'is-active' : ''}`} role="radio" aria-checked={placement === 'into'} onClick={() => setPlacement('into')}>
                <span className="choice-radio" aria-hidden />
                <span>
                  <span className="choice-title">A copy of one of my worlds</span>
                  <span className="choice-sub">Your world is not changed: a new copy with the build in it is saved.</span>
                </span>
              </button>
            </div>
            {into && (
              <div className="into">
                <button className="btn btn-secondary btn-block" onClick={() => send({ type: 'pick_into_world' })}>
                  <Icon name="globe" />
                  {convert.intoWorld ? basename(convert.intoWorld) : 'Choose a .mcworld…'}
                </button>
                <div className="field">
                  <span className="field-label">Position of the build's corner (x, y, z)</span>
                  <div className="xyz">
                    {(['x', 'y', 'z'] as const).map((axis, i) => (
                      <label key={axis} className="xyz-field">
                        <span className="mono">{axis}</span>
                        <input
                          className={`input mono ${/^-?\d+$/.test(pos[i].trim()) ? '' : 'is-invalid'}`}
                          inputMode="numeric"
                          value={pos[i]}
                          onChange={(e) => setPos((p) => p.map((v, j) => (j === i ? e.target.value : v)) as [string, string, string])}
                        />
                      </label>
                    ))}
                  </div>
                </div>
              </div>
            )}
          </div>
        )}
        <div className="advanced">
          <button className="advanced-toggle" onClick={() => setAdvanced((a) => !a)} aria-expanded={advanced}>
            <Icon name="chevron" size={16} />
            Advanced
            {(entities !== 'decorations' || !player || fallback !== 'air') && <span className="advanced-badge">changed</span>}
          </button>
          {advanced && (
            <div className="advanced-body">
              <div className="field">
                <span className="field-label">Entities</span>
                <Segmented<EntityMode>
                  label="Entities"
                  value={entities}
                  onChange={setEntities}
                  options={[
                    ['decorations', 'Frames and paintings'],
                    ['all', 'Everything'],
                    ['none', 'None'],
                  ]}
                />
                <p className="field-hint">
                  {entities === 'decorations' && 'Item frames and paintings are converted; mobs and dropped items are listed in the report.'}
                  {entities === 'all' &&
                    'Also mobs, villagers, pets, armor stands and dropped items, with their look, names, health and equipment. New: check them in the game.'}
                  {entities === 'none' && 'No entities are written; each one is listed in the report.'}
                </p>
              </div>
              {isWorld && (
                <label className="check">
                  <input type="checkbox" checked={player} onChange={(e) => setPlayer(e.target.checked)} />
                  <span>Keep the player's position and inventory</span>
                </label>
              )}
              <div className="field">
                <span className="field-label">Blocks with no counterpart</span>
                <Segmented<Fallback>
                  label="Blocks with no counterpart"
                  value={fallback}
                  onChange={setFallback}
                  options={[
                    ['air', 'Leave empty'],
                    ['marker', 'Marker block'],
                  ]}
                />
                <p className="field-hint">
                  {fallback === 'air' ? 'They become air and are listed in the report.' : 'A marker block shows where each one was, so you can find them in the game.'}
                </p>
              </div>
              {structureToWorld && (
                <>
                  <div className="field">
                    <span className="field-label">Air in the build</span>
                    <Segmented label="Air in the build" value={air} onChange={setAir} options={[['keep', 'Keeps what is there'], ['replace', 'Clears it']]} />
                  </div>
                  <div className="field">
                    <span className="field-label">If it does not fit the build height</span>
                    <Segmented label="Build height" value={height} onChange={setHeight} options={[['fail', 'Stop'], ['shift', 'Move it'], ['clip', 'Cut it']]} />
                  </div>
                  {into && (
                    <label className="check">
                      <input type="checkbox" checked={clearEntities} onChange={(e) => setClearEntities(e.target.checked)} />
                      <span>Remove mobs and items inside the build's area</span>
                    </label>
                  )}
                </>
              )}
            </div>
          )}
        </div>
      </section>

      <section className="flow-section">
        <h3 className="section-title">
          <span className="step-num">03</span> Save as
        </h3>
        <button className={`save-target ${convert.output ? 'has-path' : ''}`} onClick={() => target && send({ type: 'pick_output', format: target })} disabled={!target}>
          <Icon name={target === 'java-world' ? 'folder' : 'file'} />
          {convert.output ? (
            <span className="save-path">
              <span className="save-name">{basename(convert.output)}</span>
              <span className="save-dir">{dirname(convert.output)}</span>
            </span>
          ) : (
            <span className="save-path">
              <span className="save-name">Choose where to save…</span>
              <span className="save-dir">{meta ? `${meta.label} (${meta.short})` : ''}</span>
            </span>
          )}
          <span className="save-change">{convert.output ? 'Change' : 'Browse'}</span>
        </button>
        {convert.error && convert.phase === 'setup' && !convert.cancelled && <p className="field-error">{convert.error}</p>}
      </section>

      <div className="flow-footer">
        <p className="safety">
          <Icon name="check" size={14} />
          Your original is never changed. The new file is checked before it is saved.
        </p>
        <button className="btn btn-primary btn-lg btn-block" onClick={start} disabled={!ready}>
          Convert{meta ? ` to ${meta.short === 'folder' ? 'a Java world' : meta.short}` : ''}
          <Icon name="arrow" />
        </button>
      </div>
    </div>
  )
}

function Running({ convert }: { convert: ConvertState }) {
  const ring = useRef<SVGCircleElement>(null)
  const C = 2 * Math.PI * 52
  useEffect(() => {
    if (!ring.current) return
    gsap.to(ring.current, { strokeDashoffset: C * (1 - Math.max(0.02, convert.fraction)), duration: dur(0.5), ease: 'power2.out', overwrite: true })
  }, [convert.fraction, C])
  return (
    <div className="flow-step running" aria-live="polite">
      <div className="ring">
        <svg viewBox="0 0 120 120" width="168" height="168" aria-hidden>
          <defs>
            <linearGradient id="ringg" x1="0" x2="1" y1="0" y2="1">
              <stop offset="0" stopColor="#6cf0c6" />
              <stop offset="1" stopColor="#2457ff" />
            </linearGradient>
          </defs>
          <circle cx="60" cy="60" r="52" className="ring-track" />
          <circle ref={ring} cx="60" cy="60" r="52" className="ring-fill" stroke="url(#ringg)" strokeDasharray={C} strokeDashoffset={C} />
        </svg>
        <div className="ring-label">
          <span className="ring-pct mono">{Math.round(convert.fraction * 100)}%</span>
        </div>
      </div>
      <div className="running-step">{convert.step || 'Working'}…</div>
      <p className="muted small">You can open the 3D preview while this runs.</p>
      <button className="btn btn-secondary" onClick={() => send({ type: 'cancel_convert' })}>
        <Icon name="stop" size={16} />
        Cancel
      </button>
    </div>
  )
}

function Done({ convert, doc }: { convert: ConvertState; doc: Doc }) {
  const s = convert.summary
  const root = useRef<HTMLDivElement>(null)
  useGSAP(
    () => {
      const tl = gsap.timeline()
      tl.fromTo('.done-check path', { strokeDashoffset: 40 }, { strokeDashoffset: 0, duration: dur(0.5), ease: 'power2.out', delay: 0.1 })
        .from('.done-badge', { scale: 0.6, autoAlpha: 0, duration: dur(0.45), ease: 'back.out(2)' }, 0)
        .from('.done-body > *', { y: 10, autoAlpha: 0, stagger: 0.05, duration: dur(0.3) }, 0.15)
    },
    { scope: root },
  )
  if (!s) return null
  const issues = issueTotal(s.report)
  const meta = formatMeta(s.output_format)
  return (
    <div className="flow-step done" ref={root}>
      <div className="done-badge">
        <svg className="done-check" viewBox="0 0 24 24" width="30" height="30" aria-hidden>
          <path d="M5 12.5l4.5 4.5L19 7.5" fill="none" stroke="currentColor" strokeWidth="2.4" strokeLinecap="round" strokeLinejoin="round" strokeDasharray="40" />
        </svg>
      </div>
      <div className="done-body">
        <h3>Saved</h3>
        <p className="done-file" title={s.output}>
          {basename(s.output)}
        </p>
        {s.target_version && (
          <p className="done-versions">
            For <b>{s.target_version}</b>
            {s.source_version ? <span className="muted"> · from {s.source_version}</span> : null}
          </p>
        )}
        <dl className="stats-grid stats-grid--3">
          <div className="stat">
            <dt className="overline">Blocks</dt>
            <dd className="mono">{s.blocks.toLocaleString()}</dd>
          </div>
          {s.chunks > 0 && (
            <div className="stat">
              <dt className="overline">Chunks</dt>
              <dd className="mono">{s.chunks.toLocaleString()}</dd>
            </div>
          )}
          <div className="stat">
            <dt className="overline">Time</dt>
            <dd className="mono">{s.ms < 1000 ? `${Math.round(s.ms)} ms` : `${(s.ms / 1000).toFixed(1)} s`}</dd>
          </div>
        </dl>
        {s.spawn && meta.edition === 'bedrock' && (
          <p className="muted small">
            In the game the build's corner is at <span className="mono">{s.origin?.join(', ')}</span>; you spawn in front of it.
          </p>
        )}
        <div className={`callout ${s.exact ? 'callout-success' : 'callout-warning'}`}>
          <Icon name={s.exact ? 'check' : 'alert'} size={18} />
          <div>
            {s.exact
              ? 'Everything was converted exactly.'
              : issues === 0
                ? `The translation recorded no problems, but ${s.warnings.length === 1 ? 'one warning below describes' : `${s.warnings.length} warnings below describe`} data that was skipped or dropped, so the result is not exact.`
                : `${issues.toLocaleString()} ${issues === 1 ? 'thing was' : 'things were'} converted approximately or not at all. Click one to see where it is.`}
          </div>
        </div>
        {issues > 0 && <ReportList report={s.report} emptyText="" />}
        {s.warnings.length > 0 && (
          <div className="warnings">
            <div className="overline">Notes</div>
            <ul>
              {s.warnings.slice(0, 12).map((w, i) => (
                <li key={i}>{w}</li>
              ))}
            </ul>
          </div>
        )}
        <div className="done-actions">
          <button className="btn btn-primary" onClick={() => send({ type: 'reveal', path: s.output })}>
            <Icon name="reveal" />
            Show in folder
          </button>
          <button className="btn btn-secondary" onClick={() => setConvert({ phase: 'setup', output: null, summary: null, target: null })}>
            Convert again
          </button>
        </div>
        {doc.kind === 'structure' && meta.edition === 'bedrock' && (
          <p className="muted small">Tip: double-click the .mcworld file to import it into Minecraft Bedrock.</p>
        )}
      </div>
    </div>
  )
}

function Failed({ convert }: { convert: ConvertState }) {
  return (
    <div className="flow-step failed">
      <div className="failed-badge">
        <Icon name="alert" size={26} />
      </div>
      <h3>The conversion stopped</h3>
      <p className="failed-message">{convert.error}</p>
      <p className="muted small">Nothing was written, and your original is unchanged.</p>
      <button className="btn btn-primary" onClick={() => setConvert({ phase: 'setup', error: null })}>
        Back to the options
      </button>
    </div>
  )
}

// The conversion flow, inline on the file page: setup -> running -> done / failed.
export function ConvertFlow() {
  const convert = useStore((s) => s.convert)
  const doc = useStore((s) => s.doc)
  const root = useRef<HTMLDivElement>(null)
  useGSAP(
    () => {
      if (root.current) gsap.fromTo(root.current.querySelector('.flow-step'), { autoAlpha: 0, y: 10 }, { autoAlpha: 1, y: 0, duration: dur(0.3) })
    },
    { dependencies: [convert.phase, doc?.path], scope: root },
  )
  if (!doc) return null
  return (
    <div className="flow" ref={root}>
      {convert.phase === 'setup' && <Setup key={doc.path} doc={doc} convert={convert} />}
      {convert.phase === 'running' && <Running convert={convert} />}
      {convert.phase === 'done' && <Done convert={convert} doc={doc} />}
      {convert.phase === 'failed' && <Failed convert={convert} />}
    </div>
  )
}
