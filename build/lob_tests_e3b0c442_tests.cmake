add_test([=[SetupTest.TypesCompile]=]  /Users/shubhj_777/quant-lob-sim/build/lob_tests [==[--gtest_filter=SetupTest.TypesCompile]==] --gtest_also_run_disabled_tests)
set_tests_properties([=[SetupTest.TypesCompile]=]
  PROPERTIES
    
    _CMAKE_TEST_BUILD_DEPENDS [==[/Users/shubhj_777/quant-lob-sim/build/lob_tests]==]
    DEF_SOURCE_LINE [==[/Users/shubhj_777/quant-lob-sim/tests/test_order_book.cpp:5]==]
    WORKING_DIRECTORY [==[/Users/shubhj_777/quant-lob-sim/build]==]
    SKIP_REGULAR_EXPRESSION [==[\[  SKIPPED \]]==]
    
)
set(lob_tests_TESTS [==[SetupTest.TypesCompile]==])
