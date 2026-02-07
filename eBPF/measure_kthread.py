from bcc import BPF
import sys
import ctypes

# Use eBPF to measure kernel thread creation time

code = """
#include <uapi/linux/ptrace.h>

struct data_t {
    u64 delta;
};

BPF_HASH(start, u32);  // 改用 u32
BPF_PERF_OUTPUT(events);

int probe_entry(struct pt_regs *ctx)
{
    u32 tid = bpf_get_current_pid_tgid();  // 自動取低 32 位
    u64 ts = bpf_ktime_get_ns();
    start.update(&tid, &ts);
    return 0;
}

int probe_return(struct pt_regs *ctx)
{
    u32 tid = bpf_get_current_pid_tgid();
    u64 *ts = start.lookup(&tid);
    if (ts != 0) {
        u64 delta = bpf_ktime_get_ns() - *ts;
        
        struct data_t data = {};
        data.delta = delta;
        
        events.perf_submit(ctx, &data, sizeof(data));
        start.delete(&tid);
    }
    return 0;
}
"""

b = BPF(text=code)
b.attach_kprobe(event="my_thread_run", fn_name="probe_entry")
b.attach_kretprobe(event="my_thread_run", fn_name="probe_return")

class Data(ctypes.Structure):
    _fields_ = [("delta", ctypes.c_ulonglong)]

with open('kthread_run_cost.txt', 'w') as f:
    f.write("Duration_ns\n")

    def print_event(cpu, data, size):
        event = ctypes.cast(data, ctypes.POINTER(Data)).contents
        print(f"Duration: {event.delta} ns")
        f.write(f"{event.delta}\n")
        f.flush()

    b["events"].open_perf_buffer(print_event)

    print("Tracing... Hit Ctrl-C to end.")
    while True:
        try:
            b.perf_buffer_poll()
        except KeyboardInterrupt:
            sys.exit()