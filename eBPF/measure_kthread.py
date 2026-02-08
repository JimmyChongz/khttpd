from bcc import BPF
import sys

code = """
BPF_HASH(start, u32, u64);

int probe_entry(void *ctx) {
    u32 tid = bpf_get_current_pid_tgid();
    u64 ts = bpf_ktime_get_ns();
    start.update(&tid, &ts);
    return 0;
}

int probe_return(void *ctx) {
    u32 tid = bpf_get_current_pid_tgid();
    u64 *ts = start.lookup(&tid);
    if (ts) {
        bpf_trace_printk("%llu\\n", bpf_ktime_get_ns() - *ts);
        start.delete(&tid);
    }
    return 0;
}
"""

b = BPF(text=code)
b.attach_kprobe(event="my_thread_run", fn_name="probe_entry")
b.attach_kretprobe(event="my_thread_run", fn_name="probe_return")

with open('kthread_run_cost.txt', 'w') as f:
    f.write("Duration_ns\n")
    print("Tracing... Hit Ctrl-C to end.")
    
    try:
        while True:
            (task, pid, cpu, flags, ts, msg) = b.trace_fields()
            duration = msg.decode('utf-8').strip() 
            f.write(f"{duration}\n") 
    except KeyboardInterrupt:
        sys.exit()