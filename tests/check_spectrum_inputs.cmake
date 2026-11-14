file(MAKE_DIRECTORY "${test_directory}")
set(output "${test_directory}/existing.png")
set(scene "${test_directory}/file.pbrt")
set(spectrum "${test_directory}/light.spd")
set(sentinel "existing output")
file(WRITE "${output}" "${sentinel}")
file(WRITE "${scene}" [=[
Film "rgb" "integer xresolution" 1 "integer yresolution" 1
Camera "perspective"
PixelFilter "box" "float xradius" 0.5 "float yradius" 0.5
Sampler "independent" "integer pixelsamples" 1
Integrator "path" "integer maxdepth" 0
WorldBegin
LightSource "infinite" "spectrum L" "light.spd"
Material "diffuse"
Shape "trianglemesh" "point3 P" [-1 -1 -3 1 -1 -3 0 1 -3]
]=])

file(WRITE "${spectrum}" "360 1# adjacent comment\n830 2")
execute_process(COMMAND "${executable}" --check-pbrt "${scene}"
    RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT exit STREQUAL "0" OR NOT stderr STREQUAL "")
    message(FATAL_ERROR "Valid spectrum preflight failed ${exit}: ${stdout}${stderr}")
endif()

function(expect_rejection expected_exit diagnostic)
    execute_process(COMMAND "${executable}" --render-pbrt "${scene}" --output "${output}"
        RESULT_VARIABLE exit OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT exit STREQUAL "${expected_exit}" OR
       NOT "${stdout}${stderr}" MATCHES "${diagnostic}" OR
       NOT stdout MATCHES "Rendering was not attempted" OR
       stdout MATCHES "Device:|Committed samples|Wrote PNG")
        message(FATAL_ERROR "Unexpected spectrum rejection ${exit}: ${stdout}${stderr}")
    endif()
    file(READ "${output}" current)
    if(NOT current STREQUAL sentinel)
        message(FATAL_ERROR "Spectrum rejection changed existing output")
    endif()
endfunction()

file(REMOVE "${spectrum}")
expect_rejection(1 "missing-file")
file(WRITE "${spectrum}" "360 1\n500 nope\n830 1\n")
expect_rejection(1 "light\\.spd.*:2:5:.*finite binary32")
file(WRITE "${spectrum}" "360 -1 830 1\n")
expect_rejection(2 "Emission spectrum must be nonnegative")
file(WRITE "${spectrum}" "360 0 830 0\n")
expect_rejection(1 "positive finite CIE Y inner product")
file(WRITE "${spectrum}" "1000 1 1100 1\n")
expect_rejection(1 "positive finite CIE Y inner product")
