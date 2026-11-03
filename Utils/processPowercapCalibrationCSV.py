import pandas as pd
import sys
from typing import Any, Literal

def remove_package_outliers_iqr(group, package_column):
    if len(group) < 4:
        return group

    q1 = group[package_column].quantile(0.25)
    q3 = group[package_column].quantile(0.75)
    iqr = q3 - q1

    if iqr == 0:
        return group

    lower_bound = q1 - 1.5 * iqr
    upper_bound = q3 + 1.5 * iqr
    filtered_group = group[group[package_column].between(lower_bound, upper_bound)]

    return filtered_group if not filtered_group.empty else group


GPSCategory = Literal[
    'C2 (Green)', 'C7 (Red)', 'Baseline / Equal', 
    'C1 (Green)', 'C4 (Green)', 'C3 (Green)', 
    'C8 (Red)', 'C5 (Red)', 'C6 (Red)', 'Neutral (Greenup=1)'
]

EPSILON = 1e-6

def _classify_green(speedup: float, powerup: float) -> GPSCategory:
    if powerup >= 1.0:
        return 'C3 (Green)'
    if speedup > 1.0:
        return 'C1 (Green)'
    return 'C4 (Green)'

def _classify_red(speedup: float, powerup: float) -> GPSCategory:
    if powerup <= 1.0:
        return 'C6 (Red)'
    if speedup < 1.0:
        return 'C8 (Red)'
    return 'C5 (Red)'

def classify_gps_category(
    speedup: Any,
    greenup: Any,
    powerup: Any
) -> GPSCategory:
    """
    Classifies software efficiency into the 8 GPS-UP categories (Abdulsalam et al., 2015).
    Avoids direct float equality and keeps cognitive complexity well below 15.
    """
    s = float(speedup)
    g = float(greenup)
    p = float(powerup)

    if abs(p - 1.0) < EPSILON:
        if s > 1.0 + EPSILON:
            return 'C2 (Green)'
        if s < 1.0 - EPSILON:
            return 'C7 (Red)'
        return 'Baseline / Equal'

    if g > 1.0 + EPSILON:
        return _classify_green(s, p)

    if g < 1.0 - EPSILON:
        return _classify_red(s, p)

    return 'Neutral (Greenup=1)'

def process_csv(filename):
    data = pd.read_csv(filename)
    package_columns = [
        column for column in data.columns
        if column == 'Package' or column.startswith('cpu.Package_')
    ]
    if not package_columns:
        print("Error: CSV does not contain a CPU Package energy metric.")
        return
    package_column = package_columns[0]
    results = {}
    grouped = data.groupby('Powercap')

    print(f"\n{'=' * 115}")
    print(f"{' Powercap GPS-UP Energy Efficiency Report (IEEE IGCC 2015) (https://ieeexplore.ieee.org/document/7393699) ':^115}")
    print(f"{'=' * 115}\n")

    for powercap, group in grouped:
        if len(group) <= 2:
            continue

        filtered_group = remove_package_outliers_iqr(group, package_column)
        average_package = filtered_group[package_column].mean()
        average_time = filtered_group['Time_ms'].mean()
        results[powercap] = {'Package': average_package, 'Time': average_time}

    baseline = results.get(-1)
    if baseline is None:
        print("Warning: No baseline (Powercap = -1) found. GPS-UP metrics cannot be calculated.")
        return

    ordered_keys = ([-1] if -1 in results else []) + sorted(k for k in results if k != -1)

    col_widths = (15, 17, 14, 20, 20, 14, 13)
    header = (f"{'Powercap':<{col_widths[0]}}"
              f"{'Avg Pkg (J)':<{col_widths[1]}}"
              f"{'Avg Time (ms)':<{col_widths[2]}}"
              f"{'Speedup':<{col_widths[3]}}"
              f"{'Greenup':<{col_widths[4]}}"
              f"{'Powerup':<{col_widths[5]}}"
              f"{'Category':<{col_widths[6]}}")
    separator = '-' * sum(col_widths)

    print(header)
    print(separator)

    for powercap in ordered_keys:
        avg_pkg  = results[powercap]['Package']
        avg_time = results[powercap]['Time']

        if powercap == -1:
            speedup_str = "baseline"
            greenup_str = "baseline"
            powerup_str = "baseline"
            category_str = "Base"
        else:
                    speedup = baseline['Time'] / avg_time
                    greenup = baseline['Package'] / avg_pkg
                    powerup = speedup / greenup

                    time_diff_pct = ((avg_time - baseline['Time']) / baseline['Time']) * 100
                    energy_diff_pct = ((avg_pkg - baseline['Package']) / baseline['Package']) * 100

                    speedup_str = f"{speedup:.4f} ({time_diff_pct:+.2f}%)"
                    greenup_str = f"{greenup:.4f} ({energy_diff_pct:+.2f}%)"

                    powerup_str = f"{powerup:.4f}"
                    category_str = classify_gps_category(speedup, greenup, powerup)

        powercap_label = str(powercap) + (" (base)" if powercap == -1 else "")
        print(f"{powercap_label:<{col_widths[0]}}"
              f"{avg_pkg:<{col_widths[1]}.2f}"
              f"{avg_time:<{col_widths[2]}.2f}"
              f"{speedup_str:<{col_widths[3]}}"
              f"{greenup_str:<{col_widths[4]}}"
              f"{powerup_str:<{col_widths[5]}}"
              f"{category_str:<{col_widths[6]}}")

    print(f"\n{'=' * sum(col_widths)}")
    if results:
        lowest_powercap = min(results, key=lambda k: results[k]['Package'])
        lowest_value    = results[lowest_powercap]['Package']
        print(f"{'Optimal Energy Powercap (Lowest Package):':<45} {lowest_powercap} W (Average: {lowest_value:.2f} J)")
    print(f"{'=' * sum(col_widths)}\n")

def main():
    if len(sys.argv) < 2:
        print("Usage: python processPowercapCalibrationCSV.py <filename>")
        sys.exit(1)
    process_csv(sys.argv[1])

if __name__ == "__main__":
    main()