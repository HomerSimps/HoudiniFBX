cmake_minimum_required( VERSION 3.6 )

project( HDK_Project )

# CMAKE_PREFIX_PATH must contain the path to the toolkit/cmake subdirectory of
# the Houdini installation. See the "Compiling with CMake" section of the HDK
# documentation for more details, which describes several options for
# specifying this path.
list( APPEND CMAKE_PREFIX_PATH "$ENV{HFS}/toolkit/cmake" )

# Locate Houdini's libraries and header files.
# Registers an imported library target named 'Houdini'.
find_package( Houdini REQUIRED )

set( library_name ROP_FBX )




# Add a library and its source files.
add_library( ${library_name} SHARED
	ROP_FBX.C
    ROP_FBX.h
	ROP_FBXExporter.C
	ROP_FBXExporter.h
    ROP_FBXExporterWrapper.C
    ROP_FBXExporterWrapper.h
	ROP_FBXActionManager.C
    ROP_FBXActionManager.h
	ROP_FBXAnimVisitor.C
    ROP_FBXAnimVisitor.h
	ROP_FBXBaseAction.C
    ROP_FBXBaseAction.h
	ROP_FBXBaseVisitor.C
    ROP_FBXBaseVisitor.h
	ROP_FBXCommon.C
	ROP_FBXCommon.h
    ROP_FBXDerivedActions.C
    ROP_FBXDerivedActions.h
	ROP_FBXErrorManager.C
    ROP_FBXErrorManager.h
	ROP_FBXMainVisitor.C
    ROP_FBXMainVisitor.h
	ROP_FBXUtil.C
    ROP_FBXUtil.h
)

# Link against the Houdini libraries, and add required include directories and
# compile definitions.
target_link_libraries( ${library_name} Houdini )

# Include ${CMAKE_CURRENT_BINARY_DIR} for the generated header.
target_include_directories( ${library_name} PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Sets several common target properties, such as the library's output directory.
houdini_configure_target( ${library_name} )
