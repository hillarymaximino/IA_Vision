# Testes de unidade sem hardware nem OpenCV (rodam com `ctest`).

add_executable(test_sinal_objeto test_sinal_objeto.cpp)
target_include_directories(test_sinal_objeto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../visao/inc)
target_compile_features(test_sinal_objeto PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(test_sinal_objeto PRIVATE /W4 /utf-8)
else()
    target_compile_options(test_sinal_objeto PRIVATE -Wall -Wextra)
endif()

add_test(NAME sinal_objeto COMMAND test_sinal_objeto)
