    NVIDIA GPU Fan Monitor + Individual Fan Controller
    GTK3 + C + NVML + Cairo animation

    Features:
      - Enumerates NVIDIA GPUs through NVML.
      - Enumerates total number of fans per selected GPU.
      - Controls selected fan individually using nvmlDeviceSetFanSpeed_v2().
      - Restores selected fan to automatic control using nvmlDeviceSetDefaultFanSpeed_v2().
      - Optional "Apply to All Fans" and "Restore All Fans Auto".
      - Live temperature, utilization, fan speed display.
      - Small animated Cairo fan graphic.
      - Animation speed follows selected fan speed percentage.
      - Uses dlopen/dlsym, so it does not require nvml.h at build time.

    Build:
      gcc nvfan_gtk3_v2.c -o nvfan_gtk3_v2 $(pkg-config --cflags --libs gtk+-3.0) -ldl -lm

    Run:
      ./nvfan_gtk3_v2

    If fan control fails because of permissions:
      sudo ./nvfan_gtk3_v2

    WARNING:
      Manual fan control can overheat or damage hardware if the fan is set too low.
      Restore automatic fan control before exiting.
