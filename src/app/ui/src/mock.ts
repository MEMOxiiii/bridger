// Browser-only stand-in for bridger-gui.exe (`npm run dev`), so layouts and motion can be
// designed without the native app. Never bundled into a code path the app takes.
import type { Doc, Incoming, Outgoing, Settings, TargetVersion } from './bridge'
import { setConvert, setState } from './store'

const javaVersions: TargetVersion[] = ['26.3', '26.2', '26.1.2', '26.1', '1.21.11', '1.21.10', '1.21.4', '1.21.1', '1.20.6', '1.20.4', '1.20.1', '1.19.4', '1.18.2'].map(
  (name, i) => ({ name, checked: i === 0 || name === '1.20.1' ? 'game' : 'data' }),
)
const bedrockVersions: TargetVersion[] = ['26.50', '26.40', '26.30', '1.21.130', '1.21.100', '1.21.50', '1.21.0', '1.20.80', '1.20.0', '1.19.80', '1.18.30'].map((name, i) => ({
  name,
  checked: i === 0 ? 'game' : 'data',
}))
const settings: Settings = { client_jar: '', packs: [], render_distance: 12, distant_terrain: 32, recent: ['C:\\Builds\\castle.schem', 'C:\\Builds\\village.mcstructure'] }

const doc: Doc = {
  kind: 'structure',
  path: 'C:\\Builds\\castle.schem',
  name: 'castle.schem',
  format: 'schem',
  edition: 'java',
  size: [48, 32, 56],
  blocks: 18234,
  block_entities: 12,
  entities: 0,
  data_version: 3465,
  source_version: { edition: 'java', name: '1.20.1', detail: 'DataVersion 3465', exact: true, text: 'Java 1.20.1 (DataVersion 3465)' },
  warnings: [],
  report: {
    lossy_block: [
      { subject: 'minecraft:chiseled_bookshelf[slot_0_occupied=true]', count: 14, examples: [[3, 1, 4], [3, 2, 4]] },
      { subject: 'minecraft:oak_sign (text color)', count: 3, examples: [[10, 1, 2]] },
    ],
    unmapped_block_entity: [{ subject: 'minecraft:decorated_pot', count: 2, examples: [[7, 1, 7]] }],
  },
  quads: 52344,
  missing_models: 0,
  targets: [
    { id: 'mcworld', ext: '.mcworld', edition: 'bedrock', description: 'Bedrock world (zipped LevelDB)', versions: bedrockVersions },
    { id: 'mcstructure', ext: '.mcstructure', edition: 'bedrock', description: 'Bedrock structure file', versions: bedrockVersions },
    { id: 'schem', ext: '.schem', edition: 'java', description: 'Sponge schematic v1-v3', versions: javaVersions },
    { id: 'litematic', ext: '.litematic', edition: 'java', description: 'Litematica schematic', versions: javaVersions },
    { id: 'nbt', ext: '.nbt', edition: 'java', description: 'Java structure block file', versions: javaVersions },
  ],
}

export function handle(m: Outgoing, dispatch: (m: Incoming) => void) {
  const later = (ms: number, msg: Incoming) => setTimeout(() => dispatch(msg), ms)
  switch (m.type) {
    case 'ready':
      if (location.hash === '#splash') return // stays on the splash for screenshots
      later(50, {
        type: 'hello',
        version: '0.4.0-dev',
        gpu: 'Browser preview',
        assets: { ok: true, jar: '%APPDATA%\\.minecraft\\versions\\26.3\\26.3.jar', error: '', java_version: '26.3', bedrock_version: '26.52' },
        settings,
      })
      setInterval(() => dispatch({ type: 'stats', fps: 144, frame_ms: 0.6, sections: 84, quads: 52344, gpu_mb: 18.4 }), 500)
      demo(location.hash.slice(1), dispatch)
      break
    case 'open_dialog':
    case 'open':
      later(10, { type: 'loading', name: doc.name, step: 'Reading', fraction: 0.1 })
      later(400, { type: 'loading', name: doc.name, step: 'Building the 3D model', fraction: 0.6 })
      later(900, { type: 'opened', doc, settings })
      later(1300, { type: 'preview_ready', missing_models: 0 })
      break
    case 'pick_output':
      later(200, { type: 'output_picked', path: `C:\\Builds\\castle.${m.format}` })
      break
    case 'convert': {
      const steps = ['Reading', 'Placing', 'Translating blocks', 'Writing and validating']
      steps.forEach((step, i) => later(300 + i * 500, { type: 'convert_progress', fraction: (i + 1) / (steps.length + 1), step }))
      later(2600, {
        type: 'convert_done',
        summary: {
          input: doc.path, output: m.output, input_format: 'schem', output_format: m.format, exact: false, blocks: 18234,
          chunks: 12, subchunks: 30, block_entities: 12, origin: [0, -60, 0], spawn: [24, -28, -4], warnings: [], report: doc.report, ms: 412,
        },
      })
      break
    }
    default:
      break
  }
}

// Screenshot scripts for design review: index.html#doc, #report, #view, #convert, #running, #done, #failed, #settings, #fly, #drop.
function demo(name: string, dispatch: (m: Incoming) => void) {
  if (!name) return
  if (name === 'drop') return void setTimeout(() => dispatch({ type: 'drop', state: 'enter' }), 200)
  if (name === 'cmdline') {
    // A file passed on the command line: it opens while the splash is still up.
    setTimeout(() => dispatch({ type: 'opened', doc, settings }), 300)
    setTimeout(() => dispatch({ type: 'preview_ready', missing_models: 0 }), 600)
    return
  }
  setState({ splash: false })
  if (name === 'home') return
  if (name === 'settings') return void setTimeout(() => setState({ settingsOpen: true }), 300)
  setTimeout(() => dispatch({ type: 'opened', doc, settings }), 100)
  setTimeout(() => dispatch({ type: 'preview_ready', missing_models: 0 }), 150)
  const at = (ms: number, f: () => void) => setTimeout(f, ms)
  if (['preview', 'view', 'fly'].includes(name)) at(250, () => setState({ view: 'preview' }))
  if (name === 'sheet') at(400, () => setState({ reportOpen: true }))
  if (name === 'openmenu') at(600, () => (document.querySelector('.open-menu button') as HTMLElement | null)?.click())
  if (name === 'report' || name === 'view') at(400, () => (document.querySelector(`[data-tab="${name}"]`) as HTMLElement | null)?.click())
  if (name === 'report') at(700, () => (document.querySelector('.issue') as HTMLElement | null)?.click())
  if (name === 'report') at(250, () => setState({ view: 'preview' }))
  if (name === 'convert') at(500, () => setConvert({ output: 'C:\\Builds\\castle.mcworld' }))
  if (name === 'running') at(500, () => setConvert({ phase: 'running', fraction: 0.62, step: 'Translating blocks' }))
  if (name === 'failed') at(500, () => setConvert({ phase: 'failed', error: 'castle.schem: the structure reaches y 340, above the Bedrock build height (319). Choose "Move it" or "Cut it" under Advanced placement.' }))
  if (name === 'done')
    at(500, () =>
      dispatch({
        type: 'convert_done',
        summary: { input: doc.path, output: 'C:\\Builds\\castle.mcworld', input_format: 'schem', output_format: 'mcworld', exact: false, blocks: 18234, chunks: 12, subchunks: 30, block_entities: 12, origin: [0, -60, 0], spawn: [24, -28, -4], warnings: [], report: doc.report, ms: 412 },
      }),
    )
  if (name === 'fly') at(400, () => dispatch({ type: 'fly', on: true }))
}
