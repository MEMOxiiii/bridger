// App-wide keyboard shortcuts. e.code = physical keys, so they work with any layout (Arabic too).
// Ignored while typing in a field.
import { useEffect } from 'react'
import { send } from './bridge'
import { enterPreview, exitPreview } from './actions'
import { getState, setState } from './store'

export function useShortcuts() {
  useEffect(() => {
    const key = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement | null
      if (target && (target.tagName === 'INPUT' || target.tagName === 'SELECT' || target.isContentEditable)) return
      const s = getState()
      if (s.splash) return
      if (e.ctrlKey && e.code === 'KeyO') {
        e.preventDefault()
        send({ type: 'open_dialog', folder: e.shiftKey })
        return
      }
      if (e.key === 'Escape') {
        if (s.settingsOpen) setState({ settingsOpen: false })
        else if (s.reportOpen) return // the sheet closes itself (with its animation)
        else if (s.focusKey) {
          setState({ focusKey: null })
          send({ type: 'clear_focus' })
        } else if (s.view === 'preview') exitPreview()
        return
      }
      if (!s.doc || e.ctrlKey || e.altKey || e.metaKey || s.settingsOpen) return
      if (s.view !== 'preview') {
        if (e.code === 'KeyP') {
          e.preventDefault()
          enterPreview()
        }
        return
      }
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
}
