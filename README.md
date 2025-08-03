# GreenMeasurer

**A comprehensive framework for measuring energy consumption of programs across multiple programming languages**

GreenMeasurer is a powerful tool that leverages RAPL (Running Average Power Limit) technology to provide accurate energy consumption measurements for any program, helping developers write more sustainable and energy-efficient code.

| Authors               | Contacts                            |
|:---------------------:|:-----------------------------------:|
| Simão Cunha           | d14299@di.uminho.pt                 |
| ???                   | ???                                 |
| João Saraiva          | saraiva@di.uminho.pt                |


## Requirements

- Debian-based Linux distributions
- Intel processor (compatible with RAPL)
- Non-containerized environment

## Required Libraries

1. RAPL
2. lm-sensors
3. Powercap
4. Raplcap

Install all dependencies by running:

```bash
sudo bash raplLibrariesSetup.sh
```

## Prerequisites and Configuration

Before running measurements, you need to configure the following:

### 1. Wi-Fi Interface Configuration
The measurement script will automatically disable Wi-Fi during measurements to reduce interference. You need to set your Wi-Fi interface name in the script:

1. Find your Wi-Fi interface name:
```bash
ip link show | grep -E "wl|wifi"
```

2. Edit `scripts/measure.sh` and update line 4:
```bash
WIFI_INTERFACE="your_interface_name"  # e.g., "wlp2s0", "wlan0", "wlo1"
```

### 2. System Permissions
The script requires sudo privileges for:
- Wi-Fi interface control
- Screen brightness adjustment  
- RAPL energy measurements

### 3. Environment Variables
Ensure the following environment variable is set:
```bash
export LANG=C.UTF-8
```

### 4. Power Limits Configuration
The script uses power limit values defined in `POWER_LIMITS` array. By default, it's set to `(-1)` which means no power capping. You can modify this in the script to test different power limits:
```bash
POWER_LIMITS=(7 11 15)  # Test with 7W, 11W, and 15W limits
```

---

## Running RAPL Measurements

**Important:** Ensure you have completed the Prerequisites and Configuration section above before running measurements.

To generate energy consumption measurements for all programs in all languages, run:

```bash
sudo -E bash scripts/measure.sh <language1> [language2] ... | all
```
- Replace <language1> [language2] ... with one or more of the supported languages listed below, or use all to run measurements for every supported language.
- Supported languages: All supported languages correspond to the folders listed in the `Languages/` directory.

This script will append the results to a top-level `measurements.csv` file.

**Example usages:**
```bash
# Measure all supported languages
sudo -E bash scripts/measure.sh all

# Measure only Python, C# and Rust
sudo -E bash scripts/measure.sh Python "C#" Rust
```
If a specified language or directory is not found, the script will exit with an error message.

### Automatic System Restoration
The measurement script automatically handles system cleanup:
- **Wi-Fi**: Automatically restored when the script completes or encounters an error
- **Screen brightness**: Automatically restored to normal levels on script exit
- **Error handling**: System settings are restored even if the script is interrupted (Ctrl+C) or crashes

This ensures your system won't be left in a dimmed state or with Wi-Fi disabled if something goes wrong during measurements.

---

## Temperature Calibration

To calibrate the average CPU core temperature, execute the following:

```bash
cd RAPL
make
./main --temperature-calibration XX
```

Replace `XX` with the number of seconds to idle (sleep) before measuring the temperature.
The output will be saved to `/tmp/cores_temperature.txt`.

---

## Powercap Calibration

To determine the most energy-efficient power cap setting for your system (based on the Fibonacci function — it executes `fibonacci(20)`), run:

```bash
cd RAPL
make
sudo -E ./main --powercap-calibration min-max VARIANCE TIME_OUT_LIMIT SECS_TO_SLEEP N_TIMES
```

- Replace `min`-`max` with the desired power cap range in Watts (e.g., 2-25).
- `VARIANCE` is the allowed temperature fluctuation (e.g., 5), and can be set to 0.
- `TIME_OUT_LIMIT` is the maximum time in minutes allowed for each test iteration.
- `SECS_TO_SLEEP` is the number of seconds where the CPU is idle before measuring the temperature.
- `N_TIMES` is the number of times to execute fibonacci(20).

Example:
```bash
sudo -E ./main --powercap-calibration 2-25 5 120 30 7
```

This will calibrate power caps from 2W to 25W, running 7 executions per power cap value, allowing for a 5°C temperature variance and a 120-minute limit per execution. If the initial temperature is not calculated, it will calibrate the temperature while waiting for 30 seconds of idle time.

## Parametrized Execution

To perform energy measurements on a specific program, use the following syntax:

```bash
./main <command> <language> <program> <ntimes> <VARIANCE> <TIME_OUT_LIMIT> <sleep_time>
```

### Arguments

| Argument          | Description                                                                 |
|-------------------|-----------------------------------------------------------------------------|
| `<command>`       | The command to execute (e.g., `./my_program`)                               |
| `<language>`      | Programming language (e.g., `C`, `Python`)                                  |
| `<program>`       | Name of the program being tested                                            |
| `<ntimes>`        | Number of times to execute the program (must be > 0)                        |
| `<VARIANCE>`      | Acceptable temperature variance (in Celsius, must be ≥ 0)                   |
| `<TIME_OUT_LIMIT>`| Timeout limit per execution (in minutes, must be > 0)                       |
| `<sleep_time>`    | Idle time before execution (in seconds, must be ≥ 0)                        |

### Example

```bash
cd RAPL
make
./main "./my_program" C my_program 5 3 20 10
```

This will:
- Perform idle measurement for 10 seconds
- Execute the program 5 times
- Add 3°C to the temperature variance 
- Terminate executions exceeding 20 minutes

---

## CSV Output Format

The `measurements.csv` contains the following columns:

|      Column       | Description                                                                 |
|-------------------|-----------------------------------------------------------------------------|
| **Language**       | Programming language                                                        |
| **Program**        | Program name                                                                |
| **PowerLimit**     | Power cap value applied (Watts)                                             |
| **Package**        | Energy used by the full socket (cores + GPU + other components)            |
| **Core**           | Energy used by CPU cores and caches                                         |
| **GPU**            | Energy used by GPU                                                          |
| **DRAM**           | Energy used by RAM                                                          |
| **Time**           | Execution time (in milliseconds)                                            |
| **Temperature**    | Average core temperature (in Celsius)                                       |
| **Memory**         | Total physical memory used (in KBytes)                                      |

---

# Makefile Target Explanation

This document explains each target in the Makefiles used to build and evaluate several programs.

## 1. `compile`

**Command:**
```sh
make compile
```

Compiles the source code into an optimized executable binary (when possible).

## 2. `run`

**Command:**
```sh
make run
```

Runs the program - either runs a executable binary or runs a interpreted program.

**Command executed:**
```sh
./my_program [input]
```

## 3. `perf`

**Command:**
```sh
make perf
```
Uses Linux `perf` to gather the following CPU performance metrics:

| Metric                              | Description                                                          |
|-------------------------------------|----------------------------------------------------------------------|
| `cycles`                            | The total number of CPU cycles consumed by the program.             |
| `instructions`                      | The total number of instructions executed by the program.           |
| `cache-misses`                      | The total number of cache misses (both data and instruction cache). |
| `branch-misses`                     | The total number of branch prediction misses.                       |
| `stalled-cycles-frontend`           | The number of cycles the instruction fetch unit was stalled.        |
| `stalled-cycles-backend`            | The number of cycles the instruction execution unit was stalled.    |
| `resource_stalls.any`               | Total number of cycles where execution was stalled due to resource limitations. |
| `mem_trans_retired.load_latency_gt_128` | Number of memory transactions with load latency greater than 128 cycles. |
| `cpu-cycles`                        | Total number of CPU cycles, a repeat of the `cycles` metric.        |
| `uops_executed.stall_cycles`        | Number of cycles during which micro-operations (uops) were stalled. |



## 4. `measure`

**Command:**
```sh
make measure
```

**Purpose:**  
Measures energy consumption and executing time by running the program using the RAPL interface.

**Command breakdown:**
```sh
sudo ../../../RAPL/main "./my_program [input]" [Language] [Program] $(NTIMES) $(VARIANCE) $(TIME_OUT_LIMIT) $(SLEEP_SECS)
```

## 5. `mem`

**Command:**
```sh
make mem
```

Profiles memory and resource usage using `/usr/bin/time -v`.

**Command executed:**
```sh
/usr/bin/time -v ./my_program [input]
```

## 6. `valgrind`

**Command:**
```sh
make valgrind
```
 
Analyzes heap memory usage with Valgrind's Massif tool.

**Command executed:**
```sh
valgrind --tool=massif --stacks=yes ./my_program [input]
```

## 7. `clean`

**Command:**
```sh
make clean
```

Removes generated files and cleans up the project directory.

---
### NOTE

1. The `config.env` file contains the paths to all interpreters and compilers installed on your machine - you may need to update these paths to match your system configuration.
