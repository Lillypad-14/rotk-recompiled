import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from frame_report import analyze


def row(frame, interval=16743, guest=4000):
    return dict(frame=str(frame), interval_us=str(interval), guest_us=str(guest),
                render_us="100", present_us="500", audio_us="100", pump_us="100")


class FrameReportTest(unittest.TestCase):
    def test_steady_with_one_startup_hitch(self):
        result = analyze([row(0, 90000)] + [row(i) for i in range(1, 10)], 1)
        self.assertEqual(result["long_gaps"], 0)
        self.assertEqual(result["missing_guest_frames"], 0)
        self.assertEqual(result["work_over_budget"], 0)

    def test_catches_gap_and_skipped_guest_frame(self):
        result = analyze([row(1), row(3, 34000, 18000)], 0)
        self.assertEqual(result["long_gaps"], 1)
        self.assertEqual(result["missing_guest_frames"], 1)
        self.assertEqual(result["work_over_budget"], 1)

    def test_empty_window_is_not_a_pass(self):
        with self.assertRaises(ValueError):
            analyze([row(1)], 120)


if __name__ == "__main__":
    unittest.main()
