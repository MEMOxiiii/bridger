// Display names for the format ids the app sends (src/formats/format.cpp, kFormats).
import type { Edition } from './bridge'

export interface FormatMeta {
  label: string
  short: string
  edition: Edition
  blurb: string
}

export const FORMATS: Record<string, FormatMeta> = {
  mcworld: { label: 'Bedrock world', short: '.mcworld', edition: 'bedrock', blurb: 'A new world you can open in Minecraft Bedrock.' },
  mcstructure: { label: 'Bedrock structure', short: '.mcstructure', edition: 'bedrock', blurb: 'Load it with a structure block in Bedrock.' },
  schem: { label: 'Sponge schematic', short: '.schem', edition: 'java', blurb: 'For WorldEdit and most Java tools.' },
  litematic: { label: 'Litematica', short: '.litematic', edition: 'java', blurb: 'For the Litematica mod.' },
  nbt: { label: 'Structure block', short: '.nbt', edition: 'java', blurb: 'Load it with a structure block in Java.' },
  schematic: { label: 'MCEdit schematic', short: '.schematic', edition: 'java', blurb: 'Legacy MCEdit format.' },
  'java-world': { label: 'Java world', short: 'folder', edition: 'java', blurb: 'A world folder for Minecraft Java.' },
}

export function formatMeta(id: string): FormatMeta {
  return FORMATS[id] ?? { label: id, short: id, edition: 'java', blurb: '' }
}

export const ISSUE_LABELS: Record<string, { title: string; hint: string; tone: 'warning' | 'danger' | 'info' }> = {
  unmapped_block: { title: 'Blocks with no equivalent', hint: 'Replaced by the fallback block.', tone: 'danger' },
  lossy_block: { title: 'Blocks converted approximately', hint: 'Some properties have no counterpart.', tone: 'warning' },
  unmapped_block_entity: { title: 'Block data not converted', hint: 'The block is placed; its contents are not.', tone: 'warning' },
  unmapped_entity: { title: 'Entities not converted', hint: 'Mobs and items are not written yet.', tone: 'info' },
  unmapped_biome: { title: 'Biomes with no equivalent', hint: 'The default biome is used.', tone: 'info' },
  dropped_secondary_layer: { title: 'Second-layer blocks dropped', hint: 'Usually water inside a block.', tone: 'warning' },
  removed_block_entity: { title: 'Existing block data replaced', hint: 'Overwritten in the target world.', tone: 'warning' },
  removed_entity: { title: 'Entities cleared', hint: 'Removed because "clear entities" was on.', tone: 'info' },
  clipped_block: { title: 'Blocks outside the build height', hint: 'Cut off by the clip option.', tone: 'danger' },
}

export function issueLabel(kind: string) {
  return ISSUE_LABELS[kind.replace(/ /g, '_')] ?? { title: kind, hint: '', tone: 'info' as const }
}

export function issueTotal(report: Record<string, { count: number }[]>) {
  let n = 0
  for (const list of Object.values(report)) for (const i of list) n += i.count
  return n
}

export function basename(path: string) {
  const parts = path.split(/[\\/]/)
  return parts[parts.length - 1] || path
}

export function dirname(path: string) {
  const i = Math.max(path.lastIndexOf('\\'), path.lastIndexOf('/'))
  return i > 0 ? path.slice(0, i) : ''
}
