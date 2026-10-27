# Contributing to Redstream

Issues and design discussion are welcome on GitHub. If you use AI coding agents, point them at
[`AGENTS.md`](AGENTS.md) first: it has the project's hard rules, build and test instructions.

## Licence

By contributing you agree that your contribution is licensed under the project's licences: LGPL-2.1-or-later for
everything outside `samples/`, MIT for `samples/` (see [`LICENSE`](LICENSE) and [`samples/LICENSE`](samples/LICENSE)).
New files start with an `SPDX-License-Identifier` line.

## Developer Certificate of Origin

Every commit must be signed off, certifying the [Developer Certificate of Origin 1.1](https://developercertificate.org/):
that you wrote the change or otherwise have the right to submit it under the project's licences. Add the line with
`git commit -s`:

```
Signed-off-by: Your Name <you@example.com>
```

Use your real name. There is no contributor licence agreement.

## Clean-room rule

Redstream is written independently from public documentation. Do not contribute code, headers, samples or
documentation copied from the NVIDIA DeepStream SDK, and do not paste text from its proprietary files into issues.
Compatible names and behaviour come from NVIDIA's public documentation and from observing DeepStream as a black box.

## Changes

- Build and test in the dev container (`AGENTS.md`, "Build and test"); warnings are errors.
- Write the failing test first, and register new tests in `tests/meson.build`.
- Keep changes small and in the style of the surrounding code.
- Do not commit models or media: tests download public assets into the git-ignored `.cache/`.
