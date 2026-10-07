"""Summarize optional raw board snapshots, preserving their timing boundary."""
import json,re
from pathlib import Path
def summarize(dest):
    def read(phase):
        text=(dest/(phase+'-board.raw')).read_text(errors='replace').replace('\r','')
        cpu=[int(x) for x in re.search(r'^cpu +(.+)$',text,re.M).group(1).split()][:8]
        counters={k:int(v) for k,v in re.findall(r'\b(hci_rx_dropped|hci_tx_dropped|worker_passes|heap_used|heap_peak|ticks)=(\d+)',text)}
        memory={k:int(v) for k,v in re.findall(r'^(MemFree|MemAvailable|Cached):\s+(\d+)',text,re.M)}
        tasks={}
        for pid,comm,fields in re.findall(r'^(\d+) \((.*)\) (.+)$',text,re.M):
            values=fields.split()
            tasks[pid]=(comm,int(values[11]),int(values[12]),int(values[19]))
        return cpu,counters,memory,tasks
    a,ac,am,at=read('before');b,bc,bm,bt=read('after')
    delta=[y-x for x,y in zip(a,b)];total=sum(delta)
    tasks=[]
    for pid,v in bt.items():
        if pid not in at or at[pid][3]!=v[3]:continue
        user=v[1]-at[pid][1];system=v[2]-at[pid][2]
        tasks.append(dict(pid=pid,name=v[0],user_ticks=user,system_ticks=system,total_ticks=user+system))
    tasks.sort(key=lambda x:x['total_ticks'],reverse=True)
    result={'cpu_busy_pct_aggregate':100*(total-delta[3]-delta[4])/total,
            'counter_delta':{k:bc[k]-ac[k] for k in ac},
            'memory_before_kB':am,'memory_after_kB':bm,'tasks':tasks,
            'note':'CPU spans snapshots around receiver, including connection/discovery and snapshot overhead; throughput uses only40s receive window.'}
    (dest/'board-summary.json').write_text(json.dumps(result,indent=2))
    return result
if __name__=='__main__':
    import sys
    print(json.dumps(summarize(Path(sys.argv[1])),indent=2))
