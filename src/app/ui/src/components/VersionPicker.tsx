// Target game version for a conversion: a button that opens a list grouped by version line (26.x,
// 1.21, 1.20, ...), newest first. Keyboard: arrows move, Enter picks, Esc closes, typing a number jumps.

import { useEffect, useMemo, useRef, useState } from 'react'
import type { Edition, TargetVersion } from '../bridge'
import { Icon } from './Icon'

function lineOf(name: string) {
  const p = name.split('.')
  return p[0] === '1' ? `${p[0]}.${p[1]}` : `${p[0]}.x`
}

export function VersionPicker({
  edition,
  versions,
  value,
  onChange,
}: {
  edition: Edition
  versions: TargetVersion[]
  value: string
  onChange: (v: string) => void
}) {
  const [open, setOpen] = useState(false)
  const [active, setActive] = useState(0)
  const root = useRef<HTMLDivElement>(null)
  const list = useRef<HTMLUListElement>(null)
  const typed = useRef({ text: '', at: 0 })
  const current = versions.find((v) => v.name === value) ?? versions[0]
  const groups = useMemo(() => {
    const out: [string, TargetVersion[]][] = []
    for (const v of versions) {
      const l = lineOf(v.name)
      if (out.length === 0 || out[out.length - 1][0] !== l) out.push([l, []])
      out[out.length - 1][1].push(v)
    }
    return out
  }, [versions])

  useEffect(() => {
    if (!open) return
    setActive(Math.max(0, versions.findIndex((v) => v.name === value)))
    const close = (e: MouseEvent) => root.current && !root.current.contains(e.target as Node) && setOpen(false)
    window.addEventListener('mousedown', close)
    return () => window.removeEventListener('mousedown', close)
  }, [open, value, versions])

  useEffect(() => {
    if (!open || !list.current) return
    list.current.querySelector<HTMLElement>(`[data-index="${active}"]`)?.scrollIntoView({ block: 'nearest' })
  }, [active, open])

  const pick = (i: number) => {
    onChange(versions[i].name)
    setOpen(false)
  }
  const onKey = (e: React.KeyboardEvent) => {
    if (!open) {
      if (e.key === 'ArrowDown' || e.key === 'Enter' || e.key === ' ') {
        e.preventDefault()
        setOpen(true)
      }
      return
    }
    if (e.key === 'Escape') {
      e.preventDefault()
      e.stopPropagation()
      setOpen(false)
    } else if (e.key === 'ArrowDown') {
      e.preventDefault()
      setActive((a) => Math.min(versions.length - 1, a + 1))
    } else if (e.key === 'ArrowUp') {
      e.preventDefault()
      setActive((a) => Math.max(0, a - 1))
    } else if (e.key === 'Home' || e.key === 'End') {
      e.preventDefault()
      setActive(e.key === 'Home' ? 0 : versions.length - 1)
    } else if (e.key === 'Enter') {
      e.preventDefault()
      pick(active)
    } else if (/^[0-9.]$/.test(e.key)) {
      const now = Date.now()
      typed.current = { text: (now - typed.current.at < 900 ? typed.current.text : '') + e.key, at: now }
      const i = versions.findIndex((v) => v.name.startsWith(typed.current.text))
      if (i >= 0) setActive(i)
    }
  }

  if (!current) return null
  let index = -1
  return (
    <div className={`version-picker version-picker--${edition} ${open ? 'is-open' : ''}`} ref={root} onKeyDown={onKey}>
      <button className="version-button" aria-haspopup="listbox" aria-expanded={open} onClick={() => setOpen((o) => !o)}>
        <span className="version-button-edition">{edition === 'java' ? 'Java' : 'Bedrock'}</span>
        <span className="version-button-name mono">{current.name}</span>
        {current === versions[0] && <span className="version-tag">Latest</span>}
        <Icon name="chevron" size={16} />
      </button>
      {open && (
        <ul className="version-list" role="listbox" ref={list} aria-label="Game version" aria-activedescendant={`ver-${active}`}>
          {groups.map(([line, items]) => (
            <li key={line} role="presentation">
              <div className="version-line">{line}</div>
              <ul role="group" aria-label={line}>
                {items.map((v) => {
                  index += 1
                  const i = index
                  return (
                    <li
                      key={v.name}
                      id={`ver-${i}`}
                      data-index={i}
                      role="option"
                      aria-selected={v.name === current.name}
                      className={`version-option ${i === active ? 'is-active' : ''} ${v.name === current.name ? 'is-selected' : ''}`}
                      onMouseEnter={() => setActive(i)}
                      onClick={() => pick(i)}
                    >
                      <span className="mono">{v.name}</span>
                      {i === 0 && <span className="version-tag">Latest</span>}
                      <span className={`version-check version-check--${v.checked}`}>{v.checked === 'game' ? 'Tested in game' : 'Checked by data'}</span>
                    </li>
                  )
                })}
              </ul>
            </li>
          ))}
        </ul>
      )}
    </div>
  )
}
