set(TARGET_NAME driver_uncapped)

add_library(${TARGET_NAME} SHARED
  driverlog.cpp
  driverlog.h
  driver_uncapped.cpp
)

add_definitions(-DDRIVER_UNCAPPED_EXPORTS)

target_link_libraries(${TARGET_NAME}
  ${OPENVR_LIBRARIES}
  ${CMAKE_DL_LIBS}
)

setTargetOutputDirectory(${TARGET_NAME})
