# 0005. Immutable snapshots for state the audio thread reads

- Status: accepted
- Date: 2026-11-12

## Context

From M1 the audio thread reads structured state: channels with their
samples, gain, pan, mute and step patterns. The control thread edits it all
the time (painting steps, loading samples) and the edits can be large (a new
sample is megabytes). The command queue (`core/SpscQueue`) only carries small
trivially copyable values, and the audio thread must never lock, allocate or
free (docs/coding-guidelines.md).

Sound must also survive edits: toggling a step must not cut a kick that is
still ringing, and removing a channel should fade its voices out, not stop
them dead.

## Decision

1. **Snapshots.** The control thread builds a complete, immutable snapshot of
   the state and publishes it with `core/SnapshotExchange<T>`: one atomic
   pointer exchange. If the audio thread has not picked up the previous one
   yet, the newer one replaces it (latest wins) and the control thread
   deletes the skipped one itself.
2. **Draining.** When the audio thread switches to a new snapshot, the old one
   is kept in a fixed-size draining list until no sound depends on it, then
   handed back through an SPSC queue. Only the control thread deletes
   snapshots (`collect()`), so destructors never run on the audio thread, and
   the audio thread never touches a `shared_ptr` reference count.
3. **Voices outlive snapshots.** Playback state (a channel's `dsp/Sampler`
   with its voice pool) is not part of a snapshot's value: it is an object
   created on the control thread when the channel is created, and every
   snapshot containing that channel points at the same object. Edits
   therefore never reset voices.
4. **When a draining snapshot is done.** A snapshot can go once, for each of
   its channels, either its sample is also in the current snapshot (still
   owned there) or that channel's sampler no longer `uses()` it. Samplers of
   channels missing from the current snapshot (deleted channels) keep being
   rendered from the draining snapshot until their voices have faded.

Small, frequent controls (play, stop, tempo, master gain) stay on the command
queue.

## Alternatives considered

- **Mutex around shared state**: the audio thread could wait on the UI
  thread, causing dropouts. Rejected by the real-time rules.
- **Commands for every edit** (toggle step, set gain...): fine for small
  values, but samples and pattern resizes need allocation, and the audio side
  would have to mirror the whole data model.
- **`std::atomic<std::shared_ptr>`**: not lock-free on the main standard
  libraries, and the last reference could be dropped (and the object freed)
  on the audio thread.
- **Epoch / hazard pointers**: more general, more code; nothing here needs
  more than one reader.

## Consequences

- Every edit copies the snapshot's top level (the channel list and their
  steps; samples are shared). Cheap at DAW scale (hundreds of channels), and
  edits arrive at UI speed, not audio speed.
- The control thread must call `collect()` regularly (publishing does it).
- At most `MaxDraining` (16) snapshots can wait for voices at once; beyond
  that a new snapshot waits one block or more. It only happens when samples
  are replaced faster than old voices finish.
