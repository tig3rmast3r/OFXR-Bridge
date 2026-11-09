# V928: held AER polls, native guides and D11 publication

Based on V927, preserving normal Framework/Director and generator choices.
AER remains Advanced, Swap remains temporal phase only, 2X remains the limit.

An application poll with neither eye freshly released closes its virtual frame
without accepting a new native image. It preserves generation history and the
last coherent stereo output. It no longer bypasses to raw staggered eyes and
re-primes the next source. Holds are handled before optional guide admission.
Actual source releases are still the authority; recopied pixels are not detected.

Private native D11 AER publication uses its existing release-readiness GPU fence
instead of blocking the producer on a second CPU publication wait. The private
consumer still waits before reading/copying the image. Native image ownership,
copy dependencies, source leases and interop reuse fences remain unchanged.

FSR, DLSS-G and Hybrid can consume exact split-eye Luke DLSS guides in AER with
reconstruction off. Producer origin marking is enabled for those consumers;
plain OFXR/RIFE and all-off gain no marker work. Each changed colour capture is
joined to its protected CRC token and exact same-eye producer history. Held
source associations are cached; bounded marker timeouts retain exact GPU fences.
The synthesis queue waits the guide producer receipts, including composer inputs.
No newest-by-size, serial-parity or cross-eye guide substitution is admitted.

Raw Luke vectors must have a producer-confirmed two-native-frame span (N2),
matching same-eye colour history. Raw N1 vectors need actual composition first;
DLSS presence alone cannot authorize them. Ambiguous packed-eye layouts remain
unavailable to native AER guide joining. Missing/invalid inputs keep real delivery
and show waiting; provider/hardware/runtime restrictions still apply. Native DLSS-G
requires its installed compatible provider; selectable is not provider acceptance.

F9 sidecar now retains held decisions (9230 fresh0), generation-prepare and
continuity reasons, actual End entry5718 and native Begin5743. Rate source13 means
accepted scene submissions, not physical headset scanout. Inspect per-source
mixed outputs and native receipts together; a 90 End/s total alone is insufficient.

Compare the same scene and size with Framework/Director and Buffer. Fixture
checks do not establish actual headset FPS, latency or quality acceptance.
