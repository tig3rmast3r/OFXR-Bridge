# V973: Preserve Luke execution ownership and discover both producer factories

Pair private allocations with the device reported by their retained execution queue, verified through backing ownership. Discover queue implementations through both the allocation and observed device factories, pin the observed factory identity, and learn the producer only from an actual submitted SR list. Retained Luke guide snapshots accept proven aliases while exact token, chronology, queue and fence checks remain mandatory. Route changes can retry failed history initialization.

Validation: Real GPU queue/fence, allocation/observed-factory, ownership-negative, protected-token and capture-rebuild checks pass. User confirmed GTA V, Cyberpunk 2077, Spider-Man, Ghost of Tsushima and Kingdom Come.

The full test suite was deliberately not run at the owner's request.
This change was accepted in local builds on the V971 baseline.
The complete V972-V977 assembly is built and packaged as V977.
