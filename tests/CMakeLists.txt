add_executable(split_tests
  main.cpp
  settings_test.cpp
  viewport_test.cpp
)

target_link_libraries(split_tests PRIVATE split)
add_test(NAME split_tests COMMAND split_tests)
