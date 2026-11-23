import importlib.util
from pathlib import Path
import unittest


SPEC = importlib.util.spec_from_file_location(
    "profile_postgres", Path(__file__).resolve().parents[1] / "profile-postgres.py"
)
PROFILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILE)


def wake(timestamp, pid):
    return (
        f"[000] {timestamp}: sched:sched_wakeup: "
        f"comm=postgres pid={pid} prio=120 target_cpu=0\n"
    )


def switch(timestamp, previous, state, following):
    return (
        f"[000] {timestamp}: sched:sched_switch: prev_comm=postgres "
        f"prev_pid={previous} prev_prio=120 prev_state={state} ==> "
        f"next_comm=postgres next_pid={following} next_prio=120\n"
    )


def sample(begin, end, workers=(20,)):
    return {
        "type": "sample", "begin_ns": begin, "end_ns": end,
        "backends": [
            {
                "pid": 10, "leader_pid": None, "backend_type": "client backend",
                "state": "active", "wait_event_type": "IPC",
                "wait_event": "MessageQueueReceive", "query_start": "query-one",
            },
            *[
                {"pid": pid, "leader_pid": 10, "backend_type": "parallel worker"}
                for pid in workers
            ],
        ],
    }


class RunqueueTests(unittest.TestCase):
    def test_perf_compact_scheduler_records_are_decoded(self):
        lines = [
            "[000]  4301.529249251:       sched:sched_wakeup: migration/0:19 [0] CPU:000\n",
            "[000]  4301.529251644:       sched:sched_switch: perf:14613 [120] R ==> migration/0:19 [0]\n",
            "[000]  4301.529261342:       sched:sched_switch: migration/0:19 [0] S ==> swapper/0:0 [120]\n",
            "[001]  4301.529275540:       sched:sched_wakeup: migration/1:43 [0] CPU:001\n",
            "[001]  4301.529276947:       sched:sched_switch: perf:14613 [120] R ==> migration/1:43 [0]\n",
            "[001]  4301.529283687:       sched:sched_switch: migration/1:43 [0] S ==> swapper/1:0 [120]\n",
        ]
        intervals, quality = PROFILE.runqueue_intervals(lines, {19, 43})
        self.assertEqual(intervals[19], [(4_301_529_249_251, 4_301_529_251_644)])
        self.assertEqual(intervals[43], [(4_301_529_275_540, 4_301_529_276_947)])
        self.assertEqual(quality["unfinished_enqueues"], 0)

    def test_mixed_formats_signed_priorities_and_preemption_are_supported(self):
        lines = [
            "[000] 1.000000000: sched:sched_wakeup_new: comm=work: queue pid=20 prio=-1 target_cpu=0\n",
            "[000] 1.010000000: sched:sched_switch: swapper/0:0 [120] S ==> work: queue:20 [-1]\n",
            "[000] 1.020000000: sched:sched_switch: work: queue:20 [-1] R+ ==> swapper/0:0 [120]\n",
            switch("1.030000000", 0, "S", 20).replace("next_prio=120", "next_prio=-1"),
            "[000] 1.040000000: sched:sched_process_exit: work: queue:20 [-1]\n",
            switch("1.050000000", 20, "X", 0).replace("prev_prio=120", "prev_prio=-1"),
        ]
        intervals, quality = PROFILE.runqueue_intervals(lines, {20})
        self.assertEqual(intervals[20], [
            (1_000_000_000, 1_010_000_000), (1_020_000_000, 1_030_000_000),
        ])
        self.assertEqual(quality["unfinished_enqueues"], 0)

    def test_malformed_compact_record_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Malformed sched_wakeup"):
            PROFILE.runqueue_intervals([
                "[000] 1.000000000: sched:sched_wakeup: postgres:20 [120] CPU:invalid\n",
            ], {20})

    def test_wake_preemption_and_running_wake_are_distinguished(self):
        lines = [
            "# cmdline : perf record -a --clockid mono -e sched:sched_switch\n",
            wake("1.000000000", 20),
            switch("1.010000000", 0, "S", 20),
            wake("1.015000000", 20),
            switch("1.020000000", 20, "R+", 0),
            switch("1.030000000", 0, "S", 20),
            switch("1.040000000", 20, "S", 0),
        ]
        intervals, quality = PROFILE.runqueue_intervals(lines, {20})
        self.assertEqual(intervals[20], [
            (1_000_000_000, 1_010_000_000), (1_020_000_000, 1_030_000_000),
        ])
        self.assertEqual(quality["unfinished_enqueues"], 0)
        self.assertEqual(quality["clock_id"], "mono")

    def test_missing_enqueue_and_unfinished_wait_are_not_completed_episodes(self):
        intervals, quality = PROFILE.runqueue_intervals([
            switch("1.000000000", 0, "S", 20),
            switch("1.010000000", 20, "R", 0),
        ], {20})
        self.assertEqual(intervals[20], [])
        self.assertEqual(quality["dispatches_without_observed_enqueue"], 1)
        self.assertEqual(quality["unfinished_enqueues"], 1)

    def test_lost_or_out_of_order_events_are_rejected(self):
        for lines in [
            ["CPU 0: LOST 5 events!\n"],
            [wake("2.000000000", 20), wake("1.000000000", 20)],
        ]:
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                PROFILE.runqueue_intervals(lines, {20})


class CorrelationTests(unittest.TestCase):
    def test_multiple_workers_do_not_double_count_a_leader_observation(self):
        result = PROFILE.assess(
            [sample(110, 120, (20, 21)), sample(130, 140, (20, 21))],
            {20: [(100, 150)], 21: [(105, 145)]},
            {"first_event_ns": 0, "last_event_ns": 200, "clock_id": "mono"},
        )
        self.assertEqual(result["leader_receive_wait_observations"], 2)
        self.assertEqual(result["receive_wait_with_confirmed_runnable_worker"], 2)
        self.assertEqual(result["distinct_candidate_runqueue_episodes"], 2)
        self.assertEqual(result["candidate_fraction_of_receive_wait_observations"], 1)

    def test_partial_overlap_is_not_a_confirmed_observation(self):
        result = PROFILE.assess(
            [sample(110, 120)], {20: [(115, 130)]},
            {"first_event_ns": 0, "last_event_ns": 200, "clock_id": "mono"},
        )
        self.assertEqual(result["assessment"], "no_candidate_observed")
        self.assertIsNone(result["candidate_episode_p99_ms"])

    def test_short_recording_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "complete activity sample window"):
            PROFILE.assess(
                [sample(110, 120)], {20: []},
                {"first_event_ns": 111, "last_event_ns": 200, "clock_id": "mono"},
            )

    def test_no_receive_wait_is_insufficient_not_a_negative_result(self):
        observation = sample(110, 120)
        observation["backends"][0]["wait_event_type"] = None
        result = PROFILE.assess(
            [observation], {20: [(100, 130)]},
            {"first_event_ns": 0, "last_event_ns": 200, "clock_id": "mono"},
        )
        self.assertEqual(result["assessment"], "insufficient_observations")
        self.assertIsNone(result["candidate_fraction_of_receive_wait_observations"])

    def test_missing_or_different_trace_clock_is_rejected(self):
        for clock_id in (None, "mono_raw"):
            with self.subTest(clock_id=clock_id), self.assertRaisesRegex(
                ValueError, "clockid mono"
            ):
                PROFILE.assess(
                    [sample(110, 120)], {20: [(100, 130)]},
                    {"first_event_ns": 0, "last_event_ns": 200, "clock_id": clock_id},
                )


if __name__ == "__main__":
    unittest.main()
