add_definitions(-D__STDC_WANT_LIB_EXT1__)

target_compile_options(JavaScriptCore PRIVATE
    "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-fno-threadsafe-statics>"
)

find_library(SECURITY_LIBRARY Security)
list(APPEND JavaScriptCore_LIBRARIES
    ${SECURITY_LIBRARY}
)

target_link_options(JavaScriptCore PRIVATE
    -Wl,-unexported_symbols_list,${JAVASCRIPTCORE_DIR}/unexported-libc++.txt
)

list(APPEND JavaScriptCore_UNIFIED_SOURCE_LIST_FILES
    "SourcesCocoa.txt"

    "inspector/remote/SourcesCocoa.txt"
)

list(APPEND JavaScriptCore_PRIVATE_INCLUDE_DIRECTORIES
    ${JAVASCRIPTCORE_DIR}/inspector/cocoa
    ${JAVASCRIPTCORE_DIR}/inspector/remote/cocoa
)

list(APPEND JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS
    inspector/remote/RemoteInspectorConstants.h

    inspector/remote/cocoa/RemoteInspectorXPCConnection.h
)

list(REMOVE_ITEM JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS
    API/glib/JSAPIWrapperGlobalObject.h
    API/glib/JSCAutocleanups.h
    API/glib/JSCCallbackFunction.h
    API/glib/JSCClassPrivate.h
    API/glib/JSCContextInternal.h
    API/glib/JSCContextPrivate.h
    API/glib/JSCExceptionPrivate.h
    API/glib/JSCGLibWrapperObject.h
    API/glib/JSCOptions.h
    API/glib/JSCValuePrivate.h
    API/glib/JSCVirtualMachinePrivate.h
    API/glib/JSCWrapperMap.h
)

if (NOT EXISTS ${JavaScriptCore_DERIVED_SOURCES_DIR}/AugmentableInspectorControllerClient.h)
    file(WRITE ${JavaScriptCore_DERIVED_SOURCES_DIR}/AugmentableInspectorControllerClient.h "#include \"inspector/augmentable/AugmentableInspectorControllerClient.h\"")
endif ()
if (NOT EXISTS ${JavaScriptCore_DERIVED_SOURCES_DIR}/InspectorFrontendRouter.h)
    file(WRITE ${JavaScriptCore_DERIVED_SOURCES_DIR}/InspectorFrontendRouter.h "#include \"inspector/InspectorFrontendRouter.h\"")
endif ()
if (NOT EXISTS ${JavaScriptCore_DERIVED_SOURCES_DIR}/InspectorBackendDispatcher.h)
    file(WRITE ${JavaScriptCore_DERIVED_SOURCES_DIR}/InspectorBackendDispatcher.h "#include \"inspector/InspectorBackendDispatcher.h\"")
endif ()
if (NOT EXISTS ${JavaScriptCore_DERIVED_SOURCES_DIR}/InspectorBackendDispatchers.h)
    file(WRITE ${JavaScriptCore_DERIVED_SOURCES_DIR}/InspectorBackendDispatchers.h "#include \"inspector/InspectorBackendDispatchers.h\"")
endif ()
if (NOT EXISTS ${JavaScriptCore_DERIVED_SOURCES_DIR}/InspectorFrontendDispatchers.h)
    file(WRITE ${JavaScriptCore_DERIVED_SOURCES_DIR}/InspectorFrontendDispatchers.h "#include \"inspector/InspectorFrontendDispatchers.h\"")
endif ()
if (NOT EXISTS ${JavaScriptCore_DERIVED_SOURCES_DIR}/InspectorProtocolObjects.h)
    file(WRITE ${JavaScriptCore_DERIVED_SOURCES_DIR}/InspectorProtocolObjects.h "#include \"inspector/InspectorProtocolObjects.h\"")
endif ()
