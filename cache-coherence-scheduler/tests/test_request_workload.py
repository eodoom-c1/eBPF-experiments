import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location(
    "request_workload", Path(__file__).resolve().parents[1] / "request-workload.py"
)
workload = importlib.util.module_from_spec(spec)
spec.loader.exec_module(workload)


class ArrivalStreamTests(unittest.TestCase):
    def stream(self, capacity=10):
        return workload.ArrivalStream(1, 4, 1_000_000_000, 1_000_000_000, capacity, workload.make_values(2))

    def reply(self, stream, request_id):
        return workload.RESPONSE.pack(request_id, 0, 64) + stream.values[stream.key(request_id)]

    def test_offers_continue_without_responses_and_account_for_capacity_loss(self):
        stream = self.stream(capacity=2)
        stream.offer_due(999_999_999)
        self.assertEqual(stream.offered, 0)
        stream.offer_due(2_000_000_000)
        result = stream.summary(1_000_000_000, 0)
        self.assertEqual(result["offered_requests"], 4)
        self.assertEqual(result["client_rejected_requests"], 2)
        self.assertEqual(result["unanswered_after_drain"], 2)
        self.assertEqual(result["dropped_requests"], 4)
        self.assertIsNone(result["p99_completed_latency_ms"])

    def test_fragmented_replies_include_intended_arrival_wait_and_drain_completion(self):
        stream = self.stream()
        stream.offer_due(2_000_000_000)
        first = self.reply(stream, 0)
        stream.accept_bytes(first[:11], 1_900_000_000)
        self.assertEqual(stream.completed, 0)
        stream.accept_bytes(first[11:], 1_900_000_000)
        stream.accept_bytes(
            b"".join(self.reply(stream, request_id) for request_id in (1, 2, 3)), 2_100_000_000
        )
        result = stream.summary(1_000_000_000, 0)
        self.assertEqual(result["completed_in_window"], 1)
        self.assertEqual(result["completed_after_window"], 3)
        self.assertEqual(result["backlog_at_window_end"], 3)
        self.assertEqual(result["dropped_requests"], 0)
        self.assertEqual(result["p99_completed_latency_ms"], 900)

    def test_response_order_and_full_value_are_checked(self):
        stream = self.stream()
        stream.offer_due(2_000_000_000)
        with self.assertRaisesRegex(ValueError, "unexpected or duplicate"):
            stream.accept_bytes(self.reply(stream, 1), 2_100_000_000)
        stream = self.stream()
        stream.offer_due(2_000_000_000)
        broken = self.reply(stream, 0)[:-1] + b"\x00"
        if broken == self.reply(stream, 0):
            broken = broken[:-1] + b"\xff"
        with self.assertRaisesRegex(ValueError, "invalid lookup response"):
            stream.accept_bytes(broken, 2_100_000_000)

    def test_late_generation_does_not_shift_the_arrival_schedule(self):
        stream = self.stream()
        stream.offer_due(1_750_000_000)
        self.assertEqual(stream.offered, 4)
        self.assertEqual(stream.arrival_ns(3), 1_750_000_000)
        stream.accept_bytes(self.reply(stream, 0), 1_800_000_000)
        self.assertEqual(stream.latencies[0], 800_000_000)
        self.assertGreater(stream.max_generation_lag_ns, 0)


if __name__ == "__main__":
    unittest.main()
