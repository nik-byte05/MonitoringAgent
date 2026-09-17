# CMakeList.txt: проект CMake для MonitoringAgent; включите исходный код и определения,
# укажите здесь логику для конкретного проекта.
#

# Добавьте источник в исполняемый файл этого проекта.
add_executable (MonitoringAgent "MonitoringAgent.cpp" "MonitoringAgent.h")

target_link_libraries(MonitoringAgent PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    ws2_32
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET MonitoringAgent PROPERTY CXX_STANDARD 20)
endif()

# TODO: Добавьте тесты и целевые объекты, если это необходимо.
