#!/usr/bin/env python3
"""Exercise interface drafts and actual menu control flow with mocked hardware."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
LIB = ROOT / 'buildroot-external/board/esp32-s31/overlay/usr/lib/esp32-config'

METADATA = '''profile\tuart1
active\t1
saved\t1
current_known\t1
route\tuart1.tx\t42\t10\t20\tmatrix-output
route\tuart1.rx\t43\t11\t21\tmatrix-input
parameter\tclock-frequency\t100000\t400000\t1000000\t100000,400000,1000000
fixed_gpio\t25
'''

STUB = '''#!/usr/bin/env python3
import json, os, pathlib, sys
root=pathlib.Path(os.environ['FIXTURE'])
args=sys.argv[1:]
name=pathlib.Path(sys.argv[0]).name
with (root/'calls').open('a') as out: out.write(json.dumps([name,args])+'\\n')
if name=='dialog':
    if not any(x in args for x in ('--menu','--inputbox','--passwordbox')): sys.exit(0)
    p=root/'answers'; answers=json.loads(p.read_text())
    if not answers: sys.exit(1)
    answer=answers.pop(0); p.write_text(json.dumps(answers))
    if answer is None: sys.exit(1)
    print(answer); sys.exit(0)
if args[0]=='describe': print((root/'metadata').read_text(),end='')
elif args[0]=='list': print('uart1\\ni2c0\\nradio-wifi\\nusb-device\\nsdmmc0\\nuart3-dma')
elif args[0]=='status':
    if (root/'status-fails').exists(): sys.exit(1)
    print('active: uart1 id=1')
elif args[0]=='apply' and (root/'fail-once').exists():
    (root/'fail-once').unlink(); print('GPIO is owned by another interface',file=sys.stderr); sys.exit(1)
'''

class InterfaceFlowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        (self.work/'bin').mkdir()
        (self.work/'run').mkdir()
        (self.work/'conf').mkdir()
        (self.work/'metadata').write_text(METADATA)
        (self.work/'answers').write_text('[]')
        (self.work/'features').write_text('uart=1\ngpio=1\ni2c=0\nmmc=0\nahb_gdma=0\n')
        for name in ('dialog','s31-overlay'):
            p = self.work/'bin'/name
            p.write_text(STUB)
            p.chmod(0o755)
        self.env = dict(os.environ, FIXTURE=str(self.work),
            PATH=f"{self.work/'bin'}:{os.environ['PATH']}",
            ESP32_CONFIG_DIR=str(self.work/'conf'),
            ESP32_CONFIG_RUN_DIR=str(self.work/'run'),
            ESP32_CONFIG_FEATURES_FILE=str(self.work/'features'))

    def run_shell(self, body, rc=0):
        result = subprocess.run(['sh','-c',f'. "{LIB}/common.sh"; . "{LIB}/interfaces.sh"; '+body],
            env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, rc, result.stderr)
        return result

    def answers(self, *answers):
        (self.work/'answers').write_text(json.dumps(answers))

    def calls(self, name):
        path=self.work/'calls'
        return [args for program,args in map(json.loads,path.read_text().splitlines()) if program==name] if path.exists() else []

    def mutations(self):
        return [call for call in self.calls('s31-overlay') if call[0] in ('apply','remove')]

    def draft(self, metadata):
        (self.work/'metadata').write_text(metadata)
        self.run_shell('interface_make_draft "$FIXTURE/metadata" "$FIXTURE/draft"')
        return (self.work/'draft').read_text()

    def test_live_settings_take_precedence_over_saved_and_defaults(self):
        draft=self.draft(METADATA)
        self.assertIn('uart1.tx\t10\t',draft)
        self.assertIn('clock-frequency\t400000\t',draft)

    def test_disabled_uses_saved_then_default_without_stale_live_values(self):
        metadata=METADATA.replace('active\t1','active\t0').replace('43\t11\t21','43\t11\t-')
        draft=self.draft(metadata)
        self.assertIn('uart1.tx\t20\t',draft)
        self.assertIn('uart1.rx\t43\t',draft)

    def test_unknown_current_values_are_never_invented(self):
        draft=self.draft(METADATA.replace('current_known\t1','current_known\t0'))
        self.assertIn('uart1.tx\t-\t',draft)

    def test_back_discards_edited_draft(self):
        self.answers('uart1.tx','12',None)
        self.run_shell('interface_edit uart1')
        self.assertEqual(self.mutations(),[])
        self.assertEqual(list((self.work/'conf').iterdir()),[])
        self.assertEqual(list((self.work/'run').iterdir()),[])

    def test_apply_reuses_current_values_for_untouched_fields(self):
        self.answers('uart1.tx','12','save')
        self.run_shell('interface_edit uart1')
        self.assertEqual(self.mutations(),[['apply','uart1','uart1.tx=12','uart1.rx=11','clock-frequency=400000']])
        self.assertFalse(any('--msgbox' in call for call in self.calls('dialog')))

    def test_parameter_menu_prefills_current_and_changes_one_value(self):
        self.answers('clock-frequency','1000000','save')
        self.run_shell('interface_edit uart1')
        picker=next(call for call in self.calls('dialog') if '--default-item' in call)
        self.assertEqual(picker[picker.index('--default-item')+1],'400000')
        self.assertEqual(self.mutations()[0][-1],'clock-frequency=1000000')

    def test_hardware_failure_retains_draft_for_retry(self):
        (self.work/'fail-once').touch()
        self.answers('uart1.tx','12','save','save')
        self.run_shell('interface_edit uart1')
        first,second=self.mutations()
        self.assertEqual(first,second)
        self.assertIn('uart1.tx=12',second)

    def test_invalid_reserved_pin_is_corrected_without_losing_other_fields(self):
        self.answers('uart1.tx','26','13','save')
        self.run_shell('interface_edit uart1')
        self.assertEqual(len(self.mutations()),1)
        self.assertIn('uart1.tx=13',self.mutations()[0])
        self.assertIn('uart1.rx=11',self.mutations()[0])

    def test_unknown_live_record_can_only_be_disabled(self):
        (self.work/'metadata').write_text(METADATA.replace('current_known\t1','current_known\t0'))
        self.answers('save','enabled','0','save')
        self.run_shell('interface_edit uart1')
        self.assertEqual(self.mutations(),[['remove','uart1']])

    def test_unavailable_manager_does_not_open_an_editable_draft(self):
        (self.work/'metadata').write_text(METADATA.replace('active\t1','active\tunknown'))
        self.run_shell('interface_edit uart1',rc=1)
        self.assertEqual(self.mutations(),[])
        self.assertFalse(any('--menu' in call for call in self.calls('dialog')))

    def test_lean_menu_filters_missing_drivers_and_shared_radio(self):
        self.answers(None)
        self.run_shell('interfaces_menu')
        menu=next(call for call in self.calls('dialog') if '--menu' in call)
        self.assertIn('uart1',menu)
        for unavailable in ('i2c0','sdmmc0','uart3-dma','radio-wifi','usb-device'):
            self.assertNotIn(unavailable,menu)
        self.assertIn('gpio',menu)
        self.assertIn('usb',menu)

    def test_same_current_and_saved_values_do_not_reprobe(self):
        (self.work/'metadata').write_text(METADATA.replace('42\t10\t20','42\t10\t10').replace('43\t11\t21','43\t11\t11').replace('400000\t1000000','400000\t400000'))
        self.answers('save')
        self.run_shell('interface_edit uart1')
        self.assertEqual(self.mutations(),[])

    def test_different_saved_values_are_visible(self):
        self.answers(None)
        self.run_shell('interface_edit uart1')
        menu=next(call for call in self.calls('dialog') if '--menu' in call)
        self.assertIn('UART1 TX GPIO: 10 (saved: 20)',menu)

    def test_disabled_draft_has_no_uncommittable_fields(self):
        self.answers('enabled','0',None)
        self.run_shell('interface_edit uart1')
        menus=[call for call in self.calls('dialog') if '--menu' in call]
        self.assertNotIn('uart1.tx',menus[-1])
        self.assertIn("Saving Disabled removes this interface's saved settings.", ' '.join(menus[-1]))

    def test_unavailable_status_is_not_reported_as_disabled(self):
        (self.work/'status-fails').touch()
        self.answers(None)
        self.run_shell('interfaces_menu')
        menu=next(call for call in self.calls('dialog') if '--menu' in call)
        self.assertIn('UART 1 - Unavailable',menu)

    def test_sdmmc_in_use_blocks_parameter_change_and_disable(self):
        proc=self.work/'proc'; proc.mkdir()
        (proc/'mounts').write_text('/dev/mmcblk0p1 /mnt/card vfat rw 0 0\n')
        (proc/'swaps').write_text('Filename Type Size Used Priority\n')
        self.env['ESP32_CONFIG_PROC_DIR']=str(proc)
        self.run_shell('interface_make_draft "$FIXTURE/metadata" "$FIXTURE/draft"; interface_draft_set "$FIXTURE/draft" uart1.tx 12; interface_commit_draft sdmmc0 1 "$FIXTURE/draft"',rc=1)
        self.run_shell('interface_commit_draft sdmmc0 0 "$FIXTURE/draft"',rc=1)
        self.assertEqual(self.mutations(),[])
        (proc/'mounts').write_text('')
        (proc/'swaps').write_text('/dev/mmcblk1p2 partition 65536 0 -2\n')
        self.run_shell('interface_commit_draft sdmmc0 0 "$FIXTURE/draft"',rc=1)
        self.assertEqual(self.mutations(),[])

if __name__=='__main__':
    unittest.main()
