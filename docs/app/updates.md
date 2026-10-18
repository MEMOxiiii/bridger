# Updates

The desktop app (`bridger-gui.exe`) can update itself (`src/app/updater.cpp`, `src/app/app.cpp`,
`src/app/ui/src/components/UpdateCard.tsx`). The command-line tool does not.

## When it looks

- **Automatically, at most once a day**, when the app starts and **Settings → Updates → "Check for a new version once
  a day"** is on (the default). Turning it off stops every automatic request.
- **"Check now"** in the same section asks at once and also says when Bridger is up to date.

A check is one HTTPS `GET https://api.github.com/repos/MEMOxiiii/bridger/releases/latest` (WinHTTP, Windows' own HTTP
client; the request carries only the usual headers and a `Bridger` user agent: no identifiers, no usage data). The
answer is GitHub's release JSON. Draft and pre-releases are never offered; a version the user skipped is not offered
again automatically (Settings still shows it).

## What it offers

A card at the bottom right: the new version, the first lines of its release notes, the download size and three
choices: **Download and install**, **Skip this version**, or close it (it comes back at the next check). Nothing is
downloaded before the user presses Download.

## How it installs

1. **Download.** Only the asset `bridger-<version>-windows-x64.zip` of this repository's release is fetched, and only
   from `https://github.com/MEMOxiiii/bridger/releases/download/…` (GitHub redirects to its download host; redirects to
   plain http are refused). At most 512 MB is accepted. Cancel stops it and nothing is kept.
2. **Verify.** The zip's SHA-256 must match the digest GitHub reports for the asset in the release JSON
   (`"digest": "sha256:…"`, a separate request to `api.github.com`); releases that publish
   `bridger-<version>-windows-x64.zip.sha256` (sha256sum format, written by `.github/workflows/release.yml`) are checked
   against that file when GitHub gives no digest. **Without a checksum the update is refused** and the card offers the
   release page instead.
3. **Unpack** into `.bridger-update/` next to the program (same volume, so the final moves are renames). Every name in
   the zip is checked first: no absolute paths, no `..`, everything inside one folder, and that folder must contain
   `bridger-gui.exe`. A zip that fails any check is not unpacked at all.
4. **Swap**, when the user presses **Restart now** (or when Bridger closes, if they keep working): each file of the new
   version is moved into the program folder; an existing file is first renamed to `<name>.old` (Windows allows renaming
   a running program). If any step fails, every file already moved is put back and the error is shown: the program
   folder is then exactly as before.
5. **Restart.** The new `bridger-gui.exe` starts and the old one quits. At the next start the app deletes the `.old`
   files it made (they are listed in `.bridger-update-old.txt`; nothing else in the folder is touched) and
   `.bridger-update/`.

A restart is refused while a conversion runs. Settings, recent files, the texture cache and every user file live
outside the program folder (`%APPDATA%/Bridger`) and are never touched by an update.

## Limits

- The program folder must be writable (the portable zip unpacked somewhere the user owns). Where it is not (for
  example under `Program Files`), the download step fails with that message and the card offers the release page.
- The new program is not signed; Windows SmartScreen may ask once when it first runs, as with a manual download.
- The bridge messages are listed in `docs/app/bridge.md`.
