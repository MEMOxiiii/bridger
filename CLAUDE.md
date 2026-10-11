@AGENTS.md

## Claude-specific notes

- Work one phase at a time. At the end of a phase, run all tests and benchmarks, summarize what was done,
  what was measured and what is risky, then **stop for review**. Never start the next phase without approval.
- Build commands need the MSVC environment. From a plain shell, wrap them:
  `cmd /c "call \"%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat\" && cmake --build --preset msvc-debug"`.
- Use the Windows-native path form for build-output paths in commands (`build\msvc-debug\bin\...`).
