#pragma once

#include "gtest/gtest.h"

#include <iomanip>

#define ASSERT_DATA_MATCHES(ACTUAL, EXPECTED, LENGTH) \
  ASSERT_PRED_FORMAT3(DataMatches, ACTUAL, EXPECTED, LENGTH)

#define ASSERT_DATA_VALUE(ACTUAL, VALUE, LENGTH) \
  ASSERT_PRED_FORMAT3(DataValue, ACTUAL, VALUE, LENGTH)

#define ASSERT_DATA_NOT_VALUE(ACTUAL, NOT_VALUE, LENGTH) \
  ASSERT_PRED_FORMAT3(DataNotValue, ACTUAL, NOT_VALUE, LENGTH)

static ::testing::AssertionResult DataMatches(
  const char* exp1, const char* exp2, const char* exp3, const uint8_t* actual, const uint8_t* expected, size_t length
) {
  for (size_t i = 0; i < length; i++){
    if (actual[i] != expected[i]) {
      std::stringstream stream;
      stream << std::hex << std::setw(2) << std::setfill('0')
        << "[0x" << i << "]: 0x" << int(actual[i]) << " (actual) != 0x" << int(expected[i]) << " (expected)";
      return ::testing::AssertionFailure() << stream.str();
    }
  }

  return ::testing::AssertionSuccess();
}

static ::testing::AssertionResult DataValue(
  const char* exp1, const char* exp2, const char* exp3, const uint8_t* actual, uint8_t value, size_t length
) {
  for (size_t i = 0; i < length; i++){
    if (actual[i] != value) {
      std::stringstream stream;
      stream << std::hex << std::setw(2) << std::setfill('0')
        << "[0x" << i << "]: 0x" << int(actual[i]) << " (actual) != 0x" << int(value) << " (expected)";
      return ::testing::AssertionFailure() << stream.str();
    }
  }

  return ::testing::AssertionSuccess();
}

static ::testing::AssertionResult DataNotValue(
  const char* exp1, const char* exp2, const char* exp3, const uint8_t* actual, uint8_t not_value, size_t length
) {
  for (size_t i = 0; i < length; i++){
    if (actual[i] == not_value) {
      std::stringstream stream;
      stream << std::hex << std::setw(2) << std::setfill('0')
        << "[0x" << i << "]: 0x" << int(actual[i]) << " (actual) == 0x" << int(not_value) << " (not expected)";
      return ::testing::AssertionFailure() << stream.str();
    }
  }

  return ::testing::AssertionSuccess();
}
