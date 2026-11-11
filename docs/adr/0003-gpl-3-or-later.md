# 0003. License: GPL-3.0-or-later

- Status: accepted
- Date: 2026-11-11

## Context

Vloom Audio software is 100% open source, funded by donations and support.
We want improvements to stay open.

## Decision

All code in vloom-engine and VloomDAW is licensed GPL-3.0-or-later.
Contributions are accepted under the same licence (inbound = outbound, no CLA).

## Alternatives considered

- **LGPL-3.0 / MPL-2.0 for the engine** — would let closed-source programs use
  the engine. Can be revisited before outside contributions arrive. After that
  every contributor would have to agree.
- **MIT** — maximises adoption but allows closed forks.

## Consequences

- Dependencies must be GPL-3.0-compatible (MIT, BSD, zlib, MPL-2.0, LGPL, Apache-2.0 are fine).
- Anyone shipping a modified VloomDAW must publish their source.
