#!/usr/bin/env python3
"""Offline PresentMon 2 (--v2_metrics --qpc_time) / KSword PerfTrace analysis.

Usage: python tools/ui_perf/analyze_presentations.py --manifest capture.json
       --output analysis.json [--session-id UUID] [--swapchain 0xADDRESS]

Only manifest-hashed files are consumed. Decimal strings in the output preserve
QPC precision; percentiles use nearest rank. Display events are ETW observations,
not optical measurements. Temporal candidates never establish action completion.
"""

from __future__ import annotations

import argparse
import bisect
import csv
from collections import Counter, defaultdict
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation, localcontext
import hashlib
import io
import json
from pathlib import Path
import re
import sys
import uuid


class EvidenceError(ValueError):
    pass


def integer(value, name, *, positive=False, string=False):
    if isinstance(value, bool) or (string and not isinstance(value, str)):
        raise EvidenceError(f"{name}: expected {'string ' if string else ''}integer")
    if isinstance(value, (int, str)) and re.fullmatch(r"[0-9]+", str(value)):
        result = int(value)
    elif isinstance(value, Decimal) and value.is_finite() and value == value.to_integral_value():
        result = int(value)
    else:
        raise EvidenceError(f"{name}: expected nonnegative integer")
    if result < 0 or (positive and result == 0):
        raise EvidenceError(f"{name}: out of range")
    return result


def number(value, name):
    if isinstance(value, bool):
        raise EvidenceError(f"{name}: expected finite nonnegative number")
    try:
        result = Decimal(str(value))
    except InvalidOperation as exc:
        raise EvidenceError(f"{name}: invalid number") from exc
    if not result.is_finite() or result < 0:
        raise EvidenceError(f"{name}: expected finite nonnegative number")
    return result


def optional_number(value, name):
    return None if value is None or str(value).strip() in ("", "NA") else number(value, name)


def decimal_text(value):
    text = format(value, "f")
    return text.rstrip("0").rstrip(".") if "." in text else text


def stats(values):
    values = sorted(values)
    result = {"samples": len(values), "percentile_method": "nearest_rank"}
    for label, percentile in (("p50", 50), ("p95", 95), ("p99", 99)):
        result[label] = decimal_text(values[(len(values) * percentile + 99) // 100 - 1]) if values else None
    return result


def utc(value, name):
    try:
        result = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (AttributeError, TypeError, ValueError) as exc:
        raise EvidenceError(f"{name}: expected UTC ISO timestamp") from exc
    if result.tzinfo is None or result.utcoffset() != timezone.utc.utcoffset(result):
        raise EvidenceError(f"{name}: timestamp must be UTC")
    return result


def sha256(value, name):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", value):
        raise EvidenceError(f"{name}: expected SHA256")
    return value.lower()


def load_json(text):
    # Decimal avoids first rounding trace nanoseconds through Python float.
    return json.loads(text, parse_float=Decimal,
                      parse_constant=lambda value: (_ for _ in ()).throw(EvidenceError(f"invalid JSON number {value}")))


def validate_manifest(manifest):
    if not isinstance(manifest, dict) or type(manifest.get("schema_version")) is not int or manifest["schema_version"] != 1:
        raise EvidenceError("unsupported manifest schema_version (requires 1)")
    try:
        uuid.UUID(manifest["capture_id"])
        target = manifest["target"]
        if not isinstance(target, dict) or type(target.get("pid")) is not int:
            raise EvidenceError("target.pid: expected positive integer")
        integer(target["pid"], "target.pid", positive=True)
        if not isinstance(target["image_path"], str) or not target["image_path"].strip():
            raise EvidenceError("target.image_path: missing")
        sha256(target["exe_sha256"], "target.exe_sha256")
        created = utc(target["creation_time_utc"], "target.creation_time_utc")
        frequency = integer(manifest["qpc_frequency"], "qpc_frequency", positive=True, string=True)
        start = integer(manifest["started_qpc"], "started_qpc", string=True)
        stop = integer(manifest["stopped_qpc"], "stopped_qpc", string=True)
        began = utc(manifest["started_utc"], "started_utc")
        ended = utc(manifest["stopped_utc"], "stopped_utc")
        if stop < start or ended < began or created > began:
            raise EvidenceError("manifest capture/process time range is inconsistent")
        if not isinstance(manifest["files"], dict):
            raise EvidenceError("files: expected object")
    except (KeyError, TypeError, ValueError) as exc:
        if isinstance(exc, EvidenceError):
            raise
        raise EvidenceError(f"invalid manifest identity/time: {exc}") from exc
    return frequency, start, stop


def verified_file(manifest, root, role, *, keep_bytes=True):
    entry = manifest["files"].get(role)
    if entry is None:
        return None
    if not isinstance(entry, dict) or not isinstance(entry.get("path"), str) or not entry["path"]:
        raise EvidenceError(f"files.{role}: invalid path")
    expected = sha256(entry.get("sha256"), f"files.{role}.sha256")
    path = (root / entry["path"]).resolve()
    digest = hashlib.sha256()
    chunks = []
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
            if keep_bytes:
                chunks.append(chunk)
    if digest.hexdigest() != expected:
        raise EvidenceError(f"files.{role}: SHA256 mismatch")
    return b"".join(chunks) if keep_bytes else {"path": str(path), "sha256": expected}


INPUT_COLUMNS = {
    "all_input_to_photon": ("AllInputToPhotonLatency", "MsAllInputToPhotonLatency"),
    "click_to_photon": ("ClickToPhotonLatency", "MsClickToPhotonLatency"),
}


def read_kernel_receipt(data, frequency, start, stop, gaps):
    receipt = load_json(data.decode("utf-8-sig"))
    if not isinstance(receipt, dict) or type(receipt.get("schema_version")) is not int or receipt["schema_version"] != 1:
        raise EvidenceError("kernel receipt requires schema_version=1")
    if type(receipt.get("complete")) is not bool:
        raise EvidenceError("kernel receipt complete: expected boolean")
    fields = ("start_status", "stack_status", "flags_status", "stop_status", "wait_result",
              "events_lost", "log_buffers_lost", "realtime_buffers_lost", "buffers_written")
    for name in fields:
        value = receipt.get(name)
        if type(value) is not int or value < 0:
            raise EvidenceError(f"kernel receipt {name}: expected nonnegative integer")
        if name.endswith("_status") or name == "wait_result":
            if value > 0xFFFFFFFF:
                raise EvidenceError(f"kernel receipt {name}: outside Win32 code range")
    if integer(receipt.get("qpc_frequency"), "kernel receipt qpc_frequency", positive=True, string=True) != frequency:
        raise EvidenceError("kernel receipt QPC frequency does not match manifest")
    began = integer(receipt.get("started_qpc"), "kernel receipt started_qpc", string=True)
    ended = integer(receipt.get("stopped_qpc"), "kernel receipt stopped_qpc", string=True)
    if not start <= began <= ended <= stop:
        raise EvidenceError("kernel receipt interval is outside manifest capture interval")
    statuses = {name: receipt[name] for name in fields[:4]}
    losses = {name: receipt[name] for name in fields[5:8]}
    if not receipt["complete"]:
        gaps.append("Kernel receipt explicitly reports incomplete capture")
    for name, value in statuses.items():
        if value:
            gaps.append(f"Kernel receipt {name} failed with Win32 status {value}")
    if receipt["wait_result"] != 258:
        gaps.append(f"Kernel receipt wait_result={receipt['wait_result']}; bounded collector completion was not confirmed by WAIT_TIMEOUT (258); cancellation or owner exit is incomplete")
    for name, value in losses.items():
        if value:
            gaps.append(f"Kernel receipt reports {name}={value}; system trace coverage has sample loss")
    if not receipt["buffers_written"]:
        gaps.append("Kernel receipt reports zero buffers_written; recorded system event coverage is unproven")
    if began != start or ended != stop:
        gaps.append("Kernel receipt interval covers only part of the manifest capture interval")
    return {"available": True, "schema_version": 1, "complete": receipt["complete"],
            "statuses": statuses, "wait_result": receipt["wait_result"], "loss_counters": losses,
            "buffers_written": receipt["buffers_written"], "qpc_frequency": str(frequency),
            "started_qpc": str(began), "stopped_qpc": str(ended),
            "collection_complete_without_reported_loss": receipt["complete"] and not any(statuses.values())
                and receipt["wait_result"] == 258 and not any(losses.values()) and receipt["buffers_written"] > 0,
            "actual_stack_coverage_verified": False,
            "scope": "Kernel collector lifecycle/loss counters only; ETL events and actual stack coverage are not decoded"}


def read_presentmon(data, manifest, frequency, start, stop, gaps):
    counters = Counter()
    groups = defaultdict(list)
    errors = []
    reader = csv.DictReader(io.StringIO(data.decode("utf-8-sig"), newline=""), strict=True)
    required = {"ProcessID", "SwapChainAddress", "CPUStartQPC", "DisplayLatency", "DisplayedTime"}
    if not reader.fieldnames or not required.issubset(reader.fieldnames) or len(set(reader.fieldnames)) != len(reader.fieldnames):
        raise EvidenceError("PresentMon CSV requires unique v2 ProcessID, SwapChainAddress, CPUStartQPC, DisplayLatency, DisplayedTime columns")
    columns = {label: next((name for name in names if name in reader.fieldnames), None)
               for label, names in INPUT_COLUMNS.items()}
    for label, column in columns.items():
        if column is None:
            gaps.append(f"PresentMon input metric unavailable: {label}")
    try:
        for row in reader:
            counters["rows"] += 1
            try:
                if None in row or any(value is None for value in row.values()):
                    raise EvidenceError("incomplete or extra CSV fields")
                pid = integer(row["ProcessID"], "ProcessID", positive=True)
                if pid != manifest["target"]["pid"]:
                    counters["foreign_pid_rows"] += 1
                    continue
                swapchain = row["SwapChainAddress"].strip().lower()
                if not re.fullmatch(r"(?:0x)?[0-9a-f]+", swapchain):
                    raise EvidenceError("invalid SwapChainAddress")
                swapchain = hex(int(swapchain, 16))
                key = (pid, swapchain, row.get("VidPnSourceId", ""), row.get("LayerIndex", ""))
                # Retain empty/dropped-only streams in the report.
                groups[key]
                cpu = integer(row["CPUStartQPC"], "CPUStartQPC")
                if not start <= cpu <= stop:
                    counters["outside_capture_rows"] += 1
                    continue
                displayed_time = optional_number(row["DisplayedTime"], "DisplayedTime")
                latency = optional_number(row["DisplayLatency"], "DisplayLatency")
                dropped = row.get("Dropped", "").strip().lower()
                if dropped not in ("", "0", "1", "false", "true"):
                    raise EvidenceError("invalid Dropped flag")
                if displayed_time is None or dropped in ("1", "true"):
                    counters["not_displayed_rows"] += 1
                    continue
                if latency is None:
                    raise EvidenceError("displayed row has no DisplayLatency")
                display = Decimal(cpu) + latency * frequency / 1000
                if not Decimal(start) <= display <= Decimal(stop):
                    counters["display_outside_capture_rows"] += 1
                    continue
                metrics = {label: optional_number(row[column], column) if column else None
                           for label, column in columns.items()}
                groups[key].append({"qpc": display, "cpu_qpc": cpu, "present_id": row.get("PresentId"),
                                    "displayed_time": displayed_time, "metrics": metrics,
                                    "csv_row": reader.line_num})
                counters["valid_displayed_rows"] += 1
            except EvidenceError as exc:
                counters["invalid_rows"] += 1
                if len(errors) < 10:
                    errors.append({"line": reader.line_num, "error": str(exc)})
    except csv.Error as exc:
        counters["parse_errors"] += 1
        errors.append({"line": reader.line_num, "error": str(exc)})
    summaries, timelines = [], {}
    for key, rows in sorted(groups.items()):
        seen, unique = set(), []
        for row in rows:
            identity = (row["qpc"], row["cpu_qpc"], row["present_id"])
            if identity not in seen:
                unique.append(row)
                seen.add(identity)
        original_times = [row["qpc"] for row in unique]
        unique.sort(key=lambda row: (row["qpc"], row["cpu_qpc"]))
        times = sorted(set(original_times))
        timelines[key] = unique
        summaries.append({
            "pid": key[0], "swapchain": key[1], "vidpn_source_id": key[2] or None, "layer_index": key[3] or None,
            "displayed_records": len(unique), "unique_display_timestamps": len(times),
            "duplicate_records_removed": len(rows) - len(unique),
            "coincident_display_records": len(unique) - len(times),
            "reordered_display_records": sum(b < a for a, b in zip(original_times, original_times[1:])),
            "first_display_qpc": decimal_text(times[0]) if times else None,
            "last_display_qpc": decimal_text(times[-1]) if times else None,
            "display_event_intervals_ms": stats([(b - a) * 1000 / frequency for a, b in zip(times, times[1:])]),
            "displayed_time_ms": stats([row["displayed_time"] for row in unique]),
            "presentmon_input_latency_ms": {label: {"column": column,
                **stats([row["metrics"][label] for row in unique if row["metrics"][label] is not None])}
                for label, column in columns.items()},
        })
    if not counters["valid_displayed_rows"]:
        gaps.append("No valid displayed PresentMon data for the manifest target and capture interval")
    if counters["invalid_rows"] or counters["parse_errors"]:
        gaps.append("PresentMon contains rejected/malformed rows; reported coverage is incomplete")
    return {"counts": dict(counters), "row_errors": errors, "streams": summaries,
            "interval_definition": "Differences between sorted unique ETW display timestamps within each PID/swapchain/output/layer; no FPS inferred",
            "displayed_time_definition": "PresentMon DisplayedTime duration, summarized separately; never treated as an absolute timestamp"}, timelines


def rounding_ns(value):
    # QJson writes qint64 ns as double. Above 2**53, source rounding predates us.
    spacing = 1 << max(0, value.bit_length() - 53)
    return Decimal(spacing) / 2 if value > 2**53 else Decimal(0)


def read_trace(data, manifest, session_id, frequency, gaps):
    sessions, events, errors = {}, [], []
    for line_number, line in enumerate(data.decode("utf-8-sig").splitlines(), 1):
        try:
            record = load_json(line)
            if not isinstance(record, dict) or not isinstance(record.get("session_id"), str) or not record["session_id"]:
                raise EvidenceError("trace record missing session_id")
            if record.get("event") == "session":
                if record["session_id"] in sessions:
                    raise EvidenceError("duplicate trace session header")
                sessions[record["session_id"]] = record
            else:
                events.append(record)
        except (EvidenceError, json.JSONDecodeError) as exc:
            errors.append({"line": line_number, "error": str(exc)})
    report = {"session_ids": list(sessions), "parse_errors": errors[:10], "malformed_lines": len(errors),
              "tail_newline_present": data.endswith(b"\n"), "joined": False}
    if errors or (data and not data.endswith(b"\n")):
        gaps.append("Trace malformed lines or missing final newline indicate possible truncation")
    if session_id is None and len(sessions) != 1:
        gaps.append("Trace session selection required: zero or multiple session headers")
        return report, None, []
    selected = session_id or next(iter(sessions))
    report["selected_session_id"] = selected
    if selected not in sessions:
        gaps.append("Requested trace session is absent")
        return report, None, []
    session = sessions[selected]
    try:
        if integer(session.get("pid"), "trace pid", positive=True) != manifest["target"]["pid"]:
            raise EvidenceError("trace PID does not match manifest")
        if sha256(session.get("exe_sha256"), "trace exe_sha256") != manifest["target"]["exe_sha256"].lower():
            raise EvidenceError("trace executable hash does not match manifest")
        session_utc = utc(session.get("utc"), "trace utc")
        if not utc(manifest["target"]["creation_time_utc"], "creation time") <= session_utc <= utc(manifest["stopped_utc"], "stop time"):
            raise EvidenceError("trace session outside target lifetime/capture end")
        if integer(session.get("qpc_frequency"), "trace qpc_frequency", positive=True, string=True) != frequency:
            raise EvidenceError("trace QPC frequency does not match manifest")
        qpc = integer(session.get("clock_sync_qpc"), "clock_sync_qpc", string=True)
        before = integer(session.get("clock_sync_before_ns"), "clock_sync_before_ns")
        after = integer(session.get("clock_sync_after_ns"), "clock_sync_after_ns")
        if after < before:
            raise EvidenceError("trace calibration ns range is reversed")
    except EvidenceError as exc:
        report["identity_or_clock_error"] = str(exc)
        gaps.append(str(exc) + "; trace will not be joined")
        return report, None, []
    selected_events = [record for record in events if record["session_id"] == selected]
    dropped, drains, regular = 0, [], []
    for record in selected_events:
        try:
            if record.get("event") == "dropped":
                dropped += integer(record.get("count"), "dropped count")
            elif record.get("event") == "writer_drain_end":
                drains.append(record)
            else:
                regular.append(record)
        except EvidenceError as exc:
            gaps.append(str(exc))
    report.update({"joined": True, "pid": session["pid"], "exe_sha256": session["exe_sha256"],
                   "source_identity": session.get("source_identity"), "ignored_other_session_records": len(events) - len(selected_events),
                   "orphan_session_records": sum(record["session_id"] not in sessions for record in events),
                   "records": len(regular), "dropped_records": dropped, "writer_drain_marker_count": len(drains),
                   "writer_flush_receipt_verified": False,
                   "clock": {"qpc_frequency": str(frequency), "clock_sync_qpc": str(qpc),
                       "sync_bracket_ns": str(after - before), "drift_measured": False,
                       "bounds_scope": "single sync bracket plus one QPC tick and source-double ns rounding; excludes clock drift and PresentMon latency rounding"}})
    if dropped:
        gaps.append(f"Trace explicitly dropped {dropped} records")
    try:
        drain_ok = len(drains) == 1 and integer(drains[0].get("written_records"), "written_records") == len(regular) and integer(drains[0].get("dropped_records"), "dropped_records") == dropped
    except EvidenceError:
        drain_ok = False
    report["writer_drain_counts_match"] = drain_ok
    if not drain_ok:
        gaps.append("Trace drain marker absent/inconsistent; tail completeness is unproven")
    gaps.append("Trace completion sidecar is not manifest-hashed; drain marker alone does not prove flush completion")
    calibration = (qpc, before, after)
    return report, calibration, regular


def scroll_paint_observations(events, gaps):
    """Qt value/Paint/swap observations, never input-to-present causality or FPS."""
    streams = {}
    invalid = 0
    for record in events:
        observation = record.get("scroll_observation")
        if observation is None:
            continue
        try:
            if not isinstance(observation, dict) or observation.get("attribution") != "signal_callback_context_only":
                raise EvidenceError("scroll observation attribution unavailable")
            if observation.get("input_causality_proven") is not False or observation.get("content_presented_proven") is not False:
                raise EvidenceError("scroll observation makes an unsupported input/presentation claim")
            viewport = integer(observation.get("viewport_id"), "viewport_id", positive=True, string=True)
            integer(observation.get("observation_revision"), "observation_revision", string=True)
            callback = integer(observation.get("callback_action_id"), "callback_action_id", string=True)
            at = integer(record.get("t_ns"), "scroll t_ns")
            stream = streams.get(viewport, {"paints": {}, "swaps": [], "boundaries": Counter(), "callbacks": set()})
            boundary = observation.get("boundary")
            if not isinstance(boundary, str):
                raise EvidenceError("scroll boundary unavailable")
            if boundary == "qt_paint_dispatch":
                scope = integer(observation.get("paint_scope_id"), "paint_scope_id", positive=True, string=True)
                if record.get("event") != "notify_dispatch" or record.get("render_boundary") != boundary:
                    raise EvidenceError("scroll Paint lacks actual notify/render boundary")
                if scope != integer(record.get("scope_id"), "scope_id", positive=True, string=True) or scope in stream["paints"]:
                    raise EvidenceError("scroll Paint scope mismatch/duplicate")
                begin = integer(record.get("begin_ns"), "scroll begin_ns")
                end = integer(record.get("end_ns"), "scroll end_ns")
                if not begin <= end <= at or type(observation.get("state_stable_at_dispatch_end")) is not bool:
                    raise EvidenceError("scroll Paint interval/stability unavailable")
                observed = observation.get("value_observed_ns")
                observed = None if observed == -1 else integer(observed, "value_observed_ns")
                if observed is not None and observed > begin:
                    raise EvidenceError("scroll value observation follows Paint entry")
                stream["paints"][scope] = (begin, end, observed, observation["state_stable_at_dispatch_end"])
            elif boundary == "qt_gl_frame_swapped_after_observed_paint":
                scope = integer(observation.get("paint_scope_id"), "swap paint_scope_id", positive=True, string=True)
                stream["swaps"].append((scope, at))
            stream["boundaries"][boundary] += 1
            if callback:
                stream["callbacks"].add(callback)
            streams[viewport] = stream
        except EvidenceError:
            invalid += 1
    result = {"available": bool(streams), "input_causality_proven": False, "content_presented_proven": False,
              "measurement": "Qt value observations, actual Paint dispatch intervals, and GL swap callbacks; not display FPS",
              "invalid_observations": invalid, "streams": []}
    if invalid:
        gaps.append(f"{invalid} scroll observations rejected for invalid/unsupported evidence")
    for viewport, stream in sorted(streams.items()):
        paints = sorted(stream["paints"].values())
        intervals = [Decimal(right[0] - left[0]) / 1000000 for left, right in zip(paints, paints[1:])]
        observed_to_paint = [Decimal(begin - observed) / 1000000 for begin, _, observed, stable in paints if stable and observed is not None]
        swaps, unmatched, used = [], 0, set()
        for scope, at in stream["swaps"]:
            paint = stream["paints"].get(scope)
            if paint is None or not paint[3] or at < paint[1] or scope in used:
                unmatched += 1
            else:
                used.add(scope)
                swaps.append(Decimal(at - paint[1]) / 1000000)
        result["streams"].append({"viewport_id": str(viewport), "boundary_counts": dict(stream["boundaries"]),
                                  "observing_callback_action_ids": [str(value) for value in sorted(stream["callbacks"])],
                                  "paint_dispatch_intervals_ms": stats(intervals),
                                  "value_observation_to_stable_paint_entry_ms": stats(observed_to_paint),
                                  "stable_paint_end_to_gl_swap_callback_ms": stats(swaps),
                                  "paint_changed_or_destroyed_during_dispatch": sum(not paint[3] for paint in paints),
                                  "unmatched_or_duplicate_swap_observations": unmatched})
    return result


def action_candidates(events, calibration, timelines, swapchain, frequency, start, stop, gaps):
    begins, ends, diagnostics = defaultdict(list), defaultdict(list), defaultdict(list)
    ready_records = defaultdict(list)
    stage_counts, stage_durations = Counter(), defaultdict(list)
    rejected = 0
    for record in events:
        try:
            if "t_ns" not in record:
                raise EvidenceError("trace event has no t_ns")
            at = integer(record["t_ns"], "t_ns")
            action = integer(record.get("action_id", "0"), "action_id")
            kind = record.get("event", "")
            op = record.get("op", "")
            if not isinstance(kind, str) or not isinstance(op, str):
                raise EvidenceError("trace event/op must be strings")
            stage_counts[f"{kind}:{op}"] += 1
            duration = None
            if "duration_ns" in record:
                duration = number(record["duration_ns"], "duration_ns") / 1000000
                stage_durations[f"{kind}:{op}"].append(duration)
            if action:
                if kind == "input_begin":
                    begins[action].append(record)
                elif kind == "input_dispatch_end":
                    ends[action].append(at)
            if kind in ("async_stage", "span", "notify_dispatch", "navigation_stage") and (action or kind == "navigation_stage"):
                stage = {"event": kind, "op": op, "request_id": record.get("request_id"),
                         "t_ns": str(at), "duration_ms": decimal_text(duration) if duration is not None else None}
                for field in ("parent_request_id", "navigation_id", "navigation_kind", "navigation_stage",
                              "origin", "target_id", "target_path", "target_index", "target_identity_scope",
                              "target_path_complete", "readiness_boundary", "outcome", "operation_scope",
                              "business_ready_proven", "present_proven"):
                    if field in record:
                        stage[field] = record[field]
                diagnostics[action].append(stage)
                if (kind == "navigation_stage" and record.get("navigation_stage") == "ready"
                        or kind == "async_stage" and op.endswith("_business_data_ready")):
                    identity = (action, kind, integer(record.get("request_id", "0"), "request_id"),
                                integer(record.get("navigation_id", "0"), "navigation_id"), op)
                    ready_records[identity].append(stage)
        except EvidenceError:
            rejected += 1
    if rejected:
        gaps.append(f"Trace has {rejected} rejected timestamp/action/duration records")
    summary = {"items": [], "stage_counts": dict(stage_counts),
               "stage_duration_ms": {name: stats(values) for name, values in sorted(stage_durations.items())},
               "diagnostic_actions_without_input_begin": len(set(diagnostics) - set(begins)),
               "diagnostic_stages_without_input_begin": [
                   {"action_id": str(action), "diagnostic_stages": diagnostics[action]}
                   for action in sorted(set(diagnostics) - set(begins))],
               "ready_display_candidates": [],
               "interpretation": "Next observed display after the earliest input clock bound, with possible shared/overlapping actions; no causal present ID exists"}
    eligible = {key: rows for key, rows in timelines.items() if rows and (swapchain is None or key[1] == swapchain)}
    if len(eligible) != 1:
        gaps.append("Action association requires exactly one displayed stream; select --swapchain (output/layer ambiguity also blocks association)")
        return summary
    key, rows = next(iter(eligible.items()))
    # A shared display timestamp is one candidate with possibly several records.
    by_time = defaultdict(list)
    for row in rows:
        by_time[row["qpc"]].append(row)
    times = sorted(by_time)
    qpc, before, after = calibration

    def qpc_bounds(at):
        rounding = rounding_ns(at) + max(rounding_ns(before), rounding_ns(after))
        return (Decimal(qpc) + (Decimal(at - after) - rounding) * frequency / 1000000000 - 1,
                Decimal(qpc) + (Decimal(at - before) + rounding) * frequency / 1000000000 + 1)

    # Keep each recorded readiness boundary separate. A shell-ready record is
    # not business readiness, and temporal ordering does not prove presentation.
    for identity, records in ready_records.items():
        action = identity[0]
        if len(records) != 1:
            gaps.append(f"Action {action} has duplicate ready records for request {identity[2]}; no ready/display association")
            continue
        stage = records[0]
        lower, upper = qpc_bounds(integer(stage["t_ns"], "ready t_ns"))
        if upper < start or lower > stop:
            continue
        # Unlike the input-only candidate, this observation must be after the
        # latest clock bound of ready; an earlier display cannot contain it.
        index = bisect.bisect_left(times, max(upper, Decimal(start)))
        display = times[index] if index < len(times) else None
        item = {"action_id": str(action), "recorded_ready_stage": stage,
                "attribution": "NONCAUSAL_AFTER_RECORDED_READY_BOUNDARY",
                "actual_action_completion_measured": False, "content_presented_proven": False,
                "ready_qpc_bounds": [decimal_text(lower), decimal_text(upper)],
                "candidate_display_qpc": decimal_text(display) if display is not None else None,
                "pid": key[0], "swapchain": key[1], "vidpn_source_id": key[2] or None, "layer_index": key[3] or None}
        if display is not None:
            item.update({"candidate_present_ids": [row["present_id"] for row in by_time[display]],
                         "candidate_csv_rows": [row["csv_row"] for row in by_time[display]],
                         "ready_to_display_delta_ms_bounds": [decimal_text((display - upper) * 1000 / frequency),
                                                              decimal_text((display - lower) * 1000 / frequency)]})
        summary["ready_display_candidates"].append(item)
    candidates = []
    for action, records in sorted(begins.items()):
        if len(records) != 1:
            gaps.append(f"Action {action} has duplicate input_begin records; no association")
            continue
        record = records[0]
        at = integer(record["t_ns"], "t_ns")
        lower, upper = qpc_bounds(at)
        if upper < start or lower > stop:
            continue
        index = bisect.bisect_left(times, max(lower, Decimal(start)))
        display = times[index] if index < len(times) else None
        dispatch_ends = [end for end in ends[action] if end >= at]
        item = {"action_id": str(action), "origin": record.get("origin"), "target": record.get("target"),
                "attribution": "NONCAUSAL_TEMPORAL_CANDIDATE", "actual_action_completion_measured": False,
                "input_qpc_bounds": [decimal_text(lower), decimal_text(upper)],
                "dispatch_duration_ms": decimal_text(Decimal(dispatch_ends[0] - at) / 1000000) if len(dispatch_ends) == 1 else None,
                "dispatch_end_count": len(dispatch_ends), "diagnostic_stages": diagnostics[action],
                "candidate_display_qpc": decimal_text(display) if display is not None else None,
                "pid": key[0], "swapchain": key[1], "vidpn_source_id": key[2] or None, "layer_index": key[3] or None}
        if display is not None:
            item.update({"candidate_present_ids": [row["present_id"] for row in by_time[display]],
                         "candidate_csv_rows": [row["csv_row"] for row in by_time[display]],
                         "candidate_may_precede_input": display < upper,
                         "delta_ms_bounds": [decimal_text((display - upper) * 1000 / frequency),
                                             decimal_text((display - lower) * 1000 / frequency)]})
        candidates.append(item)
    shared = defaultdict(list)
    for item in candidates:
        if item["candidate_display_qpc"] is not None:
            shared[item["candidate_display_qpc"]].append(item["action_id"])
    for item in candidates:
        item["same_candidate_action_ids"] = shared[item["candidate_display_qpc"]] if item["candidate_display_qpc"] is not None else []
    if any(not item["dispatch_end_count"] for item in candidates):
        gaps.append("Some input actions lack dispatch-end records")
    summary["items"] = candidates
    return summary


def analyze(manifest_path, *, session_id=None, swapchain=None):
    report = {"analysis_schema_version": 1, "status": "invalid_input", "input_validated": False,
              "full_t00_complete": False, "physical_photons_measured": False,
              "actual_stack_coverage_verified": False,
              "measurement": "PresentMon ETW display events and optional KSword diagnostic timestamps",
              "coverage_gaps": [], "errors": []}
    gaps = report["coverage_gaps"]
    try:
        path = Path(manifest_path).resolve()
        manifest = load_json(path.read_text(encoding="utf-8-sig"))
        frequency, start, stop = validate_manifest(manifest)
        if swapchain is not None:
            if not re.fullmatch(r"(?:0x)?[0-9a-fA-F]+", swapchain):
                raise EvidenceError("--swapchain: expected hexadecimal address")
            swapchain = hex(int(swapchain, 16))
        pm_data = verified_file(manifest, path.parent, "presentmon_csv")
        trace_data = verified_file(manifest, path.parent, "trace_jsonl")
        etl = verified_file(manifest, path.parent, "system_etl", keep_bytes=False)
        kernel_receipt_data = verified_file(manifest, path.parent, "kernel_receipt")
        report.update({"capture_id": manifest["capture_id"], "target": manifest["target"],
                       "qpc_frequency": str(frequency), "started_qpc": str(start), "stopped_qpc": str(stop),
                       "input_validated": True, "status": "diagnostic_only",
                       "system_etl": {"available": etl is not None, "stacks_analyzed": False}})
        if manifest.get("capture_complete") is False:
            gaps.append("Manifest explicitly reports capture_complete=false; capture is incomplete")
        if kernel_receipt_data is None:
            report["kernel_receipt"] = {"available": False, "actual_stack_coverage_verified": False}
            gaps.append("Kernel collector lifecycle/loss receipt absent; completion and collector loss are unknown")
        else:
            report["kernel_receipt"] = read_kernel_receipt(kernel_receipt_data, frequency, start, stop, gaps)
        gaps.extend(["PresentMon ETW loss counters and capture finalization/truncation are not established by CSV or file hashes; a kernel receipt covers a separate collector",
                     "Single QPC calibration cannot bound clock drift; PresentMon DisplayLatency decimal rounding adds unquantified uncertainty",
                     "Manifest target identity is a capture claim; CSV PID alone cannot independently prove executable build, process lifetime, or boot identity",
                     "No optical/camera measurement or causal action-to-present identifier; these results cannot close full T00"])
        gaps.append("System stack evidence is present but not decoded by this offline analyzer" if etl is not None else "System stack/ETL evidence absent")
        with localcontext() as context:
            context.prec = 80
            timelines = {}
            if pm_data is None:
                gaps.append("PresentMon CSV evidence absent")
                report["presentmon"] = {"available": False, "streams": []}
            else:
                report["presentmon"], timelines = read_presentmon(pm_data, manifest, frequency, start, stop, gaps)
                report["presentmon"]["available"] = True
            if trace_data is None:
                report["trace"] = {"available": False, "joined": False}
                gaps.append("PerfTrace JSONL evidence absent; action/stage associations unavailable")
            else:
                trace_report, calibration, events = read_trace(trace_data, manifest, session_id, frequency, gaps)
                report["trace"] = {"available": True, **trace_report}
                report["scroll_paint_observations"] = scroll_paint_observations(events, gaps)
                if calibration is not None:
                    report["action_display_candidates"] = action_candidates(events, calibration, timelines, swapchain, frequency, start, stop, gaps)
    except (EvidenceError, OSError, UnicodeError, json.JSONDecodeError, csv.Error) as exc:
        report["status"] = "invalid_input"
        report["input_validated"] = False
        report["errors"].append(str(exc))
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--session-id", help="Required for a JSONL containing multiple session headers")
    parser.add_argument("--swapchain", help="Hex address; required to associate actions when several displayed streams exist")
    args = parser.parse_args(argv)
    report = analyze(args.manifest, session_id=args.session_id, swapchain=args.swapchain)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{report['status']}: {args.output}; full_t00_complete=false")
    return 2 if report["status"] == "invalid_input" else 0


if __name__ == "__main__":
    sys.exit(main())
