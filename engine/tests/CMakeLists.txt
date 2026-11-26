find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Unit tests: no database, no network beyond loopback.
add_executable(poni_unit_tests
  unit/backoff_test.cpp
  unit/body_test.cpp
  unit/config_test.cpp
  unit/connectors_test.cpp
  unit/core_test.cpp
  unit/etag_test.cpp
  unit/directory_connector_test.cpp
  unit/h2_test.cpp
  unit/http_client_test.cpp
  unit/llm_client_test.cpp
  unit/log_test.cpp
  unit/post_help_test.cpp
  unit/posting_schedule_test.cpp
  unit/push_test.cpp
  unit/report_test.cpp
  unit/review_domain_test.cpp
  unit/router_test.cpp
  unit/scheduler_test.cpp
  unit/local_post_copy_test.cpp
  unit/local_post_edit_test.cpp
  unit/local_posts_connector_test.cpp
  unit/signin_test.cpp
  unit/tls_test.cpp
  unit/upload_test.cpp
  unit/write_path_test.cpp
)
target_include_directories(poni_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(poni_unit_tests PRIVATE PONI_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# nghttp2 for the HTTP/2 server the push tests talk to in memory.
target_link_libraries(poni_unit_tests PRIVATE poni_engine nghttp2::nghttp2 GTest::gtest GTest::gmock GTest::gtest_main)
gtest_discover_tests(poni_unit_tests PROPERTIES LABELS unit)

# Where the knowledge base's files are: the tests read the same files the seed does (D-078 point 9).
get_filename_component(PONI_REPOSITORY_DIR "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
set(PONI_KNOWLEDGE_DIR "${PONI_REPOSITORY_DIR}/knowledge")

# Integration tests: need a real PostgreSQL (PONI_TEST_DATABASE_URL, default: the dev compose database).
# Each test starts from a copy of one database that the real scripts migrated
# and seeded (poni-tools test-db, D-079). ctest prepares it once per run, as a
# fixture; its name carries the build's name, so the native build and the Linux
# build can run against one server at the same time.
get_filename_component(PONI_BUILD_NAME "${CMAKE_BINARY_DIR}" NAME)
string(MAKE_C_IDENTIFIER "${PONI_BUILD_NAME}" PONI_BUILD_NAME)
string(TOLOWER "poni_test_${PONI_BUILD_NAME}" PONI_TEST_TEMPLATE)
find_program(PONI_UV uv REQUIRED)
add_test(NAME prepare_test_database
  COMMAND "${PONI_UV}" run --project "${PONI_REPOSITORY_DIR}/tools" --locked --no-dev poni-tools test-db --name "${PONI_TEST_TEMPLATE}")
set_tests_properties(prepare_test_database PROPERTIES FIXTURES_SETUP prepared_database LABELS integration)
add_executable(poni_integration_tests
  integration/analysis_test.cpp
  integration/api_analysis_test.cpp
  integration/api_bodies_test.cpp
  integration/api_replies_test.cpp
  integration/api_test.cpp
  integration/app_sign_in_test.cpp
  integration/audit_location_test.cpp
  integration/auth_test.cpp
  integration/auto_reply_test.cpp
  integration/connect_test.cpp
  integration/go_live_test.cpp
  integration/google_target_test.cpp
  integration/health_test.cpp
  integration/jobs_test.cpp
  integration/local_posts_edit_test.cpp
  integration/local_posts_publish_test.cpp
  integration/local_posts_remove_test.cpp
  integration/local_posts_sync_test.cpp
  integration/local_posts_test.cpp
  integration/migrations_test.cpp
  integration/devices_api_test.cpp
  integration/notifications_test.cpp
  integration/post_help_test.cpp
  integration/posting_test.cpp
  integration/push_test.cpp
  integration/reports_test.cpp
  integration/sql_test.cpp
  integration/staff_check_test.cpp
  integration/sync_test.cpp
  integration/users_api_test.cpp
  integration/users_directory_test.cpp
)
target_include_directories(poni_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(poni_integration_tests PRIVATE
  PONI_AI_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ai/fixtures"
  PONI_KNOWLEDGE_DIR="${PONI_KNOWLEDGE_DIR}"
  PONI_TEST_TEMPLATE="${PONI_TEST_TEMPLATE}")
target_link_libraries(poni_integration_tests PRIVATE poni_engine GTest::gtest GTest::gmock GTest::gtest_main)
gtest_discover_tests(poni_integration_tests PROPERTIES LABELS integration FIXTURES_REQUIRED prepared_database)

# AI tests (architecture section 10): unit tests for every mechanical check
# and the offline adversarial suite. No database, no network, no model: CI
# runs them on every build, and an attack that reaches auto reply fails it.
add_executable(poni_ai_tests
  ai/adversarial_suite_test.cpp
  ai/checks_test.cpp
  ai/go_live_domain_test.cpp
  ai/policy_test.cpp
  ai/prompts_test.cpp
  ai/mainland_china_golden_test.cpp
  ai/reply_settings_golden_test.cpp
  ai/text_test.cpp
)
target_include_directories(poni_ai_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(poni_ai_tests PRIVATE
  PONI_AI_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ai/fixtures"
  PONI_KNOWLEDGE_DIR="${PONI_KNOWLEDGE_DIR}")
target_link_libraries(poni_ai_tests PRIVATE poni_engine GTest::gtest GTest::gmock GTest::gtest_main)
gtest_discover_tests(poni_ai_tests PROPERTIES LABELS "unit;ai")

# The same suite against the real model. Built with the tests, never run by
# ctest: it needs a provider's PONI_LLM_<NAME>_BASE_URL and _API_KEY and calls the LLM
# (engine/scripts/linux.sh ai-live, or the "AI live suite" workflow, both started by hand).
add_executable(poni_ai_live ai/live_suite.cpp)
target_include_directories(poni_ai_live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ai)
target_compile_definitions(poni_ai_live PRIVATE
  PONI_AI_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ai/fixtures"
  PONI_KNOWLEDGE_DIR="${PONI_KNOWLEDGE_DIR}")
target_link_libraries(poni_ai_live PRIVATE poni_engine)
