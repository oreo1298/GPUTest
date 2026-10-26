set(_qt_components Widgets Sql Svg)
if(UNIX AND NOT APPLE)
  list(APPEND _qt_components DBus)
endif()
find_package(Qt6 6.4 COMPONENTS ${_qt_components})
if(NOT Qt6_FOUND)
  message(WARNING "Qt 6 not found: the GUI will not be built (set CMAKE_PREFIX_PATH to a Qt 6.8+ install)")
  return()
endif()
if(Qt6_VERSION VERSION_LESS 6.8)
  message(WARNING "Qt ${Qt6_VERSION} < 6.8: theme-following features are reduced (development fallback only)")
endif()

qt_standard_project_setup()

qt_add_executable(gt_gui WIN32
  main.cpp
  mainwindow.cpp
  appstate.cpp
  runnerclient.cpp
  settings.cpp
  theme.cpp
  widgets.cpp
  chart.cpp
  history.cpp
  telemetryview.cpp
  pages/pages.h
  pages/page.cpp
  pages/homepage.cpp
  pages/testspage.cpp
  pages/runpage.cpp
  pages/resultspage.cpp
  pages/historypage.cpp
  pages/comparepage.cpp
  pages/systempage.cpp
  pages/settingspage.cpp
)
qt_add_resources(gt_gui "icons" PREFIX "/icons" BASE icons FILES
  icons/activity.svg icons/chart-column.svg icons/circle-check.svg icons/circle-x.svg icons/clock.svg
  icons/copy.svg icons/cpu.svg icons/download.svg icons/external-link.svg icons/fan.svg icons/file-code.svg
  icons/file-text.svg icons/flame.svg icons/folder-open.svg icons/gauge.svg icons/git-compare.svg icons/house.svg
  icons/info.svg icons/layers.svg icons/list-checks.svg icons/memory-stick.svg icons/monitor.svg icons/play.svg
  icons/plus.svg icons/refresh-cw.svg icons/save.svg icons/search.svg icons/settings.svg icons/shield-check.svg
  icons/square.svg icons/thermometer.svg icons/timer.svg icons/trash.svg icons/triangle-alert.svg icons/x.svg
  icons/zap.svg)
qt_add_resources(gt_gui "appicon" PREFIX "/app" BASE "${CMAKE_SOURCE_DIR}/packaging/icons" FILES
  "${CMAKE_SOURCE_DIR}/packaging/icons/app.svg")
set_target_properties(gt_gui PROPERTIES OUTPUT_NAME "${APP_GUI_NAME}")
target_include_directories(gt_gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gt_gui PRIVATE gt_common Qt6::Widgets Qt6::Sql Qt6::Svg)
if(TARGET Qt6::DBus)
  target_link_libraries(gt_gui PRIVATE Qt6::DBus)
  target_compile_definitions(gt_gui PRIVATE GT_HAVE_QTDBUS=1)
endif()
target_compile_definitions(gt_gui PRIVATE QT_NO_KEYWORDS)
gt_set_warnings(gt_gui)
install(TARGETS gt_gui RUNTIME DESTINATION "${GT_INSTALL_BINDIR}")
