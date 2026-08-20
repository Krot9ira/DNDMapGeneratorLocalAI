cmake_minimum_required(VERSION 3.20)
project(DndBattlemapGenerator LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    app/include
    app/src
    app/vendor
    app/vendor/imgui
    app/vendor/imgui/backends
    app/vendor/stb
    app/vendor/nlohmann
)

set(SOURCES
    app/src/main.cpp
    app/src/map_rasterizer.cpp
    app/vendor/imgui/imgui.cpp
    app/vendor/imgui/imgui_draw.cpp
    app/vendor/imgui/imgui_tables.cpp
    app/vendor/imgui/imgui_widgets.cpp
    app/vendor/imgui/backends/imgui_impl_win32.cpp
    app/vendor/imgui/backends/imgui_impl_dx11.cpp
    app/resources/app.rc
)

add_executable(DndBattlemapGenerator WIN32 ${SOURCES})

target_link_libraries(DndBattlemapGenerator PRIVATE
    d3d11
    dxgi
    winhttp
    dwmapi
)

# Development build. It deliberately has no styles/ beside it: the app walks up
# to the repository root, so the app and the command line tools share one copy
# of every style while you are working on them.
set_target_properties(DndBattlemapGenerator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

# ---------------------------------------------------------------------------
# Shippable package
#
# Assembled next to the build so the whole folder can be zipped and handed to
# someone who has never seen the source. It carries the app, its data, the user
# manual and the Python tooling agents need - nothing else is required.
# ---------------------------------------------------------------------------
set(DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist/DndBattlemapGenerator")

add_custom_command(TARGET DndBattlemapGenerator POST_BUILD
    COMMENT "Assembling shippable package in dist/DndBattlemapGenerator"

    COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/output"

    COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:DndBattlemapGenerator>" "${DIST_DIR}/"

    # Data the app reads at run time. It sits beside the executable, so the
    # package is self-contained wherever it is unzipped.
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/styles"  "${DIST_DIR}/styles"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/presets" "${DIST_DIR}/presets"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json" "${DIST_DIR}/"

    # Command line and AI agent tooling. Copied file by file so Python's
    # __pycache__ never ends up in a release.
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/tools"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/agent_api.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/architect.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/comfy.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ideogram_prompt.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ollama_client.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/paths.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/pipeline.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/planner.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/render.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/workflow.py"
            "${DIST_DIR}/tools"

    # Documentation. Only the finished manual ships; the HTML source and the
    # development report stay in the repository.
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/docs"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/Manual.pdf" "${DIST_DIR}/docs/"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/AGENTS.md" "${DIST_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/READ_ME_FIRST.txt" "${DIST_DIR}/"

    # Licences. The executable statically contains Dear ImGui, nlohmann/json and
    # stb, all of which require their notice to travel with any copy - so both
    # files ship, not just ours.
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md"
            "${DIST_DIR}/"
    VERBATIM
)
