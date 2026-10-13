// Home: no file open. One clear job — bring a structure or world in — plus recent files.
import { useRef } from 'react'
import { send } from '../bridge'
import { openFile, openFolder } from '../actions'
import { basename, dirname, formatMeta } from '../formats'
import { dur, gsap, useGSAP } from '../motion'
import { useStore } from '../store'
import { Wordmark } from './Brand'
import { Icon } from './Icon'

const JAVA = ['schem', 'litematic', 'nbt', 'schematic', 'java-world']
const BEDROCK = ['mcstructure', 'mcworld']

function FormatRow({ ids, edition }: { ids: string[]; edition: 'java' | 'bedrock' }) {
  return (
    <div className={`format-row format-row--${edition}`}>
      <span className="format-row-label">{edition === 'java' ? 'Java Edition' : 'Bedrock Edition'}</span>
      <span className="format-row-items">
        {ids.map((id) => {
          const m = formatMeta(id)
          return (
            <span key={id} className="mono">
              {m.short === 'folder' ? 'world folder' : m.short}
            </span>
          )
        })}
      </span>
    </div>
  )
}

export function Home() {
  const root = useRef<HTMLDivElement>(null)
  const recent = useStore((s) => s.settings?.recent)
  const assets = useStore((s) => s.assets)

  useGSAP(
    () => {
      gsap
        .timeline({ defaults: { duration: dur(0.5), ease: 'power3.out' } })
        .from('.home-mark', { y: -10, autoAlpha: 0 })
        .from('.dropslot', { y: 18, autoAlpha: 0 }, '<0.08')
        .from('.home-side > *', { y: 12, autoAlpha: 0, stagger: 0.06 }, '<0.12')
    },
    { scope: root },
  )

  return (
    <main className="page home" ref={root}>
      <header className="home-head">
        <Wordmark className="home-mark" height={64} />
        <p className="home-tagline">Convert Minecraft builds between Java and Bedrock — and look at them in 3D first.</p>
      </header>

      <div className="home-grid">
        <section className="dropslot" aria-label="Open a file">
          <div className="dropslot-art" aria-hidden>
            <span className="slot" />
            <span className="slot slot--filled">
              <Icon name="cube" size={30} />
            </span>
            <span className="slot" />
          </div>
          <h1 className="dropslot-title">Drop a structure or a world here</h1>
          <p className="dropslot-sub">Anywhere on this window works. Nothing you open is ever changed.</p>
          <div className="dropslot-actions">
            <button className="btn btn-primary btn-lg" onClick={openFile}>
              <Icon name="file" />
              Choose a file
              <kbd>Ctrl O</kbd>
            </button>
            <button className="btn btn-secondary btn-lg" onClick={openFolder}>
              <Icon name="folder" />
              Java world folder
            </button>
          </div>
          <div className="format-rows">
            <FormatRow ids={JAVA} edition="java" />
            <FormatRow ids={BEDROCK} edition="bedrock" />
          </div>
        </section>

        <aside className="home-side">
          <div className="side-block">
            <div className="side-title">
              <Icon name="clock" size={16} />
              Recent
            </div>
            {recent && recent.length > 0 ? (
              <ul className="recent">
                {recent.map((path) => (
                  <li key={path}>
                    <button className="recent-item" onClick={() => send({ type: 'open', path })} title={path}>
                      <span className="recent-icon">
                        <Icon name={/\.[a-z]+$/i.test(path) ? 'file' : 'folder'} size={16} />
                      </span>
                      <span className="recent-text">
                        <span className="recent-name">{basename(path)}</span>
                        <span className="recent-dir">{dirname(path)}</span>
                      </span>
                      <Icon name="chevron" size={16} />
                    </button>
                  </li>
                ))}
              </ul>
            ) : (
              <p className="side-empty">Files you open show up here.</p>
            )}
          </div>
          <div className="side-block side-steps">
            <div className="side-title">How it works</div>
            <ol>
              <li>
                <b>Open</b> a file or world.
              </li>
              <li>
                <b>Preview</b> it in 3D if you like — orbit, fly, slice.
              </li>
              <li>
                <b>Convert</b> it for the other edition. You get a report of anything that changed.
              </li>
            </ol>
          </div>
          {assets && assets.viewer === false && (
            <div className="side-block side-note">
              <Icon name="info" size={16} />
              <span>The 3D preview is off on this computer (it needs a GPU with Vulkan 1.3). Opening and converting work normally.</span>
            </div>
          )}
          {assets && assets.viewer !== false && !assets.ok && (
            <div className="side-block side-note">
              <Icon name="info" size={16} />
              <span>
                The 3D preview uses your Minecraft Java {assets.java_version} textures, which were not found. Converting works without them;
                pick the client jar in Settings to enable the preview.
              </span>
            </div>
          )}
        </aside>
      </div>
    </main>
  )
}
