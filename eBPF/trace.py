from bcc import BPF
import sys
import ctypes

def get_true_function_name(name):
    with open("/proc/kallsyms", "r") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3:
                sym_name = parts[2]
                if sym_name == name or sym_name.startswith(name + "."):
                    return sym_name
    return name

code = """
#include <uapi/linux/ptrace.h>

struct data_t {
    u64 delta;
};

BPF_HASH(start_time, u64, u64);
BPF_PERF_OUTPUT(events);

int probe_handler(struct pt_regs *ctx)
{
	u64 pid = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();
	start_time.update(&pid, &ts);
	return 0;
}

int end_function(struct pt_regs *ctx)
{
	u64 pid = bpf_get_current_pid_tgid();
	u64 *tsp = start_time.lookup(&pid);
	if (tsp != 0) {
		struct data_t data = {};
		data.delta = bpf_ktime_get_ns() - *tsp;
		events.perf_submit(ctx, &data, sizeof(data));
		start_time.delete(&pid);
	}
	return 0;
}
"""

target_fn = get_true_function_name("http_server_worker")
print(f"Attaching kprobe to function: {target_fn}")

b = BPF(text = code)
b.attach_kprobe(event = target_fn, fn_name = 'probe_handler')
b.attach_kretprobe(event = target_fn, fn_name = 'end_function')

# 定義 Python 對應的 C 結構
class Data(ctypes.Structure):
    _fields_ = [("delta", ctypes.c_ulonglong)]

def print_event(cpu, data, size):
    event = ctypes.cast(data, ctypes.POINTER(Data)).contents
    print(f"Duration: {event.delta} ns")

b["events"].open_perf_buffer(print_event)

print("Tracing... Hit Ctrl-C to end.")
while True:
	try:
		b.perf_buffer_poll()
	except KeyboardInterrupt:
		sys.exit()
