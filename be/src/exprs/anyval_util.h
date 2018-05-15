// Modifications copyright (C) 2017, Baidu.com, Inc.
// Copyright 2017 The Apache Software Foundation

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef BDG_PALO_BE_SRC_QUERY_EXPRS_ANYVAL_UTIL_H
#define BDG_PALO_BE_SRC_QUERY_EXPRS_ANYVAL_UTIL_H

#include "codegen/llvm_codegen.h"
#include "runtime/primitive_type.h"
#include "udf/udf.h"
#include "util/hash_util.hpp"
#include "common/status.h"

namespace palo {

class MemPool;

// Utilities for AnyVals
class AnyValUtil {
public:
    static uint32_t hash(const palo_udf::BooleanVal& v, int seed) {
        return HashUtil::hash(&v.val, 1, seed);
    }

    static uint32_t hash(const palo_udf::TinyIntVal& v, int seed) {
        return HashUtil::hash(&v.val, 1, seed);
    }

    static uint32_t hash(const palo_udf::SmallIntVal& v, int seed) {
        return HashUtil::hash(&v.val, 2, seed);
    }

    static uint32_t hash(const palo_udf::IntVal& v, int seed) {
        return HashUtil::hash(&v.val, 4, seed);
    }

    static uint32_t hash(const palo_udf::BigIntVal& v, int seed) {
        return HashUtil::hash(&v.val, 8, seed);
    }

    static uint32_t hash(const palo_udf::FloatVal& v, int seed) {
        return HashUtil::hash(&v.val, 4, seed);
    }

    static uint32_t hash(const palo_udf::DoubleVal& v, int seed) {
        return HashUtil::hash(&v.val, 8, seed);
    }

    static uint32_t hash(const palo_udf::StringVal& v, int seed) {
        return HashUtil::hash(v.ptr, v.len, seed);
    }

    static uint32_t hash(const palo_udf::DateTimeVal& v, int seed) {
        DateTimeValue tv = DateTimeValue::from_datetime_val(v);
        return tv.hash(seed);
    }

    static uint32_t hash(const palo_udf::DecimalVal& v, int seed) {
        DecimalValue tv = DecimalValue::from_decimal_val(v);
        return tv.hash(seed);
    }

    static uint32_t hash(const palo_udf::LargeIntVal& v, int seed) {
        return HashUtil::hash(&v.val, 8, seed);
    }

    static uint64_t hash64(const palo_udf::BooleanVal& v, int64_t seed) {
        return HashUtil::fnv_hash64(&v.val, 1, seed);
    }

    static uint64_t hash64(const palo_udf::TinyIntVal& v, int64_t seed) {
        return HashUtil::fnv_hash64(&v.val, 1, seed);
    }

    static uint64_t hash64(const palo_udf::SmallIntVal& v, int64_t seed) {
        return HashUtil::fnv_hash64(&v.val, 2, seed);
    }

    static uint64_t hash64(const palo_udf::IntVal& v, int64_t seed) {
        return HashUtil::fnv_hash64(&v.val, 4, seed);
    }

    static uint64_t hash64(const palo_udf::BigIntVal& v, int64_t seed) {
        return HashUtil::fnv_hash64(&v.val, 8, seed);
    }

    static uint64_t hash64(const palo_udf::FloatVal& v, int64_t seed) {
        return HashUtil::fnv_hash64(&v.val, 4, seed);
    }

    static uint64_t hash64(const palo_udf::DoubleVal& v, int64_t seed) {
        return HashUtil::fnv_hash64(&v.val, 8, seed);
    }

    static uint64_t hash64(const palo_udf::StringVal& v, int64_t seed) {
        return HashUtil::fnv_hash64(v.ptr, v.len, seed);
    }

    static uint64_t hash64(const palo_udf::DateTimeVal& v, int64_t seed) {
        DateTimeValue tv = DateTimeValue::from_datetime_val(v);
        return HashUtil::fnv_hash64(&tv, 12, seed);
    }

    // TODO(lingbin): fix this method. can not use sizeof directly, because there are a lot of
    // storage way for one value. 
    static uint64_t hash64(const palo_udf::DecimalVal& v, int64_t seed) {
        DecimalValue tv = DecimalValue::from_decimal_val(v);
        return HashUtil::fnv_hash64(&tv, sizeof(DecimalValue), seed);
    }

    static uint64_t hash64(const palo_udf::LargeIntVal& v, int64_t seed) {
        return HashUtil::fnv_hash64(&v.val, 8, seed);
    }

    static uint64_t hash64_murmur(const palo_udf::BooleanVal& v, int64_t seed) {
        return HashUtil::murmur_hash64A(&v.val, 1, seed);
    }

    static uint64_t hash64_murmur(const palo_udf::TinyIntVal& v, int64_t seed) {
        return HashUtil::murmur_hash64A(&v.val, 1, seed);
    }

    static uint64_t hash64_murmur(const palo_udf::SmallIntVal& v, int64_t seed) {
        return HashUtil::murmur_hash64A(&v.val, 2, seed);
    }

    static uint64_t hash64_murmur(const palo_udf::IntVal& v, int64_t seed) {
        return HashUtil::murmur_hash64A(&v.val, 4, seed);
    }

    static uint64_t hash64_murmur(const palo_udf::BigIntVal& v, int64_t seed) {
        return HashUtil::murmur_hash64A(&v.val, 8, seed);
    }

    static uint64_t hash64_murmur(const palo_udf::FloatVal& v, int64_t seed) {
        return HashUtil::murmur_hash64A(&v.val, 4, seed);
    }

    static uint64_t hash64_murmur(const palo_udf::DoubleVal& v, int64_t seed) {
        return HashUtil::murmur_hash64A(&v.val, 8, seed);
    }

    static uint64_t hash64_murmur(const palo_udf::StringVal& v, int64_t seed) {
        return HashUtil::murmur_hash64A(v.ptr, v.len, seed);
    }

    static uint64_t hash64_murmur(const palo_udf::DateTimeVal& v, int64_t seed) {
        DateTimeValue tv = DateTimeValue::from_datetime_val(v);
        return HashUtil::murmur_hash64A(&tv, 12, seed);
    }

    static uint64_t hash64_murmur(const palo_udf::DecimalVal& v, int64_t seed) {
        DecimalValue tv = DecimalValue::from_decimal_val(v);
        return HashUtil::murmur_hash64A(&tv, sizeof(DecimalValue), seed);
    }

    static uint64_t hash64_murmur(const palo_udf::LargeIntVal& v, int64_t seed) {
        return HashUtil::murmur_hash64A(&v.val, 8, seed);
    }

    static palo_udf::FunctionContext::Type primitive_type_to_type(const PrimitiveType& type) {
    switch (type) {
    case TYPE_NULL:
        return palo_udf::FunctionContext::TYPE_NULL;
    case TYPE_BOOLEAN:
        return palo_udf::FunctionContext::TYPE_BOOLEAN;
    case TYPE_TINYINT:
        return palo_udf::FunctionContext::TYPE_TINYINT;
    case TYPE_SMALLINT:
        return palo_udf::FunctionContext::TYPE_SMALLINT;
    case TYPE_INT:
        return palo_udf::FunctionContext::TYPE_INT;
    case TYPE_BIGINT:
        return palo_udf::FunctionContext::TYPE_BIGINT;
    case TYPE_LARGEINT:
        return palo_udf::FunctionContext::TYPE_LARGEINT;
    case TYPE_FLOAT:
        return palo_udf::FunctionContext::TYPE_FLOAT;
    case TYPE_DOUBLE:
        return palo_udf::FunctionContext::TYPE_DOUBLE;
    case TYPE_DATE:
        return palo_udf::FunctionContext::TYPE_DATE;
    case TYPE_DATETIME:
        return palo_udf::FunctionContext::TYPE_DATETIME;
    case TYPE_HLL:
    case TYPE_CHAR:
    case TYPE_VARCHAR:
        return palo_udf::FunctionContext::TYPE_STRING;
    case TYPE_DECIMAL:
        return palo_udf::FunctionContext::TYPE_DECIMAL;
    break;
    default:
    DCHECK(false) << "Unknown type: " << type;
    }
        return palo_udf::FunctionContext::TYPE_NULL;
    }
    // Returns the byte size of *Val for type t.
    static int any_val_size(const TypeDescriptor& t) {
        switch (t.type) {
        case TYPE_BOOLEAN:
            return sizeof(palo_udf::BooleanVal);

        case TYPE_TINYINT:
            return sizeof(palo_udf::TinyIntVal);

        case TYPE_SMALLINT:
            return sizeof(palo_udf::SmallIntVal);

        case TYPE_INT:
            return sizeof(palo_udf::IntVal);

        case TYPE_BIGINT:
            return sizeof(palo_udf::BigIntVal);

        case TYPE_LARGEINT:
            return sizeof(palo_udf::LargeIntVal);

        case TYPE_FLOAT:
            return sizeof(palo_udf::FloatVal);

        case TYPE_DOUBLE:
            return sizeof(palo_udf::DoubleVal);

        case TYPE_HLL:
        case TYPE_CHAR:
        case TYPE_VARCHAR:
            return sizeof(palo_udf::StringVal);

        case TYPE_DATE:
        case TYPE_DATETIME:
            return sizeof(palo_udf::DateTimeVal);

        case TYPE_DECIMAL:
            return sizeof(palo_udf::DecimalVal);

        default:
            DCHECK(false) << t;
            return 0;
        }
    }

    /// Returns the byte alignment of *Val for type t.
    static int any_val_alignment(const TypeDescriptor& t) {
      switch (t.type) {
        case TYPE_BOOLEAN: return alignof(BooleanVal);
        case TYPE_TINYINT: return alignof(TinyIntVal);
        case TYPE_SMALLINT: return alignof(SmallIntVal);
        case TYPE_INT: return alignof(IntVal);
        case TYPE_BIGINT: return alignof(BigIntVal);
        case TYPE_LARGEINT: return alignof(LargeIntVal);
        case TYPE_FLOAT: return alignof(FloatVal);
        case TYPE_DOUBLE: return alignof(DoubleVal);
        case TYPE_HLL:
        case TYPE_VARCHAR:
        case TYPE_CHAR:
          return alignof(StringVal);
        case TYPE_DATETIME: 
        case TYPE_DATE:
          return alignof(DateTimeVal);
        case TYPE_DECIMAL: return alignof(DecimalVal);
        default:
            DCHECK(false) << t;
            return 0;
        }
    }

    static std::string to_string(const StringVal& v) {
        return std::string(reinterpret_cast<char*>(v.ptr), v.len);
    }

    static StringVal from_string(FunctionContext* ctx, const std::string& s) {
        StringVal val = from_buffer(ctx, s.c_str(), s.size());
        return val;
    }

    static StringVal from_buffer(FunctionContext* ctx, const char* ptr, int len) {
        StringVal result(ctx, len);
        memcpy(result.ptr, ptr, len);
        return result;
    }

    static StringVal from_string_temp(FunctionContext* ctx, const std::string& s) {
        StringVal val = from_buffer_temp(ctx, s.c_str(), s.size());
        return val;
    }

    static StringVal from_buffer_temp(FunctionContext* ctx, const char* ptr, int len) {
        StringVal result = StringVal::create_temp_string_val(ctx, len);
        memcpy(result.ptr, ptr, len);
        return result;
    }

    static FunctionContext::TypeDesc column_type_to_type_desc(const TypeDescriptor& type);

    // Utility to put val into an AnyVal struct
    static void set_any_val(const void* slot, const TypeDescriptor& type, palo_udf::AnyVal* dst) {
        if (slot == NULL) {
            dst->is_null = true;
            return;
        }

        dst->is_null = false;
        switch (type.type) {
        case TYPE_NULL: return;
        case TYPE_BOOLEAN:
            reinterpret_cast<palo_udf::BooleanVal*>(dst)->val = 
                *reinterpret_cast<const bool*>(slot);
            return;
        case TYPE_TINYINT:
            reinterpret_cast<palo_udf::TinyIntVal*>(dst)->val =
                 *reinterpret_cast<const int8_t*>(slot);
            return;
        case TYPE_SMALLINT:
            reinterpret_cast<palo_udf::SmallIntVal*>(dst)->val =
                 *reinterpret_cast<const int16_t*>(slot);
            return;
        case TYPE_INT:
            reinterpret_cast<palo_udf::IntVal*>(dst)->val =
                 *reinterpret_cast<const int32_t*>(slot);
            return;
        case TYPE_BIGINT:
            reinterpret_cast<palo_udf::BigIntVal*>(dst)->val = 
                *reinterpret_cast<const int64_t*>(slot);
            return;
        case TYPE_LARGEINT:
            memcpy(&reinterpret_cast<palo_udf::LargeIntVal*>(dst)->val, slot, sizeof(__int128));
            return;
        case TYPE_FLOAT:
            reinterpret_cast<palo_udf::FloatVal*>(dst)->val = 
                *reinterpret_cast<const float*>(slot);
            return;
        case TYPE_DOUBLE:
            reinterpret_cast<palo_udf::DoubleVal*>(dst)->val = 
                *reinterpret_cast<const double*>(slot);
            return;
        case TYPE_CHAR:
        case TYPE_VARCHAR:
        case TYPE_HLL:
            reinterpret_cast<const StringValue*>(slot)->to_string_val(
            reinterpret_cast<palo_udf::StringVal*>(dst));
            return;
        case TYPE_DECIMAL:
            reinterpret_cast<const DecimalValue*>(slot)->to_decimal_val(
            reinterpret_cast<palo_udf::DecimalVal*>(dst));
            return; 
        case TYPE_DATE:
            reinterpret_cast<const DateTimeValue*>(slot)->to_datetime_val(
            reinterpret_cast<palo_udf::DateTimeVal*>(dst));
        return;
        case TYPE_DATETIME:
            reinterpret_cast<const DateTimeValue*>(slot)->to_datetime_val(
            reinterpret_cast<palo_udf::DateTimeVal*>(dst));
            return;
        default:
        DCHECK(false) << "NYI";
        }
    }

    /// Templated equality functions. These assume the input values are not NULL.
    template<typename T>
    static inline bool equals(const PrimitiveType& type, const T& x, const T& y) {
        return equals_intenal(x, y);
    }

    /// Templated equality functions. These assume the input values are not NULL.
    template<typename T>
    static inline bool equals(const T& x, const T& y) {
        return equals_intenal(x, y);
    }

    template<typename T>
    static inline bool equals(const TypeDescriptor& type, const T& x, const T& y) {
        return equals_intenal(x, y);
    }

    template<typename T>
    static inline bool equals(const FunctionContext::TypeDesc& type, const T& x, const T& y) {
        return equals_intenal(x, y);
    }
private:
    /// Implementations of Equals().
    template<typename T>
    static inline bool equals_intenal(const T& x, const T& y);

};

template<typename T>
inline bool AnyValUtil::equals_intenal(const T& x, const T& y) {
    DCHECK(!x.is_null);
    DCHECK(!y.is_null);
    return x.val == y.val;
}

template<> 
inline bool AnyValUtil::equals_intenal(const StringVal& x, const StringVal& y) {
    DCHECK(!x.is_null);
    DCHECK(!y.is_null);
    StringValue x_sv = StringValue::from_string_val(x);
    StringValue y_sv = StringValue::from_string_val(y);
    return x_sv == y_sv;
}

template<> 
inline bool AnyValUtil::equals_intenal(const DateTimeVal& x, const DateTimeVal& y) {
    DCHECK(!x.is_null);
    DCHECK(!y.is_null);
    DateTimeValue x_tv = DateTimeValue::from_datetime_val(x);
    DateTimeValue y_tv = DateTimeValue::from_datetime_val(y);
    return x_tv == y_tv;
}

template<> 
inline bool AnyValUtil::equals_intenal(const DecimalVal& x, const DecimalVal& y) {
    DCHECK(!x.is_null);
    DCHECK(!y.is_null);
    return x == y;
}

// Creates the corresponding AnyVal subclass for type. The object is added to the pool.
palo_udf::AnyVal* create_any_val(ObjectPool* pool, const TypeDescriptor& type);

/// Allocates an AnyVal subclass of 'type' from 'pool'. The AnyVal's memory is
/// initialized to all 0's. Returns a MemLimitExceeded() error with message
/// 'mem_limit_exceeded_msg' if the allocation cannot be made because of a memory
/// limit. 
Status allocate_any_val(RuntimeState* state, MemPool* pool, const TypeDescriptor& type,
    const std::string& mem_limit_exceeded_msg, AnyVal** result);

}
#endif
