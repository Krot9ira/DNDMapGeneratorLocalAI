cmake_minimum_required(VERSION 3.20)
project(DndBattlemapGenerator LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

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
    app/src/core/process_runner.cpp
    app/src/core/file_dialogs.cpp
    app/src/core/dx11_renderer.cpp
    app/src/core/window.cpp
    app/src/services/connection_monitor.cpp
    app/src/services/render_service.cpp
    app/src/ui/texture_loader.cpp
    app/src/ui/ui_context.cpp
    app/src/ui/map_canvas.cpp
    app/src/ui/tab_prompt.cpp
    app/src/ui/tab_design.cpp
    app/src/ui/tab_paint.cpp
    app/src/ui/tab_gallery.cpp
    app/src/ui/tab_dungeondraft.cpp
    app/src/ui/tab_settings.cpp
    app/src/ui/tab_docs.cpp
    app/src/ui/app_ui.cpp
    app/vendor/imgui/imgui.cpp
    app/vendor/imgui/imgui_draw.cpp
    app/vendor/imgui/imgui_tables.cpp
    app/vendor/imgui/imgui_widgets.cpp
    app/vendor/imgui/backends/imgui_impl_win32.cpp
    app/vendor/imgui/backends/imgui_impl_dx11.cpp
    app/resources/app.rc
)

set(HEADERS
    app/src/app_state.h
    app/src/core/window.h
    app/src/core/dx11_renderer.h
    app/src/core/process_runner.h
    app/src/core/file_dialogs.h
    app/src/services/connection_monitor.h
    app/src/services/render_service.h
    app/src/ui/texture_loader.h
    app/src/ui/ui_context.h
    app/src/ui/map_canvas.h
    app/src/ui/tab_prompt.h
    app/src/ui/tab_design.h
    app/src/ui/tab_paint.h
    app/src/ui/tab_gallery.h
    app/src/ui/tab_dungeondraft.h
    app/src/ui/tab_settings.h
    app/src/ui/tab_docs.h
    app/src/ui/app_ui.h
    app/include/app_config.h
    app/include/app_theme.h
    app/include/comfy_service.h
    app/include/ideogram_caption.h
    app/include/map_architect.h
    app/include/map_types.h
    app/include/map_rasterizer.h
    app/include/map_serializer.h
    app/include/ollama_service.h
    app/include/style_manager.h
    app/include/winhttp_client.h
)

add_executable(DndBattlemapGenerator WIN32 ${SOURCES} ${HEADERS})

# Mirror on-disk folder structure in VS Solution Explorer
set(APP_SOURCES
    app/src/main.cpp
    app/src/map_rasterizer.cpp
    app/src/core/process_runner.cpp
    app/src/core/file_dialogs.cpp
    app/src/core/dx11_renderer.cpp
    app/src/core/window.cpp
    app/src/services/connection_monitor.cpp
    app/src/services/render_service.cpp
    app/src/ui/texture_loader.cpp
    app/src/ui/ui_context.cpp
    app/src/ui/map_canvas.cpp
    app/src/ui/tab_prompt.cpp
    app/src/ui/tab_design.cpp
    app/src/ui/tab_paint.cpp
    app/src/ui/tab_gallery.cpp
    app/src/ui/tab_dungeondraft.cpp
    app/src/ui/tab_settings.cpp
    app/src/ui/tab_docs.cpp
    app/src/ui/app_ui.cpp
    app/src/app_state.h
    app/src/core/window.h
    app/src/core/dx11_renderer.h
    app/src/core/process_runner.h
    app/src/core/file_dialogs.h
    app/src/services/connection_monitor.h
    app/src/services/render_service.h
    app/src/ui/texture_loader.h
    app/src/ui/ui_context.h
    app/src/ui/map_canvas.h
    app/src/ui/tab_prompt.h
    app/src/ui/tab_design.h
    app/src/ui/tab_paint.h
    app/src/ui/tab_gallery.h
    app/src/ui/tab_dungeondraft.h
    app/src/ui/tab_settings.h
    app/src/ui/tab_docs.h
    app/src/ui/app_ui.h
)
set(INCLUDE_HEADERS
    app/include/app_config.h
    app/include/app_theme.h
    app/include/comfy_service.h
    app/include/ideogram_caption.h
    app/include/map_architect.h
    app/include/map_types.h
    app/include/map_rasterizer.h
    app/include/map_serializer.h
    app/include/ollama_service.h
    app/include/style_manager.h
    app/include/winhttp_client.h
)
set(VENDOR_SOURCES
    app/vendor/imgui/imgui.cpp
    app/vendor/imgui/imgui_draw.cpp
    app/vendor/imgui/imgui_tables.cpp
    app/vendor/imgui/imgui_widgets.cpp
    app/vendor/imgui/backends/imgui_impl_win32.cpp
    app/vendor/imgui/backends/imgui_impl_dx11.cpp
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/app/src"     PREFIX "Source"  FILES ${APP_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/app/include" PREFIX "Include" FILES ${INCLUDE_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/app/vendor"  PREFIX "Vendor"  FILES ${VENDOR_SOURCES})

target_link_libraries(DndBattlemapGenerator PRIVATE
    d3d11
    dxgi
    winhttp
    comdlg32
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

    # Refreshed in place, never rm -rf'd whole: output/ beside the executable
    # is where the app saves maps, and a rebuild must never throw a player's
    # work away. Only the folders we ship are cleared, so nothing stale
    # survives and nothing user-made is lost.
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/output"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_DIR}/styles"   "${DIST_DIR}/presets"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_DIR}/tools"    "${DIST_DIR}/docs"

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
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_layouts.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_caption_parity.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_captions.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_scenes.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/render_scene.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/comfy.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ideogram_prompt.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ollama_client.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/paths.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/pipeline.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/planner.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/render.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/workflow.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/dungeondraft_pck.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/dungeondraft_db.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/dungeondraft_indexer.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/dungeondraft_enrich.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/dungeondraft_matcher.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/dungeondraft_assembler.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/dungeondraft_foundry.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_dungeondraft.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_enrichment_quality.py"
            "${DIST_DIR}/tools"
    # The scene files and the descriptions they were built from travel together:
    # a scene without its description cannot be checked against anything.
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/scenes" "${DIST_DIR}/tools/scenes"

    # The asset library is built on the user's machine from the packs they own,
    # by the Dungeondraft tab. Only the folder ships. Copying this machine's
    # copy would ship 250 000 thumbnails and an index of somebody else's packs,
    # and rewrite it on every build.
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/data"

    # Documentation. Only the finished manual ships; the HTML source and the
    # development report stay in the repository.
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/docs"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/Manual.pdf"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/dungeondraft_map_format.md"
            "${DIST_DIR}/docs/"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/AGENTS.md" "${DIST_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/READ_ME_FIRST.txt" "${DIST_DIR}/"

    # Licences. The executable statically contains Dear ImGui, nlohmann/json and
    # stb, all of which require their notice to travel with any copy - so both
    # files ship, not just ours.
    COMMAND ${CMAKE_COMMAND} -E rm -f "${DIST_DIR}/imgui.ini"

    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md"
            "${DIST_DIR}/"
    VERBATIM
)
