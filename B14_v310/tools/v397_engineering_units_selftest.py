from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
main=(root/'src/main.c').read_text(encoding='utf-8')
dash=(root/'src/dashboard.c').read_text(encoding='utf-8')
hdr=(root/'src/dashboard.h').read_text(encoding='utf-8')
checks={
 'firmware v3.9.7': '3.9.7-POWER-ON-DISPLAY-FIX-FULL' in main,
 'maf g/s model': all(x in main for x in ['ENGINE_DISPLACEMENT_L', 'AIR_DENSITY_G_PER_L', 'estimate_maf_gps', 'maf_gps_est']),
 'maf raw retained': 'maf_voltage_v' in main and 'RAW=%4.3f V' in main and 'G/S RAW%.2fV' in dash,
 'tps percent model': all(x in main for x in ['DEFAULT_TPS_CLOSED_V', 'DEFAULT_TPS_WOT_V', 'estimate_tps_pct', 'throttle_position_pct_est']),
 'tps raw retained': 'TPS=%5.1f%% (RAW=%4.2f V)' in main and 'RAW%.2fV' in dash,
 'o2 state': all(x in main for x in ['O2_LEAN_THRESHOLD_V', 'O2_RICH_THRESHOLD_V', 'o2_state_name']) and 'O2 STATE' in dash,
 'ign btdc atdc': 'format_ignition' in main and '+BTDC/-ATDC' in dash,
 'fuel correction': 'fuel_correction_delta_pct' in main and 'FUEL_CORR=' in main and 'AAC/CORR' in dash,
 'units command': 'strcasecmp(cmd, "UNITS")' in main and 'ENGINEERING UNITS v3.9.7' in main,
 'no sd init': all(x not in main for x in ['sdspi','sdmmc','esp_vfs_fat','MICROSD','CSV_FILE_PATH']),
 'no rtc': 'NO-RTC' in main,
 'splash': 'JOEVOHAN@261' in dash,
 'brace main': main.count('{') == main.count('}'),
 'brace dash': dash.count('{') == dash.count('}'),
}
failed=[]
for k,v in checks.items():
    print(('PASS' if v else 'FAIL'), k)
    if not v: failed.append(k)
if failed:
    print('FAILED:', ', '.join(failed)); sys.exit(1)
print('v3.9.7 engineering-unit self-test: PASS')
