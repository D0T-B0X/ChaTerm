cmake_minimum_required(VERSION 3.23)
project(ChaTerm)

find_package(OpenSSL REQUIRED)

add_executable(
    ${PROJECT_NAME}
    ${CMAKE_SOURCE_DIR}/main.c
    ${CMAKE_SOURCE_DIR}/src/helper/UI/AppMenu.c
    ${CMAKE_SOURCE_DIR}/src/helper/UI/ServerUI.c
    ${CMAKE_SOURCE_DIR}/src/helper/UI/ClientUI.c
    ${CMAKE_SOURCE_DIR}/src/helper/ClientTable.c
    ${CMAKE_SOURCE_DIR}/src/helper/ThreadPool.c
    ${CMAKE_SOURCE_DIR}/src/helper/TaskQueue.c
    ${CMAKE_SOURCE_DIR}/src/helper/Worker.c
    ${CMAKE_SOURCE_DIR}/src/client/Client.c
    ${CMAKE_SOURCE_DIR}/src/server/Server.c
    ${CMAKE_SOURCE_DIR}/src/data/LocalData.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE m OpenSSL::Crypto)