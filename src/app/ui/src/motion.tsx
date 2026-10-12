// Motion setup (gsap-core / gsap-react / gsap-performance skills, lenis-react skill):
// - one place for durations and eases: ease-out entrances (~200-300 ms), faster ease-in exits;
// - transforms and autoAlpha only (compositor-friendly);
// - prefers-reduced-motion: tweens collapse to near-instant fades, Lenis is not used;
// - Lenis instances run off GSAP's ticker (autoRaf: false), one clock for everything.
import gsap from 'gsap'
import { Flip } from 'gsap/Flip'
import { useGSAP } from '@gsap/react'
import { ReactLenis, type LenisRef } from 'lenis/react'
import { useEffect, useRef, useState, type ReactNode } from 'react'

gsap.registerPlugin(useGSAP, Flip)
gsap.defaults({ duration: 0.28, ease: 'power3.out' })
gsap.ticker.lagSmoothing(0)

export const EASE_IN = 'power2.in'
export const EASE_OUT = 'power3.out'
export const EASE_EMPHASIS = 'expo.out'

const reducedQuery = window.matchMedia('(prefers-reduced-motion: reduce)')

export function prefersReducedMotion() {
  return reducedQuery.matches
}

export function useReducedMotion() {
  const [reduced, setReduced] = useState(reducedQuery.matches)
  useEffect(() => {
    const on = () => setReduced(reducedQuery.matches)
    reducedQuery.addEventListener('change', on)
    return () => reducedQuery.removeEventListener('change', on)
  }, [])
  return reduced
}

// Duration helper: motion shrinks to a quick fade for reduced motion.
export function dur(seconds: number) {
  return prefersReducedMotion() ? 0.01 : seconds
}

export { gsap, Flip, useGSAP }

// A vertically scrolling area with Lenis smooth wheel scrolling (native scroll when reduced).
export function SmoothScroll({ className, children }: { className?: string; children: ReactNode }) {
  const reduced = useReducedMotion()
  const lenisRef = useRef<LenisRef>(null)
  useEffect(() => {
    if (reduced) return
    const update = (time: number) => lenisRef.current?.lenis?.raf(time * 1000)
    gsap.ticker.add(update)
    return () => gsap.ticker.remove(update)
  }, [reduced])
  if (reduced) return <div className={`scroll ${className ?? ''}`}>{children}</div>
  return (
    <ReactLenis
      ref={lenisRef}
      className={`scroll ${className ?? ''}`}
      options={{ autoRaf: false, lerp: 0.14, smoothWheel: true, wheelMultiplier: 0.9 }}
    >
      {children}
    </ReactLenis>
  )
}

// Animated number (counts up when the value changes; gsap snap keeps it integral).
export function Counter({ value, format = (n: number) => Math.round(n).toLocaleString() }: { value: number; format?: (n: number) => string }) {
  const ref = useRef<HTMLSpanElement>(null)
  const current = useRef({ v: 0 })
  useGSAP(
    () => {
      gsap.to(current.current, {
        v: value,
        duration: dur(0.6),
        ease: 'power2.out',
        onUpdate: () => {
          if (ref.current) ref.current.textContent = format(current.current.v)
        },
      })
    },
    { dependencies: [value] },
  )
  return <span ref={ref}>{format(current.current.v)}</span>
}
