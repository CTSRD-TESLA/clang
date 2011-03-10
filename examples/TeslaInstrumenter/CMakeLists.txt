set(MODULE TRUE)

set( LLVM_USED_LIBS
  clangFrontend
  clangAST
  )

set( LLVM_LINK_COMPONENTS support mc)

add_clang_library(TeslaInstrumenter
    Instrumentation.cpp
    TeslaInstrumenter.cpp
    )

set_target_properties(TeslaInstrumenter
  PROPERTIES
  LINKER_LANGUAGE CXX
  PREFIX "")
