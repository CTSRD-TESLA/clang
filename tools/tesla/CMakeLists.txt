add_clang_executable(tesla Tesla.cpp tool.cpp)

target_link_libraries(tesla
  clangTooling
  clangBasic
)

