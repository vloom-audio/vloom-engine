# 0007. Mixer routing and delay compensation

- Status: accepted
- Date: 2026-11-14

## Context

Until M4 the mixer was flat. Each channel played into one insert, and each
insert played into the master. M5 adds two things that change this:

- **Sends.** An insert sends a copy of its sound to another insert, such as
  a reverb or a bus. FL Studio also lets an insert route its output to
  another insert instead of the master.
- **Plugin delay compensation (PDC).** A plugin with latency (a look-ahead
  limiter, a linear-phase EQ) delays the sound on its path. Without
  compensation that path comes late against the others and against the
  metronome. The CLAP host already reports each plugin's latency (ADR 0006).

Both work on the same structure: a graph of inserts that is built from the
routes. Compensation needs the whole graph to know how far to delay each
path. So both are designed together.

The real-time rules still hold (docs/coding-guidelines.md). The audio
thread must not allocate, free or lock. Routing must not cut ringing sound
when the user edits the mixer.

## Decision

1. **Routing lives in `InsertState`.** Each insert has two kinds of route:
   - `output`: where it plays after volume and pan. 0 is the master,
     1..kMaxInserts is another insert, and anything else means nowhere
     (sends only).
   - `sends`: up to kMaxSends `Send { target, gain, preFader }`. A pre-fader
     send is taken after the effects and before volume and pan, so muting
     the insert does not mute it. A post-fader send is taken after volume
     and pan.

   Channels still pick their insert with `ChannelState::insert`.
2. **The engine compiles the state into a `MixerGraph` on the control
   thread.** `Engine::setMixerState()` builds it, and the graph is what the
   snapshot exchange (ADR 0005) hands over. It holds:
   - the processing order: every insert comes after all the inserts that
     play into it;
   - the routes, grouped by source;
   - the compensation delays.

   The audio thread walks the order and makes no decisions. Sources are
   always processed before their targets, so a whole block passes through
   the graph in one pass.
3. **Loops are left out.** Routes are added in insert order, each insert's
   output before its sends. A route that would let an insert reach itself is
   ignored, and so is a route to an invalid insert. The result is
   deterministic. An insert whose only route closed a loop plays nowhere.
4. **Delay compensation lines paths up at every node where they meet.**
   - For each insert, in graph order, the engine works out when its input is
     lined up (`arrive`). That is the latest `ready` time of anything
     routed into it. Its own `ready` time is `arrive` plus the latency of
     its effects.
   - A route from S to T is delayed by `arrive[T] - ready[S]`.
   - The direct input of an insert or of the master (channels, and the
     metronome at the master) is delayed by `arrive` of that node.
   - The master's `arrive` is the engine's latency
     (`Engine::latencyFrames()`). The metronome is delayed by it too, so it
     stays in time with what is heard. The preview is not delayed: it is
     not part of the song.
5. **Latency is read on the control thread.**
   - `PluginInstance::latencyFrames()` is the processor's latency at
     activation, capped at kMaxLatencyFrames (65536). An inactive instance
     counts as 0.
   - The graph is compensated when it is built. `Engine::prepare()`
     compensates again after re-activating plugins at a new rate.
   - A plugin whose latency changes needs a new published state. This
     happens only through the host, until CLAP latency-change notifications
     are wired up.
6. **Bypass keeps the latency.** A bypassed instance delays its dry sound by
   its latency (`DelayLine`), so bypassing does not shift the path and the
   compensation stays right.
7. **Delay lines are allocated on the control thread and shared across
   graphs.**
   - Each route keeps a `MixerRouteState`: its delay line, plus the gain it
     last applied, so send changes ramp.
   - Each node keeps an input delay line.
   - A new graph reuses the previous graph's state for the same route (same
     insert, slot, target and kind), and the previous input lines, when they
     are long enough. An edit then neither cuts what a delay holds nor jumps
     a send's gain.
   - The audio thread processes an insert while a delay line still holds
     sound, so tails come out even after the input stops.
8. **Export leaves out the lead-in.** `exportSong()` drops the first
   `latencyFrames()` frames it renders, so the file starts with the music,
   not with the compensation's silence.

## Consequences

- With no plugin latency, every delay is 0 and no delay line is allocated.
  The cost is a few branches per insert per block.
- An insert's sound passes through one post-fader scratch buffer before its
  routes. A delayed route also goes through a second scratch buffer.
- Memory for compensation grows with the latency: 2 × (delay + 1) floats
  per delayed route or input. A graph holds at most
  `MixerGraph::kMaxCompensationFrames` (8 Mi frames, 64 MiB) in all; inputs
  are served first, then routes in order, and a path past the cap is left
  uncompensated rather than allocating without bound. The metronome's line
  has room for kMaxLatencyFrames from the start.
- Building a graph allocates: about 125 routes plus a reachability search
  per route. That is fine for UI edits. A volume drag publishes a graph per
  change, as it did with the flat state.
- Not yet:
  - instrument plugins' latency on channels (not compensated);
  - latency changes reported by plugins while running (the host republishes);
  - sidechain inputs;
  - a send's own pan.
