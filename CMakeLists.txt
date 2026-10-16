cmake_minimum_required(VERSION 3.16)
project(VisionTest CXX)

# Exige padrao C++17 moderno
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Localiza as bibliotecas e cabecalhos do OpenCV compiladas no Debian
find_package(OpenCV REQUIRED)

# Diretorios de cabecalhos
include_directories(
    inc
    ${OpenCV_INCLUDE_DIRS}
    /home/iona50/Desktop/pasta/drivers_Camera/Galaxy_Linux-x86_Gige-U3_32bits-64bits_2.6.2606.9251/Galaxy_Linux-x86_Gige-U3_32bits-64bits_2.6.2606.9251/Galaxy_camera/inc
)

# Diretorios de bibliotecas compiladas
link_directories(
    /usr/local/lib
    /home/iona50/Desktop/pasta/drivers_Camera/Galaxy_Linux-x86_Gige-U3_32bits-64bits_2.6.2606.9251/Galaxy_Linux-x86_Gige-U3_32bits-64bits_2.6.2606.9251/Galaxy_camera/lib
)

# Compila o executavel com todos os modulos desacoplados
add_executable(VisionTest
    src/main.cpp
    src/VisionModule.cpp
    src/CameraModule.cpp
    src/Orchestrator.cpp
)

# Vincula as bibliotecas estaticas/dinamicas de visao, camera e threads
target_link_libraries(VisionTest
    ${OpenCV_LIBS}
    gxiapi
    pthread
)