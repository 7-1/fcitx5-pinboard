add_library(pinboard MODULE pinboard.cpp)

target_link_libraries(pinboard
    Fcitx5::Core
    Fcitx5::Config
    Fcitx5::Utils
    Fcitx5::Module::Clipboard
)

set_target_properties(pinboard PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/fcitx5"
)

configure_file(pinboard.conf.in pinboard.conf @ONLY)

install(TARGETS pinboard DESTINATION "${FCITX_INSTALL_ADDONDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/pinboard.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon")
