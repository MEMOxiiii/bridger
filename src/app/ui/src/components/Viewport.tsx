// Transparent layer over the 3D scene that turns pointer input into camera messages.
// Left drag: orbit (pan in map mode) · right/middle drag or Shift+left: pan · wheel: zoom ·
// double-click: enter fly mode. Deltas are batched to one message per animation frame.
import { useEffect, useRef } from 'react'
import { send } from '../bridge'
import { getState, setState } from '../store'

export function Viewport() {
  const ref = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const el = ref.current
    if (!el) return
    let mode: 'orbit' | 'pan' | null = null
    let dx = 0
    let dy = 0
    let wheel = 0
    let frame = 0

    const flush = () => {
      frame = 0
      if (mode && (dx !== 0 || dy !== 0)) send({ type: mode, dx, dy })
      if (wheel !== 0) send({ type: 'zoom', delta: wheel })
      dx = dy = wheel = 0
    }
    const schedule = () => {
      if (!frame) frame = requestAnimationFrame(flush)
    }
    const down = (e: PointerEvent) => {
      if (!getState().doc) return
      const pan = e.button === 1 || e.button === 2 || (e.button === 0 && (e.shiftKey || getState().map))
      mode = pan ? 'pan' : 'orbit'
      el.setPointerCapture(e.pointerId)
      el.classList.add('is-dragging')
    }
    const move = (e: PointerEvent) => {
      if (!mode) return
      dx += e.movementX
      dy += e.movementY
      schedule()
    }
    const up = (e: PointerEvent) => {
      flush()
      mode = null
      if (el.hasPointerCapture(e.pointerId)) el.releasePointerCapture(e.pointerId)
      el.classList.remove('is-dragging')
    }
    const onWheel = (e: WheelEvent) => {
      if (!getState().doc) return
      e.preventDefault()
      wheel += -e.deltaY / 100
      schedule()
    }
    const dbl = () => {
      if (getState().doc && !getState().map) send({ type: 'fly', on: true })
    }
    const menu = (e: Event) => e.preventDefault()
    el.addEventListener('pointerdown', down)
    el.addEventListener('pointermove', move)
    el.addEventListener('pointerup', up)
    el.addEventListener('pointercancel', up)
    el.addEventListener('wheel', onWheel, { passive: false })
    el.addEventListener('dblclick', dbl)
    el.addEventListener('contextmenu', menu)
    return () => {
      cancelAnimationFrame(frame)
      el.removeEventListener('pointerdown', down)
      el.removeEventListener('pointermove', move)
      el.removeEventListener('pointerup', up)
      el.removeEventListener('pointercancel', up)
      el.removeEventListener('wheel', onWheel)
      el.removeEventListener('dblclick', dbl)
      el.removeEventListener('contextmenu', menu)
    }
  }, [])

  // Keyboard shortcuts (ignored while typing in a field).
  useEffect(() => {
    const key = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement | null
      if (target && (target.tagName === 'INPUT' || target.tagName === 'SELECT' || target.isContentEditable)) return
      const s = getState()
      // e.code: physical keys, so the shortcuts work with any keyboard layout (Arabic too).
      if (e.ctrlKey && e.code === 'KeyO') {
        e.preventDefault()
        send({ type: 'open_dialog', folder: e.shiftKey })
        return
      }
      if (e.key === 'Escape') {
        if (s.settingsOpen) setState({ settingsOpen: false })
        else if (s.convert.open && s.convert.phase !== 'running') setState({ convert: { ...s.convert, open: false } })
        else if (s.focusKey) {
          setState({ focusKey: null })
          send({ type: 'clear_focus' })
        }
        return
      }
      if (!s.doc || e.ctrlKey || e.altKey || e.metaKey) return
      switch (e.code) {
        case 'KeyF':
          send({ type: 'fly', on: true })
          break
        case 'KeyM': {
          const map = !s.map
          setState({ map })
          send({ type: 'view', map })
          break
        }
        case 'KeyR':
          send({ type: 'reset_camera' })
          break
        default:
          return
      }
      e.preventDefault()
    }
    window.addEventListener('keydown', key)
    return () => window.removeEventListener('keydown', key)
  }, [])

  return <div ref={ref} className="viewport" aria-label="3D view. Drag to orbit, right-drag to pan, wheel to zoom, double-click to fly." />
}
