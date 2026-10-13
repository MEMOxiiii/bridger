// Navigation between the file page and the 3D preview (the app renders only while previewing).
import { send } from './bridge'
import { getState, setState } from './store'

export function enterPreview() {
  const s = getState()
  if (!s.doc || !s.previewReady) return
  setState({ view: 'preview', reportOpen: false })
  send({ type: 'preview', on: true })
}

export function exitPreview() {
  if (getState().view !== 'preview') return
  setState({ view: 'file', fly: false })
  send({ type: 'preview', on: false })
}

// Outline a report entry's blocks and fly there; opens the preview when needed. Clicking the active
// entry again clears it.
export function showIssue(key: string, positions: [number, number, number][]) {
  const s = getState()
  if (s.focusKey === key || positions.length === 0) {
    setState({ focusKey: null })
    send({ type: 'clear_focus' })
    return
  }
  setState({ focusKey: key })
  send({ type: 'focus', positions })
  if (s.view !== 'preview') enterPreview()
}

export function openFile() {
  send({ type: 'open_dialog', folder: false })
}

export function openFolder() {
  send({ type: 'open_dialog', folder: true })
}
