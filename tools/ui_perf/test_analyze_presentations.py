"""Run with: python -m unittest discover -s tools/ui_perf -p test_analyze_presentations.py"""

import csv
from decimal import Decimal
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest

import analyze_presentations as analyzer


class AnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / "capture.json"
        self.base = 2**53 + 101
        self.frequency = 10000000
        self.hash = "a" * 64
        self.manifest = {
            "schema_version": 1, "capture_id": "12345678-1234-1234-1234-123456789abc",
            "target": {"pid": 42, "image_path": "Ksword5.1.exe", "exe_sha256": self.hash,
                       "creation_time_utc": "2026-10-22T11:00:00Z"},
            "qpc_frequency": str(self.frequency), "started_qpc": str(self.base),
            "stopped_qpc": str(self.base + 10000000),
            "started_utc": "2026-10-22T12:00:00Z", "stopped_utc": "2026-10-22T12:00:01Z",
            "files": {},
        }

    def file(self, role, name, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        (self.root / name).write_bytes(data)
        self.manifest["files"][role] = {"path": name, "sha256": hashlib.sha256(data).hexdigest()}

    def csv(self, rows, *, columns=None):
        columns = columns or ["ProcessID", "SwapChainAddress", "PresentId", "CPUStartQPC", "DisplayLatency",
                              "DisplayedTime", "AllInputToPhotonLatency", "ClickToPhotonLatency"]
        text = io.StringIO(newline="")
        writer = csv.DictWriter(text, fieldnames=columns)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
        self.file("presentmon_csv", "presentmon.csv", text.getvalue())

    def row(self, offset=0, **changes):
        row = {"ProcessID": 42, "SwapChainAddress": "0xABC", "PresentId": str(offset),
               "CPUStartQPC": str(self.base + offset), "DisplayLatency": "0.0001", "DisplayedTime": "16.7",
               "AllInputToPhotonLatency": "NA", "ClickToPhotonLatency": "NA"}
        row.update(changes)
        return row

    def session(self, session_id="session-A", **changes):
        session = {"event": "session", "session_id": session_id, "pid": 42, "exe_sha256": self.hash,
                   "utc": "2026-10-22T12:00:00Z", "qpc_frequency": str(self.frequency),
                   "clock_sync_qpc": str(self.base), "clock_sync_before_ns": 1000, "clock_sync_after_ns": 2000}
        session.update(changes)
        return session

    def event(self, kind, at=3000, action="1", **changes):
        record = {"session_id": "session-A", "event": kind, "op": "perf_" + kind,
                  "t_ns": at, "action_id": action, "request_id": "0"}
        record.update(changes)
        return record

    def trace(self, records, *, tail="\n", drain=True):
        if drain:
            selected = [record for record in records if record.get("session_id") == "session-A"]
            regular = sum(record["event"] not in ("session", "dropped", "writer_drain_end") for record in selected)
            loss = sum(int(record["count"]) for record in selected if record["event"] == "dropped")
            records = records + [{"event": "writer_drain_end", "session_id": "session-A",
                                  "written_records": str(regular), "dropped_records": str(loss)}]
        self.file("trace_jsonl", "perf.jsonl", "\n".join(json.dumps(record) for record in records) + tail)

    def run_analysis(self, **options):
        self.path.write_text(json.dumps(self.manifest), encoding="utf-8")
        return analyzer.analyze(self.path, **options)

    def test_qpc_above_double_precision_retains_one_tick_intervals(self):
        self.csv([self.row(0), self.row(1), self.row(2, AllInputToPhotonLatency="3.25", ClickToPhotonLatency="1.75")])
        report = self.run_analysis()
        stream = report["presentmon"]["streams"][0]
        self.assertEqual(stream["first_display_qpc"], str(self.base + 1))
        self.assertEqual(stream["display_event_intervals_ms"]["p99"], "0.0001")
        self.assertEqual(stream["presentmon_input_latency_ms"]["all_input_to_photon"]["p50"], "3.25")
        self.assertFalse(report["full_t00_complete"])
        self.assertFalse(report["physical_photons_measured"])

    def test_na_dropped_zero_and_invalid_values_are_distinct(self):
        self.csv([self.row(0, DisplayedTime="NA", DisplayLatency="NA"),
                  self.row(10, DisplayedTime="0", DisplayLatency="0"),
                  self.row(20, DisplayLatency="NaN"), self.row(30, DisplayLatency="Infinity"),
                  self.row(40, CPUStartQPC="-1"), self.row(50, ClickToPhotonLatency="-2")])
        report = self.run_analysis()
        counts = report["presentmon"]["counts"]
        self.assertEqual(counts["not_displayed_rows"], 1)
        self.assertEqual(counts["invalid_rows"], 4)
        self.assertEqual(counts["valid_displayed_rows"], 1)
        self.assertEqual(report["presentmon"]["streams"][0]["display_event_intervals_ms"]["samples"], 0)

    def test_explicit_dropped_flag_never_counts_as_display(self):
        columns = list(self.row()) + ["Dropped"]
        self.csv([self.row(0, Dropped="1"), self.row(100, Dropped="false")], columns=columns)
        report = self.run_analysis()
        self.assertEqual(report["presentmon"]["counts"]["valid_displayed_rows"], 1)

    def test_reordered_duplicate_mixed_streams_do_not_merge(self):
        first = self.row(100)
        self.csv([self.row(300), first, first, self.row(100, PresentId="other"),
                  self.row(200, SwapChainAddress="0xDEF"), self.row(200, ProcessID=99)])
        report = self.run_analysis()
        streams = report["presentmon"]["streams"]
        self.assertEqual(len(streams), 2)
        self.assertEqual(streams[0]["duplicate_records_removed"], 1)
        self.assertEqual(streams[0]["coincident_display_records"], 1)
        self.assertEqual(streams[0]["reordered_display_records"], 1)
        self.assertEqual(streams[0]["display_event_intervals_ms"]["samples"], 1)
        self.assertEqual(streams[0]["display_event_intervals_ms"]["p50"], "0.02")
        self.assertEqual(report["presentmon"]["counts"]["foreign_pid_rows"], 1)

    def test_manifest_bad_schema_frequency_identity_and_hash_fail_closed(self):
        self.csv([self.row()])
        for name, value in (("schema_version", 2), ("qpc_frequency", "0"), ("qpc_frequency", "NaN"),
                            ("qpc_frequency", 10000000), ("capture_id", "invalid"), ("stopped_qpc", "-1")):
            with self.subTest(name=name, value=value):
                old = self.manifest[name]
                self.manifest[name] = value
                report = self.run_analysis()
                self.assertEqual(report["status"], "invalid_input")
                self.manifest[name] = old
        self.manifest["files"]["presentmon_csv"]["sha256"] = "0" * 64
        report = self.run_analysis()
        self.assertEqual(report["status"], "invalid_input")
        self.assertIn("SHA256 mismatch", report["errors"][0])

    def test_wrong_trace_pid_hash_or_frequency_never_joins(self):
        self.csv([self.row(100)])
        for changes in ({"pid": 7}, {"exe_sha256": "b" * 64}, {"qpc_frequency": "1"},
                        {"clock_sync_before_ns": 3000}, {"clock_sync_qpc": "-1"}):
            with self.subTest(changes=changes):
                self.trace([self.session(**changes), self.event("input_begin")])
                report = self.run_analysis()
                self.assertFalse(report["trace"]["joined"])
                self.assertIn("identity_or_clock_error", report["trace"])
                self.assertNotIn("action_display_candidates", report)

    def test_multiple_sessions_require_explicit_selection(self):
        self.csv([self.row(100)])
        self.trace([self.session(), self.session("session-B"), self.event("input_begin"),
                    self.event("input_begin", session_id="session-B")])
        self.assertFalse(self.run_analysis()["trace"]["joined"])
        chosen = self.run_analysis(session_id="session-A")
        self.assertTrue(chosen["trace"]["joined"])
        self.assertEqual(chosen["trace"]["ignored_other_session_records"], 1)
        self.assertFalse(self.run_analysis(session_id="missing")["trace"]["joined"])

    def test_swapchain_and_output_selection_preserve_diagnostic_stages(self):
        self.csv([self.row(100), self.row(200, SwapChainAddress="0xDEF")])
        self.trace([self.session(), self.event("input_begin"),
                    self.event("async_stage", op="perf_async_requested", request_id="8")])
        report = self.run_analysis()
        self.assertEqual(report["action_display_candidates"]["items"], [])
        self.assertEqual(report["action_display_candidates"]["stage_counts"]["async_stage:perf_async_requested"], 1)
        chosen = self.run_analysis(swapchain="0xABC")
        self.assertEqual(len(chosen["action_display_candidates"]["items"]), 1)
        columns = list(self.row()) + ["VidPnSourceId", "LayerIndex"]
        self.csv([self.row(100, VidPnSourceId="0", LayerIndex="0"),
                  self.row(200, VidPnSourceId="1", LayerIndex="0")], columns=columns)
        self.assertEqual(self.run_analysis(swapchain="0xABC")["action_display_candidates"]["items"], [])

    def test_overlapping_actions_share_noncausal_candidate_with_clock_bounds(self):
        self.csv([self.row(100)])
        self.trace([self.session(), self.event("input_begin", 3000, "1"), self.event("input_begin", 4000, "2"),
                    self.event("input_dispatch_end", 5000, "1"),
                    self.event("async_stage", 7000, "1", op="perf_model_accept", duration_ns=1000, request_id="8")])
        report = self.run_analysis()
        actions = report["action_display_candidates"]["items"]
        self.assertEqual(len(actions), 2)
        self.assertEqual(actions[0]["same_candidate_action_ids"], ["1", "2"])
        self.assertEqual(actions[0]["dispatch_duration_ms"], "0.002")
        self.assertEqual(actions[0]["diagnostic_stages"][0]["request_id"], "8")
        self.assertEqual(actions[0]["attribution"], "NONCAUSAL_TEMPORAL_CANDIDATE")
        self.assertFalse(actions[0]["actual_action_completion_measured"])
        self.assertGreater(Decimal(actions[0]["delta_ms_bounds"][1]), Decimal(actions[0]["delta_ms_bounds"][0]))
        self.assertFalse(report["trace"]["writer_flush_receipt_verified"])

    def test_display_inside_clock_bracket_is_ambiguous(self):
        self.csv([self.row(10, DisplayLatency="0")])
        self.trace([self.session(), self.event("input_begin", 3000)])
        item = self.run_analysis()["action_display_candidates"]["items"][0]
        self.assertTrue(item["candidate_may_precede_input"])
        self.assertLess(Decimal(item["delta_ms_bounds"][0]), 0)

    def test_sample_loss_malformed_tail_and_drain_mismatch_are_reported(self):
        self.csv([self.row(100)])
        self.trace([self.session(), self.event("input_begin"),
                    {"event": "dropped", "session_id": "session-A", "count": "5"}], tail="\n{", drain=False)
        report = self.run_analysis()
        self.assertEqual(report["trace"]["dropped_records"], 5)
        self.assertEqual(report["trace"]["malformed_lines"], 1)
        self.assertFalse(report["trace"]["writer_drain_counts_match"])
        self.assertTrue(any("truncation" in gap for gap in report["coverage_gaps"]))
        self.assertFalse(report["full_t00_complete"])

    def test_missing_evidence_and_wrong_pid_are_explicit(self):
        report = self.run_analysis()
        self.assertFalse(report["presentmon"]["available"])
        self.assertFalse(report["trace"]["available"])
        self.assertFalse(report["system_etl"]["available"])
        self.csv([self.row(ProcessID=7)])
        report = self.run_analysis()
        self.assertEqual(report["presentmon"]["streams"], [])
        self.assertTrue(any("No valid displayed" in gap for gap in report["coverage_gaps"]))

    def test_legacy_ms_aliases_are_explicitly_supported(self):
        row = self.row()
        row["MsAllInputToPhotonLatency"] = row.pop("AllInputToPhotonLatency")
        row["MsClickToPhotonLatency"] = row.pop("ClickToPhotonLatency")
        row["MsClickToPhotonLatency"] = "2.5"
        self.csv([row], columns=list(row))
        report = self.run_analysis()
        metric = report["presentmon"]["streams"][0]["presentmon_input_latency_ms"]["click_to_photon"]
        self.assertEqual(metric["column"], "MsClickToPhotonLatency")
        self.assertEqual(metric["p50"], "2.5")

    def test_malformed_csv_and_missing_columns_do_not_succeed(self):
        self.file("presentmon_csv", "presentmon.csv", "ProcessID,CPUStartQPC\n42,100\n")
        self.assertEqual(self.run_analysis()["status"], "invalid_input")
        self.csv([self.row(100)])
        raw = (self.root / "presentmon.csv").read_text() + '42,"unterminated'
        self.file("presentmon_csv", "presentmon.csv", raw)
        report = self.run_analysis()
        self.assertEqual(report["presentmon"]["counts"]["parse_errors"], 1)
        self.assertFalse(report["full_t00_complete"])

    def test_cli_writes_reviewable_report_and_invalid_exit_code(self):
        self.manifest["schema_version"] = 999
        self.path.write_text(json.dumps(self.manifest), encoding="utf-8")
        output = self.root / "analysis.json"
        self.assertEqual(analyzer.main(["--manifest", str(self.path), "--output", str(output)]), 2)
        self.assertFalse(json.loads(output.read_text())["input_validated"])

    def kernel_receipt(self, **changes):
        receipt = {"schema_version": 1, "complete": True, "start_status": 0,
                   "stack_status": 0, "flags_status": 0, "stop_status": 0, "wait_result": 258,
                   "events_lost": 0, "log_buffers_lost": 0, "realtime_buffers_lost": 0,
                   "buffers_written": 10, "qpc_frequency": str(self.frequency),
                   "started_qpc": str(self.base), "stopped_qpc": self.manifest["stopped_qpc"]}
        receipt.update(changes)
        self.file("kernel_receipt", "kernel-receipt.json", json.dumps(receipt))

    def test_clean_kernel_receipt_verifies_counters_without_claiming_stacks(self):
        self.kernel_receipt()
        self.file("system_etl", "system.etl", b"hash-only ETL fixture")
        report = self.run_analysis()
        receipt = report["kernel_receipt"]
        self.assertEqual(report["status"], "diagnostic_only")
        self.assertTrue(receipt["complete"])
        self.assertTrue(receipt["collection_complete_without_reported_loss"])
        self.assertEqual(receipt["started_qpc"], str(self.base))
        self.assertFalse(receipt["actual_stack_coverage_verified"])
        self.assertFalse(report["actual_stack_coverage_verified"])
        self.assertFalse(report["system_etl"]["stacks_analyzed"])
        self.assertFalse(report["full_t00_complete"])

    def test_lost_incomplete_kernel_receipt_and_capture_are_explicit(self):
        self.manifest["capture_complete"] = False
        self.kernel_receipt(complete=False, events_lost=7, log_buffers_lost=2,
                            realtime_buffers_lost=1, stack_status=5, stop_status=6,
                            wait_result=0, buffers_written=0, started_qpc=str(self.base + 1))
        report = self.run_analysis()
        self.assertFalse(report["kernel_receipt"]["collection_complete_without_reported_loss"])
        gaps = "\n".join(report["coverage_gaps"])
        for text in ("capture_complete=false", "incomplete capture", "events_lost=7", "log_buffers_lost=2",
                     "realtime_buffers_lost=1", "stack_status", "stop_status", "wait_result=0",
                     "zero buffers_written", "only part"):
            self.assertIn(text, gaps)
        for wait_result in (0, 1):
            with self.subTest(wait_result=wait_result):
                self.kernel_receipt(wait_result=wait_result)
                cancelled = self.run_analysis()
                self.assertFalse(cancelled["kernel_receipt"]["collection_complete_without_reported_loss"])
                self.assertTrue(any(f"wait_result={wait_result}" in gap and "incomplete" in gap
                                    for gap in cancelled["coverage_gaps"]))

    def test_kernel_receipt_wrong_clock_schema_or_invalid_counters_are_rejected(self):
        for changes in ({"qpc_frequency": "1"}, {"qpc_frequency": self.frequency},
                        {"stopped_qpc": str(self.base + 10000001)}, {"started_qpc": "-1"},
                        {"schema_version": 2}, {"complete": "true"}, {"events_lost": -1},
                        {"stop_status": True}, {"wait_result": 0x100000000}):
            with self.subTest(changes=changes):
                self.kernel_receipt(**changes)
                report = self.run_analysis()
                self.assertEqual(report["status"], "invalid_input")
                self.assertFalse(report["input_validated"])
                self.assertIn("kernel receipt", report["errors"][0])


if __name__ == "__main__":
    unittest.main()
