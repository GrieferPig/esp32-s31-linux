import json,tempfile,unittest
from pathlib import Path
from analyze_cpu_comparison import native,linux
class Normalization(unittest.TestCase):
 def test_native_two_core_capacity_and_counter_wrap(self):
  # Six CPU seconds of work over30 wall seconds =10% of two-core capacity.
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);start=0xffff0000;end=(start+30000000)&0xffffffff
   w={'start_us':start,'end_us':end,'before_count':3,'after_count':3}
   lines=['CPU_WINDOW '+json.dumps(w)]
   for phase in (0,1):
    for i,(name,elapsed) in enumerate([('IDLE0',25000000),('IDLE1',29000000),('work',6000000)]):
     lines.append('CPU_TASK '+json.dumps({'phase':phase,'id':i,'name':name,'runtime_us':phase*elapsed}))
   (p/'cpu-uart.raw').write_text('\n'.join(lines))
   x=native(p)
   self.assertAlmostEqual(x['total_capacity_busy_pct'],10)
   self.assertAlmostEqual(x['busy_core_equivalents'],.2)
   self.assertAlmostEqual(x['task_accounting_coverage_pct'],100)
 def test_linux_two_core_capacity(self):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td)
   (p/'cpu-idle.raw').write_text('CPU_SNAPSHOT phase=before begin_ns=0 end_ns=0\ncpu 0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0\ncpu1 0 0 0 0 0 0 0 0\nCPU_SNAPSHOT phase=after begin_ns=30000000000 end_ns=30000000000\ncpu 1000 0 1000 2700 0 0 0 0\ncpu0 500 0 500 900 0 0 0 0\ncpu1 500 0 500 1800 0 0 0 0\n')
   x=linux(p)
   self.assertAlmostEqual(x['per_core_busy_pct'][0],70)
   self.assertAlmostEqual(x['per_core_busy_pct'][1],40)
   self.assertAlmostEqual(x['total_capacity_busy_pct'],55)
   self.assertEqual(x['window_seconds'],30)
   self.assertLess(x['tick_accounting_coverage_pct'],80)
if __name__=='__main__':unittest.main()
