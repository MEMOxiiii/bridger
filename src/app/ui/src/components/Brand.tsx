// The Bridger wordmark and the launch splash.
import { useRef } from 'react'
import wordmarkUrl from '../assets/wordmark.png'
import { gsap, prefersReducedMotion, useGSAP } from '../motion'
import { setState, useStore } from '../store'

export function Wordmark({ height, className }: { height?: number; className?: string }) {
  return <img className={`wordmark ${className ?? ''}`} src={wordmarkUrl} alt="Bridger" height={height} draggable={false} />
}

// Shown once at launch: the wordmark rises out of its own shadow like a block being placed, a line
// sweeps under it, then everything clears for the home screen. Any click skips it. It waits for the
// app to answer (`connected`) and lasts at least ~1.4 s so it never flickers.
export function Splash() {
  const root = useRef<HTMLDivElement>(null)
  const connected = useStore((s) => s.connected)
  const intro = useRef<gsap.core.Timeline | null>(null)
  const leaving = useRef(false)

  const leave = () => {
    if (leaving.current || !root.current) return
    leaving.current = true
    const reduce = prefersReducedMotion()
    gsap
      .timeline({ onComplete: () => setState({ splash: false }) })
      .to('.splash-mark', { y: reduce ? 0 : -24, autoAlpha: 0, duration: reduce ? 0.15 : 0.35, ease: 'power2.in' })
      .to('.splash-meta', { autoAlpha: 0, duration: 0.2, ease: 'power2.in' }, 0)
      .to(root.current, { autoAlpha: 0, duration: reduce ? 0.15 : 0.3, ease: 'power2.in' }, reduce ? 0 : 0.15)
  }

  useGSAP(
    () => {
      const reduce = prefersReducedMotion()
      const tl = gsap.timeline({ defaults: { ease: 'power3.out' } })
      intro.current = tl
      if (reduce) {
        tl.from('.splash-mark, .splash-meta', { autoAlpha: 0, duration: 0.2 }).to({}, { duration: 0.6 })
      } else {
        tl.from('.splash-mark', { y: 46, scale: 0.94, autoAlpha: 0, duration: 0.7, ease: 'back.out(1.5)' })
          .from('.splash-shadow', { scaleX: 0.3, autoAlpha: 0, duration: 0.6 }, '<0.05')
          .from('.splash-line', { scaleX: 0, duration: 0.7, ease: 'expo.out' }, '-=0.25')
          .from('.splash-meta > *', { y: 8, autoAlpha: 0, stagger: 0.07, duration: 0.4 }, '-=0.45')
          .to({}, { duration: 0.35 })
      }
    },
    { scope: root },
  )

  // Leave once the intro played and the app is connected.
  useGSAP(
    () => {
      const tl = intro.current
      if (!tl || !connected) return
      if (tl.progress() >= 1) leave()
      else tl.eventCallback('onComplete', leave)
    },
    { dependencies: [connected], scope: root },
  )

  return (
    <div className="splash" ref={root} onClick={() => connected && leave()} role="presentation">
      <div className="splash-stage">
        <Wordmark className="splash-mark" />
        <div className="splash-shadow" aria-hidden />
        <div className="splash-line" aria-hidden />
        <div className="splash-meta">
          <span>Java</span>
          <span className="splash-swap" aria-hidden>
            ⇄
          </span>
          <span>Bedrock</span>
          <span className="splash-dot" aria-hidden />
          <span className="muted">block for block</span>
        </div>
      </div>
    </div>
  )
}
