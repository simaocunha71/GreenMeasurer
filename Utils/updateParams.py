import re
import sys

if len(sys.argv) < 3:
    print("Usage: python3 updateParams.py KEY=VALUE [KEY=VALUE ...] <file1> [<file2> ...]")
    sys.exit(1)

# Separate parameters and files
param_args = [arg for arg in sys.argv[1:] if '=' in arg]
file_args = [arg for arg in sys.argv[1:] if '=' not in arg]

params = {}
for arg in param_args:
    key, value = arg.split('=', 1)
    params[key] = value

# Regular expression patterns to update
patterns = {
    "NTIMES": r'NTIMES=.*',
    "TIME_OUT_LIMIT": r'TIME_OUT_LIMIT=.*',
    "VARIANCE": r'VARIANCE=.*',
    "SLEEP_SECS": r'SLEEP_SECS=.*',
}

for input_file in file_args:
    with open(input_file, 'r') as f:
        data = f.read()

    for key, pattern in patterns.items():
        if key in params:
            data = re.sub(pattern, f'{key}={params[key]}', data)

    with open(input_file, 'w') as f:
        f.write(data)
