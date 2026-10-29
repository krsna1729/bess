#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for tools/bench_trend.py.

No GitHub, no compiler, no network: every decision is exercised on synthetic
samples and synthetic run records.  Run with:

    python3 tools/test_bench_trend.py
    python3 -m unittest discover -s tools -p 'test_*.py'
"""

import json
import statistics
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

try:  # PyYAML is in the repo's dependency set; the tool itself needs only stdlib.
    import yaml
except ImportError:  # pragma: no cover
    yaml = None

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_trend as bt  # noqa: E402


def samples(value, count=5):
    return [float(value)] * count


def aggregate(sample_list):
    return {
        "median": statistics.median(sample_list),
        "mean": statistics.fmean(sample_list),
        "cv": bt.coefficient_of_variation(sample_list),
    }


def make_commit(sha, values, confirm=None, date="2026-10-01T00:00:00+00:00"):
    commit = {
        "sha": sha,
        "date": date,
        "subject": f"commit {sha}",
        "sample": {"benchmarks": {
            name: {"samples": list(vals), "aggregates": aggregate(vals)}
            for name, vals in values.items()
        }},
    }
    if confirm:
        commit["confirm"] = {"benchmarks": {
            name: {"samples": list(vals), "aggregates": aggregate(vals)}
            for name, vals in confirm.items()
        }}
    return commit


def run_file(commits, kind="nightly", hardware=None):
    return {
        "schema": 1,
        "kind": kind,
        "hardware": hardware or {"model": "Test CPU", "kernel": "6.0", "fingerprint": "abc"},
        "commits": commits,
    }


def verdicts_file(flagged=(), comparisons=None, hardware=None, measured_tip="c4"):
    return {
        "schema": 1,
        "kind": "nightly",
        "measured_tip": measured_tip,
        "hardware": hardware or {"model": "Test CPU", "kernel": "6.0", "fingerprint": "abc"},
        "commits": [],
        "comparisons": list(comparisons or []),
        "flagged": list(flagged),
        "needs_rerun": [],
    }


def flagged_entry(name="BM_Test", commit="c4", confirm=True):
    return {
        "benchmark": name,
        "commit": commit,
        "baseline_commit": "c3",
        "commit_range": {"first_bad": commit, "last_good": "c3"},
        "current_median": 120.0,
        "baseline_median": 100.0,
        "ratio": 1.2,
        "threshold": 0.05,
        "ci": [1.15, 1.25],
        "historical_cv": 0.01,
        "confirm_ratio": 1.19,
        "confirm_ci": [1.14, 1.24],
        "confirmed": confirm,
    }


class StatisticsTest(unittest.TestCase):
    def test_coefficient_of_variation(self):
        self.assertEqual(bt.coefficient_of_variation([100.0]), 0.0)
        self.assertAlmostEqual(bt.coefficient_of_variation([100.0, 100.0]), 0.0)
        # Population stdev of [90, 110] is 10 against a mean of 100.
        self.assertAlmostEqual(bt.coefficient_of_variation([90.0, 110.0]), 0.10)

    def test_bootstrap_ci_excludes_one_for_a_clear_shift(self):
        low, high = bt.bootstrap_ratio_ci(samples(130), samples(100), iterations=2000)
        self.assertGreater(low, 1.0)
        self.assertAlmostEqual(low, 1.30)
        self.assertAlmostEqual(high, 1.30)

    def test_bootstrap_ci_includes_one_for_overlapping_samples(self):
        low, high = bt.bootstrap_ratio_ci([95, 105, 95, 105, 100],
                                          [95, 105, 95, 105, 100],
                                          iterations=2000)
        self.assertLessEqual(low, 1.0)
        self.assertGreaterEqual(high, 1.0)

    def test_bootstrap_is_deterministic_for_a_seed(self):
        first = bt.bootstrap_ratio_ci([90, 100, 110, 95, 105],
                                      [92, 102, 112, 97, 107], iterations=500, seed=7)
        second = bt.bootstrap_ratio_ci([90, 100, 110, 95, 105],
                                       [92, 102, 112, 97, 107], iterations=500, seed=7)
        self.assertEqual(first, second)


class ParsingTest(unittest.TestCase):
    def test_aggregates_and_samples_are_split(self):
        document = {"benchmarks": [
            {"name": "BM_A", "run_name": "BM_A", "run_type": "iteration",
             "cpu_time": 100.0, "time_unit": "ns"},
            {"name": "BM_A", "run_name": "BM_A", "run_type": "iteration",
             "cpu_time": 102.0, "time_unit": "ns"},
            {"name": "BM_A_median", "run_name": "BM_A", "run_type": "aggregate",
             "aggregate_name": "median", "cpu_time": 101.0, "time_unit": "ns"},
        ]}
        parsed = bt.parse_benchmark_json(json.dumps(document))
        self.assertEqual(parsed["BM_A"]["samples"], [100.0, 102.0])
        self.assertEqual(parsed["BM_A"]["aggregates"]["median"], 101.0)

    def test_time_units_are_converted_to_nanoseconds(self):
        document = {"benchmarks": [
            {"name": "BM_A", "run_name": "BM_A", "run_type": "iteration",
             "cpu_time": 1.5, "time_unit": "us"},
        ]}
        parsed = bt.parse_benchmark_json(json.dumps(document))
        self.assertEqual(parsed["BM_A"]["samples"], [1500.0])

    def test_leading_output_before_the_json_document_is_tolerated(self):
        document = {"benchmarks": [
            {"name": "BM_A", "run_name": "BM_A", "run_type": "iteration",
             "cpu_time": 2.0, "time_unit": "ns"},
        ]}
        parsed = bt.parse_benchmark_json("EAL: a warning\n" + json.dumps(document))
        self.assertEqual(parsed["BM_A"]["samples"], [2.0])

    def test_skipped_runs_are_not_recorded_as_zero_timings(self):
        # The shape Google Benchmark emits for SkipWithMessage: cpu_time 0.
        document = {"benchmarks": [
            {"name": "BM_A/1", "run_name": "BM_A/1", "run_type": "iteration",
             "cpu_time": 50.0, "time_unit": "ns"},
            {"name": "BM_A/4", "run_name": "BM_A/4", "run_type": "iteration",
             "error_occurred": True, "error_message": "producer count exceeds",
             "cpu_time": 0.0, "time_unit": "ns"},
            {"name": "BM_A/16", "run_name": "BM_A/16", "run_type": "iteration",
             "skipped": "skipped_with_message", "cpu_time": 0.0, "time_unit": "ns"},
        ]}
        parsed = bt.parse_benchmark_json(json.dumps(document))
        self.assertEqual(sorted(parsed), ["BM_A/1"])

    def test_command_carries_the_required_flags(self):
        command = bt.benchmark_command("core/ring_bench", "^BM_Ring", "1s", 5, True)
        joined = " ".join(command)
        self.assertIn("--benchmark_min_time=1s", joined)
        self.assertIn("--benchmark_repetitions=5", joined)
        self.assertIn("--benchmark_report_aggregates_only=true", joined)
        self.assertIn("--benchmark_format=json", joined)
        sample_command = bt.benchmark_command("core/ring_bench", "^BM_Ring", "1s",
                                              5, False)
        self.assertNotIn("--benchmark_report_aggregates_only=true",
                         " ".join(sample_command))


class GateTest(unittest.TestCase):
    def config(self, threshold=0.05, cv_limit=0.10):
        return {"default": threshold, "cv_limit": cv_limit, "benchmarks": {}}

    def history(self, medians):
        return [{"samples": samples(value), "aggregates": aggregate(samples(value))}
                for value in medians]

    def test_a_zero_baseline_is_not_a_comparison(self):
        # History recorded before skipped runs were filtered holds zeros;
        # dividing by them crashed the nightly analysis.
        history = [{"samples": [0.0] * 5, "aggregates": {}} for _ in range(3)]
        current = {"samples": samples(100), "aggregates": aggregate(samples(100))}
        entry = bt.compare_benchmark("BM_Test", current, history, history,
                                     self.config(), 1000, "nightly")
        self.assertIsNone(entry["ratio"])
        self.assertFalse(entry["regressed"])
        self.assertIn("baseline median is not positive", entry["reasons"])

    def test_threshold_is_per_benchmark_and_defaults_to_five_percent(self):
        config = self.config()
        config["benchmarks"]["BM_Loud"] = 0.20
        self.assertEqual(bt.threshold_for("BM_Loud", config), 0.20)
        self.assertEqual(bt.threshold_for("BM_Loud/8", config), 0.20)
        self.assertEqual(bt.threshold_for("BM_Quiet/8", config), 0.05)

    def test_regression_flagged_only_above_per_benchmark_threshold(self):
        history = self.history([100, 100, 100])
        current = {"samples": samples(106), "aggregates": aggregate(samples(106))}
        loose = bt.compare_benchmark("BM_Test", current, history, history,
                                     self.config(0.10), 1000, "nightly")
        strict = bt.compare_benchmark("BM_Test", current, history, history,
                                      self.config(0.05), 1000, "nightly")
        self.assertFalse(loose["regressed"])
        self.assertTrue(any("threshold" in r for r in loose["reasons"]))
        self.assertTrue(strict["regressed"])

    def test_wide_ci_blocks_a_ratio_above_threshold(self):
        history = self.history([100, 100, 100])
        current = {"samples": [100, 110, 100, 110, 105],
                   "aggregates": aggregate([100, 110, 100, 110, 105])}
        entry = bt.compare_benchmark("BM_Test", current, history, history,
                                     self.config(0.04), 3000, "nightly")
        self.assertGreater(entry["ratio"], 0.04)
        self.assertFalse(entry["regressed"])
        self.assertTrue(any("CI" in r for r in entry["reasons"]))

    def test_high_historical_cv_blocks_a_clear_shift(self):
        unstable = self.history([80, 120, 80, 120])
        current = {"samples": samples(200), "aggregates": aggregate(samples(200))}
        entry = bt.compare_benchmark("BM_Test", current, unstable, unstable,
                                     self.config(), 1000, "nightly")
        self.assertGreater(entry["current_median"] / entry["baseline_median"], 1.05)
        self.assertTrue(entry["eligible"])
        self.assertFalse(entry["stable"])
        self.assertFalse(entry["regressed"])
        self.assertTrue(any("CV" in r for r in entry["reasons"]))

    def test_fewer_than_three_prior_commits_is_not_evidence(self):
        history = self.history([100, 100])
        current = {"samples": samples(200), "aggregates": aggregate(samples(200))}
        entry = bt.compare_benchmark("BM_Test", current, history, history,
                                     self.config(), 1000, "nightly")
        self.assertFalse(entry["regressed"])
        self.assertFalse(entry["eligible"])


class AnalyzeTest(unittest.TestCase):
    ROOT = str(Path(__file__).resolve().parents[1])

    def analyze(self, run, kind=None):
        with tempfile.TemporaryDirectory() as directory:
            run_path = Path(directory) / "run.json"
            out_path = Path(directory) / "verdicts.json"
            run_path.write_text(json.dumps(run))
            argv = ["--repo", self.ROOT,
                    "analyze", "--run", str(run_path), "--out", str(out_path),
                    "--bootstrap-iterations", "1500"]
            if kind:
                argv += ["--kind", kind]
            self.assertEqual(bt.main(argv), 0)
            return json.loads(out_path.read_text())

    def stable_run(self, last_value=130, last_confirm=None):
        commits = [make_commit(f"c{i}", {"BM_Test": samples(100)})
                   for i in range(4)]
        commits.append(make_commit("c4", {"BM_Test": samples(last_value)},
                                   confirm=({"BM_Test": samples(last_confirm)}
                                            if last_confirm is not None else None)))
        return run_file(commits)

    def test_regression_needs_a_confirmation_run_before_it_can_open_an_issue(self):
        verdicts = self.analyze(self.stable_run())
        self.assertEqual(verdicts["flagged"], [])
        self.assertEqual(len(verdicts["needs_rerun"]), 1)
        self.assertEqual(verdicts["needs_rerun"][0]["commit"], "c4")
        candidate = next(entry for entry in verdicts["comparisons"]
                         if entry["commit"] == "c4")
        self.assertTrue(candidate["regressed"])
        self.assertFalse(candidate["confirmed"])

    def test_confirmation_reproduces_and_flags(self):
        verdicts = self.analyze(self.stable_run(last_confirm=131))
        self.assertEqual([entry["benchmark"] for entry in verdicts["flagged"]],
                         ["BM_Test"])
        self.assertEqual(verdicts["needs_rerun"], [])
        self.assertEqual(verdicts["measured_tip"], "c4")
        flagged = verdicts["flagged"][0]
        self.assertEqual(flagged["commit_range"]["first_bad"], "c4")
        self.assertEqual(flagged["commit_range"]["last_good"], "c3")

    def test_failed_confirmation_does_not_flag(self):
        verdicts = self.analyze(self.stable_run(last_value=130, last_confirm=100))
        self.assertEqual(verdicts["flagged"], [])
        candidate = next(entry for entry in verdicts["comparisons"]
                         if entry["commit"] == "c4")
        self.assertTrue(candidate["confirmation_failed"])
        self.assertFalse(candidate["confirmed"])
        self.assertEqual(verdicts["needs_rerun"], [])

    def test_attribution_ignores_a_rejected_candidate(self):
        commits = [make_commit(f"c{i}", {"BM_Test": samples(100)})
                   for i in range(3)]
        commits.append(make_commit("c3", {"BM_Test": samples(110)},
                                   confirm={"BM_Test": samples(100)}))
        commits.append(make_commit("c4", {"BM_Test": samples(131)},
                                   confirm={"BM_Test": samples(131)}))
        verdicts = self.analyze(run_file(commits))
        flagged = verdicts["flagged"][0]
        self.assertEqual(flagged["commit"], "c4")
        self.assertEqual(flagged["commit_range"]["first_bad"], "c4")
        self.assertEqual(flagged["commit_range"]["last_good"], "c3")

    def test_attribution_ignores_a_candidate_the_run_recovered_from(self):
        commits = [make_commit(f"c{i}", {"BM_Test": samples(100)})
                   for i in range(3)]
        commits.append(make_commit("c3", {"BM_Test": samples(110)}))
        commits.append(make_commit("c4", {"BM_Test": samples(100)}))
        commits.append(make_commit("c5", {"BM_Test": samples(131)},
                                   confirm={"BM_Test": samples(131)}))
        verdicts = self.analyze(run_file(commits))
        flagged = verdicts["flagged"][0]
        self.assertEqual(flagged["commit"], "c5")
        self.assertEqual(flagged["commit_range"]["first_bad"], "c5")
        self.assertEqual(flagged["commit_range"]["last_good"], "c4")

    def test_attribution_finds_the_first_commit_that_crossed_the_gate(self):
        commits = [make_commit(f"c{i}", {"BM_Test": samples(100)})
                   for i in range(3)]
        commits.append(make_commit("c3", {"BM_Test": samples(110)}))
        commits.append(make_commit("c4", {"BM_Test": samples(131)},
                                   confirm={"BM_Test": samples(131)}))
        verdicts = self.analyze(run_file(commits))
        flagged = verdicts["flagged"][0]
        self.assertEqual(flagged["commit"], "c4")
        self.assertEqual(flagged["commit_range"]["first_bad"], "c3")
        self.assertEqual(flagged["commit_range"]["last_good"], "c2")

    def test_a_single_commit_run_makes_no_decision(self):
        verdicts = self.analyze(run_file(
            [make_commit("c0", {"BM_Test": samples(100)})]))
        self.assertEqual(verdicts["comparisons"], [])
        self.assertEqual(verdicts["flagged"], [])
        self.assertEqual(verdicts["needs_rerun"], [])
        self.assertEqual(verdicts["measured_tip"], "c0")
        self.assertEqual(verdicts["commits"][0]["sha"], "c0")

    def test_weekly_trend_compares_the_latest_day_with_the_prior_days(self):
        values = [100, 100, 101, 100, 100, 100, 130]
        commits = [make_commit(f"d{i}", {"BM_Test": samples(value)},
                               date=f"2026-10-0{i + 1}T00:00:00+00:00")
                   for i, value in enumerate(values)]
        verdicts = self.analyze(run_file(commits, kind="weekly"), kind="weekly")
        candidate = next(entry for entry in verdicts["comparisons"]
                         if entry["commit"] == "d6")
        self.assertAlmostEqual(candidate["baseline_median"], 100.0)
        self.assertTrue(candidate["regressed"])
        self.assertEqual(verdicts["needs_rerun"][0]["commit"], "d6")

    def test_verdicts_do_not_depend_on_the_run_hardware(self):
        # Decisions are made inside the run; the hardware is report metadata.
        run = self.stable_run(last_confirm=131)
        first = self.analyze(run)
        run["hardware"] = {"model": "Another CPU", "kernel": "9.9", "fingerprint": "zzz"}
        second = self.analyze(run)
        self.assertEqual([e["benchmark"] for e in first["flagged"]],
                         [e["benchmark"] for e in second["flagged"]])


class CommitSelectionTest(unittest.TestCase):
    LOG = "\n".join(
        f"{sha}\x002026-10-0{i + 1}T12:00:00+00:00\x00subject {sha}"
        for i, sha in enumerate(["a", "b", "c", "d"]))  # newest first

    def test_nightly_takes_the_last_n_first_parent_commits_oldest_first(self):
        def fake_git(root, *args, check=True):
            self.assertIn("-n3", args)
            return "\n".join(self.LOG.splitlines()[:3])

        with mock.patch.object(bt, "git", side_effect=fake_git):
            commits = bt.nightly_commits(Path("."), "develop", 3)
        self.assertEqual([c["sha"] for c in commits], ["c", "b", "a"])

    def test_weekly_picks_the_newest_commit_before_each_utc_day(self):
        shas = iter(["s5", "s4", "s4", "s3", "s2", "s1", "s0"])
        calls = []

        def fake_git(root, *args, check=True):
            calls.append(args)
            return next(shas)

        with mock.patch.object(bt, "git", side_effect=fake_git), \
                mock.patch.object(bt, "commit_metadata",
                                  side_effect=lambda root, sha: {"sha": sha}):
            commits = bt.weekly_commits(Path("."), "develop", 7,
                                        today="2026-10-07")
        self.assertEqual(len(calls), 7)
        self.assertIn("--before=2026-10-01T00:00:00+00:00", calls[0])
        self.assertIn("--before=2026-10-07T00:00:00+00:00", calls[-1])
        # A quiet day returns the same commit as the boundary before it.
        self.assertEqual([c["sha"] for c in commits], ["s5", "s4", "s3", "s2", "s1", "s0"])

    def test_weekly_honours_a_reproducible_today(self):
        with mock.patch.object(bt, "git", return_value="") as fake_git, \
                mock.patch.object(bt, "commit_metadata"):
            bt.weekly_commits(Path("."), "develop", 2, today="2026-01-02")
        first = fake_git.call_args_list[0][0]
        self.assertIn("--before=2026-01-01T00:00:00+00:00", first)


class IssuePlanTest(unittest.TestCase):
    REPO = "owner/bess"
    URL = "https://github.com/owner/bess/blob/benchmarks/bench-history.jsonl"

    def plan(self, flagged, comparisons=None, history=None, open_issues=None,
             measured_tip="c4"):
        return bt.plan_issue_actions(
            verdicts_file(flagged=flagged, comparisons=comparisons,
                          measured_tip=measured_tip),
            history or [], open_issues or [], self.REPO, self.URL,
            "https://github.com/owner/bess/actions/runs/1")

    def test_one_open_issue_is_opened_per_benchmark(self):
        actions = self.plan([flagged_entry("BM_A"), flagged_entry("BM_B")])
        self.assertEqual([(a["action"], a["benchmark"]) for a in actions],
                         [("open", "BM_A"), ("open", "BM_B")])
        self.assertIn("[bench] BM_A regression", actions[0]["title"])
        self.assertIn("bench-history.jsonl", actions[0]["body"])

    def test_an_existing_open_issue_is_commented_not_duplicated(self):
        issue = {"number": 7, "title": "[bench] BM_Test regression",
                 "body": "old", "labels": [{"name": bt.ISSUE_LABEL}]}
        actions = self.plan([flagged_entry()], open_issues=[issue])
        self.assertEqual(len(actions), 1)
        self.assertEqual(actions[0]["action"], "comment")
        self.assertEqual(actions[0]["number"], 7)

    def test_the_same_run_is_not_commented_twice(self):
        issue = {"number": 7, "title": "[bench] BM_Test regression",
                 "body": bt._marker("BM_Test", "c4"),
                 "labels": [{"name": bt.ISSUE_LABEL}]}
        self.assertEqual(self.plan([flagged_entry()], open_issues=[issue]), [])

    def test_a_marker_in_an_existing_comment_deduplicates_too(self):
        issue = {"number": 7, "title": "[bench] BM_Test regression",
                 "body": "opened by an earlier run",
                 "comments": [{"body": bt._marker("BM_Test", "c4")}],
                 "labels": [{"name": bt.ISSUE_LABEL}]}
        self.assertEqual(self.plan([flagged_entry()], open_issues=[issue]), [])

    def test_duplicate_open_issues_are_closed_against_the_oldest(self):
        issues = [
            {"number": 9, "title": "[bench] BM_Test regression", "body": "",
             "labels": [{"name": bt.ISSUE_LABEL}]},
            {"number": 4, "title": "[bench] BM_Test regression", "body": "",
             "labels": [{"name": bt.ISSUE_LABEL}]},
        ]
        actions = self.plan([flagged_entry()], open_issues=issues)
        self.assertEqual([(a["action"], a["number"]) for a in actions],
                         [("comment", 4), ("close", 9)])

    def test_issue_titles_are_the_deduplication_key(self):
        unrelated = {"number": 3, "title": "Something else", "body": "",
                     "labels": [{"name": bt.ISSUE_LABEL}]}
        actions = self.plan([flagged_entry()], open_issues=[unrelated])
        self.assertEqual(actions[0]["action"], "open")

    def test_recovery_comments_on_the_first_clean_run_and_closes_on_the_second(self):
        issue = {"number": 5, "title": "[bench] BM_Test regression",
                 "body": "", "labels": [{"name": bt.ISSUE_LABEL}]}
        comparisons = [{"benchmark": "BM_Test", "commit": "c4", "ratio": 1.0,
                        "ci": [0.98, 1.02], "current_median": 100.0,
                        "baseline_median": 100.0, "historical_cv": 0.01,
                        "threshold": 0.05, "eligible": True, "stable": True,
                        "reasons": []}]
        first = self.plan([], comparisons, history=[{"flagged": ["BM_Test"]}],
                          open_issues=[issue])
        self.assertEqual(first[0]["action"], "comment")
        second = self.plan([], comparisons, history=[{"clean": ["BM_Test"]}],
                           open_issues=[issue])
        self.assertEqual(second[0]["action"], "close")

    def test_a_record_that_did_not_measure_the_benchmark_does_not_count_as_clean(self):
        issue = {"number": 5, "title": "[bench] BM_Test regression",
                 "body": "", "labels": [{"name": bt.ISSUE_LABEL}]}
        comparisons = [{"benchmark": "BM_Test", "commit": "c4", "ratio": 1.0,
                        "ci": [0.98, 1.02], "current_median": 100.0,
                        "baseline_median": 100.0, "historical_cv": 0.01,
                        "threshold": 0.05, "eligible": True, "reasons": []}]
        actions = self.plan([], comparisons,
                            history=[{"flagged": [], "benchmarks": {}}],
                            open_issues=[issue])
        self.assertEqual(actions, [])

    def test_an_unstable_clean_verdict_does_not_start_recovery(self):
        issue = {"number": 5, "title": "[bench] BM_Test regression",
                 "body": "", "labels": [{"name": bt.ISSUE_LABEL}]}
        # The real producer shape for a high-CV benchmark: eligible (enough
        # data) but not stable (the CV gate rejected it).
        comparisons = [{"benchmark": "BM_Test", "commit": "c4", "ratio": 1.0,
                        "ci": [0.9, 1.1], "current_median": 100.0,
                        "baseline_median": 100.0, "historical_cv": 0.2,
                        "threshold": 0.05, "eligible": True, "stable": False,
                        "reasons": ["historical CV"]}]
        actions = self.plan([], comparisons,
                            history=[{"flagged": ["BM_Test"]}],
                            open_issues=[issue])
        self.assertEqual(actions, [])

    def test_recovery_requires_a_verdict_from_the_final_measured_commit(self):
        issue = {"number": 5, "title": "[bench] BM_Test regression",
                 "body": "", "labels": [{"name": bt.ISSUE_LABEL}]}
        comparisons = [{"benchmark": "BM_Test", "commit": "c3", "ratio": 1.0,
                        "ci": [0.98, 1.02], "current_median": 100.0,
                        "baseline_median": 100.0, "historical_cv": 0.01,
                        "threshold": 0.05, "eligible": True, "stable": True,
                        "reasons": []}]
        actions = self.plan([], comparisons, measured_tip="c4",
                            history=[{"clean": ["BM_Test"]}],
                            open_issues=[issue])
        self.assertEqual(actions, [])

    def test_a_current_flag_does_not_trigger_recovery(self):
        issue = {"number": 5, "title": "[bench] BM_Test regression",
                 "body": "", "labels": [{"name": bt.ISSUE_LABEL}]}
        actions = self.plan([flagged_entry()], open_issues=[issue])
        self.assertEqual(actions[0]["action"], "comment")
        self.assertNotIn("clean run", actions[0]["body"])


class RunFileTest(unittest.TestCase):
    def test_history_records_one_jsonl_line_per_run(self):
        run = run_file([make_commit("c0", {"BM_Test": samples(100)}),
                        make_commit("c1", {"BM_Test": samples(120)})])
        verdicts = verdicts_file(
            flagged=[flagged_entry()],
            comparisons=[{"benchmark": "BM_Test", "commit": "c1", "ratio": 1.2,
                          "current_median": 120.0, "baseline_median": 100.0,
                          "ci": [1.15, 1.25], "historical_cv": 0.01}])
        with tempfile.TemporaryDirectory() as directory:
            run_path = Path(directory) / "run.json"
            verdicts_path = Path(directory) / "verdicts.json"
            history_path = Path(directory) / "history.jsonl"
            run_path.write_text(json.dumps(run))
            verdicts_path.write_text(json.dumps(verdicts))
            self.assertEqual(bt.main(["history", "--run", str(run_path),
                                      "--verdicts", str(verdicts_path),
                                      "--history", str(history_path)]), 0)
            bt.main(["history", "--run", str(run_path),
                     "--verdicts", str(verdicts_path),
                     "--history", str(history_path)])
            lines = history_path.read_text().splitlines()
            self.assertEqual(len(lines), 2)
            record = json.loads(lines[0])
            self.assertEqual(record["kind"], "nightly")
            self.assertEqual(record["hardware"]["model"], "Test CPU")
            self.assertEqual(len(record["commits"]), 2)
            self.assertEqual(record["flagged"], ["BM_Test"])
            self.assertEqual(record["clean"], [])
            self.assertEqual(record["commit"], "c4")
            self.assertEqual(record["benchmarks"]["BM_Test"][0]["median_ns"], 120.0)

    def test_history_clean_requires_a_stable_verdict_at_the_measured_tip(self):
        clean_entry = {"ratio": 1.0, "current_median": 100.0,
                       "baseline_median": 100.0, "ci": [0.98, 1.02],
                       "historical_cv": 0.01, "eligible": True, "stable": True,
                       "regressed": False}
        stale = dict(clean_entry, benchmark="BM_Earlier", commit="c3")
        tip = dict(clean_entry, benchmark="BM_Tip", commit="c4")
        unstable = dict(clean_entry, benchmark="BM_Noisy", commit="c4",
                        stable=False)
        verdicts = verdicts_file(comparisons=[stale, tip, unstable],
                                 measured_tip="c4")
        run = run_file([make_commit("c3", {"BM_Tip": samples(100)}),
                        make_commit("c4", {"BM_Tip": samples(100)})])
        with tempfile.TemporaryDirectory() as directory:
            run_path = Path(directory) / "run.json"
            verdicts_path = Path(directory) / "verdicts.json"
            history_path = Path(directory) / "history.jsonl"
            run_path.write_text(json.dumps(run))
            verdicts_path.write_text(json.dumps(verdicts))
            bt.main(["history", "--run", str(run_path),
                     "--verdicts", str(verdicts_path),
                     "--history", str(history_path)])
            record = json.loads(history_path.read_text().splitlines()[0])
        self.assertEqual(record["clean"], ["BM_Tip"])
        self.assertEqual(record["commit"], "c4")

    def test_rerun_shas_are_unique_and_bounded(self):
        verdicts = verdicts_file()
        verdicts["needs_rerun"] = [
            {"commit": "c1", "benchmarks": ["BM_A"]},
            {"commit": "c1", "benchmarks": ["BM_B"]},
            {"commit": "c2", "benchmarks": ["BM_A"]},
            {"commit": "c3", "benchmarks": ["BM_A"]},
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "verdicts.json"
            path.write_text(json.dumps(verdicts))
            with mock.patch("sys.stdout") as stdout:
                bt.main(["rerun-shas", "--verdicts", str(path), "--limit", "2"])
            printed = [call.args[0] for call in stdout.write.call_args_list]
        self.assertEqual("".join(printed), "c1\nc2\n")

    def test_threshold_config_is_read_from_a_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "thresholds.json"
            path.write_text(json.dumps({"default": 0.04, "cv_limit": 0.07,
                                        "benchmarks": {"BM_Test": 0.11}}))
            config = bt.load_config(path)
        self.assertEqual(bt.threshold_for("BM_Test", config), 0.11)
        self.assertEqual(bt.threshold_for("BM_Other", config), 0.04)
        self.assertEqual(config["cv_limit"], 0.07)

    def test_run_one_dry_run_prints_the_commands_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            run_path = Path(directory) / "run.json"
            argv = ["run-one", "--run", str(run_path), "--commit", "HEAD",
                    "--build-dir", str(Path(directory) / "missing"),
                    "--dry-run"]
            self.assertEqual(bt.main(argv), 0)
            self.assertFalse(run_path.exists())

    def test_targets_lists_the_meson_targets(self):
        with mock.patch("sys.stdout") as stdout:
            bt.targets(None)
        printed = "".join(call.args[0] for call in stdout.write.call_args_list)
        for binary in ("core/ring_bench", "core/fib_bench",
                       "core/classifier_bench"):
            self.assertIn(binary, printed)


@unittest.skipUnless(yaml, "PyYAML is not installed")
class WorkflowTest(unittest.TestCase):
    """Structural assertions on the workflow, without a GitHub runner."""

    WORKFLOW = Path(__file__).resolve().parents[1] / ".github" / "workflows" / "nightly-bench.yml"

    @classmethod
    def setUpClass(cls):
        cls.document = yaml.safe_load(cls.WORKFLOW.read_text())
        cls.steps = cls.document["jobs"]["bench"]["steps"]

    def step(self, name):
        return next(step for step in self.steps if step.get("name") == name)

    def test_workflow_env_does_not_use_the_runner_context(self):
        # Only github, secrets, inputs and vars are available at workflow level.
        for key, value in self.document["env"].items():
            self.assertNotIn("runner.", str(value), key)

    def test_runner_dependent_paths_come_from_runner_temp_in_a_step(self):
        step = self.step("Resolve the run's paths")
        self.assertIn("RUNNER_TEMP", step["run"])
        self.assertIn("GITHUB_ENV", step["run"])
        for key in ("BENCH_TOOL", "BENCH_PROFILE", "BENCH_CONFIG",
                    "RUN_DIR", "HISTORY_DIR"):
            self.assertIn(key, step["run"])

    def test_bootstrap_prints_commands_only_in_a_dry_run(self):
        step = self.step("Bootstrap pinned DPDK")
        self.assertIn("BENCH_DRY_RUN", step["run"])
        self.assertIn("exit 0", step["run"])
        library_path = self.step("Record the DPDK library path")
        self.assertEqual(library_path["if"], "env.BENCH_DRY_RUN != 'true'")

    def test_issue_list_fetches_comment_bodies_for_dedupe(self):
        step = self.step("Open, update or close one issue per benchmark")
        self.assertIn("--json number,title,body,labels,comments", step["run"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
