from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = (root / 'src' / 'main.c').read_text(encoding='utf-8')
dash = (root / 'src' / 'dashboard.c').read_text(encoding='utf-8')

checks = {
    'firmware version': '3.7.0-FULL-NORTC' in main,
    'IGN GPIO17': 'GPIO_NUM_17' in main,
    'IGN graceful finalize': 'finalize_active_trip("IGN_OFF", false)' in main,
    'NO RTC NEXTDAY command': 'strcasecmp(cmd, "NEXTDAY")' in main,
    'NEXT DAY TFT menu': '"NEXT DAY"' in dash,
    'Last Trip TFT mode': 'trip_showing_last' in main and 'trip_showing_last' in dash,
    '30 second snapshots': 'SNAPSHOT_INTERVAL_MS         30000U' in main,
    '0.5 km snapshots': 'SNAPSHOT_DISTANCE_STEP_KM      0.5' in main,
    'Trip v3.7 CSV': '/data/trips_v37.csv' in main,
    'Event CSV': '/data/events_v37.csv' in main,
    'Event queue': 'xQueueCreate(EVENT_QUEUE_DEPTH' in main,
    'ECT high event': '"ECT_HIGH"' in main,
    'Battery events': '"BAT_LOW"' in main and '"BAT_HIGH"' in main,
    'Sensor invalid event': '"INVALID_FRAMES"' in main,
    'v3.6 migration': 'Migrating v3.6 settings' in main,
    'visible O2/AAC/AF implemented': 'AAC/AF' in dash and 'visible_mask' in dash,
}

failed = []
for name, ok in checks.items():
    print(f"{'PASS' if ok else 'FAIL'}: {name}")
    if not ok:
        failed.append(name)

if failed:
    raise SystemExit(f"FAILED: {', '.join(failed)}")
print('PASS: v3.7 source feature checks complete')
