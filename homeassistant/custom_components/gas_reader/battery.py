"""Voltage-only estimate for the project's single 4.2 V Li-ion cell.

This is a generic heuristic, not a measured curve for the installed 18650.
Temperature, cell chemistry, aging and charging/load can change the result.
"""

# Millivolts / estimated percent, with interpolation between these points.
LI_ION_VOLTAGE_CURVE = (
    (3300, 0), (3500, 10), (3600, 20), (3700, 35), (3800, 55),
    (3900, 70), (4000, 80), (4100, 90), (4200, 100),
)


def estimated_battery_percent(millivolts: int | None) -> int | None:
    """Return a bounded whole percent; missing/invalid input stays unknown."""
    if type(millivolts) is not int or millivolts <= 0:
        return None
    if millivolts <= LI_ION_VOLTAGE_CURVE[0][0]:
        return 0
    for (low_mv, low_pct), (high_mv, high_pct) in zip(
        LI_ION_VOLTAGE_CURVE, LI_ION_VOLTAGE_CURVE[1:]
    ):
        if millivolts <= high_mv:
            span = high_mv - low_mv
            return low_pct + ((millivolts - low_mv) * (high_pct - low_pct) + span // 2) // span
    return 100
