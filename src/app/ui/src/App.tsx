import { useEffect, useRef } from 'react'
import { send } from './bridge'
import { Splash } from './components/Brand'
import { Dock, Hud, TopBar } from './components/Chrome'
import { FilePage, ReportSheet } from './components/FilePage'
import { Home } from './components/Home'
import { Inspector } from './components/Inspector'
import { DropOverlay, FlyHint, LoadingOverlay, SettingsDialog, Toasts } from './components/Overlays'
import { Viewport } from './components/Viewport'
import { dur, gsap, useGSAP } from './motion'
import { useShortcuts } from './shortcuts'
import { useStore } from './store'

// Screens: splash -> home -> file <-> preview (store.ts).
export function App() {
  const doc = useStore((s) => s.doc)
  const view = useStore((s) => s.view)
  const splash = useStore((s) => s.splash)
  const fly = useStore((s) => s.fly)
  const root = useRef<HTMLDivElement>(null)
  const screen = !doc ? 'home' : view
  useShortcuts()

  useEffect(() => send({ type: 'ready' }), [])

  // Entering the preview: the page lifts away and the viewer chrome arrives.
  useGSAP(
    () => {
      if (screen !== 'preview') return
      gsap
        .timeline({ defaults: { duration: dur(0.45), ease: 'power3.out' } })
        .from('.inspector', { x: -24, autoAlpha: 0 })
        .from('.dock', { y: 24, autoAlpha: 0 }, '<0.06')
        .from('.hud', { autoAlpha: 0 }, '<0.1')
    },
    { dependencies: [screen], scope: root },
  )

  return (
    <div className={`app screen-${screen} ${fly ? 'is-flying' : ''}`} ref={root}>
      {screen === 'preview' && <Viewport />}
      {screen === 'home' && !splash && <Home />}
      {screen === 'file' && <FilePage />}
      {!splash && <TopBar />}
      {screen === 'preview' && <Inspector />}
      {screen === 'preview' && <Dock />}
      <Hud />
      {screen === 'file' && <ReportSheet />}
      <LoadingOverlay />
      <FlyHint />
      <DropOverlay />
      <SettingsDialog />
      <Toasts />
      {splash && <Splash />}
    </div>
  )
}
