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
