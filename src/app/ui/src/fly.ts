// Fly mode input when the host leaves it to the page (Linux and macOS, docs/adr/0010; `hello.fly_input` is "page").
// The page locks the pointer and sends the mouse deltas (`look`) and the held keys (`fly_keys`); keys are read by
// KeyboardEvent.code, the physical key, so WASD works on any keyboard layout. Without pointer lock (refused, or a
// Wayland compositor without pointer constraints) the view turns while a mouse button is held. On Windows the app
// reads the mouse and keys itself and none of this runs.
import { send } from './bridge'

let pageInput = false
let active = false
let locked = false
let dx = 0
let dy = 0
let wheel = 0
let frame = 0
const held = new Set<string>()

export function setFlyInput(mode: string | undefined) {
  pageInput = mode === 'page'
}

// Starts fly mode. Called from the click or key press itself: pointer lock is only granted during a user gesture.
export function requestFly() {
  if (pageInput) void Promise.resolve(document.documentElement.requestPointerLock?.()).catch(() => {})
  send({ type: 'fly', on: true })
}

// The app confirmed fly mode on or off (the `fly` message).
export function flyChanged(on: boolean) {
  if (!pageInput || on === active) return
  active = on
  if (on) start()
  else stop()
}

const keyMap: Record<string, string> = {
  KeyW: 'forward',
  KeyS: 'back',
  KeyA: 'left',
  KeyD: 'right',
  Space: 'up',
  ShiftLeft: 'down',
  ShiftRight: 'down',
  ControlLeft: 'fast',
}

function sendKeys() {
  const keys: Record<string, boolean> = {}
  for (const code of held) if (keyMap[code]) keys[keyMap[code]] = true
  send({ type: 'fly_keys', keys })
}

function flush() {
  frame = 0
  if (dx !== 0 || dy !== 0) send({ type: 'look', dx, dy })
  if (wheel !== 0) send({ type: 'zoom', delta: wheel })
  dx = dy = wheel = 0
}

function onMove(e: MouseEvent) {
  if (!locked && e.buttons === 0) return // without pointer lock: look while a button is held
  dx += e.movementX
  dy += e.movementY
  if (!frame) frame = requestAnimationFrame(flush)
}

function onWheel(e: WheelEvent) {
  e.preventDefault()
  wheel += -e.deltaY / 100
  if (!frame) frame = requestAnimationFrame(flush)
}

function onKey(e: KeyboardEvent) {
  if (e.code === 'Escape') {
    e.preventDefault()
    e.stopPropagation() // not the shortcut that leaves the preview
    if (e.type === 'keydown') send({ type: 'fly', on: false })
    return
  }
  if (!keyMap[e.code]) return
  e.preventDefault()
  e.stopPropagation()
  const had = held.has(e.code)
  if (e.type === 'keydown') held.add(e.code)
  else held.delete(e.code)
  if (had !== held.has(e.code)) sendKeys()
}

function onLockChange() {
  const now = document.pointerLockElement != null
  // Losing the lock (Esc, which the browser keeps for itself, or switching windows) ends fly mode.
  if (locked && !now && active) send({ type: 'fly', on: false })
  locked = now
}

function onBlur() {
  if (active) send({ type: 'fly', on: false })
}

function start() {
  locked = document.pointerLockElement != null
  document.addEventListener('mousemove', onMove, true)
  document.addEventListener('wheel', onWheel, { capture: true, passive: false })
  document.addEventListener('keydown', onKey, true)
  document.addEventListener('keyup', onKey, true)
  document.addEventListener('pointerlockchange', onLockChange)
  window.addEventListener('blur', onBlur)
}

function stop() {
  document.removeEventListener('mousemove', onMove, true)
  document.removeEventListener('wheel', onWheel, true)
  document.removeEventListener('keydown', onKey, true)
  document.removeEventListener('keyup', onKey, true)
  document.removeEventListener('pointerlockchange', onLockChange)
  window.removeEventListener('blur', onBlur)
  cancelAnimationFrame(frame)
  frame = 0
  dx = dy = wheel = 0
  held.clear()
  sendKeys()
  locked = false
  if (document.pointerLockElement) document.exitPointerLock()
}
