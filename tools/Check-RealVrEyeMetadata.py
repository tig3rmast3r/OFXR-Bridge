"""Check copied-forward REAL VR eyes against the existing OFXR marker trace."""

import argparse
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
import re


RECORD = re.compile(rb" op=eye_frame_metadata result=(-?\d+) dur_us=\d+ a=(\d+) b=(\d+) c=(\d+)")


@dataclass
class Release:
    slot: int | None = None
    render: dict[int, int] = field(default_factory=dict)
    stream: dict[int, int] = field(default_factory=dict)
    marker: dict[int, int] = field(default_factory=dict)


def check(path: Path) -> int:
    releases: dict[tuple[int, int], Release] = {}
    guide_decisions: dict[int, set[tuple[int, int]]] = defaultdict(set)
    tagged_guides: set[int] = set()
    with path.open("rb") as log:
        for line in log:
            event = RECORD.search(line)
            if event is None:
                continue
            result, chain, serial, value = map(int, event.groups())
            if result == 49:
                tagged_guides.add(value)
                continue
            if result in (42, 43, 44, 45, 46, 47, 48, 50, 51):
                guide_decisions[result].add((chain, serial))
                continue
            if result not in (0, 2, 3, 18, 19, 36, 37):
                continue
            release = releases.setdefault((chain, serial), Release())
            if result == 0:
                release.slot = value
            elif result in (2, 3):
                release.render[result - 2] = value
            elif result in (18, 19):
                release.stream[result - 18] = value
            else:
                release.marker[result - 36] = value

    previous: dict[tuple[int, int, int], tuple[int, Release]] = {}
    observed = copies = matches = disagreements = missing = 0
    for (chain, serial), current in sorted(releases.items()):
        if current.slot is None:
            continue
        for eye, render in current.render.items():
            if eye not in current.stream:
                continue
            observed += 1
            key = (chain, eye, current.stream[eye])
            prior = previous.get(key)
            previous[key] = (serial, current)
            if prior is None:
                continue
            old_serial, old = prior
            if old.slot == current.slot:
                continue
            older_marker = old.marker.get(eye)
            newer_marker = current.marker.get(eye)
            same_render = old.render[eye] == render
            if same_render:
                copies += 1
            if not older_marker or not newer_marker:
                missing += same_render
                continue
            same_marker = older_marker == newer_marker
            description = (
                f"eye {eye}, releases {old_serial} -> {serial}, slots {old.slot} -> {current.slot}, "
                f"render {old.render[eye]} -> {render}, marker {older_marker} -> {newer_marker}"
            )
            if same_render != same_marker:
                disagreements += 1
                if disagreements <= 5:
                    print(f"Disagreement: {description}")
            elif same_render:
                matches += 1
                if matches <= 5:
                    print(f"Confirmed: {description}")
    print(
        f"{observed} REAL VR eye receipts, {copies} same-render releases into a different slot, "
        f"{matches} confirmed by markers, {missing} without two markers, {disagreements} disagreements."
    )
    if guide_decisions or tagged_guides:
        print(
            f"Guide lookup by capture: {len(tagged_guides)} tagged guides, "
            f"{len(guide_decisions[51])} confirmed bindings, "
            f"{len(guide_decisions[42])} metadata/marker agreements, "
            f"{len(guide_decisions[45])} metadata recoveries on marker misses, "
            f"{len(guide_decisions[50])} of them ambiguous, "
            f"{len(guide_decisions[46])} metadata selections without a marker read, "
            f"{len(guide_decisions[43])} marker selections, {len(guide_decisions[44])} marker conflicts, "
            f"{len(guide_decisions[47])} render-ID and {len(guide_decisions[48])} owner mismatches during calibration."
        )
    if disagreements:
        return 1
    if matches == 0:
        print("Copy-forward check inconclusive. The trace needs two different slots carrying the same render and valid markers.")
        return 2
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("flight_log", type=Path, help="One OFXR flight log from the instrumented game")
    raise SystemExit(check(parser.parse_args().flight_log))
