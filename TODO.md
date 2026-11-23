# Publication checklist

Nothing here is done unless ticked. Nothing has been pushed anywhere.

- [x] **Legal basis.** Decided: the release zip includes the built `eboot.bin` so players only add the game data, which
      Epic has released for free. No licence is attached to the port's own files. See `LICENSING.md`.
- [x] **Artwork.** Decided: the Unreal Tournament icon and background stay in (`art/`).
- [ ] **Build reproducibility.** The engine tree has no pinned remote: it is the ut99dc source inside a local UT99-Android
      checkout. Pin it (a commit, or a document of what to fetch) once the legal question is settled.
- [ ] **Repository.** `git init`, one commit, the remote, the `v1.0.0` tag and release with
      `dist/UT99-PS5-v1.0.0.zip` (made by `tools/package-release.sh`; it holds the eboot and the patched menus, no game data). Ask before pushing.
- [ ] **Check on the console what the Status table in the README says is unverified**, or leave it as written.

Never commit: game files, `data/` (settings and logs), `dist/`, `build/`, `.DS_Store`.
