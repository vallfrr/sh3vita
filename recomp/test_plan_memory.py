import unittest
from plan_memory import MIB, plan


class LayoutTests(unittest.TestCase):
    def test_observed_sh3_image(self):
        stack_lo, stack_hi, heap_lo, heap_hi = plan(0x400000, 0x6D7C0A8, 200 * MIB)
        self.assertEqual((stack_lo, stack_hi), (0x200000, 0x300000))
        self.assertEqual(heap_lo, 0x7180000)
        self.assertLess(heap_lo, heap_hi)
        self.assertGreaterEqual(heap_lo, 0x400000 + 0x6D7C0A8)

    def test_stack_overlap_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "stack overlaps"):
            plan(0x280000, 0x200000, 200 * MIB)

    def test_image_exceeding_budget_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "no guest heap"):
            plan(0x400000, 0x6D7C0A8, 100 * MIB)

    def test_32bit_overflow_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "overflow"):
            plan(0xF0000000, 0x20000000, 0x100000000)


if __name__ == "__main__":
    unittest.main()
