/**
 * @file tests/unit/test_process_environment.cpp
 *
 * The environment handed to a launched app. These exist because the frame
 * limiter's variables reached the child only partially: MANGOHUD=1 arrived while
 * MANGOHUD_CONFIG and LD_PRELOAD did not, which looked like the limiter failing
 * when it had in fact set all three correctly.
 */
#include <gtest/gtest.h>

#include <src/boost_process_shim.h>

namespace bp = boost_process_shim;

#include <algorithm>
#include <string>
#include <vector>

namespace {

  std::vector<std::string> strings_of(const bp::environment &env) {
    const auto owned = env.to_environment_strings();
    return {owned.begin(), owned.end()};
  }

  bool contains(const std::vector<std::string> &haystack, const std::string &needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
  }

  TEST(ProcessEnvironment, RendersEveryVariableAsNameEqualsValue) {
    bp::environment env;
    env["MANGOHUD"] = "1";
    env["MANGOHUD_CONFIG"] = "fps_limit=120,no_display";
    env["LD_PRELOAD"] = "/usr/lib/mangohud/libMangoHud_shim.so";

    const auto rendered = strings_of(env);
    EXPECT_TRUE(contains(rendered, "MANGOHUD=1"));
    // A value containing '=' and ',' must survive intact: this is the exact
    // string that carries the frame limiter's cap.
    EXPECT_TRUE(contains(rendered, "MANGOHUD_CONFIG=fps_limit=120,no_display"));
    EXPECT_TRUE(contains(rendered, "LD_PRELOAD=/usr/lib/mangohud/libMangoHud_shim.so"));
  }

  TEST(ProcessEnvironment, ReturnsOwningStringsRatherThanBorrowedPointers) {
    // boost::process::v2::process_environment stores bare c_str() pointers when
    // its argument's elements convert to cstring_ref, which std::string does, so
    // it never copies them. The conversion must therefore hand back storage the
    // caller can keep alive; returning a ready-made process_environment built
    // from a local vector left the child reading freed memory, and which
    // variables survived depended on what happened to reuse it.
    static_assert(
      std::is_same_v<decltype(std::declval<bp::environment>().to_environment_strings()), std::vector<bp::environment::environment_string_t>>,
      "to_environment_strings() must return owned strings"
    );

    std::vector<std::string> rendered;
    {
      bp::environment env;
      env["KEEP_ME"] = "value-that-must-outlive-the-environment";
      rendered = strings_of(env);
    }
    EXPECT_TRUE(contains(rendered, "KEEP_ME=value-that-must-outlive-the-environment"));
  }

}  // namespace
