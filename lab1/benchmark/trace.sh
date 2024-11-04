#!/usr/bin/env bpftrace

tracepoint:syscalls:sys_enter_read /comm == "ema-search-str"/ {
    if (!@bytes_read[pid]) {
        @bytes_read[pid] = 0;
    }
}

tracepoint:syscalls:sys_exit_read /comm == "ema-search-str"/ {
    @bytes_read[pid] += args->ret;
}

END {
    print(@bytes_read);
}
