# Desktop app bridge: messages between the UI and `bridger-gui.exe`

The web UI (`src/app/ui`) and the native app (`src/app/app.cpp`) exchange JSON objects with a `type` field
(`docs/adr/0009-hybrid-desktop-ui.md`). TypeScript types: `src/app/ui/src/bridge.ts`. Paths are UTF-8 strings.
Unknown types and malformed messages are ignored on both sides.

## UI → app

| `type` | Fields | Effect |
|---|---|---|
| `ready` | | The page is loaded. The app answers with `hello` and delivers messages queued before. |
| `orbit` | `dx`, `dy` (pixels) | Orbit the camera (smoothed). |
| `pan` | `dx`, `dy` (pixels) | Pan; in map mode moves the map. |
| `zoom` | `delta` (wheel steps, > 0 = in) | Zoom; in fly mode changes the speed. |
| `reset_camera` | | Animate back to the whole document. |
| `preview` | `on` | Show or hide the 3D preview. The app renders and streams worlds only while it is on; the file page covers the window otherwise. |
| `fly` | `on` | Enter fly mode, in the preview only (the app leaves it on Esc or when the window loses focus). |
| `view` | `map?` (bool), `slice?` (number or `null`) | Top-down map; hide blocks at and above `slice`. |
| `focus` | `positions` ([[x,y,z], ...]) | Outline these blocks and fly the camera to them (report highlights). |
| `clear_focus` | | Remove the outlines. |
| `open` | `path` | Open a structure file or world. |
| `open_dialog` | `folder` | Show the open-file (or folder) dialog, then open the choice. |
| `pick_output` | `format` (format name) | Save dialog for that format (folder dialog for `java-world`); answered by `output_picked`. |
| `pick_into_world` | | Open dialog for a `.mcworld` to place into; answered by `into_world_picked`. |
| `convert` | `output`, `format`, `level_name`, `air` (`keep`/`replace`), `height` (`fail`/`clip`/`shift`), `clear_entities`, `place_at?`, `into_world?`, `overwrite` | Start a conversion (`convert::convert`) on a worker thread. |
| `cancel_convert` | | Ask the running conversion to stop. Nothing is written when it stops (`ConvertOptions::cancel`). |
| `reveal` | `path` | Show the file in Explorer. |
| `pick_client_jar`, `add_pack` | | Dialogs for the textures' client jar / a resource pack. |
| `settings` | `client_jar?`, `packs?`, `render_distance?` | Change and save settings (`%APPDATA%/Bridger/settings.json`). |

## App → UI

| `type` | Fields | Meaning |
|---|---|---|
| `hello` | `version`, `gpu`, `assets`, `settings` | Answer to `ready`. |
| `settings` | `settings`, `assets` | Settings or assets changed. |
| `loading` | `name`, `step`, `fraction` | Opening progress. |
| `opened` | `doc`, `settings` | A document is open; the UI shows its file page. `doc`: `kind` (`structure`/`world`), `path`, `name`, `format`, `edition`, `size`, `blocks`, `block_entities`, `entities`, `data_version`, `chunks`, `spawn`, `seed`, `warnings`, `report`, `targets`. |
| `preview_ready` | `missing_models` | The 3D model of the open document is built (structures mesh after `opened`; worlds are ready at once and stream on demand). |
| `open_failed` | `message` | |
| `stats` | `fps`, `frame_ms`, `sections`, `quads`, `gpu_mb`, `chunks_loaded?`, `chunks_total?`, `chunks_pending?` | Twice a second. `fps` counts rendered frames: an idle viewer renders none. |
| `fly` | `on` | Fly mode changed. |
| `drop` | `state` (`enter`/`leave`) | Files dragged over the window (the app opens what is dropped). |
| `output_picked` | `path` or `null` | Save dialog result. |
| `into_world_picked` | `path` | |
| `convert_progress` | `fraction`, `step` | |
| `convert_done` | `summary` | The `--report` JSON (`convert::summary_to_json`) plus `report` (issues grouped by kind, at most 500 subjects each, largest first), `ms`, `output`. |
| `convert_failed` | `message`, `cancelled` | |
| `toast` | `kind` (`info`/`error`/`success`), `message` | |

`report` objects map an issue kind (`translate::to_string(IssueKind)`, e.g. `"lossy block"`) to a list of
`{subject, count, examples: [[x,y,z], ...]}`. Example positions are in the document's coordinates, so the UI
can pass them straight to `focus`.
