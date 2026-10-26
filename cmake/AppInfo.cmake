# Single source of truth for the product name. Rename the app here; everything
# else (binary names, window titles, config/data dirs, desktop files, report
# headers) is derived from these values.
set(APP_DISPLAY_NAME "Kiln")                     # human-readable name
set(APP_ID "kiln")                               # lowercase id: binaries, dirs, file prefixes
set(APP_RDNN "io.github.oreo1298.Kiln")          # reverse-DNS id (desktop file, Flatpak, AppStream)
set(APP_DESCRIPTION "Open-source GPU benchmark, feature tester and diagnostic suite")
set(APP_HOMEPAGE "https://github.com/oreo1298/GPUTest")
set(APP_VERSION "0.1.0")

set(APP_RUNNER_NAME "${APP_ID}-run")             # benchmark engine / CLI
set(APP_GUI_NAME "${APP_ID}")                    # Qt front-end
