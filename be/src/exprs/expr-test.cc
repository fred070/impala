// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string>
#include <math.h>
#include <gtest/gtest.h>
#include <boost/assign/list_of.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/unordered_map.hpp>

#include "common/object-pool.h"
#include "runtime/raw-value.h"
#include "runtime/primitive-type.h"
#include "runtime/string-value.h"
#include "gen-cpp/Exprs_types.h"
#include "exprs/bool-literal.h"
#include "exprs/float-literal.h"
#include "exprs/function-call.h"
#include "exprs/int-literal.h"
#include "exprs/is-null-predicate.h"
#include "exprs/like-predicate.h"
#include "exprs/literal-predicate.h"
#include "exprs/null-literal.h"
#include "exprs/string-literal.h"
#include "codegen/llvm-codegen.h"
#include "util/cpu-info.h"
#include "util/disk-info.h"
#include "util/debug-util.h"
#include "util/jni-util.h"
#include "util/string-parser.h"
#include "util/thrift-server.h"
#include "util/thrift-client.h"
#include "util/thrift-util.h"
#include "testutil/test-exec-env.h"
#include "testutil/impalad-query-executor.h"
#include "service/impala-server.h"
#include "service/fe-support.h"
#include "gen-cpp/hive_metastore_types.h"

DECLARE_int32(be_port);
DECLARE_int32(beeswax_port);
DECLARE_string(impalad);

using namespace impala;
using namespace llvm;
using namespace std;
using namespace boost;
using namespace boost::assign;
using namespace Apache::Hadoop::Hive;

namespace impala {
ImpaladQueryExecutor* executor_;
TestExecEnv* test_env_;
ThriftServer* beeswax_server_;
ThriftServer* be_server_;

class ExprTest : public testing::Test {
 protected:
  // Maps from enum value of primitive integer type to
  // the minimum value that is outside of the next smaller-resolution type.
  // For example the value for type TYPE_SMALLINT is numeric_limits<int8_t>::max()+1.
  unordered_map<int, int64_t> min_int_values_;

  // Maps from primitive float type to smallest positive value that is larger
  // than the largest value of the next smaller-resolution type.
  unordered_map<int, double> min_float_values_;

  // Maps from enum value of primitive type to
  // a string representation of a default value for testing.
  // For int and float types the strings represent
  // the corresponding min values (in the maps above).
  // For non-numeric types the default values are listed below.
  unordered_map<int, string> default_type_strs_;
  string default_bool_str_;
  string default_string_str_;
  string default_timestamp_str_;
  // Corresponding default values.
  bool default_bool_val_;
  string default_string_val_;
  TimestampValue default_timestamp_val_;

  // This is used to hold the return values from the query executor.
  ExprValue expr_value_;

  virtual void SetUp() {

    min_int_values_[TYPE_TINYINT] = 1;
    min_int_values_[TYPE_SMALLINT] =
        static_cast<int64_t>(numeric_limits<int8_t>::max()) + 1;
    min_int_values_[TYPE_INT] = static_cast<int64_t>(numeric_limits<int16_t>::max()) + 1;
    min_int_values_[TYPE_BIGINT] =
        static_cast<int64_t>(numeric_limits<int32_t>::max()) + 1;

    min_float_values_[TYPE_FLOAT] = 1.1;
    min_float_values_[TYPE_DOUBLE] =
        static_cast<double>(numeric_limits<float>::max()) + 1.1;

    // Set up default test types, values, and strings.
    default_bool_str_ = "false";
    default_string_str_ = "'abc'";
    default_timestamp_str_ = "cast('2011-01-01 09:01:01' as timestamp)";
    default_bool_val_ = false;
    default_string_val_ = "abc";
    default_timestamp_val_ = TimestampValue(1293872461);
    default_type_strs_[TYPE_TINYINT] =
        lexical_cast<string>(min_int_values_[TYPE_TINYINT]);
    default_type_strs_[TYPE_SMALLINT] =
        lexical_cast<string>(min_int_values_[TYPE_SMALLINT]);
    default_type_strs_[TYPE_INT] =
        lexical_cast<string>(min_int_values_[TYPE_INT]);
    default_type_strs_[TYPE_BIGINT] =
        lexical_cast<string>(min_int_values_[TYPE_BIGINT]);
    // Don't use lexical case here because it results
    // in a string 1.1000000000000001 that messes up the tests.
    default_type_strs_[TYPE_FLOAT] =
        lexical_cast<string>(min_float_values_[TYPE_FLOAT]);
    default_type_strs_[TYPE_DOUBLE] =
        lexical_cast<string>(min_float_values_[TYPE_DOUBLE]);
    default_type_strs_[TYPE_BOOLEAN] = default_bool_str_;
    default_type_strs_[TYPE_STRING] = default_string_str_;
    default_type_strs_[TYPE_TIMESTAMP] = default_timestamp_str_;
  }

  void GetValue(const string& expr, PrimitiveType expr_type, void** interpreted_value) {
    string stmt = "select " + expr;
    vector<FieldSchema> result_types;
    Status status = executor_->Exec(stmt, &result_types);
    ASSERT_TRUE(status.ok()) << "stmt: " << stmt << "\nerror: " << status.GetErrorMsg();
    string result_row;
    ASSERT_TRUE(executor_->FetchResult(&result_row).ok());
    EXPECT_EQ(TypeToOdbcString(expr_type), result_types[0].type);
    *interpreted_value = ConvertValue(expr_type, result_row);
  }

  void* ConvertValue(PrimitiveType type, const string& value) {
    StringParser::ParseResult result;
    if (value.compare("NULL") == 0) return NULL;
    switch (type) {
      case TYPE_STRING:
        // Float and double get conversion errors so leave them as strings.
        // We convert the expected result to string.
      case TYPE_FLOAT:
      case TYPE_DOUBLE:
        expr_value_.string_data = value;
        expr_value_.SyncStringVal();
        return &expr_value_.string_val;
      case TYPE_TINYINT:
        expr_value_.tinyint_val =
            StringParser::StringToInt<int8_t>(&value[0], value.size(), &result);
        return &expr_value_.tinyint_val;
      case TYPE_SMALLINT:
        expr_value_.smallint_val =
            StringParser::StringToInt<int16_t>(&value[0], value.size(), &result);
        return &expr_value_.smallint_val;
      case TYPE_INT:
        expr_value_.int_val =
            StringParser::StringToInt<int32_t>(&value[0], value.size(), &result);
        return &expr_value_.int_val;
      case TYPE_BIGINT:
        expr_value_.bigint_val =
            StringParser::StringToInt<int64_t>(&value[0], value.size(), &result);
        return &expr_value_.bigint_val;
      case TYPE_BOOLEAN:
        expr_value_.bool_val = value.compare("false");
        return &expr_value_.bool_val;
      case TYPE_TIMESTAMP:
        expr_value_.timestamp_val = TimestampValue(&value[0], value.size());
        return &expr_value_.timestamp_val;
      default:
        DCHECK(type);
    }
    return NULL;

  }

  void TestStringValue(const string& expr, const string& expected_result) {
    StringValue* result;
    GetValue(expr, TYPE_STRING, reinterpret_cast<void**>(&result));
    string tmp(result->ptr, result->len);
    EXPECT_EQ(tmp, expected_result) << expr;
  }

  // We can't put this into TestValue() because GTest can't resolve
  // the ambiguity in TimestampValue::operator==, even with the appropriate casts.
  void TestTimestampValue(const string& expr, const TimestampValue& expected_result) {
    TimestampValue* result;
    GetValue(expr, TYPE_TIMESTAMP, reinterpret_cast<void**>(&result));
    EXPECT_EQ(*result, expected_result);
  }

  // Tests whether the returned TimestampValue is valid.
  // We use this function for tests where the expected value is unknown, e.g., now().
  void TestValidTimestampValue(const string& expr) {
    TimestampValue* result;
    GetValue(expr, TYPE_TIMESTAMP, reinterpret_cast<void**>(&result));
    EXPECT_FALSE(result->NotADateTime());
  }

  template <class T> void TestValue(const string& expr, PrimitiveType expr_type,
                                    const T& expected_result) {
    void* result;
    GetValue(expr, expr_type, &result);

    string expected_str;
    switch (expr_type) {
      case TYPE_BOOLEAN:
        EXPECT_EQ(*reinterpret_cast<bool*>(result), expected_result) << expr;
        break;
      case TYPE_TINYINT:
        EXPECT_EQ(*reinterpret_cast<int8_t*>(result), expected_result) << expr;
        break;
      case TYPE_SMALLINT:
        EXPECT_EQ(*reinterpret_cast<int16_t*>(result), expected_result) << expr;
        break;
      case TYPE_INT:
        EXPECT_EQ(*reinterpret_cast<int32_t*>(result), expected_result) << expr;
        break;
      case TYPE_BIGINT:
        EXPECT_EQ(*reinterpret_cast<int64_t*>(result), expected_result) << expr;
        break;
      case TYPE_FLOAT: {
        // Converting the float back from a string is inaccurate so convert
        // the expected result to a string.
        RawValue::PrintValue(reinterpret_cast<const void*>(&expected_result),
                             TYPE_FLOAT, &expected_str);
        EXPECT_EQ(*reinterpret_cast<string*>(result), expected_str) << expr;
        break;
      }
      case TYPE_DOUBLE: {
        RawValue::PrintValue(reinterpret_cast<const void*>(&expected_result),
                             TYPE_DOUBLE, &expected_str);
        EXPECT_EQ(*reinterpret_cast<string*>(result), expected_str) << expr;
        break;
      }
      default:
        ASSERT_TRUE(false) << "invalid TestValue() type: " << TypeToString(expr_type);
    }
  }

  void TestIsNull(const string& expr, PrimitiveType expr_type) {
    void* result;
    GetValue(expr, expr_type, &result);
    EXPECT_TRUE(result == NULL);
  }

  void TestNonOkStatus(const string& expr) {
    string stmt = "select " + expr;
    vector<FieldSchema> result_types;
    Status status = executor_->Exec(stmt, &result_types);
    ASSERT_FALSE(status.ok()) << "stmt: " << stmt << "\nunexpected Status::OK.";
  }

  template <typename T> void TestFixedPointComparisons(bool test_boundaries) {
    int64_t t_min = numeric_limits<T>::min();
    int64_t t_max = numeric_limits<T>::max();
    TestComparison(lexical_cast<string>(t_min), lexical_cast<string>(t_max), true);
    TestEqual(lexical_cast<string>(t_min), true);
    TestEqual(lexical_cast<string>(t_max), true);
    if (test_boundaries) {
      // this requires a cast of the second operand to a higher-resolution type
      TestComparison(lexical_cast<string>(t_min - 1),
                   lexical_cast<string>(t_max), true);
      // this requires a cast of the first operand to a higher-resolution type
      TestComparison(lexical_cast<string>(t_min),
                   lexical_cast<string>(t_max + 1), true);
    }
  }

  template <typename T> void TestFloatingPointComparisons(bool test_boundaries) {
    // t_min is the smallest positive value
    T t_min = numeric_limits<T>::min();
    T t_max = numeric_limits<T>::max();
    TestComparison(lexical_cast<string>(t_min), lexical_cast<string>(t_max), true);
    TestComparison(lexical_cast<string>(-1.0 * t_max), lexical_cast<string>(t_max), true);
    TestEqual(lexical_cast<string>(t_min), true);
    TestEqual(lexical_cast<string>(t_max), true);
    if (test_boundaries) {
      // this requires a cast of the second operand to a higher-resolution type
      TestComparison(lexical_cast<string>(numeric_limits<T>::min() - 1),
                   lexical_cast<string>(numeric_limits<T>::max()), true);
      // this requires a cast of the first operand to a higher-resolution type
      TestComparison(lexical_cast<string>(numeric_limits<T>::min()),
                   lexical_cast<string>(numeric_limits<T>::max() + 1), true);
    }
  }

  void TestStringComparisons() {
    TestValue<bool>("'abc' = 'abc'", TYPE_BOOLEAN, true);
    TestValue<bool>("'abc' = 'abcd'", TYPE_BOOLEAN, false);
    TestValue<bool>("'abc' != 'abcd'", TYPE_BOOLEAN, true);
    TestValue<bool>("'abc' != 'abc'", TYPE_BOOLEAN, false);
    TestValue<bool>("'abc' < 'abcd'", TYPE_BOOLEAN, true);
    TestValue<bool>("'abcd' < 'abc'", TYPE_BOOLEAN, false);
    TestValue<bool>("'abcd' < 'abcd'", TYPE_BOOLEAN, false);
    TestValue<bool>("'abc' > 'abcd'", TYPE_BOOLEAN, false);
    TestValue<bool>("'abcd' > 'abc'", TYPE_BOOLEAN, true);
    TestValue<bool>("'abcd' > 'abcd'", TYPE_BOOLEAN, false);
    TestValue<bool>("'abc' <= 'abcd'", TYPE_BOOLEAN, true);
    TestValue<bool>("'abcd' <= 'abc'", TYPE_BOOLEAN, false);
    TestValue<bool>("'abcd' <= 'abcd'", TYPE_BOOLEAN, true);
    TestValue<bool>("'abc' >= 'abcd'", TYPE_BOOLEAN, false);
    TestValue<bool>("'abcd' >= 'abc'", TYPE_BOOLEAN, true);
    TestValue<bool>("'abcd' >= 'abcd'", TYPE_BOOLEAN, true);

    // Test some empty strings
    TestValue<bool>("'abcd' >= ''", TYPE_BOOLEAN, true);
    TestValue<bool>("'' > ''", TYPE_BOOLEAN, false);
    TestValue<bool>("'' = ''", TYPE_BOOLEAN, true);
  }

  // Generate all possible tests for combinations of <smaller> <op> <larger>.
  // Also test conversions from strings.
  void TestComparison(const string& smaller, const string& larger, bool compare_strings) {
    // disabled for now, because our implicit casts from strings are broken
    // and might return analysis errors when they shouldn't
    // TODO: fix and re-enable tests
    compare_strings = false;
    string eq_pred = smaller + " = " + larger;
    TestValue(eq_pred, TYPE_BOOLEAN, false);
    if (compare_strings) {
      eq_pred = smaller + " = '" + larger + "'";
      TestValue(eq_pred, TYPE_BOOLEAN, false);
    }
    string ne_pred = smaller + " != " + larger;
    TestValue(ne_pred, TYPE_BOOLEAN, true);
    if (compare_strings) {
      ne_pred = smaller + " != '" + larger + "'";
      TestValue(ne_pred, TYPE_BOOLEAN, true);
    }
    string ne2_pred = smaller + " <> " + larger;
    TestValue(ne2_pred, TYPE_BOOLEAN, true);
    if (compare_strings) {
      ne2_pred = smaller + " <> '" + larger + "'";
      TestValue(ne2_pred, TYPE_BOOLEAN, true);
    }
    string lt_pred = smaller + " < " + larger;
    TestValue(lt_pred, TYPE_BOOLEAN, true);
    if (compare_strings) {
      lt_pred = smaller + " < '" + larger + "'";
      TestValue(lt_pred, TYPE_BOOLEAN, true);
    }
    string le_pred = smaller + " <= " + larger;
    TestValue(le_pred, TYPE_BOOLEAN, true);
    if (compare_strings) {
      le_pred = smaller + " <= '" + larger + "'";
      TestValue(le_pred, TYPE_BOOLEAN, true);
    }
    string gt_pred = smaller + " > " + larger;
    TestValue(gt_pred, TYPE_BOOLEAN, false);
    if (compare_strings) {
      gt_pred = smaller + " > '" + larger + "'";
      TestValue(gt_pred, TYPE_BOOLEAN, false);
    }
    string ge_pred = smaller + " >= " + larger;
    TestValue(ge_pred, TYPE_BOOLEAN, false);
    if (compare_strings) {
      ge_pred = smaller + " >= '" + larger + "'";
      TestValue(ge_pred, TYPE_BOOLEAN, false);
    }
  }

  // Generate all possible tests for combinations of <value> <op> <value>
  // Also test conversions from strings.
  void TestEqual(const string& value, bool compare_strings) {
    // disabled for now, because our implicit casts from strings are broken
    // and might return analysis errors when they shouldn't
    // TODO: fix and re-enable tests
    compare_strings = false;
    string eq_pred = value + " = " + value;
    TestValue(eq_pred, TYPE_BOOLEAN, true);
    if (compare_strings) {
      eq_pred = value + " = '" + value + "'";
      TestValue(eq_pred, TYPE_BOOLEAN, true);
    }
    string ne_pred = value + " != " + value;
    TestValue(ne_pred, TYPE_BOOLEAN, false);
    if (compare_strings)  {
      ne_pred = value + " != '" + value + "'";
      TestValue(ne_pred, TYPE_BOOLEAN, false);
    }
    string ne2_pred = value + " <> " + value;
    TestValue(ne2_pred, TYPE_BOOLEAN, false);
    if (compare_strings)  {
      ne2_pred = value + " <> '" + value + "'";
      TestValue(ne2_pred, TYPE_BOOLEAN, false);
    }
    string lt_pred = value + " < " + value;
    TestValue(lt_pred, TYPE_BOOLEAN, false);
    if (compare_strings)  {
      lt_pred = value + " < '" + value + "'";
      TestValue(lt_pred, TYPE_BOOLEAN, false);
    }
    string le_pred = value + " <= " + value;
    TestValue(le_pred, TYPE_BOOLEAN, true);
    if (compare_strings)  {
      le_pred = value + " <= '" + value + "'";
      TestValue(le_pred, TYPE_BOOLEAN, true);
    }
    string gt_pred = value + " > " + value;
    TestValue(gt_pred, TYPE_BOOLEAN, false);
    if (compare_strings)  {
      gt_pred = value + " > '" + value + "'";
      TestValue(gt_pred, TYPE_BOOLEAN, false);
    }
    string ge_pred = value + " >= " + value;
    TestValue(ge_pred, TYPE_BOOLEAN, true);
    if (compare_strings)  {
      ge_pred = value + " >= '" + value + "'";
      TestValue(ge_pred, TYPE_BOOLEAN, true);
    }
  }

  template <typename T> void TestFixedPointLimits(PrimitiveType type) {
    // cast to non-char type first, otherwise we might end up interpreting
    // the min/max as an ascii value
    // We need to add 1 to t_min because there are no negative integer literals.
    // The reason is that whether a minus belongs to an
    // arithmetic expr or a literal must be decided by the parser, not the lexer.
    int64_t t_min = numeric_limits<T>::min() + 1;
    int64_t t_max = numeric_limits<T>::max();
    TestValue(lexical_cast<string>(t_min), type, numeric_limits<T>::min() + 1);
    TestValue(lexical_cast<string>(t_max), type, numeric_limits<T>::max());
  }

  template <typename T> void TestFloatingPointLimits(PrimitiveType type) {
    // numeric_limits<>::min() is the smallest positive value
    TestValue(lexical_cast<string>(numeric_limits<T>::min()), type,
              numeric_limits<T>::min());
    TestValue(lexical_cast<string>(-1.0 * numeric_limits<T>::min()), type,
              -1.0 * numeric_limits<T>::min());
    TestValue(lexical_cast<string>(-1.0 * numeric_limits<T>::max()), type,
              -1.0 * numeric_limits<T>::max());
    TestValue(lexical_cast<string>(numeric_limits<T>::max() - 1.0), type,
              numeric_limits<T>::max());
  }

  // Test ops that that always promote to a fixed type (e.g., max resolution type):
  // ADD, SUBTRACT, MULTIPLY, DIVIDE.
  // Note that adding the " " when generating the expression is not just cosmetic.
  // We have "--" as a comment element in our lexer,
  // so subtraction of a negative value will be ignored without " ".
  template <typename LeftOp, typename RightOp, typename Result>
  void TestFixedResultTypeOps(LeftOp a, RightOp b, PrimitiveType expected_type) {
    Result cast_a = static_cast<Result>(a);
    Result cast_b = static_cast<Result>(b);
    string a_str = lexical_cast<string>(cast_a);
    string b_str = lexical_cast<string>(cast_b);
    TestValue(a_str + " + " + b_str, expected_type, cast_a + cast_b);
    TestValue(a_str + " - " + b_str, expected_type, cast_a - cast_b);
    TestValue(a_str + " * " + b_str, expected_type, cast_a * cast_b);
    TestValue(a_str + " / " + b_str, TYPE_DOUBLE,
        static_cast<double>(a) / static_cast<double>(b));
  }

  // Test int ops that promote to assignment compatible type: BITAND, BITOR, BITXOR,
  // BITNOT, INT_DIVIDE, MOD.
  // As a convention we use RightOp should denote the higher resolution type.
  template <typename LeftOp, typename RightOp>
  void TestVariableResultTypeIntOps(LeftOp a, RightOp b, PrimitiveType expected_type) {
    RightOp cast_a = static_cast<RightOp>(a);
    RightOp cast_b = static_cast<RightOp>(b);
    string a_str = lexical_cast<string>(static_cast<int64_t>(a));
    string b_str = lexical_cast<string>(static_cast<int64_t>(b));
    TestValue(a_str + " & " + b_str, expected_type, cast_a & cast_b);
    TestValue(a_str + " | " + b_str, expected_type, cast_a | cast_b);
    TestValue(a_str + " ^ " + b_str, expected_type, cast_a ^ cast_b);
    // Exclusively use b of type RightOp for unary op BITNOT.
    TestValue("~" + b_str, expected_type, ~cast_b);
    TestValue(a_str + " DIV " + b_str, expected_type, cast_a / cast_b);
    TestValue(a_str + " % " + b_str, expected_type, cast_a % cast_b);
  }

  // Test casting stmt to all types.  Expected result is val.
  template<typename T>
  void TestCast(const string& stmt, T val) {
    TestValue("cast(" + stmt + " as boolean)",
        TYPE_BOOLEAN, static_cast<bool>(val));
    TestValue("cast(" + stmt + " as tinyint)",
        TYPE_TINYINT, static_cast<int8_t>(val));
    TestValue("cast(" + stmt + " as smallint)",
        TYPE_SMALLINT, static_cast<int16_t>(val));
    TestValue("cast(" + stmt + " as int)",
        TYPE_INT, static_cast<int32_t>(val));
    TestValue("cast(" + stmt + " as bigint)",
        TYPE_BIGINT, static_cast<int64_t>(val));
    TestValue("cast(" + stmt + " as float)",
        TYPE_FLOAT, static_cast<float>(val));
    TestValue("cast(" + stmt + " as double)",
        TYPE_DOUBLE, static_cast<double>(val));
    TestStringValue("cast(" + stmt + " as string)",
        lexical_cast<string>(val));
  }
};

// TODO: Remove this specialization once the parser supports
// int literals with value numeric_limits<int64_t>::min().
// Currently the parser can handle negative int literals up to
// numeric_limits<int64_t>::min()+1.
template <>
void ExprTest::TestFixedPointComparisons<int64_t>(bool test_boundaries) {
  int64_t t_min = numeric_limits<int64_t>::min() + 1;
  int64_t t_max = numeric_limits<int64_t>::max();
  TestComparison(lexical_cast<string>(t_min), lexical_cast<string>(t_max), true);
  TestEqual(lexical_cast<string>(t_min), true);
  TestEqual(lexical_cast<string>(t_max), true);
  if (test_boundaries) {
    // this requires a cast of the second operand to a higher-resolution type
    TestComparison(lexical_cast<string>(t_min - 1),
                 lexical_cast<string>(t_max), true);
    // this requires a cast of the first operand to a higher-resolution type
    TestComparison(lexical_cast<string>(t_min),
                 lexical_cast<string>(t_max + 1), true);
  }
}

// Test casting 'stmt' to each of the native types.  The result should be 'val'
// 'stmt' is a partial stmt that could be of any valid type.
template<>
void ExprTest::TestCast(const string& stmt, const char* val) {
  try {
    int8_t val8 = static_cast<int8_t>(lexical_cast<int16_t>(val));
#if 0
    // Hive has weird semantics.  What do we want to do?
    TestValue(stmt + " as boolean)", TYPE_BOOLEAN, lexical_cast<bool>(val));
#endif
    TestValue("cast(" + stmt + " as tinyint)", TYPE_TINYINT, val8);
    TestValue("cast(" + stmt + " as smallint)", TYPE_SMALLINT,
        lexical_cast<int16_t>(val));
    TestValue("cast(" + stmt + " as int)", TYPE_INT,
        lexical_cast<int32_t>(val));
    TestValue("cast(" + stmt + " as bigint)", TYPE_BIGINT,
        lexical_cast<int64_t>(val));
    TestValue("cast(" + stmt + " as float)", TYPE_FLOAT,
        lexical_cast<float>(val));
    TestValue("cast(" + stmt + " as double)", TYPE_DOUBLE,
        lexical_cast<double>(val));
  } catch (bad_lexical_cast& e) {
    EXPECT_TRUE(false) << e.what();
  }
}

void TestSingleLiteralConstruction(PrimitiveType type, void* value, const string& string_val) {
  ObjectPool pool;
  RowDescriptor desc;

  Expr* expr = Expr::CreateLiteral(&pool, type, value);
  EXPECT_TRUE(expr != NULL);
  Expr::Prepare(expr, NULL, desc);
  EXPECT_EQ(RawValue::Compare(expr->GetValue(NULL), value, type), 0);

  expr = Expr::CreateLiteral(&pool, type, string_val);
  EXPECT_TRUE(expr != NULL);
  Expr::Prepare(expr, NULL, desc);
  EXPECT_EQ(RawValue::Compare(expr->GetValue(NULL), value, type), 0);
}

TEST_F(ExprTest, NullLiteral) {
  // TODO: how do we get the planner to use null literal vs literal predicate?
  // For now, just make it manually.  It is used by the partition creation code.
  RuntimeState state;

  for (int type = TYPE_BOOLEAN; type != TYPE_DATE; ++type) {
    NullLiteral expr(static_cast<PrimitiveType>(type));
    Status status = Expr::Prepare(&expr, &state, RowDescriptor());
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(expr.GetValue(NULL) == NULL);
  }
}


TEST_F(ExprTest, LiteralConstruction) {
  bool b_val = true;
  int8_t c_val = 'f';
  int16_t s_val = 123;
  int32_t i_val = 234;
  int64_t l_val = 1234;
  float f_val = 3.14f;
  double d_val = 1.23;
  string str_input = "Hello";
  StringValue str_val(const_cast<char*>(str_input.data()), str_input.length());

  TestSingleLiteralConstruction(TYPE_BOOLEAN, &b_val, "1");
  TestSingleLiteralConstruction(TYPE_TINYINT, &c_val, "f");
  TestSingleLiteralConstruction(TYPE_SMALLINT, &s_val, "123");
  TestSingleLiteralConstruction(TYPE_INT, &i_val, "234");
  TestSingleLiteralConstruction(TYPE_BIGINT, &l_val, "1234");
  TestSingleLiteralConstruction(TYPE_FLOAT, &f_val, "3.14");
  TestSingleLiteralConstruction(TYPE_DOUBLE, &d_val, "1.23");
  TestSingleLiteralConstruction(TYPE_STRING, &str_val, "Hello");

  // Min/Max Boundary value test for tiny/small/int/long
  c_val = 127;
  const char c_array_max[] = {(const char)127}; // avoid implicit casting
  string c_input_max(c_array_max, 1);
  s_val = 32767;
  i_val = 2147483647;
  l_val = 9223372036854775807l;
  TestSingleLiteralConstruction(TYPE_TINYINT, &c_val, c_input_max);
  TestSingleLiteralConstruction(TYPE_SMALLINT, &s_val, "32767");
  TestSingleLiteralConstruction(TYPE_INT, &i_val, "2147483647");
  TestSingleLiteralConstruction(TYPE_BIGINT, &l_val, "9223372036854775807");

  const char c_array_min[] = {(const char)(-128)}; // avoid implicit casting
  string c_input_min(c_array_min, 1);
  c_val = -128;
  s_val = -32768;
  i_val = -2147483648;
  l_val = -9223372036854775807l-1;
  TestSingleLiteralConstruction(TYPE_TINYINT, &c_val, c_input_min);
  TestSingleLiteralConstruction(TYPE_SMALLINT, &s_val, "-32768");
  TestSingleLiteralConstruction(TYPE_INT, &i_val, "-2147483648");
  TestSingleLiteralConstruction(TYPE_BIGINT, &l_val, "-9223372036854775808");
}


TEST_F(ExprTest, LiteralExprs) {
  TestFixedPointLimits<int8_t>(TYPE_TINYINT);
  TestFixedPointLimits<int16_t>(TYPE_SMALLINT);
  TestFixedPointLimits<int32_t>(TYPE_INT);
  TestFixedPointLimits<int64_t>(TYPE_BIGINT);
  // The value is not an exact FLOAT so it gets compared as a DOUBLE
  // and fails.  This needs to be researched.
  // TestFloatingPointLimits<float>(TYPE_FLOAT);
  TestFloatingPointLimits<double>(TYPE_DOUBLE);

  TestValue("true", TYPE_BOOLEAN, true);
  TestValue("false", TYPE_BOOLEAN, false);
  TestStringValue("'test'", "test");
  TestIsNull("null", TYPE_BOOLEAN);
}

TEST_F(ExprTest, ArithmeticExprs) {
  // Test float ops.
  TestFixedResultTypeOps<float, float, double>(min_float_values_[TYPE_FLOAT],
      min_float_values_[TYPE_FLOAT], TYPE_DOUBLE);
  TestFixedResultTypeOps<float, double, double>(min_float_values_[TYPE_FLOAT],
      min_float_values_[TYPE_DOUBLE], TYPE_DOUBLE);
  TestFixedResultTypeOps<double, double, double>(min_float_values_[TYPE_DOUBLE],
      min_float_values_[TYPE_DOUBLE], TYPE_DOUBLE);

  // Test behavior of float ops at max/min value boundaries.
  // The tests with float type should trivially pass, since their results are double.
  TestFixedResultTypeOps<float, float, double>(numeric_limits<float>::min(),
      numeric_limits<float>::min(), TYPE_DOUBLE);
  TestFixedResultTypeOps<float, float, double>(numeric_limits<float>::max(),
      numeric_limits<float>::max(), TYPE_DOUBLE);
  TestFixedResultTypeOps<float, float, double>(numeric_limits<float>::min(),
      numeric_limits<float>::max(), TYPE_DOUBLE);
  TestFixedResultTypeOps<float, float, double>(numeric_limits<float>::max(),
      numeric_limits<float>::min(), TYPE_DOUBLE);
  TestFixedResultTypeOps<double, double, double>(numeric_limits<double>::min(),
      numeric_limits<double>::min(), TYPE_DOUBLE);
  TestFixedResultTypeOps<double, double, double>(numeric_limits<double>::max(),
      numeric_limits<double>::max(), TYPE_DOUBLE);
  TestFixedResultTypeOps<double, double, double>(numeric_limits<double>::min(),
      numeric_limits<double>::max(), TYPE_DOUBLE);
  TestFixedResultTypeOps<double, double, double>(numeric_limits<double>::max(),
      numeric_limits<double>::min(), TYPE_DOUBLE);

  // Test behavior with zero (especially for division by zero).
  TestFixedResultTypeOps<float, float, double>(min_float_values_[TYPE_FLOAT],
      0.0f, TYPE_DOUBLE);
  TestFixedResultTypeOps<double, double, double>(min_float_values_[TYPE_DOUBLE],
      0.0, TYPE_DOUBLE);

  // Test ops that always promote to fixed type (e.g., max resolution type).
  TestFixedResultTypeOps<int8_t, int8_t, int64_t>(min_int_values_[TYPE_TINYINT],
      min_int_values_[TYPE_TINYINT], TYPE_BIGINT);
  TestFixedResultTypeOps<int8_t, int16_t, int64_t>(min_int_values_[TYPE_TINYINT],
      min_int_values_[TYPE_SMALLINT], TYPE_BIGINT);
  TestFixedResultTypeOps<int8_t, int32_t, int64_t>(min_int_values_[TYPE_TINYINT],
      min_int_values_[TYPE_INT], TYPE_BIGINT);
  TestFixedResultTypeOps<int8_t, int64_t, int64_t>(min_int_values_[TYPE_TINYINT],
      min_int_values_[TYPE_BIGINT], TYPE_BIGINT);
  TestFixedResultTypeOps<int16_t, int16_t, int64_t>(min_int_values_[TYPE_SMALLINT],
      min_int_values_[TYPE_SMALLINT], TYPE_BIGINT);
  TestFixedResultTypeOps<int16_t, int32_t, int64_t>(min_int_values_[TYPE_SMALLINT],
      min_int_values_[TYPE_INT], TYPE_BIGINT);
  TestFixedResultTypeOps<int16_t, int64_t, int64_t>(min_int_values_[TYPE_SMALLINT],
      min_int_values_[TYPE_BIGINT], TYPE_BIGINT);
  TestFixedResultTypeOps<int32_t, int32_t, int64_t>(min_int_values_[TYPE_INT],
      min_int_values_[TYPE_INT], TYPE_BIGINT);
  TestFixedResultTypeOps<int32_t, int64_t, int64_t>(min_int_values_[TYPE_INT],
      min_int_values_[TYPE_BIGINT], TYPE_BIGINT);
  TestFixedResultTypeOps<int64_t, int64_t, int64_t>(min_int_values_[TYPE_BIGINT],
      min_int_values_[TYPE_BIGINT], TYPE_BIGINT);

  // Test behavior on overflow/underflow.
  TestFixedResultTypeOps<int64_t, int64_t, int64_t>(numeric_limits<int64_t>::min()+1,
      numeric_limits<int64_t>::min()+1, TYPE_BIGINT);
  TestFixedResultTypeOps<int64_t, int64_t, int64_t>(numeric_limits<int64_t>::max(),
      numeric_limits<int64_t>::max(), TYPE_BIGINT);
  TestFixedResultTypeOps<int64_t, int64_t, int64_t>(numeric_limits<int64_t>::min()+1,
      numeric_limits<int64_t>::max(), TYPE_BIGINT);
  TestFixedResultTypeOps<int64_t, int64_t, int64_t>(numeric_limits<int64_t>::max(),
      numeric_limits<int64_t>::min()+1, TYPE_BIGINT);

  // Test int ops that promote to assignment compatible type.
  TestVariableResultTypeIntOps<int8_t, int8_t>(min_int_values_[TYPE_TINYINT],
      min_int_values_[TYPE_TINYINT], TYPE_TINYINT);
  TestVariableResultTypeIntOps<int8_t, int16_t>(min_int_values_[TYPE_TINYINT],
      min_int_values_[TYPE_SMALLINT], TYPE_SMALLINT);
  TestVariableResultTypeIntOps<int8_t, int32_t>(min_int_values_[TYPE_TINYINT],
      min_int_values_[TYPE_INT], TYPE_INT);
  TestVariableResultTypeIntOps<int8_t, int64_t>(min_int_values_[TYPE_TINYINT],
      min_int_values_[TYPE_BIGINT], TYPE_BIGINT);
  TestVariableResultTypeIntOps<int16_t, int16_t>(min_int_values_[TYPE_SMALLINT],
      min_int_values_[TYPE_SMALLINT], TYPE_SMALLINT);
  TestVariableResultTypeIntOps<int16_t, int32_t>(min_int_values_[TYPE_SMALLINT],
      min_int_values_[TYPE_INT],TYPE_INT);
  TestVariableResultTypeIntOps<int16_t, int64_t>(min_int_values_[TYPE_SMALLINT],
      min_int_values_[TYPE_BIGINT], TYPE_BIGINT);
  TestVariableResultTypeIntOps<int32_t, int32_t>(min_int_values_[TYPE_INT],
      min_int_values_[TYPE_INT], TYPE_INT);
  TestVariableResultTypeIntOps<int32_t, int64_t>(min_int_values_[TYPE_INT],
      min_int_values_[TYPE_BIGINT], TYPE_BIGINT);
  TestVariableResultTypeIntOps<int64_t, int64_t>(min_int_values_[TYPE_BIGINT],
      min_int_values_[TYPE_BIGINT], TYPE_BIGINT);

  // Tests for dealing with '-'.
  TestValue("-1", TYPE_TINYINT, -1);
  TestValue("1 - 1", TYPE_BIGINT, 0);
  TestValue("1 - - 1", TYPE_BIGINT, 2);
  TestValue("1 - - - 1", TYPE_BIGINT, 0);
  TestValue("- 1 - 1", TYPE_BIGINT, -2);
  TestValue("- 1 - - 1", TYPE_BIGINT, 0);
  // The "--" indicates a comment to be ignored.
  // Therefore, the result should be -1.
  TestValue("- 1 --1", TYPE_TINYINT, -1);
}

// There are two tests of ranges, the second of which requires a cast
// of the second operand to a higher-resolution type.
TEST_F(ExprTest, BinaryPredicates) {
  TestComparison("false", "true", false);
  TestEqual("false", false);
  TestEqual("true", false);
  TestFixedPointComparisons<int8_t>(true);
  TestFixedPointComparisons<int16_t>(true);
  TestFixedPointComparisons<int32_t>(true);
  TestFixedPointComparisons<int64_t>(false);
  TestFloatingPointComparisons<float>(true);
  TestFloatingPointComparisons<double>(false);
  TestStringComparisons();
}

// Test casting from all types to all other types
TEST_F(ExprTest, CastExprs) {
  // From tinyint
  TestCast("cast(0 as tinyint)", 0);
  TestCast("cast(5 as tinyint)", 5);
  TestCast("cast(-5 as tinyint)", -5);

  // From smallint
  TestCast("cast(0 as smallint)", 0);
  TestCast("cast(5 as smallint)", 5);
  TestCast("cast(-5 as smallint)", -5);

  // From int
  TestCast("cast(0 as int)", 0);
  TestCast("cast(5 as int)", 5);
  TestCast("cast(-5 as int)", -5);

  // From bigint
  TestCast("cast(0 as bigint)", 0);
  TestCast("cast(5 as bigint)", 5);
  TestCast("cast(-5 as bigint)", -5);

  // From boolean
  TestCast("cast(0 as boolean)", 0);
  TestCast("cast(5 as boolean)", 1);
  TestCast("cast(-5 as boolean)", 1);

  // From Float
  TestCast("cast(0.0 as float)", 0.0f);
  TestCast("cast(5.0 as float)", 5.0f);
  TestCast("cast(-5.0 as float)", -5.0f);

  // From Double
  TestCast("cast(0.0 as double)", 0.0);
  TestCast("cast(5.0 as double)", 5.0);
  TestCast("cast(-5.0 as double)", -5.0);

  // From String
  TestCast("'0'", "0");
  TestCast("'5'", "5");
  TestCast("'-5'", "-5");

#if 0
  // Test overflow.  TODO: Hive casting rules are very weird here also.  It seems for
  // types < TYPE_INT, it will just overflow and keep the low order bits.  For TYPE_INT,
  // it caps it and int_max/int_min.  Is this what we want to do?
  int val = 10000000;
  TestValue<int8_t>("cast(10000000 as tinyint)", TYPE_TINYINT, val & 0xff);
  TestValue<int8_t>("cast(-10000000 as tinyint)", TYPE_TINYINT, -val & 0xff);
  TestValue<int16_t>("cast(10000000 as smallint)", TYPE_SMALLINT, val & 0xffff);
  TestValue<int16_t>("cast(-10000000 as smallint)", TYPE_SMALLINT, -val & 0xffff);
#endif
}

TEST_F(ExprTest, CompoundPredicates) {
  TestValue("TRUE AND TRUE", TYPE_BOOLEAN, true);
  TestValue("TRUE AND FALSE", TYPE_BOOLEAN, false);
  TestValue("FALSE AND TRUE", TYPE_BOOLEAN, false);
  TestValue("FALSE AND FALSE", TYPE_BOOLEAN, false);
  TestValue("TRUE && TRUE", TYPE_BOOLEAN, true);
  TestValue("TRUE && FALSE", TYPE_BOOLEAN, false);
  TestValue("FALSE && TRUE", TYPE_BOOLEAN, false);
  TestValue("FALSE && FALSE", TYPE_BOOLEAN, false);
  TestValue("TRUE OR TRUE", TYPE_BOOLEAN, true);
  TestValue("TRUE OR FALSE", TYPE_BOOLEAN, true);
  TestValue("FALSE OR TRUE", TYPE_BOOLEAN, true);
  TestValue("FALSE OR FALSE", TYPE_BOOLEAN, false);
  TestValue("TRUE || TRUE", TYPE_BOOLEAN, true);
  TestValue("TRUE || FALSE", TYPE_BOOLEAN, true);
  TestValue("FALSE || TRUE", TYPE_BOOLEAN, true);
  TestValue("FALSE || FALSE", TYPE_BOOLEAN, false);
  TestValue("NOT TRUE", TYPE_BOOLEAN, false);
  TestValue("NOT FALSE", TYPE_BOOLEAN, true);
  TestValue("!TRUE", TYPE_BOOLEAN, false);
  TestValue("!FALSE", TYPE_BOOLEAN, true);
  TestValue("TRUE AND (TRUE OR FALSE)", TYPE_BOOLEAN, true);
  TestValue("(TRUE AND TRUE) OR FALSE", TYPE_BOOLEAN, true);
  TestValue("(TRUE OR FALSE) AND TRUE", TYPE_BOOLEAN, true);
  TestValue("TRUE OR (FALSE AND TRUE)", TYPE_BOOLEAN, true);
  TestValue("TRUE AND TRUE OR FALSE", TYPE_BOOLEAN, true);
  TestValue("TRUE && (TRUE || FALSE)", TYPE_BOOLEAN, true);
  TestValue("(TRUE && TRUE) || FALSE", TYPE_BOOLEAN, true);
  TestValue("(TRUE || FALSE) && TRUE", TYPE_BOOLEAN, true);
  TestValue("TRUE || (FALSE && TRUE)", TYPE_BOOLEAN, true);
  TestValue("TRUE && TRUE || FALSE", TYPE_BOOLEAN, true);
  TestIsNull("TRUE AND NULL", TYPE_BOOLEAN);
  TestValue("FALSE AND NULL", TYPE_BOOLEAN, false);
  TestValue("TRUE OR NULL", TYPE_BOOLEAN, true);
  TestIsNull("FALSE OR NULL", TYPE_BOOLEAN);
  TestIsNull("NOT NULL", TYPE_BOOLEAN);
  TestIsNull("TRUE && NULL", TYPE_BOOLEAN);
  TestValue("FALSE && NULL", TYPE_BOOLEAN, false);
  TestValue("TRUE || NULL", TYPE_BOOLEAN, true);
  TestIsNull("FALSE || NULL", TYPE_BOOLEAN);
  TestIsNull("!NULL", TYPE_BOOLEAN);
}

TEST_F(ExprTest, IsNullPredicate) {
  TestValue("5 IS NULL", TYPE_BOOLEAN, false);
  TestValue("5 IS NOT NULL", TYPE_BOOLEAN, true);
}

TEST_F(ExprTest, LikePredicate) {
  TestValue("'a' LIKE '%a%'", TYPE_BOOLEAN, true);
  TestValue("'a' LIKE '%abcde'", TYPE_BOOLEAN, false);
  TestValue("'a' LIKE 'abcde%'", TYPE_BOOLEAN, false);
  TestValue("'abcde' LIKE 'abcde%'", TYPE_BOOLEAN, true);
  TestValue("'abcde' LIKE '%abcde'", TYPE_BOOLEAN, true);
  TestValue("'abcde' LIKE '%abcde%'", TYPE_BOOLEAN, true);
  // IMP-117
  TestValue("'ab' LIKE '%a'", TYPE_BOOLEAN, false);
  TestValue("'ab' NOT LIKE '%a'", TYPE_BOOLEAN, true);
  // IMP-117
  TestValue("'ba' LIKE 'a%'", TYPE_BOOLEAN, false);
  TestValue("'a' LIKE '_'", TYPE_BOOLEAN, true);
  TestValue("'a' NOT LIKE '_'", TYPE_BOOLEAN, false);
  TestValue("'a' LIKE 'a'", TYPE_BOOLEAN, true);
  TestValue("'a' LIKE 'b'", TYPE_BOOLEAN, false);
  TestValue("'a' NOT LIKE 'a'", TYPE_BOOLEAN, false);
  TestValue("'a' NOT LIKE 'b'", TYPE_BOOLEAN, true);
  // IMP-117 -- initial part of pattern appears earlier in string.
  TestValue("'LARGE BRUSHED BRASS' LIKE '%BRASS'", TYPE_BOOLEAN, true);
  TestValue("'BRASS LARGE BRUSHED' LIKE '%BRASS'", TYPE_BOOLEAN, false);
  TestValue("'BRASS LARGE BRUSHED' LIKE 'BRUSHED%'", TYPE_BOOLEAN, false);
  TestValue("'BRASS LARGE BRUSHED' LIKE 'BRASS%'", TYPE_BOOLEAN, true);
  TestValue("'prefix1234' LIKE 'prefix%'", TYPE_BOOLEAN, true);
  TestValue("'1234suffix' LIKE '%suffix'", TYPE_BOOLEAN, true);
  TestValue("'1234substr5678' LIKE '%substr%'", TYPE_BOOLEAN, true);
  TestValue("'a%a' LIKE 'a\\%a'", TYPE_BOOLEAN, true);
  TestValue("'a123a' LIKE 'a\\%a'", TYPE_BOOLEAN, false);
  TestValue("'a_a' LIKE 'a\\_a'", TYPE_BOOLEAN, true);
  TestValue("'a1a' LIKE 'a\\_a'", TYPE_BOOLEAN, false);
  TestValue("'abla' LIKE 'a%a'", TYPE_BOOLEAN, true);
  TestValue("'ablb' LIKE 'a%a'", TYPE_BOOLEAN, false);
  TestValue("'abxcy1234a' LIKE 'a_x_y%a'", TYPE_BOOLEAN, true);
  TestValue("'axcy1234a' LIKE 'a_x_y%a'", TYPE_BOOLEAN, false);
  TestValue("'abxcy1234a' REGEXP 'a.x.y.*a'", TYPE_BOOLEAN, true);
  TestValue("'a.x.y.*a' REGEXP 'a\\.x\\.y\\.\\*a'", TYPE_BOOLEAN, true);
  TestValue("'abxcy1234a' REGEXP 'a\\.x\\.y\\.\\*a'", TYPE_BOOLEAN, false);
  TestValue("'abxcy1234a' RLIKE 'a.x.y.*a'", TYPE_BOOLEAN, true);
  TestValue("'axcy1234a' REGEXP 'a.x.y.*a'", TYPE_BOOLEAN, false);
  TestValue("'axcy1234a' RLIKE 'a.x.y.*a'", TYPE_BOOLEAN, false);
  // regex escape chars; insert special character in the middle to prevent
  // it from being matched as a substring
  TestValue("'.[]{}()x\\*+?|^$' LIKE '.[]{}()_\\\\*+?|^$'", TYPE_BOOLEAN, true);
  // escaped _ matches single _
  TestValue("'\\_' LIKE '\\_'", TYPE_BOOLEAN, false);
  TestValue("'_' LIKE '\\_'", TYPE_BOOLEAN, true);
  TestValue("'a' LIKE '\\_'", TYPE_BOOLEAN, false);
  // escaped escape char
  TestValue("'\\a' LIKE '\\\\_'", TYPE_BOOLEAN, true);
  TestValue("'_' LIKE '\\\\_'", TYPE_BOOLEAN, false);
  // make sure the 3rd \ counts toward the _
  TestValue("'\\_' LIKE '\\\\\\_'", TYPE_BOOLEAN, true);
  TestValue("'\\\\a' LIKE '\\\\\\_'", TYPE_BOOLEAN, false);
  // Test invalid patterns, unmatched parenthesis.
  TestNonOkStatus("'a' RLIKE '(./'");
  TestNonOkStatus("'a' REGEXP '(./'");
  // Pattern is converted for LIKE, and should not throw.
  TestValue("'a' LIKE '(./'", TYPE_BOOLEAN, false);
}

TEST_F(ExprTest, BetweenPredicate) {
  // Between is rewritten into a conjunctive compound predicate.
  // Compound predicates are also tested elsewere, so we just do basic testing here.
  TestValue("5 between 0 and 10", TYPE_BOOLEAN, true);
  TestValue("5 between 5 and 5", TYPE_BOOLEAN, true);
  TestValue("5 between 6 and 10", TYPE_BOOLEAN, false);
  TestValue("5 not between 0 and 10", TYPE_BOOLEAN, false);
  TestValue("5 not between 5 and 5", TYPE_BOOLEAN, false);
  TestValue("5 not between 6 and 10", TYPE_BOOLEAN, true);
  // Test operator precedence.
  TestValue("5+1 between 4 and 10", TYPE_BOOLEAN, true);
  TestValue("5+1 not between 4 and 10", TYPE_BOOLEAN, false);
  // Test TimestampValues.
  TestValue("cast('2011-10-22 09:10:11' as timestamp) between "
      "cast('2011-09-22 09:10:11' as timestamp) and "
      "cast('2011-12-22 09:10:11' as timestamp)", TYPE_BOOLEAN, true);
  TestValue("cast('2011-10-22 09:10:11' as timestamp) between "
        "cast('2011-11-22 09:10:11' as timestamp) and "
        "cast('2011-12-22 09:10:11' as timestamp)", TYPE_BOOLEAN, false);
  TestValue("cast('2011-10-22 09:10:11' as timestamp) not between "
      "cast('2011-09-22 09:10:11' as timestamp) and "
      "cast('2011-12-22 09:10:11' as timestamp)", TYPE_BOOLEAN, false);
  TestValue("cast('2011-10-22 09:10:11' as timestamp) not between "
      "cast('2011-11-22 09:10:11' as timestamp) and "
      "cast('2011-12-22 09:10:11' as timestamp)", TYPE_BOOLEAN, true);
  // Test strings.
  TestValue("'abc' between 'a' and 'z'", TYPE_BOOLEAN, true);
  TestValue("'abc' between 'aaa' and 'aab'", TYPE_BOOLEAN, false);
  TestValue("'abc' not between 'a' and 'z'", TYPE_BOOLEAN, false);
  TestValue("'abc' not between 'aaa' and 'aab'", TYPE_BOOLEAN, true);
}

// Tests with NULLs are in the FE QueryTest.
TEST_F(ExprTest, InPredicate) {
  // Test integers.
  unordered_map<int, int64_t>::iterator int_iter;
  for(int_iter = min_int_values_.begin(); int_iter != min_int_values_.end();
      ++int_iter) {
    string& val = default_type_strs_[int_iter->first];
    TestValue(val + " in (2, 3, " + val + ")", TYPE_BOOLEAN, true);
    TestValue(val + " in (2, 3, 4)", TYPE_BOOLEAN, false);
    TestValue(val + " not in (2, 3, " + val + ")", TYPE_BOOLEAN, false);
    TestValue(val + " not in (2, 3, 4)", TYPE_BOOLEAN, true);
  }

  // Test floats.
  unordered_map<int, double>::iterator float_iter;
  for(float_iter = min_float_values_.begin(); float_iter != min_float_values_.end();
      ++float_iter) {
    string& val = default_type_strs_[float_iter->first];
    TestValue(val + " in (2, 3, " + val + ")", TYPE_BOOLEAN, true);
    TestValue(val + " in (2, 3, 4)", TYPE_BOOLEAN, false);
    TestValue(val + " not in (2, 3, " + val + ")", TYPE_BOOLEAN, false);
    TestValue(val + " not in (2, 3, 4)", TYPE_BOOLEAN, true);
  }

  // Test bools.
  TestValue("true in (true, false, false)", TYPE_BOOLEAN, true);
  TestValue("true in (false, false, false)", TYPE_BOOLEAN, false);
  TestValue("true not in (true, false, false)", TYPE_BOOLEAN, false);
  TestValue("true not in (false, false, false)", TYPE_BOOLEAN, true);

  // Test strings.
  TestValue("'ab' in ('ab', 'cd', 'efg')", TYPE_BOOLEAN, true);
  TestValue("'ab' in ('cd', 'efg', 'h')", TYPE_BOOLEAN, false);
  TestValue("'ab' not in ('ab', 'cd', 'efg')", TYPE_BOOLEAN, false);
  TestValue("'ab' not in ('cd', 'efg', 'h')", TYPE_BOOLEAN, true);

  // Test timestamps.
  TestValue(default_timestamp_str_ + " "
      "in (cast('2011-11-23 09:10:11' as timestamp), "
      "cast('2011-11-24 09:11:12' as timestamp), " +
      default_timestamp_str_ + ")", TYPE_BOOLEAN, true);
  TestValue(default_timestamp_str_ + " "
        "in (cast('2011-11-22 09:10:11' as timestamp), "
        "cast('2011-11-23 09:11:12' as timestamp), "
        "cast('2011-11-24 09:12:13' as timestamp))", TYPE_BOOLEAN, false);
  TestValue(default_timestamp_str_ + " "
      "not in (cast('2011-11-22 09:10:11' as timestamp), "
      "cast('2011-11-23 09:11:12' as timestamp), " +
      default_timestamp_str_ + ")", TYPE_BOOLEAN, false);
  TestValue(default_timestamp_str_ + " "
      "not in (cast('2011-11-22 09:10:11' as timestamp), "
      "cast('2011-11-23 09:11:12' as timestamp), "
      "cast('2011-11-24 09:12:13' as timestamp))", TYPE_BOOLEAN, true);

  // Test operator precedence.
  TestValue("5+1 in (3, 6, 10)", TYPE_BOOLEAN, true);
  TestValue("5+1 not in (3, 6, 10)", TYPE_BOOLEAN, false);
}

TEST_F(ExprTest, StringFunctions) {
  TestStringValue("substring('Hello', 1)", "Hello");
  TestStringValue("substring('Hello', -2)", "lo");
  TestStringValue("substring('Hello', 0)", "");
  TestStringValue("substring('Hello', -5)", "Hello");
  TestStringValue("substring('Hello', -6)", "");
  TestStringValue("substring('Hello', 100)", "");
  TestStringValue("substring('Hello', 1, 1)", "H");
  TestStringValue("substring('Hello', 2, 100)", "ello");
  TestStringValue("substring('Hello', -3, 2)", "ll");

  TestStringValue("lower('')", "");
  TestStringValue("lower('HELLO')", "hello");
  TestStringValue("lower('Hello')", "hello");
  TestStringValue("lower('hello!')", "hello!");
  TestStringValue("lcase('HELLO')", "hello");

  TestStringValue("upper('')", "");
  TestStringValue("upper('HELLO')", "HELLO");
  TestStringValue("upper('Hello')", "HELLO");
  TestStringValue("upper('hello!')", "HELLO!");
  TestStringValue("ucase('hello')", "HELLO");

  TestValue("length('')", TYPE_INT, 0);
  TestValue("length('a')", TYPE_INT, 1);
  TestValue("length('abcdefg')", TYPE_INT, 7);

  TestStringValue("reverse('abcdefg')", "gfedcba");
  TestStringValue("reverse('')", "");
  TestStringValue("strleft('abcdefg', 3)", "abc");
  TestStringValue("strleft('abcdefg', 10)", "abcdefg");
  TestStringValue("strright('abcdefg', 3)", "efg");
  TestStringValue("strright('abcdefg', 10)", "abcdefg");

  TestStringValue("trim('')", "");
  TestStringValue("trim('      ')", "");
  TestStringValue("trim('   abcdefg   ')", "abcdefg");
  TestStringValue("trim('abcdefg   ')", "abcdefg");
  TestStringValue("trim('   abcdefg')", "abcdefg");
  TestStringValue("trim('abc  defg')", "abc  defg");
  TestStringValue("ltrim('')", "");
  TestStringValue("ltrim('      ')", "");
  TestStringValue("ltrim('   abcdefg   ')", "abcdefg   ");
  TestStringValue("ltrim('abcdefg   ')", "abcdefg   ");
  TestStringValue("ltrim('   abcdefg')", "abcdefg");
  TestStringValue("ltrim('abc  defg')", "abc  defg");
  TestStringValue("rtrim('')", "");
  TestStringValue("rtrim('      ')", "");
  TestStringValue("rtrim('   abcdefg   ')", "   abcdefg");
  TestStringValue("rtrim('abcdefg   ')", "abcdefg");
  TestStringValue("rtrim('   abcdefg')", "   abcdefg");
  TestStringValue("rtrim('abc  defg')", "abc  defg");

  TestStringValue("space(0)", "");
  TestStringValue("space(-1)", "");
  TestStringValue("space(1)", " ");
  TestStringValue("space(6)", "      ");

  TestStringValue("repeat('', 0)", "");
  TestStringValue("repeat('', 6)", "");
  TestStringValue("repeat('ab', 0)", "");
  TestStringValue("repeat('ab', -1)", "");
  TestStringValue("repeat('ab', 1)", "ab");
  TestStringValue("repeat('ab', 6)", "abababababab");

  TestValue("ascii('')", TYPE_INT, 0);
  TestValue("ascii('abcde')", TYPE_INT, 'a');
  TestValue("ascii('Abcde')", TYPE_INT, 'A');
  TestValue("ascii('dddd')", TYPE_INT, 'd');
  TestValue("ascii(' ')", TYPE_INT, ' ');

  TestStringValue("lpad('', 0, '')", "");
  TestStringValue("lpad('abc', 0, '')", "");
  TestStringValue("lpad('abc', 3, '')", "abc");
  TestStringValue("lpad('abc', 2, 'xyz')", "ab");
  TestStringValue("lpad('abc', 6, 'xyz')", "xyzabc");
  TestStringValue("lpad('abc', 5, 'xyz')", "xyabc");
  TestStringValue("lpad('abc', 10, 'xyz')", "xyzxyzxabc");
  TestStringValue("rpad('', 0, '')", "");
  TestStringValue("rpad('abc', 0, '')", "");
  TestStringValue("rpad('abc', 3, '')", "abc");
  TestStringValue("rpad('abc', 2, 'xyz')", "ab");
  TestStringValue("rpad('abc', 6, 'xyz')", "abcxyz");
  TestStringValue("rpad('abc', 5, 'xyz')", "abcxy");
  TestStringValue("rpad('abc', 10, 'xyz')", "abcxyzxyzx");

  // Note that Hive returns positions starting from 1.
  // Hive returns 0 if substr was not found in str (or on other error coditions).
  TestValue("instr('', '')", TYPE_INT, 0);
  TestValue("instr('', 'abc')", TYPE_INT, 0);
  TestValue("instr('abc', '')", TYPE_INT, 0);
  TestValue("instr('abc', 'abc')", TYPE_INT, 1);
  TestValue("instr('xyzabc', 'abc')", TYPE_INT, 4);
  TestValue("instr('xyzabcxyz', 'bcx')", TYPE_INT, 5);
  TestValue("locate('', '')", TYPE_INT, 0);
  TestValue("locate('abc', '')", TYPE_INT, 0);
  TestValue("locate('', 'abc')", TYPE_INT, 0);
  TestValue("locate('abc', 'abc')", TYPE_INT, 1);
  TestValue("locate('abc', 'xyzabc')", TYPE_INT, 4);
  TestValue("locate('bcx', 'xyzabcxyz')", TYPE_INT, 5);
  // Test locate with starting pos param.
  // Note that Hive expects positions starting from 1 as input.
  TestValue("locate('', '', 0)", TYPE_INT, 0);
  TestValue("locate('abc', '', 0)", TYPE_INT, 0);
  TestValue("locate('', 'abc', 0)", TYPE_INT, 0);
  TestValue("locate('', 'abc', -1)", TYPE_INT, 0);
  TestValue("locate('', '', 1)", TYPE_INT, 0);
  TestValue("locate('', 'abcde', 10)", TYPE_INT, 0);
  TestValue("locate('abcde', 'abcde', -1)", TYPE_INT, 0);
  TestValue("locate('abcde', 'abcde', 10)", TYPE_INT, 0);
  TestValue("locate('abc', 'abcdef', 0)", TYPE_INT, 0);
  TestValue("locate('abc', 'abcdef', 1)", TYPE_INT, 1);
  TestValue("locate('abc', 'xyzabcdef', 3)", TYPE_INT, 4);
  TestValue("locate('abc', 'xyzabcdef', 4)", TYPE_INT, 4);
  TestValue("locate('abc', 'abcabcabc', 5)", TYPE_INT, 7);

  TestStringValue("concat('a')", "a");
  TestStringValue("concat('a', 'b')", "ab");
  TestStringValue("concat('a', 'b', 'cde')", "abcde");
  TestStringValue("concat('a', 'b', 'cde', 'fg')", "abcdefg");
  TestStringValue("concat('a', 'b', 'cde', '', 'fg', '')", "abcdefg");

  TestStringValue("concat_ws(',', 'a')", "a");
  TestStringValue("concat_ws(',', 'a', 'b')", "a,b");
  TestStringValue("concat_ws(',', 'a', 'b', 'cde')", "a,b,cde");
  TestStringValue("concat_ws('', 'a', '', 'b', 'cde')", "abcde");
  TestStringValue("concat_ws('%%', 'a', 'b', 'cde', 'fg')", "a%%b%%cde%%fg");
  TestStringValue("concat_ws('|','a', 'b', 'cde', '', 'fg', '')", "a|b|cde||fg|");
  TestStringValue("concat_ws('', '', '', '')", "");

  TestValue("find_in_set('ab', 'ab,ab,ab,ade,cde')", TYPE_INT, 1);
  TestValue("find_in_set('ab', 'abc,xyz,abc,ade,ab')", TYPE_INT, 5);
  TestValue("find_in_set('ab', 'abc,ad,ab,ade,cde')", TYPE_INT, 3);
  TestValue("find_in_set('xyz', 'abc,ad,ab,ade,cde')", TYPE_INT, 0);
  TestValue("find_in_set('ab', ',,,,ab,,,,')", TYPE_INT, 5);
  TestValue("find_in_set('', ',ad,ab,ade,cde')", TYPE_INT,1);
  TestValue("find_in_set('', 'abc,ad,ab,ade,,')", TYPE_INT, 5);
  TestValue("find_in_set('', 'abc,ad,,ade,cde,')", TYPE_INT,3);
  // First param contains comma.
  TestValue("find_in_set('abc,def', 'abc,ad,,ade,cde,')", TYPE_INT, 0);

  TestStringValue("version()", GetVersionString());

  // TODO: tests with NULL arguments, currently we can't parse them
  // inside function calls.
  // e.g.   TestValue("length(NULL)", TYPE_INT, NULL);
}

TEST_F(ExprTest, StringRegexpFunctions) {
  // Single group.
  TestStringValue("regexp_extract('abxcy1234a', 'a.x', 0)", "abx");
  TestStringValue("regexp_extract('abxcy1234a', 'a.x.*a', 0)", "abxcy1234a");
  TestStringValue("regexp_extract('abxcy1234a', 'a.x.y.*a', 0)", "abxcy1234a");
  TestStringValue("regexp_extract('a.x.y.*a', 'a\\.x\\.y\\.\\*a', 0)", "a.x.y.*a");
  TestStringValue("regexp_extract('abxcy1234a', 'abczy', 0)", "");
  TestStringValue("regexp_extract('abxcy1234a', 'a\\.x\\.y\\.\\*a', 0)", "");
  TestStringValue("regexp_extract('axcy1234a', 'a.x.y.*a', 0)","");
  // Accessing non-existant group should return empty string.
  TestStringValue("regexp_extract('abxcy1234a', 'a.x', 2)", "");
  TestStringValue("regexp_extract('abxcy1234a', 'a.x.*a', 1)", "");
  TestStringValue("regexp_extract('abxcy1234a', 'a.x.y.*a', 5)", "");
  // Multiple groups enclosed in ().
  TestStringValue("regexp_extract('abxcy1234a', '(a.x)(.y.*)(3.*a)', 0)", "abxcy1234a");
  TestStringValue("regexp_extract('abxcy1234a', '(a.x)(.y.*)(3.*a)', 1)", "abx");
  TestStringValue("regexp_extract('abxcy1234a', '(a.x)(.y.*)(3.*a)', 2)", "cy12");
  TestStringValue("regexp_extract('abxcy1234a', '(a.x)(.y.*)(3.*a)', 3)", "34a");
  TestStringValue("regexp_extract('abxcy1234a', '(a.x)(.y.*)(3.*a)', 4)", "");
  // Empty strings.
  TestStringValue("regexp_extract('', '', 0)", "");
  TestStringValue("regexp_extract('abxcy1234a', '', 0)", "");
  TestStringValue("regexp_extract('', 'abx', 0)", "");
  // Invalid regex patter, unmatched parenthesis.
  TestIsNull("regexp_extract('abxcy1234a', '(/.', 0)", TYPE_STRING);

  TestStringValue("regexp_replace('axcaycazc', 'a.c', 'a')", "aaa");
  TestStringValue("regexp_replace('axcaycazc', 'a.c', '')", "");
  TestStringValue("regexp_replace('axcaycazc', 'a.*', 'abcde')", "abcde");
  TestStringValue("regexp_replace('axcaycazc', 'a.*y.*z', 'xyz')", "xyzc");
  // No match for pattern.
  TestStringValue("regexp_replace('axcaycazc', 'a.z', 'xyz')", "axcaycazc");
  TestStringValue("regexp_replace('axcaycazc', 'a.*y.z', 'xyz')", "axcaycazc");
  // Empty strings.
  TestStringValue("regexp_replace('', '', '')", "");
  TestStringValue("regexp_replace('axcaycazc', '', '')", "axcaycazc");
  TestStringValue("regexp_replace('', 'err', '')", "");
  TestStringValue("regexp_replace('', '', 'abc')", "abc");
  TestStringValue("regexp_replace('axcaycazc', '', 'r')", "rarxrcraryrcrarzrcr");
  // Invalid regex patter, unmatched parenthesis.
  TestIsNull("regexp_replace('abxcy1234a', '(/.', 'x')", TYPE_STRING);
}

TEST_F(ExprTest, StringParseUrlFunction) {
  // TODO: For now, our parse_url my not behave exactly like Hive
  // when given malformed URLs.
  // If necessary, we can closely follow Java's URL implementation
  // to behave exactly like Hive.

  // AUTHORITY part.
  TestStringValue("parse_url('http://user:pass@example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'AUTHORITY')", "user:pass@example.com:80");
  TestStringValue("parse_url('http://user:pass@example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'AUTHORITY')", "user:pass@example.com");
  // Without user and pass.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'AUTHORITY')", "example.com:80");
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'AUTHORITY')", "example.com");
  // Exactly what Hive returns as well.
  TestStringValue("parse_url('http://example.com_xyzabc^&*', 'AUTHORITY')",
      "example.com_xyzabc^&*");
  // Missing protocol.
  TestIsNull("parse_url('example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'HOST')", TYPE_STRING);

  // FILE part.
  TestStringValue("parse_url('http://user:pass@example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'FILE')",
      "/docs/books/tutorial/index.html?name=networking");
  TestStringValue("parse_url('http://user:pass@example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'FILE')",
      "/docs/books/tutorial/index.html?name=networking");
  // Without user and pass.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'FILE')",
      "/docs/books/tutorial/index.html?name=networking");
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'FILE')",
      "/docs/books/tutorial/index.html?name=networking");
  // With trimming.
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.html?name=networking   ', 'FILE')",
      "/docs/books/tutorial/index.html?name=networking");
  // No question mark but a hash (consistent with Hive).
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.html#something', 'FILE')",
      "/docs/books/tutorial/index.html");
  // No hash or question mark (consistent with Hive).
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.htmlsomething', 'FILE')",
      "/docs/books/tutorial/index.htmlsomething");
  // Missing protocol.
  TestIsNull("parse_url('example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'FILE')", TYPE_STRING);

  // HOST part.
  TestStringValue("parse_url('http://user:pass@example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'HOST')", "example.com");
  TestStringValue("parse_url('http://user:pass@example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'HOST')", "example.com");
  // Without user and pass.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'HOST')", "example.com");
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'HOST')", "example.com");
  // Exactly what Hive returns as well.
  TestStringValue("parse_url('http://example.com_xyzabc^&*', 'HOST')",
      "example.com_xyzabc^&*");
  // Missing protocol.
  TestIsNull("parse_url('example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'HOST')", TYPE_STRING);

  // PATH part.
  TestStringValue("parse_url('http://user:pass@example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PATH')",
      "/docs/books/tutorial/index.html");
  TestStringValue("parse_url('http://user:pass@example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PATH')",
      "/docs/books/tutorial/index.html");
  // Without user and pass.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PATH')",
      "/docs/books/tutorial/index.html");
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PATH')",
      "/docs/books/tutorial/index.html");
  // With trimming.
  TestStringValue("parse_url('http://user:pass@example.com/docs/books/tutorial/"
      "index.html   ', 'PATH')",
      "/docs/books/tutorial/index.html");
  // No question mark but a hash (consistent with Hive).
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.html#something', 'PATH')",
      "/docs/books/tutorial/index.html");
  // No hash or question mark (consistent with Hive).
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.htmlsomething', 'PATH')",
      "/docs/books/tutorial/index.htmlsomething");
  // Missing protocol.
  TestIsNull("parse_url('example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PATH')", TYPE_STRING);

  // PROTOCOL part.
  TestStringValue("parse_url('http://user:pass@example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PROTOCOL')", "http");
  TestStringValue("parse_url('https://user:pass@example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PROTOCOL')", "https");
  // Without user and pass.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PROTOCOL')", "http");
  TestStringValue("parse_url('https://example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PROTOCOL')", "https");
  // With trimming.
  TestStringValue("parse_url('   https://user:pass@example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PROTOCOL')", "https");
  // Missing protocol.
  TestIsNull("parse_url('user:pass@example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PROTOCOL')", TYPE_STRING);

  // QUERY part.
  TestStringValue("parse_url('http://user:pass@example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'QUERY')", "name=networking");
  TestStringValue("parse_url('http://user:pass@example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'QUERY')", "name=networking");
  // Without user and pass.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'QUERY')", "name=networking");
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'QUERY')", "name=networking");
  // With trimming.
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.html?name=networking   ', 'QUERY')", "name=networking");
  // No '?'. Hive also returns NULL.
  TestIsNull("parse_url('http://example.com_xyzabc^&*', 'QUERY')", TYPE_STRING);
  // Missing protocol.
  TestIsNull("parse_url('example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'QUERY')", TYPE_STRING);

  // PATH part.
  TestStringValue("parse_url('http://user:pass@example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PATH')",
      "/docs/books/tutorial/index.html");
  TestStringValue("parse_url('http://user:pass@example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PATH')",
      "/docs/books/tutorial/index.html");
  // Without user and pass.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PATH')",
      "/docs/books/tutorial/index.html");
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PATH')",
      "/docs/books/tutorial/index.html");
  // With trimming.
  TestStringValue("parse_url('http://user:pass@example.com/docs/books/tutorial/"
      "index.html   ', 'PATH')",
      "/docs/books/tutorial/index.html");
  // No question mark but a hash (consistent with Hive).
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.html#something', 'PATH')",
      "/docs/books/tutorial/index.html");
  // No hash or question mark (consistent with Hive).
  TestStringValue("parse_url('http://example.com/docs/books/tutorial/"
      "index.htmlsomething', 'PATH')",
      "/docs/books/tutorial/index.htmlsomething");
  // Missing protocol.
  TestIsNull("parse_url('example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'PATH')", TYPE_STRING);

  // USERINFO part.
  TestStringValue("parse_url('http://user:pass@example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'USERINFO')", "user:pass");
  TestStringValue("parse_url('http://user:pass@example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'USERINFO')", "user:pass");
  // Only user given.
  TestStringValue("parse_url('http://user@example.com/docs/books/tutorial/"
        "index.html?name=networking#DOWNLOADING', 'USERINFO')", "user");
  // No user or pass. Hive also returns NULL.
  TestIsNull("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'USERINFO')", TYPE_STRING);
  // Missing protocol.
  TestIsNull("parse_url('example.com/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'USERINFO')", TYPE_STRING);

  // Invalid part parameters.
  // All characters in the part parameter must be uppercase (conistent with Hive).
  TestIsNull("parse_url('http://example.com', 'authority')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'Authority')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'AUTHORITYXYZ')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'file')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'File')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'FILEXYZ')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'host')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'Host')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'HOSTXYZ')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'path')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'Path')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'PATHXYZ')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'protocol')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'Protocol')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'PROTOCOLXYZ')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'query')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'Query')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'QUERYXYZ')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'ref')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'Ref')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'REFXYZ')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'userinfo')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'Userinfo')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com', 'USERINFOXYZ')", TYPE_STRING);

  // Key's value is terminated by '#'.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking#DOWNLOADING', 'QUERY', 'name')", "networking");
  // Key's value is terminated by end of string.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking', 'QUERY', 'name')", "networking");
  // Key's value is terminated by end of string, with trimming.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking   ', 'QUERY', 'name')", "networking");
  // Key's value is terminated by '&'.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?name=networking&test=true', 'QUERY', 'name')", "networking");
  // Key's value is some query param in the middle.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?test=true&name=networking&op=true', 'QUERY', 'name')", "networking");
  // Key string appears in various parts of the url.
  TestStringValue("parse_url('http://name.name:80/name/books/tutorial/"
      "name.html?name_fake=true&name=networking&op=true#name', 'QUERY', 'name')", "networking");
  // We can still match this even though no '?' was given.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.htmltest=true&name=networking&op=true', 'QUERY', 'name')", "networking");
  // Requested key doesn't exist.
  TestIsNull("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?test=true&name=networking&op=true', 'QUERY', 'error')", TYPE_STRING);
  // Requested key doesn't exist in query part.
  TestIsNull("parse_url('http://example.com:80/docs/books/tutorial/"
      "name.html?test=true&op=true', 'QUERY', 'name')", TYPE_STRING);
  // Requested key doesn't exist in query part, but matches at end of string.
  TestIsNull("parse_url('http://example.com:80/docs/books/tutorial/"
      "name.html?test=true&op=name', 'QUERY', 'name')", TYPE_STRING);
  // Malformed urls with incorrectly positioned '?' or '='.
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?test=true&name=net?working&op=true', 'QUERY', 'name')", "net?working");
  TestStringValue("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?test=true&name=net=working&op=true', 'QUERY', 'name')", "net=working");
  // Key paremeter given without QUERY part.
  TestIsNull("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?test=true&name=networking&op=true', 'AUTHORITY', 'name')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?test=true&name=networking&op=true', 'FILE', 'name')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?test=true&name=networking&op=true', 'PATH', 'name')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?test=true&name=networking&op=true', 'PROTOCOL', 'name')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?test=true&name=networking&op=true', 'REF', 'name')", TYPE_STRING);
  TestIsNull("parse_url('http://example.com:80/docs/books/tutorial/"
      "index.html?test=true&name=networking&op=true', 'XYZ', 'name')", TYPE_STRING);
}

TEST_F(ExprTest, MathTrigonometricFunctions) {
  // It is important to calculate the expected values
  // using math functions, and not simply use constants.
  // Otherwise, floating point imprecisions may lead to failed tests.
  TestValue("sin(0.0)", TYPE_DOUBLE, sin(0.0));
  TestValue("sin(pi())", TYPE_DOUBLE, sin(M_PI));
  TestValue("sin(pi() / 2.0)", TYPE_DOUBLE, sin(M_PI / 2.0));
  TestValue("asin(-1.0)", TYPE_DOUBLE, asin(-1.0));
  TestValue("asin(1.0)", TYPE_DOUBLE, asin(1.0));
  TestValue("cos(0.0)", TYPE_DOUBLE, cos(0.0));
  TestValue("cos(pi())", TYPE_DOUBLE, cos(M_PI));
  TestValue("acos(-1.0)", TYPE_DOUBLE, acos(-1.0));
  TestValue("acos(1.0)", TYPE_DOUBLE, acos(1.0));
  TestValue("tan(pi() * -1.0)", TYPE_DOUBLE, tan(M_PI * -1.0));
  TestValue("tan(pi())", TYPE_DOUBLE, tan(M_PI));
  TestValue("atan(pi())", TYPE_DOUBLE, atan(M_PI));
  TestValue("atan(pi() * - 1.0)", TYPE_DOUBLE, atan(M_PI * -1.0));
  // this gets a very very small number rather than 0.
  // TestValue("radians(0)", TYPE_DOUBLE, 0);
  TestValue("radians(180.0)", TYPE_DOUBLE, M_PI);
  TestValue("degrees(0)", TYPE_DOUBLE, 0.0);
  TestValue("degrees(pi())", TYPE_DOUBLE, 180.0);

  // TODO: tests with NULL arguments, currently we can't parse them inside function calls.
}

TEST_F(ExprTest, MathConversionFunctions) {

  TestStringValue("bin(0)", "0");
  TestStringValue("bin(1)", "1");
  TestStringValue("bin(12)", "1100");
  TestStringValue("bin(1234567)", "100101101011010000111");
  TestStringValue("bin(" + lexical_cast<string>(numeric_limits<int64_t>::max()) + ")",
      "111111111111111111111111111111111111111111111111111111111111111");
  TestStringValue("bin(" + lexical_cast<string>(numeric_limits<int64_t>::min()+1) + ")",
      "1000000000000000000000000000000000000000000000000000000000000001");

  TestStringValue("hex(0)", "0");
  TestStringValue("hex(15)", "F");
  TestStringValue("hex(16)", "10");
  TestStringValue("hex(" + lexical_cast<string>(numeric_limits<int64_t>::max()) + ")",
      "7FFFFFFFFFFFFFFF");
  TestStringValue("hex(" + lexical_cast<string>(numeric_limits<int64_t>::min()+1) + ")",
      "8000000000000001");
  TestStringValue("hex('0')", "30");
  TestStringValue("hex('aAzZ')", "61417A5A");
  TestStringValue("hex('Impala')", "496D70616C61");
  TestStringValue("hex('impalA')", "696D70616C41");
  TestStringValue("unhex('30')", "0");
  TestStringValue("unhex('61417A5A')", "aAzZ");
  TestStringValue("unhex('496D70616C61')", "Impala");
  TestStringValue("unhex('696D70616C41')", "impalA");
  // Character not in hex alphabet results in empty string.
  TestStringValue("unhex('30GA')", "");
  // Uneven number of chars results in empty string.
  TestStringValue("unhex('30A')", "");

  // Run the test suite twice, once with a bigint parameter, and once with string parameters.
  for (int i = 0; i < 2; ++i) {
    // First iteration is with bigint, second with string parameter.
    string q = (i == 0) ? "" : "'";
    // Invalid input: Base below -36 or above 36.
    TestIsNull("conv(" + q + "10" + q + ", 10, 37)", TYPE_STRING);
    TestIsNull("conv(" + q + "10" + q + ", 37, 10)", TYPE_STRING);
    TestIsNull("conv(" + q + "10" + q + ", 10, -37)", TYPE_STRING);
    TestIsNull("conv(" + q + "10" + q + ", -37, 10)", TYPE_STRING);
    // Invalid input: Base between -2 and 2.
    TestIsNull("conv(" + q + "10" + q + ", 10, 1)", TYPE_STRING);
    TestIsNull("conv(" + q + "10" + q + ", 1, 10)", TYPE_STRING);
    TestIsNull("conv(" + q + "10" + q + ", 10, -1)", TYPE_STRING);
    TestIsNull("conv(" + q + "10" + q + ", -1, 10)", TYPE_STRING);
    // Invalid input: Positive number but negative src base.
    TestIsNull("conv(" + q + "10" + q + ", -10, 10)", TYPE_STRING);
    // Test positive numbers.
    TestStringValue("conv(" + q + "10" + q + ", 10, 10)", "10");
    TestStringValue("conv(" + q + "10" + q + ", 2, 10)", "2");
    TestStringValue("conv(" + q + "11" + q + ", 36, 10)", "37");
    TestStringValue("conv(" + q + "11" + q + ", 36, 2)", "100101");
    TestStringValue("conv(" + q + "100101" + q + ", 2, 36)", "11");
    TestStringValue("conv(" + q + "0" + q + ", 10, 2)", "0");
    // Test negative numbers (tests from Hive).
    // If to_base is positive, the number should be handled as a 2's complement (64-bit).
    TestStringValue("conv(" + q + "-641" + q + ", 10, -10)", "-641");
    TestStringValue("conv(" + q + "1011" + q + ", 2, -16)", "B");
    TestStringValue("conv(" + q + "-1" + q + ", 10, 16)", "FFFFFFFFFFFFFFFF");
    TestStringValue("conv(" + q + "-15" + q + ", 10, 16)", "FFFFFFFFFFFFFFF1");
    // Test digits that are not available in srcbase. We expect those digits
    // from left-to-right that can be interpreted in srcbase to form the result
    // (i.e., the paring bails only when it encounters a digit not in srcbase).
    TestStringValue("conv(" + q + "17" + q + ", 7, 10)", "1");
    TestStringValue("conv(" + q + "371" + q + ", 7, 10)", "3");
    TestStringValue("conv(" + q + "371" + q + ", 7, 10)", "3");
    TestStringValue("conv(" + q + "445" + q + ", 5, 10)", "24");
    // Test overflow (tests from Hive).
    // If a number is two large, the result should be -1 (if signed),
    // or MAX_LONG (if unsigned).
    TestStringValue("conv(" + q + lexical_cast<string>(numeric_limits<int64_t>::max())
        + q + ", 36, 16)", "FFFFFFFFFFFFFFFF");
    TestStringValue("conv(" + q + lexical_cast<string>(numeric_limits<int64_t>::max())
        + q + ", 36, -16)", "-1");
    TestStringValue("conv(" + q + lexical_cast<string>(numeric_limits<int64_t>::min()+1)
        + q + ", 36, 16)", "FFFFFFFFFFFFFFFF");
    TestStringValue("conv(" + q + lexical_cast<string>(numeric_limits<int64_t>::min()+1)
        + q + ", 36, -16)", "-1");
  }
  // Test invalid input strings that start with an invalid digit.
  // Hive returns "0" in such cases.
  TestStringValue("conv('@', 16, 10)", "0");
  TestStringValue("conv('$123', 12, 2)", "0");
  TestStringValue("conv('*12g', 32, 5)", "0");

  // TODO: tests with NULL arguments, currently we can't parse them inside function calls.
}

TEST_F(ExprTest, MathFunctions) {
  TestValue("pi()", TYPE_DOUBLE, M_PI);
  TestValue("e()", TYPE_DOUBLE, M_E);
  TestValue("abs(-1.0)", TYPE_DOUBLE, 1.0);
  TestValue("abs(1.0)", TYPE_DOUBLE, 1.0);
  TestValue("sign(0.0)", TYPE_FLOAT, 1.0f);
  TestValue("sign(10.0)", TYPE_FLOAT, 1.0f);
  TestValue("sign(-10.0)", TYPE_FLOAT, -1.0f);

  // It is important to calculate the expected values
  // using math functions, and not simply use constants.
  // Otherwise, floating point imprecisions may lead to failed tests.
  TestValue("exp(2)", TYPE_DOUBLE, exp(2));
  TestValue("exp(e())", TYPE_DOUBLE, exp(M_E));
  TestValue("ln(e())", TYPE_DOUBLE, 1.0);
  TestValue("ln(255.0)", TYPE_DOUBLE, log(255.0));
  TestValue("log10(1000.0)", TYPE_DOUBLE, 3.0);
  TestValue("log10(50.0)", TYPE_DOUBLE, log10(50.0));
  TestValue("log2(64.0)", TYPE_DOUBLE, 6.0);
  TestValue("log2(678.0)", TYPE_DOUBLE, log(678.0) / log(2.0));
  TestValue("log(10.0, 1000.0)", TYPE_DOUBLE, log(1000.0) / log(10.0));
  TestValue("log(2.0, 64.0)", TYPE_DOUBLE, 6.0);
  TestValue("pow(2.0, 10.0)", TYPE_DOUBLE, pow(2.0, 10.0));
  TestValue("pow(e(), 2.0)", TYPE_DOUBLE, M_E * M_E);
  TestValue("power(2.0, 10.0)", TYPE_DOUBLE, pow(2.0, 10.0));
  TestValue("power(e(), 2.0)", TYPE_DOUBLE, M_E * M_E);
  TestValue("sqrt(121.0)", TYPE_DOUBLE, 11.0);
  TestValue("sqrt(2.0)", TYPE_DOUBLE, sqrt(2.0));

  // Run twice to test deterministic behavior.
  uint32_t seed = 0;
  double expected = static_cast<double>(rand_r(&seed)) / static_cast<double>(RAND_MAX);
  TestValue("rand()", TYPE_DOUBLE, expected);
  TestValue("rand()", TYPE_DOUBLE, expected);
  seed = 1234;
  expected = static_cast<double>(rand_r(&seed)) / static_cast<double>(RAND_MAX);
  TestValue("rand(1234)", TYPE_DOUBLE, expected);
  TestValue("rand(1234)", TYPE_DOUBLE, expected);

  // Test bigint param.
  TestValue("pmod(10, 3)", TYPE_BIGINT, 1);
  TestValue("pmod(-10, 3)", TYPE_BIGINT, 2);
  TestValue("pmod(10, -3)", TYPE_BIGINT, -2);
  TestValue("pmod(-10, -3)", TYPE_BIGINT, -1);
  TestValue("pmod(1234567890, 13)", TYPE_BIGINT, 10);
  TestValue("pmod(-1234567890, 13)", TYPE_BIGINT, 3);
  TestValue("pmod(1234567890, -13)", TYPE_BIGINT, -3);
  TestValue("pmod(-1234567890, -13)", TYPE_BIGINT, -10);
  // Test double param.
  TestValue("pmod(12.3, 4.0)", TYPE_DOUBLE, fmod(fmod(12.3, 4.0) + 4.0, 4.0));
  TestValue("pmod(-12.3, 4.0)", TYPE_DOUBLE, fmod(fmod(-12.3, 4.0) + 4.0, 4.0));
  TestValue("pmod(12.3, -4.0)", TYPE_DOUBLE, fmod(fmod(12.3, -4.0) - 4.0, -4.0));
  TestValue("pmod(-12.3, -4.0)", TYPE_DOUBLE, fmod(fmod(-12.3, -4.0) - 4.0, -4.0));
  TestValue("pmod(123456.789, 13.456)", TYPE_DOUBLE,
      fmod(fmod(123456.789, 13.456) + 13.456, 13.456));
  TestValue("pmod(-123456.789, 13.456)", TYPE_DOUBLE,
      fmod(fmod(-123456.789, 13.456) + 13.456, 13.456));
  TestValue("pmod(123456.789, -13.456)", TYPE_DOUBLE,
      fmod(fmod(123456.789, -13.456) - 13.456, -13.456));
  TestValue("pmod(-123456.789, -13.456)", TYPE_DOUBLE,
      fmod(fmod(-123456.789, -13.456) - 13.456, -13.456));

  // Test bigint param.
  TestValue("positive(1234567890)", TYPE_BIGINT, 1234567890);
  TestValue("positive(-1234567890)", TYPE_BIGINT, -1234567890);
  TestValue("negative(1234567890)", TYPE_BIGINT, -1234567890);
  TestValue("negative(-1234567890)", TYPE_BIGINT, 1234567890);
  // Test double param.
  TestValue("positive(3.14159265)", TYPE_DOUBLE, 3.14159265);
  TestValue("positive(-3.14159265)", TYPE_DOUBLE, -3.14159265);
  TestValue("negative(3.14159265)", TYPE_DOUBLE, -3.14159265);
  TestValue("negative(-3.14159265)", TYPE_DOUBLE, 3.14159265);

  // TODO: tests with NULL arguments, currently we can't parse them inside function calls.
}

TEST_F(ExprTest, MathRoundingFunctions) {
  TestValue("ceil(0.1)", TYPE_BIGINT, 1);
  TestValue("ceil(-10.05)", TYPE_BIGINT, -10);
  TestValue("ceiling(0.1)", TYPE_BIGINT, 1);
  TestValue("ceiling(-10.05)", TYPE_BIGINT, -10);
  TestValue("floor(0.1)", TYPE_BIGINT, 0);
  TestValue("floor(-10.007)", TYPE_BIGINT, -11);

  TestValue("round(1.499999)", TYPE_BIGINT, 1);
  TestValue("round(1.5)", TYPE_BIGINT, 2);
  TestValue("round(1.500001)", TYPE_BIGINT, 2);
  TestValue("round(-1.499999)", TYPE_BIGINT, -1);
  TestValue("round(-1.5)", TYPE_BIGINT, -2);
  TestValue("round(-1.500001)", TYPE_BIGINT, -2);

  TestValue("round(3.14159265, 0)", TYPE_DOUBLE, 3.0);
  TestValue("round(3.14159265, 1)", TYPE_DOUBLE, 3.1);
  TestValue("round(3.14159265, 2)", TYPE_DOUBLE, 3.14);
  TestValue("round(3.14159265, 3)", TYPE_DOUBLE, 3.142);
  TestValue("round(3.14159265, 4)", TYPE_DOUBLE, 3.1416);
  TestValue("round(3.14159265, 5)", TYPE_DOUBLE, 3.14159);
  TestValue("round(-3.14159265, 0)", TYPE_DOUBLE, -3.0);
  TestValue("round(-3.14159265, 1)", TYPE_DOUBLE, -3.1);
  TestValue("round(-3.14159265, 2)", TYPE_DOUBLE, -3.14);
  TestValue("round(-3.14159265, 3)", TYPE_DOUBLE, -3.142);
  TestValue("round(-3.14159265, 4)", TYPE_DOUBLE, -3.1416);
  TestValue("round(-3.14159265, 5)", TYPE_DOUBLE, -3.14159);
}

// TODO: I think a lot of these casts are not necessary and we should fix this
TEST_F(ExprTest, TimestampFunctions) {
  TestStringValue("cast(cast('2012-01-01 09:10:11.123456789' as timestamp) as string)",
      "2012-01-01 09:10:11.123456789");
  // Add/sub years.
  TestStringValue("cast(date_add(cast('2012-01-01 09:10:11.123456789' "
      "as timestamp), interval 10 years) as string)",
      "2022-01-01 09:10:11.123456789");
  TestStringValue("cast(date_sub(cast('2012-01-01 09:10:11.123456789' "
      "as timestamp), interval 10 years) as string)",
      "2002-01-01 09:10:11.123456789");
  // Add/sub months.
  TestStringValue("cast(date_add(cast('2012-01-01 09:10:11.123456789' "
      "as timestamp), interval 13 months) as string)",
      "2013-02-01 09:10:11.123456789");
  TestStringValue("cast(date_sub(cast('2013-02-01 09:10:11.123456789' "
      "as timestamp), interval 13 months) as string)",
      "2012-01-01 09:10:11.123456789");
  TestStringValue("cast(date_add(cast('2012-01-31 09:10:11.123456789' "
      "as timestamp), interval 1 month) as string)",
      "2012-02-29 09:10:11.123456789");
  TestStringValue("cast(date_sub(cast('2012-02-29 09:10:11.123456789' "
      "as timestamp), interval 1 month) as string)",
      "2012-01-31 09:10:11.123456789");
  // Add/sub weeks.
  TestStringValue("cast(date_add(cast('2012-01-01 09:10:11.123456789' "
      "as timestamp), interval 2 weeks) as string)",
      "2012-01-15 09:10:11.123456789");
  TestStringValue("cast(date_sub(cast('2012-01-15 09:10:11.123456789' "
      "as timestamp), interval 2 weeks) as string)",
      "2012-01-01 09:10:11.123456789");
  TestStringValue("cast(date_add(cast('2012-01-01 09:10:11.123456789' "
      "as timestamp), interval 53 weeks) as string)",
      "2013-01-06 09:10:11.123456789");
  TestStringValue("cast(date_sub(cast('2013-01-06 09:10:11.123456789' "
      "as timestamp), interval 53 weeks) as string)",
      "2012-01-01 09:10:11.123456789");
  // Add/sub days.
  TestStringValue("cast(date_add(cast('2012-01-01 09:10:11.123456789' "
      "as timestamp), interval 10 days) as string)",
      "2012-01-11 09:10:11.123456789");
  TestStringValue("cast(date_sub(cast('2012-01-01 09:10:11.123456789' "
      "as timestamp), interval 10 days) as string)",
      "2011-12-22 09:10:11.123456789");
  TestStringValue("cast(date_add(cast('2011-12-22 09:10:11.12345678' "
      "as timestamp), interval 10 days) as string)",
      "2012-01-01 09:10:11.123456780");
  TestStringValue("cast(date_sub(cast('2011-12-22 09:10:11.12345678' "
      "as timestamp), interval 365 days) as string)",
      "2010-12-22 09:10:11.123456780");
  // Add/sub days (HIVE's date_add/sub variant).
  TestStringValue("cast(date_add(cast('2012-01-01 09:10:11.123456789' "
      "as timestamp), 10) as string)",
      "2012-01-11 09:10:11.123456789");
  TestStringValue(
      "cast(date_sub(cast('2012-01-01 09:10:11.123456789' as timestamp), 10) as string)",
      "2011-12-22 09:10:11.123456789");
  TestStringValue(
      "cast(date_add(cast('2011-12-22 09:10:11.12345678' as timestamp), 10) as string)",
      "2012-01-01 09:10:11.123456780");
  TestStringValue(
      "cast(date_sub(cast('2011-12-22 09:10:11.12345678' as timestamp), 365) as string)",
      "2010-12-22 09:10:11.123456780");
  // Add/sub hours.
  TestStringValue("cast(date_add(cast('2012-01-01 00:00:00.123456789' "
      "as timestamp), interval 25 hours) as string)",
      "2012-01-02 01:00:00.123456789");
  TestStringValue("cast(date_sub(cast('2012-01-02 01:00:00.123456789' "
      "as timestamp), interval 25 hours) as string)",
      "2012-01-01 00:00:00.123456789");
  // Add/sub minutes.
  TestStringValue("cast(date_add(cast('2012-01-01 00:00:00.123456789' "
      "as timestamp), interval 1533 minutes) as string)",
      "2012-01-02 01:33:00.123456789");
  TestStringValue("cast(date_sub(cast('2012-01-02 01:33:00.123456789' "
      "as timestamp), interval 1533 minutes) as string)",
      "2012-01-01 00:00:00.123456789");
  // Add/sub seconds.
  TestStringValue("cast(date_add(cast('2012-01-01 00:00:00.123456789' "
      "as timestamp), interval 90033 seconds) as string)",
      "2012-01-02 01:00:33.123456789");
  TestStringValue("cast(date_sub(cast('2012-01-02 01:00:33.123456789' "
      "as timestamp), interval 90033 seconds) as string)",
      "2012-01-01 00:00:00.123456789");
  // Add/sub milliseconds.
  TestStringValue("cast(date_add(cast('2012-01-01 00:00:00.000000001' "
      "as timestamp), interval 90000033 milliseconds) as string)",
      "2012-01-02 01:00:00.033000001");
  TestStringValue("cast(date_sub(cast('2012-01-02 01:00:00.033000001' "
      "as timestamp), interval 90000033 milliseconds) as string)",
      "2012-01-01 00:00:00.000000001");
  // Add/sub microseconds.
  TestStringValue("cast(date_add(cast('2012-01-01 00:00:00.000000001' "
      "as timestamp), interval 1033 microseconds) as string)",
      "2012-01-01 00:00:00.001033001");
  TestStringValue("cast(date_sub(cast('2012-01-01 00:00:00.001033001' "
      "as timestamp), interval 1033 microseconds) as string)",
      "2012-01-01 00:00:00.000000001");
  // Add/sub nanoseconds.
  TestStringValue("cast(date_add(cast('2012-01-01 00:00:00.000000001' "
      "as timestamp), interval 1033 nanoseconds) as string)",
      "2012-01-01 00:00:00.000001034");
  TestStringValue("cast(date_sub(cast('2012-01-01 00:00:00.000001034' "
      "as timestamp), interval 1033 nanoseconds) as string)",
      "2012-01-01 00:00:00.000000001");

  // Test add/sub behavior with very large time values.
  string max_int = lexical_cast<string>(numeric_limits<int32_t>::max());
  TestStringValue(
        "cast(years_add(cast('2000-01-01 00:00:00' "
        "as timestamp), " + max_int + ") as string)",
        "1999-01-01 00:00:00");
  TestStringValue(
      "cast(years_sub(cast('2000-01-01 00:00:00' "
      "as timestamp), " + max_int + ") as string)",
      "2001-01-01 00:00:00");

  TestValue("unix_timestamp(cast('1970-01-01 00:00:00' as timestamp))", TYPE_INT, 0);
  TestValue("unix_timestamp('1970-01-01 00:00:00')", TYPE_INT, 0);
  TestValue("unix_timestamp('1970-01-01 00:00:00', 'yyyy-MM-dd HH:mm:ss')", TYPE_INT, 0);
  TestValue("unix_timestamp('1970-01-01', 'yyyy-MM-dd')", TYPE_INT, 0);
  TestValue("unix_timestamp('   1970-01-01 ', 'yyyy-MM-dd')", TYPE_INT, 0);
  TestValue("unix_timestamp('1970-01-01 10:10:10', 'yyyy-MM-dd')", TYPE_INT, 0);
  TestValue("unix_timestamp('1970-01-01 00:00:00 extra text', 'yyyy-MM-dd HH:mm:ss')",
            TYPE_INT, 0);
  TestStringValue("cast(cast(0 as timestamp) as string)", "1970-01-01 00:00:00");
  TestStringValue("from_unixtime(0)", "1970-01-01 00:00:00");
  TestStringValue("from_unixtime(0, 'yyyy-MM-dd HH:mm:ss')", "1970-01-01 00:00:00");
  TestStringValue("from_unixtime(0, 'yyyy-MM-dd')", "1970-01-01");
  TestStringValue("from_unixtime(unix_timestamp('1999-01-01 10:10:10'), \
      'yyyy-MM-dd')", "1999-01-01");
  TestStringValue("from_unixtime(unix_timestamp('1999-01-01 10:10:10'), \
      'yyyy-MM-dd HH:mm:ss')", "1999-01-01 10:10:10");
  TestValue("cast('2011-12-22 09:10:11.123456789' as timestamp) > \
      cast('2011-12-22 09:10:11.12345678' as timestamp)", TYPE_BOOLEAN, true);
  TestValue("cast('2011-12-22 08:10:11.123456789' as timestamp) > \
      cast('2011-12-22 09:10:11.12345678' as timestamp)", TYPE_BOOLEAN, false);
  TestValue("cast('2011-12-22 09:10:11.000000' as timestamp) = \
      cast('2011-12-22 09:10:11' as timestamp)", TYPE_BOOLEAN, true);
  TestValue("year(cast('2011-12-22 09:10:11.000000' as timestamp))", TYPE_INT, 2011);
  TestValue("month(cast('2011-12-22 09:10:11.000000' as timestamp))", TYPE_INT, 12);
  TestValue("dayofmonth(cast('2011-12-22 09:10:11.000000' as timestamp))", TYPE_INT, 22);
  TestValue("day(cast('2011-12-22 09:10:11.000000' as timestamp))", TYPE_INT, 356);
  TestValue("weekofyear(cast('2011-12-22 09:10:11.000000' as timestamp))", TYPE_INT, 51);
  TestValue("hour(cast('2011-12-22 09:10:11.000000' as timestamp))", TYPE_INT, 9);
  TestValue("minute(cast('2011-12-22 09:10:11.000000' as timestamp))", TYPE_INT, 10);
  TestValue("second(cast('2011-12-22 09:10:11.000000' as timestamp))", TYPE_INT, 11);
  TestValue("year(cast('2011-12-22' as timestamp))", TYPE_INT, 2011);
  TestValue("month(cast('2011-12-22' as timestamp))", TYPE_INT, 12);
  TestValue("dayofmonth(cast('2011-12-22' as timestamp))", TYPE_INT, 22);
  TestValue("day(cast('2011-12-22' as timestamp))", TYPE_INT, 356);
  TestValue("weekofyear(cast('2011-12-22' as timestamp))", TYPE_INT, 51);
  TestValue("hour(cast('09:10:11.000000' as timestamp))", TYPE_INT, 9);
  TestValue("minute(cast('09:10:11.000000' as timestamp))", TYPE_INT, 10);
  TestValue("second(cast('09:10:11.000000' as timestamp))", TYPE_INT, 11);
  TestStringValue(
      "to_date(cast('2011-12-22 09:10:11.12345678' as timestamp))", "2011-12-22");

  TestValue("datediff('2011-12-22 09:10:11.12345678', '2012-12-22')", TYPE_INT, -366);
  TestValue("datediff('2012-12-22', '2011-12-22 09:10:11.12345678')", TYPE_INT, 366);

  TestIsNull("year(cast('09:10:11.000000' as timestamp))", TYPE_INT);
  TestIsNull("month(cast('09:10:11.000000' as timestamp))", TYPE_INT);
  TestIsNull("dayofmonth(cast('09:10:11.000000' as timestamp))", TYPE_INT);
  TestIsNull("day(cast('09:10:11.000000' as timestamp))", TYPE_INT);
  TestIsNull("weekofyear(cast('09:10:11.000000' as timestamp))", TYPE_INT);
  TestIsNull("datediff(cast('09:10:11.12345678' as timestamp), "
      "cast('2012-12-22' as timestamp))", TYPE_INT);

  // Tests from Hive
  // The hive documentation states that timestamps are timezoneless, but the tests
  // show that they treat them as being in the current timezone so these tests
  // use the utc conversion to correct for that and get the same answers as
  // are in the hive test output.
  TestValue("cast(to_utc_timestamp(cast('2011-01-01 01:01:01' as timestamp), 'PST') "
      "as boolean)", TYPE_BOOLEAN, true);
  TestValue("cast(to_utc_timestamp(cast('2011-01-01 01:01:01' as timestamp), 'PST') "
      "as tinyint)", TYPE_TINYINT, 77);
  TestValue("cast(to_utc_timestamp(cast('2011-01-01 01:01:01' as timestamp), 'PST') "
      "as smallint)", TYPE_SMALLINT, -4787);
  TestValue("cast(to_utc_timestamp(cast('2011-01-01 01:01:01' as timestamp), 'PST') "
      "as int)", TYPE_INT, 1293872461);
  TestValue("cast(to_utc_timestamp(cast('2011-01-01 01:01:01' as timestamp), 'PST') "
      "as bigint)", TYPE_BIGINT, 1293872461);
  // We have some rounding errors going backend to front, so do it as a string.
  TestStringValue("cast(cast (to_utc_timestamp(cast('2011-01-01 01:01:01' "
      "as timestamp), 'PST') as float) as string)", "1.29387251e+09");
  TestValue("cast(to_utc_timestamp(cast('2011-01-01 01:01:01' as timestamp), 'PST') "
      "as double)", TYPE_DOUBLE, 1.293872461E9);
  TestValue("cast(to_utc_timestamp(cast('2011-01-01 01:01:01.1' as timestamp), 'PST') "
      "as double)", TYPE_DOUBLE, 1.2938724611E9);
  TestValue("cast(to_utc_timestamp(cast('2011-01-01 01:01:01.0001' as timestamp), 'PST') "
      "as double)", TYPE_DOUBLE, 1.2938724610001E9);
  // We get some decimal-binary skew here
  TestStringValue("cast(from_utc_timestamp(cast(1.3041352164485E9 as timestamp), 'PST') "
      "as string)", "2011-04-29 20:46:56.448499917");

  // Hive silently ignores bad timezones.  We log a problem.
  TestStringValue(
      "cast(from_utc_timestamp("
      "cast('1970-01-01 00:00:00' as timestamp), 'FOOBAR') as string)",
      "1970-01-01 00:00:00");

  // With support of date strings this generates a date and 0 time.
  TestStringValue(
      "cast(cast('1999-01-10' as timestamp) as string)", "1999-01-10 00:00:00");

  // Test functions with unknown expected value.
  TestValidTimestampValue("now()");
  TestValidTimestampValue("current_timestamp()");
  TestValidTimestampValue("cast(unix_timestamp() as timestamp)");

  // Test alias
  TestValue("now() = current_timestamp()", TYPE_BOOLEAN, true);

  // Test invalid formats returning NULL
  TestIsNull("unix_timestamp('1970-01-01 0:00:00', 'yyyy-MM-dd HH:mm:ss')", TYPE_INT);
  TestIsNull("unix_timestamp('1970-01-01 00:00:00', 'yyyy-MM-dd hh:mm:ss')", TYPE_INT);
  TestIsNull("unix_timestamp('1970-01-01 00:00:00', 'yy-MM-dd HH:mm:ss')", TYPE_INT);
  TestIsNull("unix_timestamp('1970-01-01', 'yyyy-MM-dd HH:mm:ss')", TYPE_INT);
  TestIsNull("unix_timestamp('1970', 'yyyy-MM-dd')", TYPE_INT);
  TestIsNull("unix_timestamp('', 'yyyy-MM-dd')", TYPE_INT);

  TestIsNull("from_unixtime(0, 'yy-MM-dd HH:mm:dd')", TYPE_STRING);
  TestIsNull("from_unixtime(0, 'yyyy-MM-dd HH::dd')", TYPE_STRING);
  TestIsNull("from_unixtime(0, 'HH:mm:dd')", TYPE_STRING);
}

// TODO: Since we currently can't analyze NULL literals as function parameters,
// we instead use a function which we know will return NULL as a workaround.
// This only works sometimes though, because the NULL-returning function
// must also have the correct return type that we are looking for.
// The commented (#if 0) tests should be enabled once we can analyze NULL literals
// as function arguments.
TEST_F(ExprTest, ConditionalFunctions) {
  // If first param evaluates to true, should return second parameter,
  // false or NULL should return the third.
  TestValue("if(TRUE, FALSE, TRUE)", TYPE_BOOLEAN, false);
  TestValue("if(FALSE, FALSE, TRUE)", TYPE_BOOLEAN, true);
  TestValue("if(TRUE, 10, 20)", TYPE_BIGINT, 10);
  TestValue("if(FALSE, 10, 20)", TYPE_BIGINT, 20);
  TestValue("if(TRUE, 5.5, 8.8)", TYPE_DOUBLE, 5.5);
  TestValue("if(FALSE, 5.5, 8.8)", TYPE_DOUBLE, 8.8);
  TestStringValue("if(TRUE, 'abc', 'defgh')", "abc");
  TestStringValue("if(FALSE, 'abc', 'defgh')", "defgh");
  TimestampValue then_val(1293872461);
  TimestampValue else_val(929387245);
  TestTimestampValue("if(TRUE, cast('2011-01-01 09:01:01' as timestamp), "
      "cast('1999-06-14 19:07:25' as timestamp))", then_val);
  TestTimestampValue("if(FALSE, cast('2011-01-01 09:01:01' as timestamp), "
      "cast('1999-06-14 19:07:25' as timestamp))", else_val);

  // Workaround: if(true, NULL, NULL) returns NULL of type BOOLEAN.
  // coalesce(NULL)
  TestIsNull("coalesce(if(true, NULL, NULL))", TYPE_BOOLEAN);
  // coalesce(NULL, NULL)
  TestIsNull("coalesce(if(true, NULL, NULL), if(true, NULL, NULL))", TYPE_BOOLEAN);
  TestValue("coalesce(TRUE)", TYPE_BOOLEAN, true);
  // coalesce(NULL, TRUE, NULL)
  TestValue("coalesce(if(true, NULL, NULL), TRUE, if(true, NULL, NULL))",
      TYPE_BOOLEAN, true);
  // coalesce(FALSE, NULL, TRUE, NULL)
  TestValue("coalesce(FALSE, if(true, NULL, NULL), TRUE, if(true, NULL, NULL))",
      TYPE_BOOLEAN, false);
  // coalesce(NULL, NULL, NULL TRUE, NULL, NULL)
  TestValue("coalesce(if(true, NULL, NULL), if(true, NULL, NULL), if(true, NULL, NULL),"
      "TRUE, if(true, NULL, NULL), if(true, NULL, NULL))", TYPE_BOOLEAN, true);
  TestValue("coalesce(10)", TYPE_BIGINT, 10);
#if 0
  TestValue("coalesce(NULL, 10, NULL)", TYPE_BIGINT, 10);
  TestValue("coalesce(20, NULL, 10, NULL)", TYPE_BIGINT);
  TestValue("coalesce(NULL, NULL, NULL, 10, NULL, NULL)", TYPE_BIGINT);
#endif
  TestValue("coalesce(5.5)", TYPE_DOUBLE, 5.5);
#if 0
  TestValue("coalesce(NULL, 5.5, NULL)", TYPE_DOUBLE, 5.5);
  TestValue("coalesce(8.8, NULL, 5.5, NULL)", TYPE_DOUBLE);
  TestValue("coalesce(NULL, NULL, NULL, 5.5, NULL, NULL)", TYPE_DOUBLE);
#endif
  TestStringValue("coalesce('abc')", "abc");
#if 0
  TestStringValue("coalesce(NULL, 'abc', NULL)", "abc");
  TestStringValue("coalesce('defgh', NULL, 'abc', NULL)", "defgh");
  TestStringValue("coalesce(NULL, NULL, NULL, 'abc', NULL, NULL)", "abc");
#endif
  TimestampValue ats(1293872461);
#if 0
  TimestampValue bts(929387245);
#endif
  TestTimestampValue("coalesce(cast('2011-01-01 09:01:01' as timestamp))", ats);
#if 0
  TestTimestampValue("coalesce(NULL, cast('2011-01-01 09:01:01' as timestamp),"
      "NULL)", ats);
  TestTimestampValue("coalesce(cast('1999-06-14 19:07:25' as timestamp), NULL,"
      "cast('2011-01-01 09:01:01' as timestamp), NULL)", bts);
  TestTimestampValue("coalesce(NULL, NULL, NULL,"
      "cast('2011-01-01 09:01:01' as timestamp), NULL, NULL)", ats);
#endif

  // Test logic of case expr using int types.
  // The different types and casting are tested below.
  TestValue("case when true then 1 end", TYPE_TINYINT, 1);
  TestValue("case when false then 1 when true then 2 end", TYPE_TINYINT, 2);
  TestValue("case when false then 1 when false then 2 when true then 3 end",
      TYPE_TINYINT, 3);
  // Test else expr.
  TestValue("case when false then 1 else 10 end", TYPE_TINYINT, 10);
  TestValue("case when false then 1 when false then 2 else 10 end", TYPE_TINYINT, 10);
  TestValue("case when false then 1 when false then 2 when false then 3 else 10 end",
      TYPE_TINYINT, 10);
  TestIsNull("case when false then 1 end", TYPE_TINYINT);
  // Test with case expr.
  TestValue("case 21 when 21 then 1 end", TYPE_TINYINT, 1);
  TestValue("case 21 when 20 then 1 when 21 then 2 end", TYPE_TINYINT, 2);
  TestValue("case 21 when 20 then 1 when 19 then 2 when 21 then 3 end", TYPE_TINYINT, 3);
  // Should skip when-exprs that are NULL
#if 0
  TestIsNull("case when NULL then 1 end", TYPE_TINYINT);
  TestIsNull("case when NULL then 1 end else NULL end", TYPE_TINYINT);
  TestValue("case when NULL then 1 else 2 end", TYPE_TINYINT, 2);
  TestValue("case when NULL then 1 when true then 2 else 3 end", TYPE_TINYINT, 2);
#endif
  // Should return else expr, if case-expr is NULL.
#if 0
  TestIsNull("case NULL when 1 then 1 end", TYPE_TINYINT);
  TestIsNull("case NULL when 1 then 1 else NULL end", TYPE_TINYINT);
  TestValue("case NULL when 1 then 1 else 2 end", TYPE_TINYINT, 2);
  TestValue("case 10 when NULL then 1 else 2 end", TYPE_TINYINT, 2);
  TestValue("case 10 when NULL then 1 when 10 then 2 else 3 end", TYPE_TINYINT, 2);
#endif

  // Test all types in case/when exprs, without casts.
  unordered_map<int, string>::iterator def_iter;
  for(def_iter = default_type_strs_.begin(); def_iter != default_type_strs_.end();
      ++def_iter) {
    TestValue("case " + def_iter->second + " when " + def_iter->second +
        " then true end", TYPE_BOOLEAN, true);
  }

  // Test all int types in then and else exprs.
  // Also tests implicit casting in all exprs.
  unordered_map<int, int64_t>::iterator int_iter;
  for (int_iter = min_int_values_.begin(); int_iter != min_int_values_.end();
      ++int_iter) {
    PrimitiveType t = static_cast<PrimitiveType>(int_iter->first);
    string& s = default_type_strs_[t];
    TestValue("case when true then " + s + " end", t, int_iter->second);
    TestValue("case when false then 1 else " + s + " end", t, int_iter->second);
    TestValue("case when true then 1 else " + s + " end", t, 1);
    TestValue("case 0 when " + s + " then true else false end", TYPE_BOOLEAN, false);
  }

  // Test all float types in then and else exprs.
  // Also tests implicit casting in all exprs.
  // TODO: Something with our float literals is broken:
  // 1.1 gets recognized as a DOUBLE, but numeric_limits<float>::max()) + 1.1 as a FLOAT.
#if 0
  unordered_map<int, double>::iterator float_iter;
  for (float_iter = min_float_values_.begin(); float_iter != min_float_values_.end();
      ++float_iter) {
    PrimitiveType t = static_cast<PrimitiveType>(float_iter->first);
    string& s = default_type_strs_[t];
    TestValue("case when true then " + s + " end", t, float_iter->second);
    TestValue("case when false then 1 else " + s + " end", t, float_iter->second);
    TestValue("case when true then 1 else " + s + " end", t, 1.0);
    TestValue("case 0 when " + s + " then true else false end", TYPE_BOOLEAN, false);
  }
#endif

  // Test all other types.
  // We don't tests casts because these types don't allow casting up to them.
  TestValue("case when true then " + default_bool_str_ + " end", TYPE_BOOLEAN,
      default_bool_val_);
  TestValue("case when false then true else " + default_bool_str_ + " end", TYPE_BOOLEAN,
      default_bool_val_);
  // String type.
  TestStringValue("case when true then " + default_string_str_ + " end",
      default_string_val_);
  TestStringValue("case when false then '1' else " + default_string_str_ + " end",
      default_string_val_);
  // Timestamp type.
  TestTimestampValue("case when true then " + default_timestamp_str_ + " end",
      default_timestamp_val_);
  TestTimestampValue("case when false then cast('1999-06-14 19:07:25' as timestamp) "
      "else " + default_timestamp_str_ + " end", default_timestamp_val_);
}

// Validates that Expr::ComputeResultsLayout() for 'exprs' is correct.
//   - expected_byte_size: total byte size to store all results for exprs
//   - expected_var_begin: byte offset where variable length types begin
//   - expected_offsets: mapping of byte sizes to a set valid offsets
//     exprs that have the same byte size can end up in a number of locations
void ValidateLayout(const vector<Expr*>& exprs, int expected_byte_size,
    int expected_var_begin, const map<int, set<int> >& expected_offsets) {

  vector<int> offsets;
  set<int> offsets_found;

  int var_begin;
  int byte_size = Expr::ComputeResultsLayout(exprs, &offsets, &var_begin);

  EXPECT_EQ(byte_size, expected_byte_size);
  EXPECT_EQ(var_begin, expected_var_begin);

  // Walk the computed offsets and make sure the resulting sets match expected_offsets
  for (int i = 0; i < exprs.size(); ++i) {
    int expr_byte_size = GetByteSize(exprs[i]->type());
    map<int, set<int> >::const_iterator iter = expected_offsets.find(expr_byte_size);
    EXPECT_TRUE(iter != expected_offsets.end());

    const set<int>& possible_offsets = iter->second;
    int computed_offset = offsets[i];
    // The computed offset has to be one of the possible.  Exprs types with the
    // same size are not ordered wrt each other.
    EXPECT_TRUE(possible_offsets.find(computed_offset) != possible_offsets.end());
    // The offset should not have been found before
    EXPECT_TRUE(offsets_found.find(computed_offset) == offsets_found.end());
    offsets_found.insert(computed_offset);
  }
}

TEST_F(ExprTest, ResultsLayoutTest) {
  ObjectPool pool;

  vector<Expr*> exprs;
  map<int, set<int> > expected_offsets;

  // Test empty exprs
  ValidateLayout(exprs, 0, -1, expected_offsets);

  // Test single Expr case
  expected_offsets.clear();
  for (int type = TYPE_BOOLEAN; type <= TYPE_STRING; ++type) {
    PrimitiveType t = static_cast<PrimitiveType>(type);
    exprs.clear();
    expected_offsets.clear();
    // With one expr, all offsets shoudl be 0.
    expected_offsets[GetByteSize(t)] = list_of(0);
    exprs.push_back(Expr::CreateLiteral(&pool, t, "0"));
    if (t == TYPE_STRING) {
      ValidateLayout(exprs, 16, 0, expected_offsets);
    } else {
      ValidateLayout(exprs, GetByteSize(t), -1, expected_offsets);
    }
  }

  int expected_byte_size = 0;
  int expected_var_begin = 0;
  expected_offsets.clear();
  exprs.clear();

  // Test layout adding a bunch of exprs.  This is designed to trigger padding.
  // The expected result is computed along the way
  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_BOOLEAN, "0"));
  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_TINYINT, "0"));
  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_TINYINT, "0"));
  expected_offsets[1].insert(expected_byte_size);
  expected_offsets[1].insert(expected_byte_size + 1);
  expected_offsets[1].insert(expected_byte_size + 2);
  expected_byte_size += 3 * 1 + 1;  // 1 byte of paddding

  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_SMALLINT, "0"));
  expected_offsets[2].insert(expected_byte_size);
  expected_byte_size += 1 * 2 + 2;  // 2 bytes of padding

  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_INT, "0"));
  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_FLOAT, "0"));
  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_FLOAT, "0"));
  expected_offsets[4].insert(expected_byte_size);
  expected_offsets[4].insert(expected_byte_size + 4);
  expected_offsets[4].insert(expected_byte_size + 8);
  expected_byte_size += 3 * 4 + 4;  // 4 bytes of padding

  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_BIGINT, "0"));
  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_BIGINT, "0"));
  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_BIGINT, "0"));
  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_DOUBLE, "0"));
  expected_offsets[8].insert(expected_byte_size);
  expected_offsets[8].insert(expected_byte_size + 8);
  expected_offsets[8].insert(expected_byte_size + 16);
  expected_offsets[8].insert(expected_byte_size + 24);
  expected_byte_size += 4 * 8;      // No more padding

  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_TIMESTAMP, "0"));
  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_TIMESTAMP, "0"));
  expected_offsets[16].insert(expected_byte_size);
  expected_offsets[16].insert(expected_byte_size + 16);
  expected_byte_size += 2 * 16;

  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_STRING, "0"));
  exprs.push_back(Expr::CreateLiteral(&pool, TYPE_STRING, "0"));
  expected_offsets[0].insert(expected_byte_size);
  expected_offsets[0].insert(expected_byte_size + 16);
  expected_var_begin = expected_byte_size;
  expected_byte_size += 2 * 16;

  // Validate computed layout
  ValidateLayout(exprs, expected_byte_size, expected_var_begin, expected_offsets);

  // Randomize the expr order and validate again.  This is implemented by a
  // sort when the layout is computed so it shouldn't be very sensitive to
  // a particular order.

  srand(0);   // Seed rand to get repeatable results
  for (int i = 0; i < 10; ++i) {
    std::random_shuffle(exprs.begin(), exprs.end());
    ValidateLayout(exprs, expected_byte_size, expected_var_begin, expected_offsets);
  }
}

}

int main(int argc, char **argv) {
  google::InitGoogleLogging(argv[0]);
  ::testing::InitGoogleTest(&argc, argv);
  InitThriftLogging();
  impala::CpuInfo::Init();
  impala::DiskInfo::Init();
  impala::LlvmCodeGen::InitializeLlvm();

  EXIT_IF_ERROR(JniUtil::Init());

  // Create an in-process Impala server and in-process backends for test environment.
  FLAGS_impalad = "localhost:21000";
  VLOG_CONNECTION << "creating test env";
  test_env_ = new TestExecEnv(2, FLAGS_be_port + 1);
  VLOG_CONNECTION << "starting backends";
  test_env_->StartBackends();

  EXIT_IF_ERROR(CreateImpalaServer(test_env_, FLAGS_beeswax_port, 0, FLAGS_be_port,
      &beeswax_server_, NULL, &be_server_, NULL));
  beeswax_server_->Start();
  be_server_->Start();

  // exec_env_.reset(new ExecEnv());
  executor_ = new ImpaladQueryExecutor();
  EXIT_IF_ERROR(executor_->Setup());

  cout << "Running with Jit" << endl;

  int ret = RUN_ALL_TESTS();
  if (ret != 0) return ret;

  vector<string> options;
  options.push_back("DISABLE_CODEGEN=1");
  executor_->setExecOptions(options);
  cout << endl << "Running without Jit" << endl;
  return RUN_ALL_TESTS();
}
