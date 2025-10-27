cmake_minimum_required(VERSION 3.27)
set(CMAKE_CXX_STANDARD 20)
set(THREADS_PREFER_PTHREAD_FLAG ON)
project("TinyWebServer")


# 定义变量
# 根目录
set(HOME ${PROJECT_SOURCE_DIR})

# 定义静态库生成路径
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${HOME}/lib)

# 定义动态库生成路径
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${HOME})

# 设置可执行文件生成路径
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${HOME})

#搜索源文件
file(GLOB_RECURSE SRC "${HOME}/src/*.cpp")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED mysqlclient)

add_executable(start  ${SRC})

target_link_libraries(start Threads::Threads)
target_include_directories(start PRIVATE ${MYSQL_INCLUDE_DIRS})
target_link_libraries(start ${MYSQL_LIBRARIES})