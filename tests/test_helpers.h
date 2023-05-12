#pragma once

#include "gtest/gtest.h"

static ::testing::AssertionResult DataMatches(const uint8_t* actual, const uint8_t* expected, size_t expected_length) {
  for (size_t i = 0; i < expected_length; i++){
    if (expected[i] != actual[i]) {
      std::stringstream stream;
      stream << "actual[" << i
        << "] (0x" << std::hex << std::setw(2) << std::setfill('0') << int(actual[i]) << ") != expected[" << i
        << "] (0x" << std::hex << std::setw(2) << std::setfill('0') << int(expected[i]) << ")";
      return ::testing::AssertionFailure() << stream.str();
    }
  }

  return ::testing::AssertionSuccess();
}
