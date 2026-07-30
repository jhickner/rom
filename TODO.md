# TODO

- [x] doom and wolfenstein cores (prboom + ecwolf, auto-fetched)
- [x] skip transmission of unchanged frames
- [x] NEON base64 in the render thread (6.1-6.9x, byte-identical)

Not done, per the measurements below: video_convert and video_upscale. The
render thread is no longer the bottleneck anyway now that unchanged frames are
never transmitted.

- [ ] these simd optimizations:

Yes — three functions are cleanly vectorizable, and I wrote and measured a NEON version of the biggest one. But the measurements say SIMD is the wrong place to spend effort. Here's why.

Per-frame cost, SNES 256×224 at 3x zoom (M3, -O2)

┌─────────────────────────────────────┬───────┬───────────────────────┐
│                Stage                │ Cost  │     Vectorizable?     │
├─────────────────────────────────────┼───────┼───────────────────────┤
│ video_convert 565→RGB24 + recolor   │ 0.09  │ yes, awkwardly (LUT   │
│ LUT (video.c:41)                    │ ms    │ gather)               │
├─────────────────────────────────────┼───────┼───────────────────────┤
│ video_upscale 3x (video.c:89)       │ 0.15  │ yes, easily           │
│                                     │ ms    │                       │
├─────────────────────────────────────┼───────┼───────────────────────┤
│ memcpy in renderer_submit           │ 0.02  │ already is            │
│                                     │ ms    │                       │
├─────────────────────────────────────┼───────┼───────────────────────┤
│ b64enc (kitty.c:13)                 │ 0.44  │ yes, cleanly          │
│                                     │ ms    │                       │
├─────────────────────────────────────┼───────┼───────────────────────┤
│ write() to the pty                  │ 5.61  │ no — it's a syscall   │
│                                     │ ms    │                       │
└─────────────────────────────────────┴───────┴───────────────────────┘

I wrote a NEON b64enc (vld3q_u8 to de-interleave, branchless offset ladder, vst4q_u8 to re-interleave) and verified it byte-for-byte identical to the scalar one across five frame sizes. It's 3.6x faster — 0.44 ms → 0.12 ms at 3x, and 3.61 ms → 1.06 ms at fullscreen retina.

That 0.32 ms saving is ~5% of what the render thread spends, and that thread already runs concurrently with emulation, so it's not on the frame-time critical path at all.
