> **v3.9.10 SDKCONFIG-FIX:** `SDKCONFIG_DEFAULTS` is explicitly layered and stale `sdkconfig.*` is removed before build, C++ exceptions are enabled for Espressif VCP, and flash-size Kconfig is pinned to 16 MB.

# Nissan B14 CONSULT Dashboard Full v3.9.10

**ENGINEERING UNITS FULL / INTERNAL FLASH / NO SD / NO RTC**

Boot splash: **JOEVOHAN@261**.

## v3.9.10 Power-On Display Fix
- Backlight GPIO18 OFF ระหว่าง boot
- TFT_CS GPIO10 HIGH ก่อน SPI init
- TFT_RST GPIO8 LOW 350 ms แล้ว HIGH 180 ms
- ILI9488 reset/init retry สูงสุด 3 ครั้ง
- Clear BLACK และวาด `JOEVOHAN@261` ก่อนเปิด Backlight
- Serial command: `DISPLAYSTATUS`


## v3.9.10 Engineering Units
- MAF: `MAF EST g/s` from 1.6 L + RPM + TPS + VE model, with raw CONSULT voltage retained.
- TPS: 0-100% engineering estimate using 0.56 V closed / 4.00 V WOT, with raw voltage retained.
- O2: narrowband voltage plus `RICH / LEAN / MID` state; no fake AFR conversion.
- Ignition: signed degrees with `+BTDC / -ATDC` legend.
- A/F Alpha: shown as fuel-correction delta `Alpha - 100%`, while raw Alpha remains in serial output.
- Existing standard units remain: rpm, km/h, ms, %, C, V, L/h, km/L, km, L, Baht.

## Storage
Internal Flash NVS only. Trip history, event ring, snapshots and recovery remain enabled. No SD and no RTC.

## Build stability inherited from v3.9.5
- ESP-IDF 5.4.1 / PlatformIO espressif32 6.11.0
- ILI9488 1.0.9
- Native USB/VCP bridge compatibility
- Project-local SPI-only esp_lcd override excludes unused RGB LCD source that triggered a Windows GCC internal compiler error.
- Recommended build command: `pio run -j 1`

See `INSTALL_FULL_v3.9.10_TH.txt`, `ENGINEERING_UNITS_v3.9.10_TH.txt` and `POWER_ON_DISPLAY_FIX_v3.9.10_TH.txt`.
