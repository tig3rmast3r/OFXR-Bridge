# V961: common RIFE native handoff pacing

Ports the final V955 running-start handoff policy (`61640dfd`) onto nightly V960,
independently of the Radeon/Vulkan backend. It applies to both NVIDIA and Vulkan
RIFE when using the private SteamVR endpoint, including Framework and Director.
Other generators and native VDXR retain their existing policy.

Only a ready RIFE packet can learn a phase from a genuinely blocking native
Wait/Begin. Its successor enters after the observed running-start gate; it cannot
claim the previous image's native turn. No fixed scanout offset or second phase
hold is introduced. Without a qualifying observation, the existing native policy
continues. Period changes require a fresh observation. All-off packets explicitly
disable this handoff policy and preserve nightly's clean native admission.

The policy is useful to test below headset refresh because closely spaced native
End calls can otherwise be counted successfully while the compositor consumes
fewer images. Existing V955 owner results are encouraging; they do not establish
that every low-FPS problem, another generator or the integrated build is fixed.

This commit contains the scheduler, native endpoint wiring, bounded diagnostics
and independent clock/image regressions. It contains no Vulkan worker, compiler,
installer, model or CUDA changes. Shared-control ABI remains92501.
