# Branches and releases

Wisp keeps three kinds of branch.

- **`main`** is the latest release. Every commit on it is a release and has
  an annotated tag, `v0.x.y`. Nothing is committed to `main` directly: it
  only moves forward to a `dev` commit that CI has passed, so CI does not
  run on `main` again.
- **`dev`** is where work gathers between releases. Every push to it runs
  the Linux workflow: GCC and Clang, Debug and Release, a build without the
  debug tools, and the address and undefined-behaviour sanitizers.
- **Streams**, `stream/<name>`, are where a feature or a fix is built. Each
  one branches from `dev` and holds one piece of work. CI does not run on a
  push to a stream; to check one on GitHub before it lands, start the Linux
  workflow on it by hand from the Actions tab.

History stays linear: a stream is rebased, never merged with a merge
commit, and `dev` and `main` only fast-forward.

## Landing a stream

1. Rebase the stream onto the latest `dev`.
2. Run the suite locally on it: `hamr run` for the fast tier, or
   `hamr run --tier nightly` for every configuration CI checks (see the
   Building section of the README).
3. Fast-forward `dev` to the stream and push `dev`.
4. Delete the stream, locally and on GitHub if it was pushed.

If CI fails on `dev`, fix it on `dev` (or on a new stream) before anything
else lands.

## Making a release

When `dev` has gathered enough for a release:

1. On `dev`, make one commit named `Changelog: 0.x.y` that sets
   `WISP_VERSION_MAJOR`, `WISP_VERSION_MINOR`, `WISP_VERSION_PATCH` and
   `WISP_VERSION_STRING` in `src/Wisp/Version.h` and adds the release's entry to
   the top of `CHANGELOG.md`. A release that changes or adds API raises the
   minor version; one with fixes only raises the patch.
2. Push `dev`, and wait for CI to pass on that commit.
3. Fast-forward `main` to it.
4. Tag it: an annotated `v0.x.y` whose message is the changelog entry,
   headed `wisp 0.x.y: <summary>`.
5. Push `main` and the tag. The tag starts the Release workflow: it checks
   the tag against the version header, builds Wisp, runs the suite,
   and publishes the GitHub release with the tag's message as its notes.

`main` then holds exactly the commit CI passed on `dev`.
