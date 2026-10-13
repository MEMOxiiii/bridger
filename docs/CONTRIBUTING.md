# Contributing to Bridger

Thanks for your interest! Please read [AGENTS.md](../AGENTS.md) first. It has the build commands, architecture,
coding standards, and the safety and performance rules every change must follow.

## Ground rules

- **Data safety first.** Never modify input files. Write output via temp file → validate → atomic replace. Never
  drop data silently. See [docs/safety.md](safety.md).
- **Cite format details.** Every binary format detail needs a source in `docs/formats/`. If the sources are
  unclear, verify in the game. Never guess.
- **Measure performance claims.** Optimizations come with before/after benchmark numbers.
- **Licensing.** Do not copy code or data from projects with incompatible licenses (for example custom
  non-commercial licenses). Every new dependency needs a one-line justification and an entry in
  [THIRD_PARTY.md](THIRD_PARTY.md). Never commit Mojang assets.

## Workflow

1. Open an issue for anything non-trivial, so the design can be discussed first. Major decisions get an ADR in
   `docs/adr/`.
2. Keep pull requests small and focused, and keep the build green.
3. Before submitting:
   - `cmake --build --preset msvc-debug` builds with no warnings.
   - `ctest --preset msvc-asan` passes.
   - Run `clang-format` on changed files.
   - Update the docs: `docs/formats/`, README support matrix, `AGENTS.md` if commands or layout changed.
4. New formats follow the "How to add a new format" checklist in AGENTS.md, **including the in-game check**.

## Reporting bugs

Include the Bridger version (`bridger --version`), the exact command, the conversion report if one was produced,
and, if you can share it, a small input file that reproduces the problem.

By contributing, you agree that your contributions are licensed under the [MPL-2.0](../LICENSE).
