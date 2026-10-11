# Release process

Ship early and often. A release claims only what has passed its acceptance criteria and in-game checks.

| Release | After phase | Contents |
|---|---|---|
| **0.1.0 (first public release)** | Phase 4 | CLI only: `.schem` → new `.mcworld`, pinned versions, conversion report. Portable zip. |
| 0.2.0 | Phase 5 | More Java formats, `.mcstructure`, placing into existing worlds |
| 0.3.0 | Phase 6 | Bedrock → Java |
| 0.4.0 | Phases 7 + 8 | 3D viewer and desktop app |
| 1.0.0 | Phase 9 | More versions, hardening, installer |

## Checklist (every release)

1. [ ] All CI jobs green on `master`.
2. [ ] Version bumped in `CMakeLists.txt` (`project(... VERSION x.y.z)`) **and** `vcpkg.json` (`version-semver`).
3. [ ] `CHANGELOG.md` updated (Added / Changed / Fixed / Known limitations).
4. [ ] Format support matrix updated in `README.md` **and** `src/formats/format.cpp`; the two agree.
5. [ ] In-game checklist run recorded in `docs/testing.md` for every writer in the release, on the pinned
       versions.
6. [ ] `docs/performance.md` baselines up to date; the README quotes only measured numbers.
7. [ ] `THIRD_PARTY.md` complete; license texts bundled in the zip (`licenses/`).
8. [ ] Portable zip built from the CI release build: `bridger.exe`, licenses, README. **No `vulkan-1.dll`**
       (the GPU driver provides the loader).
9. [ ] Tag `vX.Y.Z` on the release commit, built by CI; the GitHub release has the zip and changelog section.
