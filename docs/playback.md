# Playback

Every frame the rig evaluates is kept, so playing or scrubbing back over a
frame doesn't evaluate it again.

## How the cache fills

A frame is keyed by every channel value. A lookup compares the whole key,
so an edit to any channel simply misses: nothing is ever invalidated, and a
stale frame is never shown.

A few times a second, the plug-in checks whether a recent evaluation
missed the cache. If one did, it works out the frames of the playback range:
it reads the animated channels at each frame, and channels with no
animation hold their current value. Background threads then evaluate the
missing frames, nearest the current frame first, each with its own copy of
the rig. The rig's own copy stays free for your edits.

## Settings

| Variable | Default | |
| --- | --- | --- |
| `RIGEXEC_MAYA_FILL_WORKERS` | An eighth of the hardware threads, at most 8 | Background threads filling the cache. `0` turns them off. |
| `RIGEXEC_MAYA_CACHE_FRAMES` | 2000 (about 1 GB for the biped) | Frames kept per rig. The least recently used go first. |

## Checking the cache

```mel
rigExecCache -wait Biped_stack_rigExec;
// Result: 120 0 //
```

`rigExecCache` returns how many frames are cached and how many are still
pending. With `-wait`, it first waits until the background threads are done.
