# Desktop integration files (Linux). Generated from templates so the app name
# lives only in cmake/AppInfo.cmake.
if(UNIX AND NOT APPLE)
  configure_file(linux/app.desktop.in "${CMAKE_BINARY_DIR}/packaging/${APP_RDNN}.desktop" @ONLY)
  configure_file(linux/app.metainfo.xml.in "${CMAKE_BINARY_DIR}/packaging/${APP_RDNN}.metainfo.xml" @ONLY)
  install(FILES "${CMAKE_BINARY_DIR}/packaging/${APP_RDNN}.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
  install(FILES "${CMAKE_BINARY_DIR}/packaging/${APP_RDNN}.metainfo.xml" DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
  install(FILES icons/app.svg DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" RENAME "${APP_RDNN}.svg")
endif()
