// JSON messages between the page and bridger-gui.exe (src/app/app.cpp, on_message / send).
// In a plain browser (`npm run dev`) a small mock answers, so the UI can be designed without the app.

export type Edition = 'java' | 'bedrock'

export interface Target {
  id: string // format name: mcworld, mcstructure, schem, litematic, nbt, java-world
  ext: string
  edition: Edition
  description: string
}

export interface Issue {
  subject: string
  count: number
  examples: [number, number, number][]
}

export type Report = Record<string, Issue[]>

export interface Doc {
  kind: 'structure' | 'world'
  path: string
  name: string
  file_name?: string
  format: string
  edition: Edition
  size?: [number, number, number]
  blocks?: number
  block_entities?: number
  entities?: number
  data_version?: number | null
  chunks?: number
  spawn?: [number, number, number]
  seed?: number | null
  warnings: string[]
  report: Report
  quads?: number
  missing_models?: number
  targets: Target[]
}

export interface Assets {
  ok: boolean
  viewer?: boolean // false: no usable GPU, the 3D preview is off (converting still works)
  viewer_error?: string
  jar: string
  error: string
  java_version?: string
  bedrock_version?: string
}

export interface Settings {
  client_jar: string
  packs: string[]
  render_distance: number
  recent: string[]
}

export interface Stats {
  fps: number
  frame_ms: number
  sections: number
  quads: number
  gpu_mb: number
  chunks_loaded?: number
  chunks_total?: number
  chunks_pending?: number
}

export interface Summary {
  input: string
  output: string
  input_format: string
  output_format: string
  exact: boolean
  blocks: number
  chunks: number
  subchunks: number
  block_entities: number
  origin?: [number, number, number]
  spawn?: [number, number, number]
  warnings: string[]
  report: Report
  ms: number
}

export type Incoming =
  | { type: 'hello'; version: string; gpu: string; assets: Assets; settings: Settings }
  | { type: 'settings'; settings: Settings; assets: Assets }
  | { type: 'loading'; name: string; step: string; fraction: number }
  | { type: 'opened'; doc: Doc; settings: Settings }
  | { type: 'preview_ready'; missing_models: number }
  | { type: 'open_failed'; message: string }
  | { type: 'stats' } & Stats
  | { type: 'fly'; on: boolean }
  | { type: 'drop'; state: 'enter' | 'leave' }
  | { type: 'output_picked'; path: string | null }
  | { type: 'into_world_picked'; path: string }
  | { type: 'convert_progress'; fraction: number; step: string }
  | { type: 'convert_done'; summary: Summary }
  | { type: 'convert_failed'; message: string; cancelled: boolean }
  | { type: 'toast'; kind: 'info' | 'error' | 'success'; message: string }

export interface ConvertRequest {
  output: string
  format: string
  level_name: string
  air: 'keep' | 'replace'
  height: 'fail' | 'clip' | 'shift'
  clear_entities: boolean
  place_at?: [number, number, number]
  into_world?: string
  overwrite: boolean
}

export type Outgoing =
  | { type: 'ready' }
  | { type: 'orbit'; dx: number; dy: number }
  | { type: 'pan'; dx: number; dy: number }
  | { type: 'zoom'; delta: number }
  | { type: 'reset_camera' }
  | { type: 'fly'; on: boolean }
  | { type: 'preview'; on: boolean }
  | { type: 'view'; map?: boolean; slice?: number | null }
  | { type: 'focus'; positions: [number, number, number][] }
  | { type: 'clear_focus' }
  | { type: 'open'; path: string }
  | { type: 'open_dialog'; folder: boolean }
  | { type: 'pick_output'; format: string }
  | { type: 'pick_into_world' }
  | ({ type: 'convert' } & ConvertRequest)
  | { type: 'cancel_convert' }
  | { type: 'reveal'; path: string }
  | { type: 'pick_client_jar' }
  | { type: 'add_pack' }
  | { type: 'settings'; client_jar?: string; packs?: string[]; render_distance?: number }

interface WebView {
  postMessage(message: unknown): void
  addEventListener(type: 'message', listener: (e: { data: unknown }) => void): void
}

const webview: WebView | undefined = (window as unknown as { chrome?: { webview?: WebView } }).chrome?.webview

type Listener = (m: Incoming) => void
const listeners = new Set<Listener>()

export function onMessage(listener: Listener): () => void {
  listeners.add(listener)
  return () => listeners.delete(listener)
}

function dispatch(m: Incoming) {
  for (const l of listeners) l(m)
}

if (webview) webview.addEventListener('message', (e) => dispatch(e.data as Incoming))

export function send(message: Outgoing) {
  if (webview) webview.postMessage(message)
  else void import('./mock').then((mock) => mock.handle(message, dispatch))
}

export const inApp = webview !== undefined
if (!inApp) document.documentElement.classList.add('preview')
