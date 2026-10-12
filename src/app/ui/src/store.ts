// App state: one small external store (useSyncExternalStore), fed by bridge messages.
import { useSyncExternalStore } from 'react'
import { onMessage, type Assets, type Doc, type Settings, type Stats, type Summary } from './bridge'

export type ConvertPhase = 'setup' | 'running' | 'done' | 'failed'

export interface ConvertState {
  open: boolean
  phase: ConvertPhase
  target: string | null
  output: string | null
  intoWorld: string | null
  fraction: number
  step: string
  summary: Summary | null
  error: string | null
  cancelled: boolean
}

export interface Toast {
  id: number
  kind: 'info' | 'error' | 'success'
  message: string
}

export interface State {
  connected: boolean
  version: string
  gpu: string
  assets: Assets | null
  settings: Settings | null
  doc: Doc | null
  loading: { name: string; step: string; fraction: number } | null
  stats: Stats | null
  fly: boolean
  map: boolean
  slice: number | null
  dropping: boolean
  focusKey: string | null // the report issue whose positions are highlighted
  convert: ConvertState
  settingsOpen: boolean
  toasts: Toast[]
}

const initialConvert: ConvertState = {
  open: false, phase: 'setup', target: null, output: null, intoWorld: null, fraction: 0, step: '', summary: null, error: null, cancelled: false,
}

let state: State = {
  connected: false, version: '', gpu: '', assets: null, settings: null, doc: null, loading: null, stats: null, fly: false, map: false,
  slice: null, dropping: false, focusKey: null, convert: initialConvert, settingsOpen: false, toasts: [],
}

const subscribers = new Set<() => void>()

export function getState() {
  return state
}

export function setState(patch: Partial<State> | ((s: State) => Partial<State>)) {
  const p = typeof patch === 'function' ? patch(state) : patch
  state = { ...state, ...p }
  for (const s of subscribers) s()
}

export function setConvert(patch: Partial<ConvertState>) {
  setState((s) => ({ convert: { ...s.convert, ...patch } }))
}

export function resetConvert(open: boolean) {
  setState({ convert: { ...initialConvert, open } })
}

let toastId = 0
export function toast(kind: Toast['kind'], message: string) {
  const id = ++toastId
  setState((s) => ({ toasts: [...s.toasts.slice(-3), { id, kind, message }] }))
  setTimeout(() => dismissToast(id), kind === 'error' ? 8000 : 4500)
}

export function dismissToast(id: number) {
  setState((s) => ({ toasts: s.toasts.filter((t) => t.id !== id) }))
}

export function useStore<T>(select: (s: State) => T): T {
  return useSyncExternalStore(
    (cb) => {
      subscribers.add(cb)
      return () => subscribers.delete(cb)
    },
    () => select(state),
  )
}

onMessage((m) => {
  switch (m.type) {
    case 'hello':
      setState({ connected: true, version: m.version, gpu: m.gpu, assets: m.assets, settings: m.settings })
      break
    case 'settings':
      setState({ settings: m.settings, assets: m.assets })
      break
    case 'loading':
      setState({ loading: { name: m.name, step: m.step, fraction: m.fraction } })
      break
    case 'opened':
      setState({ doc: m.doc, settings: m.settings, loading: null, map: false, slice: null, fly: false, focusKey: null, convert: initialConvert })
      break
    case 'open_failed':
      setState({ loading: null })
      toast('error', m.message)
      break
    case 'stats': {
      const { type: _type, ...stats } = m
      void _type
      setState({ stats })
      break
    }
    case 'fly':
      setState({ fly: m.on, map: m.on ? false : state.map })
      break
    case 'drop':
      setState({ dropping: m.state === 'enter' })
      break
    case 'output_picked':
      if (m.path) setConvert({ output: m.path })
      break
    case 'into_world_picked':
      setConvert({ intoWorld: m.path })
      break
    case 'convert_progress':
      setConvert({ phase: 'running', fraction: m.fraction, step: m.step })
      break
    case 'convert_done':
      setConvert({ phase: 'done', fraction: 1, summary: m.summary })
      break
    case 'convert_failed':
      setConvert({ phase: m.cancelled ? 'setup' : 'failed', error: m.message, cancelled: m.cancelled })
      if (m.cancelled) toast('info', 'Conversion cancelled. Nothing was written.')
      break
    case 'toast':
      toast(m.kind, m.message)
      break
  }
})
