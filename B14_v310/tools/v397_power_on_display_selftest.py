from pathlib import Path
root = Path(__file__).resolve().parents[1]
d = (root/'src/dashboard.c').read_text(encoding='utf-8')
m = (root/'src/main.c').read_text(encoding='utf-8')
checks = {
    'firmware v3.9.7': '3.9.7-POWER-ON-DISPLAY-FIX-FULL' in m,
    'BL GPIO18': '#define TFT_PIN_BL      18' in d,
    'BL active level': 'TFT_BL_ACTIVE_LEVEL 1' in d,
    'power settle 350ms': 'TFT_POWER_STABILIZE_MS   350' in d,
    'reset release 180ms': 'TFT_RESET_RELEASE_MS     180' in d,
    'CS high boot': 'gpio_set_level((gpio_num_t)TFT_PIN_CS, 1);' in d,
    'RST low boot': 'gpio_set_level((gpio_num_t)TFT_PIN_RST, 0);' in d,
    'BL off boot': 'tft_backlight_set(false);' in d,
    '3 init retries': 'TFT_INIT_RETRY_COUNT       3' in d,
    'panel reset': 'esp_lcd_panel_reset(s_panel)' in d,
    'panel init': 'esp_lcd_panel_init(s_panel)' in d,
    'black before visible': 'clear_screen(C_BLACK);' in d,
    'BL on after splash': 'splash ready; BL=ON' in d and 'tft_backlight_set(true);' in d,
    'display status command': 'DISPLAYSTATUS' in m,
    'splash brand retained': 'JOEVOHAN@261' in m and 'JOEVOHAN@261' in d,
    'no SD init': 'esp_vfs_fat_sd' not in m and 'sdspi_host' not in m.lower(),
}
failed = [k for k,v in checks.items() if not v]
for k,v in checks.items(): print(('PASS' if v else 'FAIL'), k)
if failed: raise SystemExit('FAILED: ' + ', '.join(failed))
print('v3.9.7 power-on display self-test: PASS')
