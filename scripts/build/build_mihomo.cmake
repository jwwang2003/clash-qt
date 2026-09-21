# Builds mihomo from the recorded local submodule checkout and records its
# provenance. Written in CMake script mode on purpose: CMake is already a hard
# prerequisite on every supported platform, whereas Python is not present on a
# clean Windows machine (see docs/BUILD_RELEASE_PLAN.md).
#
# Invoked as:
#   cmake -DSOURCE_DIR=... -DOUTPUT=... -DGO=... [-DRELEASE=ON] [-DGOOS=] [-DGOARCH=]
#         -P scripts/build/build_mihomo.cmake
#
# Never downloads a prebuilt engine and never advances the submodule ref.

foreach(required SOURCE_DIR OUTPUT GO)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "build_mihomo: ${required} is required")
    endif()
endforeach()

if(NOT EXISTS "${SOURCE_DIR}/go.mod")
    message(FATAL_ERROR
        "mihomo source not found at ${SOURCE_DIR}.\n"
        "The submodule is not initialised. Run:  make setup")
endif()

find_program(git_program git REQUIRED)

execute_process(COMMAND "${git_program}" rev-parse HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE source_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${git_program}" describe --tags
    WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE source_describe
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND "${git_program}" status --porcelain
    WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE source_dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(source_dirty STREQUAL "")
    set(dirty_state "clean")
else()
    set(dirty_state "dirty")
    if(RELEASE)
        message(FATAL_ERROR
            "Refusing a release build: ${SOURCE_DIR} has uncommitted changes.\n"
            "${source_dirty}\n"
            "Developer builds may proceed with local edits; releases may not.")
    endif()
    message(WARNING "mihomo source is dirty; provenance records this build as unreproducible.")
endif()

execute_process(COMMAND "${GO}" version OUTPUT_VARIABLE go_version
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)

# The upstream Makefile derives VERSION from `git branch --show-current`, which is
# empty in a detached submodule, and stamps BuildTime from wall-clock `date`. Both
# are replaced here with controlled values so the output is reproducible.
set(build_tags "with_gvisor")
if(source_describe STREQUAL "")
    set(version_string "${source_commit}")
else()
    set(version_string "${source_describe}")
endif()

set(go_env "CGO_ENABLED=0")
if(DEFINED GOOS AND NOT GOOS STREQUAL "")
    list(APPEND go_env "GOOS=${GOOS}")
endif()
if(DEFINED GOARCH AND NOT GOARCH STREQUAL "")
    list(APPEND go_env "GOARCH=${GOARCH}")
    # Upstream's generic amd64 targets select GOAMD64=v3. Pin v1 for broad
    # x86_64 desktop compatibility unless the support policy says otherwise.
    if(GOARCH STREQUAL "amd64")
        list(APPEND go_env "GOAMD64=v1")
    endif()
endif()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

message(STATUS "Building mihomo ${version_string} (${source_commit}, ${dirty_state})")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env ${go_env}
        # -buildvcs=false: Go otherwise walks up from the submodule and stamps the
        # SUPERPROJECT's revision and dirty state into the engine, which records
        # clash-qt's commit where the engine's own provenance belongs, contradicts
        # the manifest below, and changes the artifact hash on every unrelated
        # commit to this repository. Provenance comes from the ldflags and the
        # manifest, both of which describe the engine source.
        "${GO}" build -mod=readonly -buildvcs=false -tags "${build_tags}" -trimpath
        -ldflags "-X github.com/metacubex/mihomo/constant.Version=${version_string} -X github.com/metacubex/mihomo/constant.BuildTime=source-build -w -s -buildid="
        -o "${OUTPUT}" .
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE build_result)

if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "mihomo build failed (exit ${build_result}).\n"
        "Its go.mod declares only 'go 1.20'; the working toolchain must be\n"
        "established by building, not read off that line. Detected: ${go_version}")
endif()

file(SHA256 "${OUTPUT}" artifact_sha256)
file(SIZE "${OUTPUT}" artifact_size)

string(JOIN "\", \"" go_env_json ${go_env})
file(WRITE "${output_dir}/mihomo-provenance.json"
"{
  \"source_path\": \"3rdparty/mihomo\",
  \"source_commit\": \"${source_commit}\",
  \"source_describe\": \"${source_describe}\",
  \"source_state\": \"${dirty_state}\",
  \"go_toolchain\": \"${go_version}\",
  \"build_tags\": \"${build_tags}\",
  \"build_env\": [\"${go_env_json}\"],
  \"artifact\": \"${OUTPUT}\",
  \"artifact_bytes\": ${artifact_size},
  \"artifact_sha256\": \"${artifact_sha256}\"
}
")
message(STATUS "mihomo -> ${OUTPUT}")
message(STATUS "  sha256 ${artifact_sha256}")
message(STATUS "  provenance ${output_dir}/mihomo-provenance.json")
