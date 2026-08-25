#!/usr/bin/env python3
"""Summarize authentic or extended native-audio request outcomes by sound ID.

The serial trace distinguishes genuine transport/lane loss, duplicate
coalescing, setting suppression, intentional image cancellation, and native
same-request retriggers.  This tool keeps those categories separate while
building the per-ID drop census used by the native-audio investigation.

Usage:
  python3 tools/analyze_native_audio_trace.py runs/20260824-172558
  python3 tools/analyze_native_audio_trace.py runs/latest --transitions
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
from pathlib import Path


GENUINE_DROP_OUTCOMES = (
    "overwritten_mailbox",
    "overwritten_port",
    "rejected_dual_busy",
    "replaced_lane",
    "extended_fifo_overflow",
)

RETRIGGER_OUTCOME = "retriggered_lane"

DUPLICATE_OUTCOMES = (
    "coalesced_mailbox_duplicate",
    "coalesced_port_duplicate",
    "coalesced_extended_duplicate",
)

DELIBERATE_OUTCOMES = (
    "suppressed_setting",
    "canceled_song_transition",
)

UNRESOLVED_OUTCOMES = (
    "pending",
    "pending_at_shutdown",
    "active_at_shutdown",
)


def load_requests(run: Path) -> list[dict[str, str]]:
    path = run / "native_audio_requests.csv"
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    # Captures predating the explicit retriggered_lane outcome called every
    # new-serial lane restart replaced_lane. Normalize those traces when the
    # replacement has the same request kind and ID, so historical censuses do
    # not overstate different-sound loss.
    by_serial = {row["serial"]: row for row in rows}
    for row in rows:
        if row["outcome"] != "replaced_lane":
            continue
        replacement = by_serial.get(row["replaced_by"])
        if replacement and effect_key(replacement) == effect_key(row):
            row["outcome"] = RETRIGGER_OUTCOME
    return rows


def integer(row: dict[str, str], field: str) -> int:
    value = row.get(field, "")
    return int(value) if value else 0


def effect_key(row: dict[str, str]) -> tuple[str, str]:
    return row["kind"], row["id"].upper()


def count_outcomes(rows: list[dict[str, str]]) -> Counter[str]:
    return Counter(row["outcome"] for row in rows)


def print_overview(rows: list[dict[str, str]]) -> None:
    outcomes = count_outcomes(rows)
    genuine = sum(outcomes[name] for name in GENUINE_DROP_OUTCOMES)
    duplicates = sum(outcomes[name] for name in DUPLICATE_OUTCOMES)
    deliberate = sum(outcomes[name] for name in DELIBERATE_OUTCOMES)
    unresolved = sum(outcomes[name] for name in UNRESOLVED_OUTCOMES)
    retriggers = outcomes[RETRIGGER_OUTCOME]
    print(
        f"requests={len(rows)} completed={outcomes['completed']} "
        f"genuine_drops={genuine} duplicates={duplicates} "
        f"retriggered={retriggers} deliberate={deliberate} "
        f"unresolved={unresolved}"
    )
    print("outcomes: " + " ".join(
        f"{name}={count}" for name, count in outcomes.most_common()
    ))


def print_effect_table(rows: list[dict[str, str]]) -> None:
    grouped: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["outcome"] not in DELIBERATE_OUTCOMES:
            grouped[effect_key(row)].append(row)

    headers = (
        "kind", "id", "posts", "done", "mbox", "port", "busy", "lane",
        "fifo", "dupes", "restart", "open", "rekeys", "music-skips",
    )
    print("\n" + " ".join(f"{header:>11}" for header in headers))
    for key in sorted(grouped):
        group = grouped[key]
        outcomes = count_outcomes(group)
        values = (
            key[0], key[1], len(group), outcomes["completed"],
            outcomes["overwritten_mailbox"], outcomes["overwritten_port"],
            outcomes["rejected_dual_busy"], outcomes["replaced_lane"],
            outcomes["extended_fifo_overflow"],
            sum(outcomes[name] for name in DUPLICATE_OUTCOMES),
            outcomes[RETRIGGER_OUTCOME],
            sum(outcomes[name] for name in UNRESOLVED_OUTCOMES),
            sum(integer(row, "native_lane_retriggers") for row in group),
            sum(integer(row, "music_updates_suppressed") for row in group),
        )
        print(" ".join(f"{str(value):>11}" for value in values))


def print_sites(rows: list[dict[str, str]]) -> None:
    sites = Counter(
        (
            row["kind"], row["id"].upper(), row["site"].upper(),
            row["caller"], row["outcome"],
        )
        for row in rows
        if row["outcome"] not in DELIBERATE_OUTCOMES
    )
    print("\nsites:")
    for (kind, effect_id, site, caller, outcome), count in sorted(sites.items()):
        print(
            f"{count:5d} {kind} ${effect_id} ${site} "
            f"{outcome:30s} {caller}"
        )


def print_transitions(rows: list[dict[str, str]]) -> None:
    by_serial = {row["serial"]: row for row in rows}
    transitions: Counter[tuple[str, str, str, str]] = Counter()
    for row in rows:
        if row["outcome"] not in ("replaced_lane", RETRIGGER_OUTCOME):
            continue
        replacement = by_serial.get(row["replaced_by"])
        if replacement:
            transitions[
                effect_key(row) + effect_key(replacement)
            ] += 1
    print("\nlane replacement/retrigger transitions:")
    for (old_kind, old_id, new_kind, new_id), count in sorted(
        transitions.items()
    ):
        print(f"{count:5d} {old_kind} ${old_id} -> {new_kind} ${new_id}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("run", type=Path)
    parser.add_argument("--sites", action="store_true")
    parser.add_argument("--transitions", action="store_true")
    args = parser.parse_args()

    rows = load_requests(args.run)
    print_overview(rows)
    print_effect_table(rows)
    if args.sites:
        print_sites(rows)
    if args.transitions:
        print_transitions(rows)


if __name__ == "__main__":
    main()
