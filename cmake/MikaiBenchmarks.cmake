# Keep these project-generated clips in step with the benchmark catalogue.
set(MIKAI_BENCHMARK_CLIPS
  "480x360.mp4"
  "640x480.mp4"
  "768x576.mp4"
  "1280x720.mp4"
  "1920x1080.mp4")
set(MIKAI_BENCHMARK_SOURCE_SUBDIR "resources/benchmarks")

function(mikai_benchmark_clips benchmarks_dir out_var)
  set(clips "")
  foreach(name IN LISTS MIKAI_BENCHMARK_CLIPS)
    set(clip "${benchmarks_dir}/${name}")
    if(NOT EXISTS "${clip}" OR IS_DIRECTORY "${clip}")
      message(FATAL_ERROR
        "Benchmark clip '${name}' is missing from ${MIKAI_BENCHMARK_SOURCE_SUBDIR}. "
        "The packaged benchmark catalogue needs all five generated clips; restore them "
        "with tests/fixtures/generate_benchmark_media.py instead of dropping the payload.")
    endif()
    file(SIZE "${clip}" clip_size)
    if(clip_size EQUAL 0)
      message(FATAL_ERROR "Benchmark clip '${name}' is empty: ${clip}")
    endif()
    list(APPEND clips "${clip}")
  endforeach()
  set(${out_var} "${clips}" PARENT_SCOPE)
endfunction()
