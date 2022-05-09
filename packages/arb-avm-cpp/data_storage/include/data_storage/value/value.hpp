/*
 * Copyright 2019-2020, Offchain Labs, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef checkpoint_value_hpp
#define checkpoint_value_hpp

#include <avm_values/value.hpp>
#include <data_storage/readtransaction.hpp>
#include <data_storage/storageresultfwd.hpp>
#include <data_storage/value/valuecache.hpp>

#include <map>
#include <set>

struct DeleteResults;
struct SaveResults;
class Transaction;

DbResult<Value> getValue(const ReadTransaction& tx,
                         uint256_t value_hash,
                         ValueCache& value_cache,
                         bool lazy_load);
SaveResults saveValue(ReadWriteTransaction& tx, const Value& val);
DeleteResults deleteValue(ReadWriteTransaction& tx, uint256_t value_hash);

struct ParsedBuffer {
    uint64_t depth;
    std::vector<uint256_t> nodes;
};

struct ParsedCodePointStub {
    uint256_t root_hash;
    uint64_t pc;
    uint256_t hash;
};

class ParsedTupValVector;
using ParsedTupVal = std::variant<ParsedTupValVector,
                                  uint256_t,
                                  ParsedCodePointStub,
                                  Buffer,
                                  BigUnloadedValue,
                                  ParsedBuffer>;

class ParsedTupValVector : public std::vector<ParsedTupVal> {};

using ParsedBufVal = std::variant<Buffer, ParsedBuffer>;

struct ParsedCodeSegment;
using ParsedSerializedVal = std::variant<std::vector<ParsedTupVal>,
                                         uint256_t,
                                         ParsedCodePointStub,
                                         Buffer,
                                         ParsedBuffer,
                                         ParsedCodeSegment>;

struct ParsedCodeSegment {
    std::vector<Operation> operations_without_immediates;
    std::vector<std::pair<uint64_t, ParsedSerializedVal>> immediate_values;
    std::vector<uint256_t> cached_hashes;
};

bool shouldInlineValue(const Value& tuple,
                       const std::vector<unsigned char>& seed);

DbResult<Value> getValueRecord(const ReadTransaction& tx,
                               const ParsedSerializedVal& record,
                               std::set<uint64_t>& segment_ids,
                               ValueCache& value_cache,
                               bool lazy_load);
ParsedSerializedVal parseRecord(const char*& buf);
std::vector<Value> serializeValue(const std::vector<unsigned char>& seed,
                                  const Value& val,
                                  std::vector<unsigned char>& value_vector);
DeleteResults deleteValueRecord(ReadWriteTransaction& tx,
                                const ParsedSerializedVal& val);

#endif /* value_hpp */
