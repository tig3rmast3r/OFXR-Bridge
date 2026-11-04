# 0.2 V576: the copy of the Win32 build's layer into ofxr_x86 beside the
# tray's ofxr folder. Nothing to copy until that build exists.
if(EXISTS "${SOURCE}")
    file(MAKE_DIRECTORY "${DESTINATION}")
    file(COPY_FILE "${SOURCE}"
        "${DESTINATION}/XR_APILAYER_XRFrameBridge_diagnostic.dll"
        ONLY_IF_DIFFERENT)
endif()
