# ==============================================================================
# PoseStudio Open Source Project
# File: CMakeLists.txt
# Description: The primary CMake build script.
#              Configures the C++ compiler, locates the Qt6 framework, handles 
#              Qt's meta-object compilation, and links all source files.
# ==============================================================================

cmake_minimum_required(VERSION 3.16)
project(PoseStudio VERSION 1.0 LANGUAGES CXX)

# ==============================================================================
# [ COMPILER CONFIGURATION ]
# ==============================================================================

# Qt 6 mandates C++17 or higher. Enforce this standard across all platforms.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==============================================================================
# [ QT6 FRAMEWORK INTEGRATION ]
# ==============================================================================

# CONTRIBUTOR NOTICE: 
# Do not hardcode your local Qt path here before submitting a Pull Request!
# If CMake cannot find Qt, pass your path via the command line when building:
# e.g., cmake -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/msvc2022_64" ..
if(NOT DEFINED CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64" CACHE PATH "Fallback Qt Path")
endif()

# Locate required Qt modules. The build will fatally abort if these are missing.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql)

# Automatically invoke Qt's pre-processors during the build:
set(CMAKE_AUTOMOC ON) # Meta-Object Compiler (Handles Signals & Slots)
set(CMAKE_AUTOUIC ON) # User Interface Compiler (Handles .ui XML files)
set(CMAKE_AUTORCC ON) # Resource Compiler (Handles .qrc binary embedding)

# ==============================================================================
# [ TARGET DEFINITION & SOURCE FILES ]
# ==============================================================================

# Define the final executable output and map all required files.
# Grouped logically to make finding files easier for contributors.
add_executable(
    PoseStudio 
    
    # --- Core Application Bootstrap ---
    src/main.cpp 
    src/constants.h  
    
    # --- UI & Layout ---
    src/menumanager.cpp        
    src/menumanager.h          
    src/splashoverlay.h      
    
    # --- Tools & Widgets ---
    src/assetmanagerwidget.cpp
    src/assetmanagerwidget.h
    
    # --- Data Layer & Storage ---
    src/database.cpp 
    src/database.h
    src/preferencesmanager.cpp
    src/preferencesmanager.h
    
    # --- Embedded Resources ---
    resources/resources.qrc   # Injects images, UI styles, and SQL templates
    resources/PoseStudio.rc   # Injects the executable icon for Windows OS (.exe)
)

# ==============================================================================
# [ INCLUDES & LINKING ]
# ==============================================================================

# Tell the compiler where to look for #include <header.h> files
target_include_directories(PoseStudio PRIVATE src)

# Link the compiled executable to the required Qt binaries
target_link_libraries(PoseStudio PRIVATE Qt6::Widgets Qt6::Sql)