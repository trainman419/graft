/*
 * Test that covariances are properly converted from Euler to Quaternion
 * space
 *
 *
 * Author: Austin Hendrix
 */

#include <cmath>

#include <gtest/gtest.h>

#include <graft/GraftUKFAbsolute.h>

TEST(QuaternionCov, Sample1) {
   double euler_cov[] = { 3.04617e-06, 3.04617e-06, 3.04617e-06 };
   double q[] = {
      0, 0, -0.976108, 0.217286,
      0, 0, -0.973991, 0.226585,
      0, 0, -0.972267, 0.233875,
      0, 0, -0.972485, 0.232967,
      0, 0, -0.974954, 0.222406,
      0, 0, -0.975206, 0.221301,
      0, 0, -0.973991, 0.226585,
      0, 0, -0.970829, 0.239773,
      0, 0, -0.976827, 0.214032,
      0, 0, -0.974483, 0.22446,
      0, 0, -0.973249, 0.229753,
      0, 0, -0.973747, 0.227632,
      0, 0, -0.970452, 0.241296,
   };
   for( int i=0; i<13; i++ ) {
      int j = i*4;
      Matrix<double, 4, 1> result = quaternionCovFromEuler(
         euler_cov[0], euler_cov[1], euler_cov[2],
         q[j], q[j+1], q[j+2], q[j+3]);
      ASSERT_TRUE(std::isfinite(result(0)));
      ASSERT_TRUE(std::isfinite(result(1)));
      ASSERT_TRUE(std::isfinite(result(2)));
      ASSERT_TRUE(std::isfinite(result(3)));
   }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
