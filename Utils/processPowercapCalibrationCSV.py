import pandas as pd
import sys


def remove_package_outliers_iqr(group):
    if len(group) < 4:
        return group

    q1 = group['Package'].quantile(0.25)
    q3 = group['Package'].quantile(0.75)
    iqr = q3 - q1

    if iqr == 0:
        return group

    lower_bound = q1 - 1.5 * iqr
    upper_bound = q3 + 1.5 * iqr
    filtered_group = group[group['Package'].between(lower_bound, upper_bound)]

    return filtered_group if not filtered_group.empty else group


def process_csv(filename):
    # Read the CSV file
    data = pd.read_csv(filename)

    # Create a dictionary to store average results
    results = {}

    # Group the data by 'Powercap'
    grouped = data.groupby('Powercap')

    print(f"\n{'=' * 90}")
    print(f"{' Powercap Average Package Report ':^90}")
    print(f"{'=' * 90}\n")

    for powercap, group in grouped:
        if len(group) <= 2:
            # If there are 2 or fewer entries, averaging is not possible
            print(f"Powercap: {powercap}, not enough entries to calculate the average.")
            continue

        # Remove Package outliers using the IQR rule.
        filtered_group = remove_package_outliers_iqr(group)

        # Calculate the averages of 'Package' and 'Time' for the remaining values
        average_package = filtered_group['Package'].mean()
        average_time = filtered_group['Time'].mean()

        # Store the result
        results[powercap] = {'Package': average_package, 'Time': average_time}

    # Baseline is powercap = -1 (no power limit)
    baseline = results.get(-1)
    if baseline is None:
        print("Warning: No baseline (Powercap = -1) found. Speedup and Greenup will not be calculated.")

    # Display order: baseline (-1) first, then the rest sorted ascending
    ordered_keys = ([-1] if -1 in results else []) + sorted(k for k in results if k != -1)

    col_widths = (16, 18, 14, 25, 25)
    header = (f"{'Powercap':<{col_widths[0]}}"
              f"{'Avg Package (J)':<{col_widths[1]}}"
              f"{'Avg Time (ms)':<{col_widths[2]}}"
              f"{'Speedup':<{col_widths[3]}}"
              f"{'Greenup':<{col_widths[4]}}")
    separator = '-' * sum(col_widths)

    print(header)
    print(separator)

    for powercap in ordered_keys:
        avg_pkg  = results[powercap]['Package']
        avg_time = results[powercap]['Time']

        if powercap == -1:
            speedup_str = "baseline"
            greenup_str = "baseline"
        elif baseline is None:
            speedup_str = "N/A"
            greenup_str = "N/A"
        else:
            # Speedup: how much faster/slower vs no-powercap baseline
            speedup = baseline['Time'] / avg_time
            # Greenup: how much more/less energy spent vs no-powercap baseline
            greenup = baseline['Package'] / avg_pkg

            speedup_pct = (speedup - 1) * 100
            greenup_pct = (greenup - 1) * 100

            speedup_str = f"{speedup:.4f} ({speedup_pct:+.2f}%)"
            greenup_str = f"{greenup:.4f} ({greenup_pct:+.2f}%)"

        powercap_label = str(powercap) + (" (baseline)" if powercap == -1 else "")
        print(f"{powercap_label:<{col_widths[0]}}"
              f"{avg_pkg:<{col_widths[1]}.6f}"
              f"{avg_time:<{col_widths[2]}.2f}"
              f"{speedup_str:<{col_widths[3]}}"
              f"{greenup_str:<{col_widths[4]}}")

    # Display the Powercap with the lowest average package
    print(f"\n{'=' * sum(col_widths)}")
    if results:
        lowest_powercap = min(results, key=lambda k: results[k]['Package'])
        lowest_value    = results[lowest_powercap]['Package']
        print(f"{'Powercap with the lowest average package:':<45} {lowest_powercap} (Average: {lowest_value:.6f})")
    else:
        print("No valid Powercaps to determine the lowest average.")
    print(f"{'=' * sum(col_widths)}")

def main():
    # Check if the filename was provided as a command-line argument
    if len(sys.argv) < 2:
        print("Usage: python processPowercapCalibrationCSV.py <filename>")
        sys.exit(1)

    # Get the filename from the command-line arguments
    filename = sys.argv[1]

    # Process the provided CSV file
    process_csv(filename)

# Entry point of the program
if __name__ == "__main__":
    main()