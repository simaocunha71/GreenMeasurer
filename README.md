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
sudo bash scripts/raplLibrariesSetup.sh
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
- RAPL energy measurements

### 3. Environment Variables
Ensure the following environment variable is set:
```bash
export LANG=C.UTF-8
```

### 4. Power Caps Configuration
The script uses power caps values defined in `POWER_CAPS` array. By default, it's set to `(-1)` which means no power capping. You can modify this in the script to test different power caps:
```bash
POWER_CAPS=(7 11 15)  # Test with 7W, 11W, and 15W power caps
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

To determine the most energy-efficient power cap setting for your system, you must specify custom programs for calibration. The tool supports both compiled and interpreted languages.

### Usage

```bash
cd RAPL
make
# For compiled languages (11 arguments)
sudo -E ./main --powercap-calibration min-max variance time_out_limit secs_to_sleep n_times "compile_cmd" "run_cmd" language program

# For interpreted languages (10 arguments - no compile command)
sudo -E ./main --powercap-calibration min-max variance time_out_limit secs_to_sleep n_times "run_cmd" language program
```

### Parameters

- Replace `min`-`max` with the desired power cap range in Watts (e.g., 2-25).
- `variance` is the allowed temperature fluctuation (e.g., 5), and can be set to 0.
- `time_out_limit` is the maximum time in minutes allowed for each test iteration.
- `secs_to_sleep` is the number of seconds where the CPU is idle before measuring the temperature.
- `n_times` is the number of times to execute the test program.
- `compile_cmd` (for compiled languages): Compilation command for your test program.
- `run_cmd`: Execution command for your test program.
- `language`: Programming language (e.g., C, Python).
- `program`: Name identifier for the program being tested.

### Examples

**C program calibration:**
```bash
sudo -E ./main --powercap-calibration 2-25 5 120 30 10 "gcc myprogram.c -o myprogram" "./myprogram" C myprogram
```

**Python script calibration:**
```bash
sudo -E ./main --powercap-calibration 2-25 5 120 30 5 "python3 myscript.py" Python myscript
```

**Compiled program with specific arguments:**
```bash
sudo -E ./main --powercap-calibration 2-25 5 120 30 7 "gcc fibonacci.c -o fib" "./fib 20" C fibonacci
```

This will calibrate power caps from 2W to 25W, running 7 executions per power cap value, allowing for a 5°C temperature variance and a 120-minute limit per execution. If the initial temperature is not calculated, it will calibrate the temperature while waiting for 30 seconds of idle time.

## Parametrized Execution

To perform energy measurements on a specific program, use the following syntax:

```bash
./main <command> <language> <program> <n_times> <variance> <time_out_limit> <sleep_time> <powercap>
```

### Arguments

| Argument          | Description                                                                 |
|-------------------|-----------------------------------------------------------------------------|
| `<command>`       | The command to execute (e.g., `./my_program`)                               |
| `<language>`      | Programming language (e.g., `C`, `Python`)                                  |
| `<program>`       | Name of the program being tested                                            |
| `<n_times>`       | Number of times to execute the program (must be > 0)                        |
| `<variance>`      | Acceptable temperature variance (in Celsius, must be ≥ 0)                   |
| `<time_out_limit>`| Timeout limit per execution (in minutes, must be > 0)                       |
| `<sleep_time>`    | Idle time before execution (in seconds, must be ≥ 0)                        |
| `<powercap>`      | Power cap in watts (-1 for no power cap, or ≥ 1 for power cap)              |

### Examples

```bash
cd RAPL
make

# Run with 15W power cap
sudo -E ./main "./my_program" C my_program 5 3 20 10 15

# Run without power cap
sudo -E ./main "./my_program" C my_program 5 3 20 10 -1

# Python example with 20W power cap
sudo -E ./main "python3 script.py" Python script 3 5 30 5 20
```

The first example will:
- Perform idle measurement for 10 seconds
- Execute the program 5 times with a 15W power cap
- Add 3°C to the temperature variance 
- Terminate executions exceeding 20 minutes

---

## CSV Output Format

The `measurements.csv` contains the following columns:

|      Column       | Description                                                                 |
|-------------------|-----------------------------------------------------------------------------|
| **Language**      | Programming language                                                        |
| **Program**       | Program name                                                                |
| **Powercap**      | Power cap value applied (Watts)                                             |
| **Package**       | Energy used by the full socket (cores + GPU + other components)            |
| **Core**          | Energy used by CPU cores and caches                                         |
| **GPU**           | Energy used by GPU                                                          |
| **DRAM**          | Energy used by RAM                                                          |
| **Time**          | Execution time (in milliseconds)                                            |
| **Temperature**   | Average core temperature (in Celsius)                                       |
| **Memory**        | Total physical memory used (in KBytes)                                      |

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

## 3. `measure`

**Command:**
```sh
make measure
```

**Purpose:**  
Measures energy consumption and executing time by running the program using the RAPL interface.

**Command breakdown:**
```sh
sudo ../../../RAPL/main "./my_program [input]" [language] [program] $(n_times) $(variance) $(time_out_limit) $(sleep_secs)
```

## 4. `clean`

**Command:**
```sh
make clean
```

Removes generated files and cleans up the project directory.

---
### NOTE

1. The `config.env` file contains the paths to all interpreters and compilers installed on your machine - you may need to update these paths to match your system configuration.