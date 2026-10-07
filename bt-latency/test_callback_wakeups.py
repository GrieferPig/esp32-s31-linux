#!/usr/bin/env python3
"""Compile the actual patched POSIX loop on the host and test both runtime modes."""
import os,sys,tempfile,subprocess,json
from pathlib import Path
root=Path(__file__).resolve().parent.parent
source=Path(sys.argv[1]).resolve()
build=next((root/'out/buildroot/build').glob('btstack-s31-*'))
work=Path(tempfile.mkdtemp(prefix='s31-callback-test-'))
(work/'btstack_config.h').write_text('#define HAVE_ASSERT\n#define HAVE_MALLOC\n#define HAVE_POSIX_TIME\n')
argv=['gcc','-O2','-pthread','-I'+str(work),'-I'+str(build/'src'),'-I'+str(build/'platform/posix'),
      '-DS31_RUNLOOP_SOURCE="'+str(source)+'"',str(root/'bt-latency/test_callback_wakeups.c'),
      *[str(build/'src'/name) for name in ['btstack_run_loop.c','btstack_run_loop_base.c','btstack_linked_list.c','btstack_util.c']],
      '-o',str(work/'check')]
subprocess.run(argv,check=True)
results=[]
for mode in ('0','1'):
    for case in ('chain','mixed'):
        for iteration in range(10):
            env=os.environ.copy();env['S31_BTSTACK_COALESCE_WAKEUPS']=mode
            result=subprocess.run([str(work/'check'),case],env=env,capture_output=True,text=True,timeout=10)
            results.append(dict(mode=mode,case=case,iteration=iteration,returncode=result.returncode,stdout=result.stdout,stderr=result.stderr))
            if result.returncode:raise RuntimeError(results[-1])
log=root/'bt-latency/logs/callback-wakeups/host-test.json'
log.write_text(json.dumps({'compile_argv':argv,'results':results},indent=2))
for x in results:
    if x['iteration']==0:print(x['mode'],x['case'],x['stdout'].strip())
print('PASS: 40 runs, no lost or duplicate callbacks; this is host-only correctness evidence.')
