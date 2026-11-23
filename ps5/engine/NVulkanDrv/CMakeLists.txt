project(NVulkanDrv CXX)

if(NOT USE_SDL)
  message(FATAL_ERROR "NVulkanDrv requires NSDLDrv to function.")
endif()

# Vulkan headers only: the driver loads libvulkan (or links RADV on the PS5) itself.
find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS $ENV{VULKAN_INCLUDE_DIR} /usr/include)

add_library(${PROJECT_NAME} ${LIB_TYPE} NVulkanDrv.cpp)

target_include_directories(${PROJECT_NAME}
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${VULKAN_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../Render/Src
)
if(NOT TARGET_IS_DREAMCAST AND NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  target_link_libraries(${PROJECT_NAME} ${CMAKE_DL_LIBS})
endif()
target_link_libraries(${PROJECT_NAME} Render Engine Core)
target_compile_definitions(${PROJECT_NAME} PRIVATE NVULKANDRV_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})
