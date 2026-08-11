from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
main=(root/'src/main.c').read_text(encoding='utf-8')
dash=(root/'src/dashboard.c').read_text(encoding='utf-8')
cmake=(root/'src/CMakeLists.txt').read_text(encoding='utf-8')
part=(root/'partitions.csv').read_text(encoding='utf-8')
checks={
 'firmware v3.9.7': '3.9.7-POWER-ON-DISPLAY-FIX-FULL' in main,
 'no sd init': all(x not in main for x in ['sdspi','sdmmc','esp_vfs_fat','MICROSD','CSV_FILE_PATH']),
 'no fatfs dependency': 'fatfs' not in cmake and 'wear_levelling' not in cmake,
 '100 trip ring': 'TRIP_HISTORY_CAPACITY          100U' in main,
 '128 event ring': 'EVENT_HISTORY_CAPACITY         128U' in main,
 'trip ring write': 'stage_trip_history_locked' in main,
 'event ring write': 'stage_event_history_locked' in main,
 'history command': 'print_history_tail' in main,
 'flash status': 'FLASHSTATUS' in main and 'nvs_get_stats' in main,
 'splash brand': 'JOEVOHAN@261' in dash and 'dashboard_show_splash' in main,
 'no tripdata partition': 'tripdata' not in part,
 'tripnvs enlarged': '0x60000' in part,
}
failed=[k for k,v in checks.items() if not v]
for k,v in checks.items(): print(('PASS' if v else 'FAIL'), k)
# basic brace sanity on changed C files
for name,text in [('main.c',main),('dashboard.c',dash)]:
    ok=text.count('{')==text.count('}')
    print(('PASS' if ok else 'FAIL'), name, 'brace balance')
    if not ok: failed.append(name+' braces')
if failed:
    print('FAILED:', ', '.join(failed))
    sys.exit(1)
print('v3.9 feature self-test: PASS')
