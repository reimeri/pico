# Keep Raylib's public Font ABI and its arbitrary-font fallback unchanged. The
# existing text/image drawing and measurement functions all reach this hook.
# Patch a build-local copy, never the fetched (possibly Nix-store) source.
function(pico_patch_raylib_text)
    set(_src "${raylib_SOURCE_DIR}/src/rtext.c")
    file(READ "${_src}" _text)
    set(_old "int GetGlyphIndex(Font font, int codepoint)\n{")
    string(FIND "${_text}" "${_old}" _match)
    if(_match EQUAL -1)
        message(FATAL_ERROR "Pico failed to patch Raylib font glyph lookup")
    endif()
    string(REPLACE "${_old}" "${_old}\n    int indexed = PicoFontMetadata_GlyphIndex(font, codepoint);\n    if (indexed >= 0) return indexed;\n" _text "${_text}")
    string(PREPEND _text "#include \"${CMAKE_CURRENT_SOURCE_DIR}/font_metadata.h\"\n")
    set(_dst "${CMAKE_CURRENT_BINARY_DIR}/pico_raylib/rtext.c")
    file(CONFIGURE OUTPUT "${_dst}" CONTENT "${_text}" @ONLY)
    set_source_files_properties("${_src}" TARGET_DIRECTORY raylib PROPERTIES HEADER_FILE_ONLY TRUE)
    target_sources(raylib PRIVATE "${_dst}" "${CMAKE_CURRENT_SOURCE_DIR}/font_metadata.c")
endfunction()

pico_patch_raylib_text()
