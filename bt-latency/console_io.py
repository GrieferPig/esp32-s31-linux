"""Bounded paced writes for the S31 serial shell, independent of read speed."""
import os,select,time

def write_paced(fd,data):
    view=memoryview(data)
    offset=0
    deadline=time.monotonic()+max(10,len(view)/100)
    while offset<len(view):
        if time.monotonic()>=deadline:
            raise TimeoutError("Serial command write timed out")
        if not select.select([], [fd], [], .5)[1]:
            continue
        try:
            count=os.write(fd,view[offset:offset+16])
        except BlockingIOError:
            continue
        if count<=0:
            raise OSError("Serial command write made no progress")
        offset+=count
        # The low-clock boot console can overrun when a whole line arrives
        # at full UART speed. Pacing affects command setup, not capture reads.
        time.sleep(.01)
