set(SOURCES
  PhoneMapping.cpp
  PhoneMapping.h
  AccessLog.cpp
  AccessLog.h
  ACL.cpp
  ACL.h
  ApiHandler.cpp
  SipHandler.cpp
  Control.cpp
  CallFwd.cpp
  DncMapping.cpp
  TollFreeMapping.cpp
  DnoMapping.cpp
  LergMapping.cpp
  YoumailMapping.cpp
  GeoMapping.cpp
  FtcMapping.cpp
  F404Mapping.cpp
  F606Mapping.cpp
  )

add_executable(callfwd ${SOURCES})
target_link_libraries(callfwd
  proxygen::proxygenhttpserver
  osips_parser
  TBB::tbb
  ${SYSTEMD_LDFLAGS}
  )

add_subdirectory(test)
