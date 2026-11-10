# One patchelf lookup for every Linux route that rewrites RUNPATHs (the mpv
# bundle, Qt deployment, the AppImage). The program is a build-only
# dependency; each caller keeps its own failure wording in `reason`, and the
# result lands in the one variable name MIKAI_PATCHELF_EXECUTABLE so the
# three sites cannot drift apart.
function(mikai_find_patchelf reason)
  find_program(MIKAI_PATCHELF_EXECUTABLE NAMES patchelf)
  if(NOT MIKAI_PATCHELF_EXECUTABLE)
    message(FATAL_ERROR "${reason}")
  endif()
  set(MIKAI_PATCHELF_EXECUTABLE "${MIKAI_PATCHELF_EXECUTABLE}" PARENT_SCOPE)
endfunction()
