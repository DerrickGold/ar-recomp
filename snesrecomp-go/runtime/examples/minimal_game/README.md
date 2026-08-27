# Minimal linked-game example

`game_module.c` is a compile-checked example of the smallest valid linked game:
immutable identity and execution tables, registration before runner creation,
opaque runner access, capability/extent checks, generation acquisition, and an
authentic-width begin-frame policy.

Replace `ExampleRunFrame` with the recompiled game's frame entry point. Add
lifecycle, state-provider, and audio tables only when the project implements
their corresponding module capability. Widescreen projects should extend
`ExampleBeginAuthenticFrame` using the ordered workflow in
`../../docs/GAME_ENHANCEMENT_INTEGRATION.md`.

The standalone runtime build compiles this source using only the public include
root. It intentionally does not link or run because a real example executable
also needs generated game code and a user-supplied ROM.
