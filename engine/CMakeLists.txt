cmake_minimum_required(VERSION 3.30)
project(poni_gbp_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# CMake >= 3.28 scans every C++23 source for modules; we don't use modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(PONI_WERROR "Treat compiler warnings as errors" OFF)
option(PONI_BUILD_TESTS "Build unit and integration tests" ON)

find_package(Boost CONFIG REQUIRED COMPONENTS asio beast describe json url)
find_package(OpenSSL REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(cctz CONFIG REQUIRED)
# nghttp2's vcpkg port installs no CMake package: its header and library are found directly (D-097).
find_path(NGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h REQUIRED)
find_library(NGHTTP2_LIBRARY NAMES nghttp2 REQUIRED)
message(STATUS "nghttp2: ${NGHTTP2_LIBRARY}")
add_library(nghttp2::nghttp2 UNKNOWN IMPORTED)
set_target_properties(nghttp2::nghttp2 PROPERTIES IMPORTED_LOCATION "${NGHTTP2_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${NGHTTP2_INCLUDE_DIR}")

# Warnings and definitions shared by the engine and its tests.
add_library(poni_build_options INTERFACE)
target_compile_options(poni_build_options INTERFACE -Wall -Wextra -Wpedantic)
if(PONI_WERROR)
  target_compile_options(poni_build_options INTERFACE -Werror)
endif()
# Designated initializers that leave defaulted members out are our normal
# style ({.kind = "x"}); clang warns about them under -Wextra.
target_compile_options(poni_build_options INTERFACE -Wno-missing-designated-field-initializers)
target_compile_definitions(poni_build_options INTERFACE
  BOOST_ASIO_NO_DEPRECATED
  BOOST_ASIO_DISABLE_BOOST_COROUTINE
)

add_library(poni_engine STATIC
  src/connectors/gbp/http_local_posts.cpp
  src/connectors/gbp/local_posts.cpp
  src/connectors/gbp/http_performance.cpp
  src/connectors/gbp/http_reader.cpp
  src/connectors/gbp/http_writer.cpp
  src/connectors/google/auth.cpp
  src/connectors/identity/google_provider.cpp
  src/connectors/llm/net_client.cpp
  src/connectors/llm/protocols.cpp
  src/connectors/notify/apns.cpp
  src/connectors/notify/gmail.cpp
  src/connectors/pubsub/subscriber.cpp
  src/connectors/storage/store.cpp
  src/connectors/workspace/http_directory.cpp
  src/core/config.cpp
  src/core/crypto.cpp
  src/core/etag.cpp
  src/core/log.cpp
  src/core/mapped_file.cpp
  src/core/rate_limiter.cpp
  src/core/text.cpp
  src/core/tls.cpp
  src/db/audit.cpp
  src/db/database.cpp
  src/db/migrations.cpp
  src/db/repo_analysis.cpp
  src/db/repo_auth.cpp
  src/db/repo_cases.cpp
  src/db/repo_go_live.cpp
  src/db/repo_google_target.cpp
  src/db/repo_knowledge.cpp
  src/db/repo_catalog.cpp
  src/db/repo_devices.cpp
  src/db/repo_notifications.cpp
  src/db/repo_auto_posting.cpp
  src/db/repo_posting.cpp
  src/db/repo_local_posts.cpp
  src/db/repo_post_help.cpp
  src/db/repo_reports.cpp
  src/db/repo_reviews.cpp
  src/db/repo_settings.cpp
  src/db/repo_users.cpp
  src/domain/dates.cpp
  src/domain/go_live.cpp
  src/domain/image_info.cpp
  src/domain/video_info.cpp
  src/domain/language.cpp
  src/domain/local_post.cpp
  src/domain/post_help.cpp
  src/domain/posting_schedule.cpp
  src/domain/prompts.cpp
  src/domain/push.cpp
  src/domain/report_post_prompts.cpp
  src/domain/reply_checks.cpp
  src/domain/reply_policy.cpp
  src/domain/reply_settings.cpp
  src/domain/report.cpp
  src/domain/report_html.cpp
  src/domain/report_schedule.cpp
  src/domain/review_pipeline.cpp
  src/domain/review_sync.cpp
  src/domain/review_text.cpp
  src/http/api.cpp
  src/http/api_analysis.cpp
  src/http/api_auto_posting.cpp
  src/http/api_go_live.cpp
  src/http/api_replies.cpp
  src/http/api_local_posts.cpp
  src/http/api_reports.cpp
  src/http/api_users.cpp
  src/http/auth.cpp
  src/http/client.cpp
  src/http/connect.cpp
  src/http/h2.cpp
  src/http/health.cpp
  src/http/router.cpp
  src/http/server.cpp
  src/jobs/local_posts.cpp
  src/jobs/post_advisor.cpp
  src/jobs/analysis.cpp
  src/jobs/backoff.cpp
  src/jobs/builtin.cpp
  src/jobs/go_live.cpp
  src/jobs/notifications.cpp
  src/jobs/posting.cpp
  src/jobs/queue.cpp
  src/jobs/reports.cpp
  src/jobs/scheduler.cpp
  src/jobs/staff_check.cpp
  src/jobs/sync.cpp
  src/jobs/worker.cpp
)
target_include_directories(poni_engine PUBLIC src)
target_compile_definitions(poni_engine PUBLIC
  PONI_VERSION="${PROJECT_VERSION}"
  # Default for development builds; deployments set PONI_MIGRATIONS_DIR.
  PONI_DEFAULT_MIGRATIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/migrations"
)
target_link_libraries(poni_engine
  PUBLIC
    poni_build_options
    Boost::asio Boost::beast Boost::describe Boost::json Boost::url
    OpenSSL::SSL OpenSSL::Crypto
    libpqxx::pqxx
  PRIVATE
    cctz::cctz
    nghttp2::nghttp2
)

add_executable(poni-gbp-engine src/main.cpp)
target_link_libraries(poni-gbp-engine PRIVATE poni_engine)

if(PONI_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
