# Jam patch stack

This repository tracks upstream WebKit directly. Jam-specific engine changes are kept as small, independent commits above the upstream `main` branch:

1. module-registry inspection and exact eviction;
2. per-global queued-task discard;
3. typed per-job embedder ownership;
4. lazy error-information materialization;
5. named-export synthetic module construction;
6. complete static JSCOnly archives, packaging, and release automation.

To update the fork, fetch `WebKit/WebKit`, rebase the patch stack onto its `main` branch, run the focused `JavaScriptCore_JamEmbedderHooks` API tests, and dispatch the `Jam WebKit` workflow for the rebased commit. The workflow builds five native static payloads, verifies their SHA-256 checksums, and publishes them under the immutable `autobuild-<commit>` release tag.

The generated archives retain Jam's existing names and `jam-webkit/` layout, so a Jam checkout can switch builds by changing only its WebKit source and commit pin.
