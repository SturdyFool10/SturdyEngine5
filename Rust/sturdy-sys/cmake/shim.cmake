# Compiles the Rust<->C++ shim inside the engine's own build so it inherits the exact flags,
# definitions and include paths of every engine package (C++26, spdlog/fmt defines, glm swizzle...).
add_library(sturdy_rs_shim STATIC ${STURDY_RS_SHIM_SOURCES})
target_include_directories(sturdy_rs_shim PRIVATE ${STURDY_RS_SHIM_INCLUDE_DIRS})
# Sturdy::FFI as well: `sturdy::net` (the sturdy-net crate) calls the C ABI's networking entry points, which need no engine session.
# Static archives only contribute the objects something references, so the rest of the C ABI does not enter the binary.
target_link_libraries(sturdy_rs_shim PUBLIC Sturdy::Foundation Sturdy::Runtime Sturdy::FFI)

# Never run. Exists so ninja can print the authoritative, correctly ordered link line
# (`ninja -t commands sturdy_rs_link_probe`) which build.rs mirrors for rustc.
file(WRITE "${CMAKE_BINARY_DIR}/sturdy_rs_link_probe.cpp" "int main() { return 0; }\n")
add_executable(sturdy_rs_link_probe "${CMAKE_BINARY_DIR}/sturdy_rs_link_probe.cpp")
target_link_libraries(sturdy_rs_link_probe PRIVATE sturdy_rs_shim)
