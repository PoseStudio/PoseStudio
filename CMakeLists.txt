cmake_minimum_required(VERSION 3.16)
project(PoseStudio VERSION 1.0 LANGUAGES CXX)

# Qt 6 requires C++17 or higher
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# IMPORTANT: If CMake cannot find Qt automatically, uncomment the line below 
# and update it to your exact Qt 6 installation path.
set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64" CACHE PATH "Qt Path")

# Locate the required Qt modules
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql)

# Enable Qt's automatic compilation features
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Define the executable and its source files
add_executable(
    PoseStudio 
    src/main.cpp 
    src/database.cpp 
    src/menumanager.cpp       
    src/menumanager.h         
    src/splashoverlay.h     
    src/constants.h  
    src/assetmanagerwidget.cpp
    src/assetmanagerwidget.h
    src/preferencesmanager.cpp
    src/preferencesmanager.h
    resources/resources.qrc 
    resources/PoseStudio.rc
)

# Tells the compiler to look in the /src folder for any included header files
target_include_directories(PoseStudio PRIVATE src)

# Link the Qt libraries to your application
target_link_libraries(PoseStudio PRIVATE Qt6::Widgets Qt6::Sql)
