"""How much work to run at once on the player's machine.

Everything that runs in parallel (compiling the C files, compressing the music) asks here,
so a 2-core laptop and a 16-core desktop both get a sensible number of workers: never more
than the cores this process may use, and never more than the free memory allows.
"""
import ctypes
import os
import platform


def available_cpus():
    """Logical cores this process may run on: the processor affinity (Windows, Linux) or the
    container limit when Python knows it, and at most IRC_JOBS when that is set (to keep
    cores free for other work while recompiling)."""
    n = None
    if platform.system() == "Windows":
        try:
            proc, system = ctypes.c_size_t(), ctypes.c_size_t()
            k32 = ctypes.windll.kernel32
            if k32.GetProcessAffinityMask(k32.GetCurrentProcess(), ctypes.byref(proc), ctypes.byref(system)):
                n = bin(proc.value).count("1")
        except (OSError, AttributeError):
            n = None
    if not n and hasattr(os, "process_cpu_count"):          # Python 3.13+
        n = os.process_cpu_count()
    elif not n and hasattr(os, "sched_getaffinity"):
        n = len(os.sched_getaffinity(0))
    n = n or os.cpu_count() or 1
    try:
        limit = int(os.environ.get("IRC_JOBS", "0"))
    except ValueError:
        limit = 0
    if limit > 0:
        n = min(n, limit)
    return max(1, n)


def physical_cpus():
    """Physical cores (hyper-threading siblings counted once), capped by available_cpus().
    Number crunching such as audio compression gains little from a core's second thread."""
    n = None
    try:
        if platform.system() == "Windows":
            # GetLogicalProcessorInformation: one RelationProcessorCore (0) entry per physical core
            entry = 32 if ctypes.sizeof(ctypes.c_void_p) == 8 else 24
            size = ctypes.c_ulong(0)
            ctypes.windll.kernel32.GetLogicalProcessorInformation(None, ctypes.byref(size))
            buf = (ctypes.c_ubyte * size.value)()
            if ctypes.windll.kernel32.GetLogicalProcessorInformation(buf, ctypes.byref(size)):
                ptr = ctypes.sizeof(ctypes.c_void_p)
                n = sum(1 for off in range(0, size.value, entry)
                        if int.from_bytes(bytes(buf[off + ptr:off + ptr + 4]), "little") == 0)
        else:
            cores = set()
            phys = core = None
            with open("/proc/cpuinfo") as f:                      # Linux
                for line in f:
                    key, _, val = line.partition(":")
                    key = key.strip()
                    if key == "physical id":
                        phys = val.strip()
                    elif key == "core id":
                        core = val.strip()
                    elif not line.strip():
                        if core is not None:
                            cores.add((phys, core))
                        phys = core = None
            if core is not None:
                cores.add((phys, core))
            n = len(cores) or None
    except (OSError, ValueError, AttributeError):
        n = None
    return max(1, min(n or available_cpus(), available_cpus()))


def available_memory():
    """Free physical memory in bytes, or None when it cannot be read."""
    try:
        if platform.system() == "Windows":
            class MemStatus(ctypes.Structure):
                _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                            ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                            ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                            ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                            ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
            st = MemStatus()
            st.dwLength = ctypes.sizeof(st)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
                return st.ullAvailPhys
            return None
        with open("/proc/meminfo") as f:              # Linux
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) * 1024
    except (OSError, ValueError, AttributeError):
        pass
    try:
        return os.sysconf("SC_AVPHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
    except (ValueError, OSError, AttributeError):
        return None


def workers(jobs, mem_per_job=0, reserve_cpus=0, physical=False):
    """Workers for `jobs` tasks: at most the usable cores (physical ones only, for pure number
    crunching), minus `reserve_cpus` kept for other work running at the same time, and at
    most what half of the free memory can hold."""
    n = min(jobs, (physical_cpus() if physical else available_cpus()) - reserve_cpus)
    if mem_per_job:
        free = available_memory()
        if free:
            n = min(n, free // 2 // mem_per_job)
    return max(1, int(n))
