# 0009. Plugin editors

- Status: accepted
- Date: 2026-11-15

## Context

Most plugins are used through their own editor, a window the plugin draws.
The engine hosts CLAP and VST 3 plugins (ADR 0006) but could not show those
windows, so the app could only offer generic knobs.

An editor lives in a native window the host makes: an HWND on Windows, an
NSView on macOS, an X11 window on Linux. The engine has no UI dependency
and does not make windows. The app (Qt) does, and can hand over a native
handle.

Editors change parameters themselves. In CLAP the plugin applies the change
and reports it. In VST 3 the edit controller asks the host
(`IComponentHandler::performEdit`), and the host must pass the value to the
audio processor, or the sound does not change. Editors also need the host's
main loop: CLAP plugins ask for timers and, on Linux, file descriptors.
VST 3 plugins on Linux ask for an `IRunLoop`.

## Decision

1. **`AudioProcessor` gets an editor API, on the control thread**
   (`hasEditor`, `openEditor(parent, listener)`, `closeEditor`,
   `isEditorOpen`, `editorSize`, `canResizeEditor`, `setEditorSize`).
   - `parent` is the platform's native window: an HWND, an NSView*, or an
     X11 Window id cast to a pointer. Only embedded editors are supported,
     not floating ones.
   - Built-in processors have no editor; the defaults say so.
2. **The host hears about the editor through an `EditorListener`:**
   - `editorResizeRequested`: the host resizes its window, then calls
     `setEditorSize()`;
   - `editorClosed`: the plugin closed the editor itself;
   - `parameterEdited`: the value is already applied, and the call is for
     the host's own view and its "project changed" state.
3. **Edits reach the sound without a round trip through the host:**
   - CLAP: the plugin applies its own edit. Output parameter events from
     `process()` go through a lock-free queue to the control thread, and
     `idle()` hands them to the listener. While the plugin is inactive,
     `idle()` runs a requested `params.flush()` and reports straight away.
   - VST 3: `performEdit` queues the value (lock-free, control thread to
     audio thread). The next `process()` passes it to the processor before
     the host's own changes for that block, so a host change in the same
     block wins.
4. **Host changes reach an open editor.** `PluginInstance::setParameter()`
   calls `AudioProcessor::parameterChangedByHost()`. The VST 3 adapter then
   calls `setParamNormalized()` on the controller. A CLAP plugin learns the
   value from its next `process()`.
5. **The plugins' main-loop needs run from `idle()`.**
   - `plugin_editor::RunLoop` holds timers and file descriptors:
     - CLAP `timer-support` and `posix-fd-support`;
     - VST 3 `IRunLoop`, which the frame offers on Linux.
   - `idle()` fires the due timers and polls the descriptors (with a
     timeout of 0) on POSIX systems. At most 64 timers and 64 descriptors
     each.
   - The host calls `idle()` from a UI timer, about 60 times a second while
     an editor is open.

## Alternatives considered

- **Generic editors only (knobs from `parameters()`).** These work for
  every plugin, but most plugins can't really be used without their own
  editor.
- **Floating editors that the plugin opens itself.** CLAP allows this, but
  VST 3 does not, and an editor that the app's window holds is easier to
  manage. They can be added if a plugin needs one.
- **Routing VST 3 edits through `PluginInstance::setParameter()`.** The
  adapter doesn't know its instance, and the app would then echo CLAP
  edits back to the plugin while the user is still dragging.

## Consequences

- The app needs a native window per open editor: a QWidget's `winId()`, or
  a QWindow embedded with `createWindowContainer`. On Linux that is X11
  only (XWayland under Wayland); Wayland editors are not supported.
- Editors run plugin code on the UI thread, with the same trust as loading
  the plugin (ADR 0006).
- The plugin's sizes are in pixels on Windows and Linux and in points on
  macOS. The app scales for high-DPI screens.
- Tests use windowless editors in the test plugins, so CI covers the
  protocol (open, resize, edits, timers) without a display.
- Not yet: floating editors, CLAP `resize_hints`, VST 3 `IPlugViewContentScaleSupport`, and
  CLAP gesture events (edits are reported without begin and end).
