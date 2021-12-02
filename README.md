# Uncapped OpenVR driver using virtual display

Handy when using e.g. Vive trackers without an HMD for measuring/mocap purposes.
This driver is a replacement for the default null driver that caps at the monitor's refresh rate.

## Installation

1. Run `vrpathreg adddriver <path_to_driver_folder>` (Driver is already compiled in `drivers\bin\drivers\uncapped`). `vrpathreg` can be found in the SteamVR installation directory.
2. Add the following to `steamvr.vrsettings` in the `steamvr` section:
    ```
    "activateMultipleDrivers" : true,
    "forcedDriver" : "uncapped",
    "requireHmd" : false,
    ```
## Uninstallation

1. Run `vrpathreg removedriver <path_to_driver_folder>`.
2. Undo changes made to `steamvr.vrsettings`.