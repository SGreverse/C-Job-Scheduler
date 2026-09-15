import os
import re

# 1. The ONLY file the user will see in their IDE
PUBLIC_HEADERS = [
    "include/sk_scheduler.h"
]

# 2. Internal structs and APIs hidden from the user
INTERNAL_HEADERS = [
    "include/sk_memory_utils.h",
    "include/sk_task.h",
    "include/sk_cl_deque.h",
    "include/sk_worker.h"
]

# 3. The actual logic
SOURCES = [
    "src/sk_cl_deque.c",
    "src/sk_worker.c",
    "src/sk_scheduler.c"
]

OUTPUT_FILE = "release/sk_scheduler.h"

def strip_internal_includes(code):
    # Removes local includes like #include "sk_task.h" 
    # but keeps standard includes like <stdatomic.h>
    return re.sub(r'#include\s+"sk_.*\.h"\n?', '', code)

def build_single_header():
    
    with open(OUTPUT_FILE, 'w') as out:
        out.write("/* To use this library, do this in EXACTLY ONE C file: */\n")
        out.write("/* #define SK_SCHEDULER_IMPLEMENTATION */\n")
        out.write("/* #include \"sk_scheduler.h\" */\n\n")
        
        out.write("#if defined(__linux__) && !defined(_GNU_SOURCE)\n")
        out.write("    #define _GNU_SOURCE\n")
        out.write("#endif\n\n")
        # --- PUBLIC API ---
        # This is the only part exposed to the user's global namespace
        for h_file in PUBLIC_HEADERS:
            with open(h_file, 'r') as f:
                code = strip_internal_includes(f.read())
                out.write(code + "\n\n")
                
        # --- HIDDEN IMPLEMENTATION BLOCK ---
        out.write("#ifdef SK_SCHEDULER_IMPLEMENTATION\n\n")

        for h_file in INTERNAL_HEADERS:
            with open(h_file, 'r') as f:
                code = strip_internal_includes(f.read())
                out.write(f"/* --- {os.path.basename(h_file)} --- */\n")
                out.write(code + "\n\n")

        for c_file in SOURCES:
            with open(c_file, 'r') as f:
                code = strip_internal_includes(f.read())
                out.write(f"/* --- {os.path.basename(c_file)} --- */\n")
                out.write(code + "\n\n")
                
        out.write("#endif /* SK_SCHEDULER_IMPLEMENTATION */\n")

if __name__ == "__main__":
    build_single_header()
    print(f"Successfully generated {OUTPUT_FILE}")