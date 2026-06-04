cmake_minimum_required(VERSION 3.15)
project(SpectraCV CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Recherche de OpenCV (Assurez-vous que OpenCV_DIR est défini dans vos variables d'environnement)
find_package(OpenCV REQUIRED)

# Inclure les headers externes (ex: nlohmann/json)
include_directories(third_party)

# Liste des fichiers sources
set(SOURCES
    src/main.cpp
    src/ScreenCapture.cpp
    src/ScreenCapture.h
)

add_executable(SpectraCV ${SOURCES})

# Liens avec OpenCV et les API natives Windows pour la capture d'écran DirectX
target_link_libraries(SpectraCV 
    PRIVATE 
    ${OpenCV_LIBS}
    d3d11
    dxgi
)

# Empêcher la suppression accidentelle des templates et configurations par Visual Studio !
# Cette commande copie "templates" depuis la racine, "agents_config.json" depuis src, et la DLL OpenCV vers le dossier de build.
add_custom_command(TARGET SpectraCV POST_BUILD
    # Copier le dossier templates
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/templates"
        "$<TARGET_FILE_DIR:SpectraCV>/templates"
        
    # Copier agents_config.json depuis le dossier src
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/agents_config.json"
        "$<TARGET_FILE_DIR:SpectraCV>/agents_config.json"
        
    # Copier la DLL OpenCV dynamiquement depuis l'installation locale de l'utilisateur
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OpenCV_DIR}/x64/vc16/bin/opencv_world4130d.dll"
        "$<TARGET_FILE_DIR:SpectraCV>/opencv_world4130d.dll"
)
