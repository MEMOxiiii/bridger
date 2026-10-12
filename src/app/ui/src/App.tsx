import { useEffect, useRef } from 'react'
import { send } from './bridge'
import { Dock, Hud, TopBar } from './components/Chrome'
import { ConvertDrawer } from './components/ConvertDrawer'
import { EmptyState } from './components/EmptyState'
import { Inspector } from './components/Inspector'
import { DropOverlay, FlyHint, LoadingOverlay, SettingsDialog, Toasts } from './components/Overlays'
import { Viewport } from './components/Viewport'
import { dur, gsap, useGSAP } from './motion'
import { useStore } from './store'

export function App() {
  const doc = useStore((s) => s.doc)
  const connected = useStore((s) => s.connected)
  const loading = useStore((s) => s.loading)
  const fly = useStore((s) => s.fly)
  const drawer = useStore((s) => s.convert.open)
  const root = useRef<HTMLDivElement>(null)

  useEffect(() => send({ type: 'ready' }), [])

  // When a document opens: the chrome arrives in a short stagger (top, left, bottom).
  useGSAP(
    () => {
      if (!doc) return
      const tl = gsap.timeline({ defaults: { duration: dur(0.5), ease: 'power3.out' } })
      tl.from('.inspector', { x: -24, autoAlpha: 0 }).from('.dock', { y: 24, autoAlpha: 0 }, '<0.08').from('.hud', { autoAlpha: 0 }, '<0.1')
    },
    { dependencies: [doc?.path], scope: root },
  )

  return (
    <div className={`app ${doc ? 'has-doc' : 'no-doc'} ${fly ? 'is-flying' : ''} ${drawer ? 'drawer-open' : ''}`} ref={root}>
      {doc && <Viewport />}
      {!doc && !loading && connected && <EmptyState />}
      <TopBar />
      {doc && <Inspector />}
      {doc && <Dock />}
      <Hud />
      <ConvertDrawer />
      <LoadingOverlay />
      <FlyHint />
      <DropOverlay />
      <SettingsDialog />
      <Toasts />
    </div>
  )
}
