/*
 * codec.cc
 * Copyright (C) 4paradigm.com 2019 wangtaize <wangtaize@4paradigm.com>
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

#include "codec/codec.h"
#include <algorithm>
#include <unordered_set>
#include "base/glog_wapper.h"
#include "boost/lexical_cast.hpp"

namespace rtidb {
namespace codec {

#define BitMapSize(size) (((size) >> 3) + !!((size)&0x07))

static const std::unordered_set<::rtidb::type::DataType> TYPE_SET(
    {::rtidb::type::kBool, ::rtidb::type::kSmallInt, ::rtidb::type::kInt,
     ::rtidb::type::kBigInt, ::rtidb::type::kFloat, ::rtidb::type::kDouble,
     ::rtidb::type::kDate, ::rtidb::type::kTimestamp, ::rtidb::type::kVarchar,
     ::rtidb::type::kString, ::rtidb::type::kBlob});

static constexpr std::array<uint32_t, 9> TYPE_SIZE_ARRAY = {
    0,
    sizeof(bool),      // kBool
    sizeof(int16_t),   // kSmallInt
    sizeof(int32_t),   // kInt
    sizeof(int64_t),   // kBigInt
    sizeof(float),     // kFloat
    sizeof(double),    // kDouble
    sizeof(int32_t),  // kDate
    sizeof(int64_t),   // kTimestamp
};

static inline uint8_t GetAddrLength(uint32_t size) {
    if (size <= UINT8_MAX) {
        return 1;
    } else if (size <= UINT16_MAX) {
        return 2;
    } else if (size <= UINT24_MAX) {
        return 3;
    } else {
        return 4;
    }
}

RowBuilder::RowBuilder(const Schema& schema)
    : schema_(schema),
      buf_(NULL),
      cnt_(0),
      size_(0),
      str_field_cnt_(0),
      str_addr_length_(0),
      str_field_start_offset_(0),
      str_offset_(0) {
    str_field_start_offset_ = HEADER_LENGTH + BitMapSize(schema.size());
    for (int idx = 0; idx < schema.size(); idx++) {
        const ::rtidb::common::ColumnDesc& column = schema.Get(idx);
        rtidb::type::DataType cur_type = column.data_type();
        if (cur_type == ::rtidb::type::kVarchar ||
            cur_type == ::rtidb::type::kString) {
            offset_vec_.push_back(str_field_cnt_);
            str_field_cnt_++;
        } else {
            if (cur_type < TYPE_SIZE_ARRAY.size() && cur_type > 0) {
                offset_vec_.push_back(str_field_start_offset_);
                str_field_start_offset_ += TYPE_SIZE_ARRAY[cur_type];
            } else if (cur_type == rtidb::type::kBlob) {
                offset_vec_.push_back(str_field_start_offset_);
                str_field_start_offset_ += sizeof(int64_t);
            } else {
                PDLOG(WARNING, "type is not supported");
            }
        }
    }
}

bool RowBuilder::SetBuffer(int8_t* buf, uint32_t size) {
    if (buf == NULL || size == 0 ||
        size < str_field_start_offset_ + str_field_cnt_) {
        return false;
    }
    buf_ = buf;
    size_ = size;
    *(buf_) = 1;      // FVersion
    *(buf_ + 1) = 1;  // SVersion
    *(reinterpret_cast<uint32_t*>(buf_ + VERSION_LENGTH)) = size;
    uint32_t bitmap_size = BitMapSize(schema_.size());
    memset(buf_ + HEADER_LENGTH, 0, bitmap_size);
    cnt_ = 0;
    str_addr_length_ = GetAddrLength(size);
    str_offset_ = str_field_start_offset_ + str_addr_length_ * str_field_cnt_;
    return true;
}

uint32_t RowBuilder::CalTotalLength(uint32_t string_length) {
    if (schema_.size() == 0) {
        return 0;
    }
    uint32_t total_length = str_field_start_offset_;
    total_length += string_length;
    if (total_length + str_field_cnt_ <= UINT8_MAX) {
        return total_length + str_field_cnt_;
    } else if (total_length + str_field_cnt_ * 2 <= UINT16_MAX) {
        return total_length + str_field_cnt_ * 2;
    } else if (total_length + str_field_cnt_ * 3 <= UINT24_MAX) {
        return total_length + str_field_cnt_ * 3;
    } else if (total_length + str_field_cnt_ * 4 <= UINT32_MAX) {
        return total_length + str_field_cnt_ * 4;
    }
    return 0;
}

bool RowBuilder::Check(uint32_t index, ::rtidb::type::DataType type) {
    if ((int32_t)index >= schema_.size()) {
        return false;
    }
    const ::rtidb::common::ColumnDesc& column = schema_.Get(index);
    if (column.data_type() != type) {
        return false;
    }
    if (TYPE_SET.find(column.data_type()) == TYPE_SET.end()) {
        return false;
    }
    return true;
}

bool RowBuilder::AppendDate(int32_t date) {
    if (!SetDate(cnt_, date)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetDate(uint32_t index, int32_t date) {
    if (!Check(index, ::rtidb::type::kDate)) return false;
    int8_t* ptr = buf_ + offset_vec_[index];
    *(reinterpret_cast<int32_t*>(ptr)) = date;
    return true;
}

bool RowBuilder::AppendDate(uint32_t year, uint32_t month, uint32_t day) {
    if (!SetDate(cnt_, year, month, day)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetDate(uint32_t index, uint32_t year, uint32_t month,
                         uint32_t day) {
    if (year < 1900 || year > 9999) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;
    if (!Check(index, ::rtidb::type::kDate)) return false;
    int8_t* ptr = buf_ + offset_vec_[index];
    int32_t data = (year - 1900) << 16;
    data = data | ((month - 1) << 8);
    data = data | day;
    *(reinterpret_cast<int32_t*>(ptr)) = data;
    return true;
}

bool RowBuilder::AppendNULL() {
    if (!SetNULL(cnt_)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetNULL(uint32_t index) {
    int8_t* ptr = buf_ + HEADER_LENGTH + (index >> 3);
    *(reinterpret_cast<uint8_t*>(ptr)) |= 1 << (index & 0x07);
    const ::rtidb::common::ColumnDesc& column = schema_.Get(index);
    if (column.data_type() == ::rtidb::type::kVarchar ||
        column.data_type() == rtidb::type::kString) {
        ptr = buf_ + str_field_start_offset_ +
              str_addr_length_ * offset_vec_[index];
        if (str_addr_length_ == 1) {
            *(reinterpret_cast<uint8_t*>(ptr)) = (uint8_t)str_offset_;
        } else if (str_addr_length_ == 2) {
            *(reinterpret_cast<uint16_t*>(ptr)) = (uint16_t)str_offset_;
        } else if (str_addr_length_ == 3) {
            *(reinterpret_cast<uint8_t*>(ptr)) = str_offset_ >> 16;
            *(reinterpret_cast<uint8_t*>(ptr + 1)) =
                (str_offset_ & 0xFF00) >> 8;
            *(reinterpret_cast<uint8_t*>(ptr + 2)) = str_offset_ & 0x00FF;
        } else {
            *(reinterpret_cast<uint32_t*>(ptr)) = str_offset_;
        }
    }
    return true;
}

bool RowBuilder::AppendBool(bool val) {
    if (!SetBool(cnt_, val)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetBool(uint32_t index, bool val) {
    if (!Check(index, ::rtidb::type::kBool)) return false;
    int8_t* ptr = buf_ + offset_vec_[index];
    *(reinterpret_cast<uint8_t*>(ptr)) = val ? 1 : 0;
    return true;
}

bool RowBuilder::AppendInt32(int32_t val) {
    if (!SetInt32(cnt_, val)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetInt32(uint32_t index, int32_t val) {
    if (!Check(index, ::rtidb::type::kInt)) return false;
    int8_t* ptr = buf_ + offset_vec_[index];
    *(reinterpret_cast<int32_t*>(ptr)) = val;
    return true;
}

bool RowBuilder::AppendInt16(int16_t val) {
    if (!SetInt16(cnt_, val)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetInt16(uint32_t index, int16_t val) {
    if (!Check(index, ::rtidb::type::kSmallInt)) return false;
    int8_t* ptr = buf_ + offset_vec_[index];
    *(reinterpret_cast<int16_t*>(ptr)) = val;
    return true;
}

bool RowBuilder::AppendTimestamp(int64_t val) {
    if (!SetTimestamp(cnt_, val)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetTimestamp(uint32_t index, int64_t val) {
    if (!Check(index, ::rtidb::type::kTimestamp)) return false;
    int8_t* ptr = buf_ + offset_vec_[index];
    *(reinterpret_cast<int64_t*>(ptr)) = val;
    return true;
}

bool RowBuilder::AppendInt64(int64_t val) {
    if (!SetInt64(cnt_, val)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetInt64(uint32_t index, int64_t val) {
    if (!Check(index, ::rtidb::type::kBigInt)) return false;
    int8_t* ptr = buf_ + offset_vec_[index];
    *(reinterpret_cast<int64_t*>(ptr)) = val;
    return true;
}

bool RowBuilder::AppendBlob(int64_t val) {
    if (!SetBlob(cnt_, val)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetBlob(uint32_t index, int64_t val) {
    if (!Check(index, ::rtidb::type::kBlob)) return false;
    int8_t* ptr = buf_ + offset_vec_[index];
    *(reinterpret_cast<int64_t*>(ptr)) = val;
    return true;
}

bool RowBuilder::AppendFloat(float val) {
    if (!SetFloat(cnt_, val)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetFloat(uint32_t index, float val) {
    if (!Check(index, ::rtidb::type::kFloat)) return false;
    int8_t* ptr = buf_ + offset_vec_[index];
    *(reinterpret_cast<float*>(ptr)) = val;
    return true;
}

bool RowBuilder::AppendDouble(double val) {
    if (!SetDouble(cnt_, val)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetDouble(uint32_t index, double val) {
    if (!Check(index, ::rtidb::type::kDouble)) return false;
    int8_t* ptr = buf_ + offset_vec_[index];
    *(reinterpret_cast<double*>(ptr)) = val;
    return true;
}

bool RowBuilder::AppendString(const char* val, uint32_t length) {
    if (!SetString(cnt_, val, length)) return false;
    cnt_++;
    return true;
}

bool RowBuilder::SetString(uint32_t index, const char* val, uint32_t length) {
    if (val == NULL || (!Check(index, ::rtidb::type::kVarchar) &&
                        !Check(index, rtidb::type::kString)))
        return false;
    if (str_offset_ + length > size_) return false;
    int8_t* ptr =
        buf_ + str_field_start_offset_ + str_addr_length_ * offset_vec_[index];
    if (str_addr_length_ == 1) {
        *(reinterpret_cast<uint8_t*>(ptr)) = (uint8_t)str_offset_;
    } else if (str_addr_length_ == 2) {
        *(reinterpret_cast<uint16_t*>(ptr)) = (uint16_t)str_offset_;
    } else if (str_addr_length_ == 3) {
        *(reinterpret_cast<uint8_t*>(ptr)) = str_offset_ >> 16;
        *(reinterpret_cast<uint8_t*>(ptr + 1)) = (str_offset_ & 0xFF00) >> 8;
        *(reinterpret_cast<uint8_t*>(ptr + 2)) = str_offset_ & 0x00FF;
    } else {
        *(reinterpret_cast<uint32_t*>(ptr)) = str_offset_;
    }
    if (length != 0) {
        memcpy(reinterpret_cast<char*>(buf_ + str_offset_), val, length);
    }
    str_offset_ += length;
    return true;
}

bool RowBuilder::AppendValue(const std::string& val) {
    bool ok = false;
    const ::rtidb::common::ColumnDesc& col = schema_.Get(cnt_);
    try {
        switch (col.data_type()) {
            case rtidb::type::kString:
            case rtidb::type::kVarchar:
                ok = AppendString(val.c_str(), val.length());
                break;
            case rtidb::type::kBool: {
                std::string b_val = val;
                std::transform(b_val.begin(), b_val.end(),
                               b_val.begin(), ::tolower);
                if (b_val == "true") {
                    ok = AppendBool(true);
                } else if (b_val == "false") {
                    ok = AppendBool(false);
                } else {
                    ok = false;
                }
                break;
            }
            case rtidb::type::kSmallInt:
                ok = AppendInt16(boost::lexical_cast<int16_t>(val));
                break;
            case rtidb::type::kInt:
                ok = AppendInt32(boost::lexical_cast<int32_t>(val));
                break;
            case rtidb::type::kBlob:
                ok = AppendBlob(boost::lexical_cast<int64_t>(val));
                break;
            case rtidb::type::kBigInt:
                ok = AppendInt64(boost::lexical_cast<int64_t>(val));
                break;
            case rtidb::type::kTimestamp:
                ok = AppendTimestamp(boost::lexical_cast<int64_t>(val));
                break;
            case rtidb::type::kFloat:
                ok = AppendFloat(boost::lexical_cast<float>(val));
                break;
            case rtidb::type::kDouble:
                ok = AppendDouble(boost::lexical_cast<double>(val));
                break;
            case rtidb::type::kDate: {
                std::vector<std::string> parts;
                ::rtidb::base::SplitString(val, "-", parts);
                if (parts.size() != 3) {
                    ok = false;
                    break;
                }
                uint32_t year = boost::lexical_cast<uint32_t>(parts[0]);
                uint32_t mon = boost::lexical_cast<uint32_t>(parts[1]);
                uint32_t day = boost::lexical_cast<uint32_t>(parts[2]);
                ok = AppendDate(year, mon, day);
                break;
            }
            default:
                ok = false;
        }
    } catch (std::exception const& e) {
        ok = false;
    }
    return ok;
}

RowView::RowView(const Schema& schema)
    : str_addr_length_(0),
      is_valid_(true),
      string_field_cnt_(0),
      str_field_start_offset_(0),
      size_(0),
      row_(NULL),
      schema_(schema),
      offset_vec_() {
    Init();
}

RowView::RowView(const Schema& schema, const int8_t* row, uint32_t size)
    : str_addr_length_(0),
      is_valid_(true),
      string_field_cnt_(0),
      str_field_start_offset_(0),
      size_(size),
      row_(row),
      schema_(schema),
      offset_vec_() {
    if (schema_.size() == 0) {
        is_valid_ = false;
        return;
    }
    if (Init()) {
        Reset(row, size);
    }
}

bool RowView::Init() {
    uint32_t offset = HEADER_LENGTH + BitMapSize(schema_.size());
    for (int idx = 0; idx < schema_.size(); idx++) {
        const ::rtidb::common::ColumnDesc& column = schema_.Get(idx);
        rtidb::type::DataType cur_type = column.data_type();
        if (cur_type == ::rtidb::type::kVarchar ||
            cur_type == ::rtidb::type::kString) {
            offset_vec_.push_back(string_field_cnt_);
            string_field_cnt_++;
        } else {
            if (cur_type < TYPE_SIZE_ARRAY.size() && cur_type > 0) {
                offset_vec_.push_back(offset);
                offset += TYPE_SIZE_ARRAY[cur_type];
            } else if (cur_type == rtidb::type::kBlob) {
                offset_vec_.push_back(offset);
                offset += sizeof(int64_t);
            } else {
                is_valid_ = false;
                return false;
            }
        }
    }
    str_field_start_offset_ = offset;
    return true;
}

bool RowView::Reset(const int8_t* row, uint32_t size) {
    if (schema_.size() == 0 || row == NULL || size <= HEADER_LENGTH ||
        *(reinterpret_cast<const uint32_t*>(row + VERSION_LENGTH)) != size) {
        is_valid_ = false;
        return false;
    }
    row_ = row;
    size_ = size;
    str_addr_length_ = GetAddrLength(size_);
    return true;
}

bool RowView::Reset(const int8_t* row) {
    if (schema_.size() == 0 || row == NULL) {
        is_valid_ = false;
        return false;
    }
    row_ = row;
    size_ = *(reinterpret_cast<const uint32_t*>(row + VERSION_LENGTH));
    if (size_ <= HEADER_LENGTH) {
        is_valid_ = false;
        return false;
    }
    str_addr_length_ = GetAddrLength(size_);
    return true;
}

bool RowView::CheckValid(uint32_t idx, ::rtidb::type::DataType type) {
    if (row_ == NULL || !is_valid_) {
        return false;
    }
    if ((int32_t)idx >= schema_.size()) {
        return false;
    }
    const ::rtidb::common::ColumnDesc& column = schema_.Get(idx);
    if (column.data_type() != type) {
        return false;
    }
    return true;
}

int32_t RowView::GetBool(uint32_t idx, bool* val) {
    if (val == NULL) {
        return -1;
    }
    if (!CheckValid(idx, ::rtidb::type::kBool)) {
        return -1;
    }
    if (IsNULL(row_, idx)) {
        return 1;
    }
    uint32_t offset = offset_vec_.at(idx);
    int8_t v = v1::GetBoolField(row_, offset);
    if (v == 1) {
        *val = true;
    } else {
        *val = false;
    }
    return 0;
}

int32_t RowView::GetDate(uint32_t idx, uint32_t* year, uint32_t* month,
                         uint32_t* day) {
    if (year == NULL || month == NULL || day == NULL) {
        return -1;
    }
    if (!CheckValid(idx, ::rtidb::type::kDate)) {
        return -1;
    }
    if (IsNULL(row_, idx)) {
        return 1;
    }
    uint32_t offset = offset_vec_.at(idx);
    int32_t date = static_cast<int32_t>(v1::GetInt32Field(row_, offset));
    *day = date & 0x0000000FF;
    date = date >> 8;
    *month = 1 + (date & 0x0000FF);
    *year = 1900 + (date >> 8);
    return 0;
}

int32_t RowView::GetDate(uint32_t idx, int32_t* val) {
    if (val == NULL) {
        return -1;
    }
    if (!CheckValid(idx, ::rtidb::type::kDate)) {
        return -1;
    }
    if (IsNULL(row_, idx)) {
        return 1;
    }
    uint32_t offset = offset_vec_.at(idx);
    *val = static_cast<int32_t>(v1::GetInt32Field(row_, offset));
    return 0;
}

int32_t RowView::GetInt32(uint32_t idx, int32_t* val) {
    if (val == NULL) {
        return -1;
    }
    if (!CheckValid(idx, ::rtidb::type::kInt)) {
        return -1;
    }
    if (IsNULL(row_, idx)) {
        return 1;
    }
    uint32_t offset = offset_vec_.at(idx);
    *val = v1::GetInt32Field(row_, offset);
    return 0;
}

int32_t RowView::GetTimestamp(uint32_t idx, int64_t* val) {
    if (val == NULL) {
        return -1;
    }
    if (!CheckValid(idx, ::rtidb::type::kTimestamp)) {
        return -1;
    }
    if (IsNULL(row_, idx)) {
        return 1;
    }
    uint32_t offset = offset_vec_.at(idx);
    *val = v1::GetInt64Field(row_, offset);
    return 0;
}

int32_t RowView::GetInt64(uint32_t idx, int64_t* val) {
    if (val == NULL) {
        return -1;
    }
    if (!CheckValid(idx, ::rtidb::type::kBigInt)) {
        return -1;
    }
    if (IsNULL(row_, idx)) {
        return 1;
    }
    uint32_t offset = offset_vec_.at(idx);
    *val = v1::GetInt64Field(row_, offset);
    return 0;
}

int32_t RowView::GetBlob(uint32_t idx, int64_t* val) {
    if (val == NULL) {
        return -1;
    }
    if (!CheckValid(idx, ::rtidb::type::kBlob)) {
        return -1;
    }
    if (IsNULL(row_, idx)) {
        return 1;
    }
    uint32_t offset = offset_vec_.at(idx);
    *val = v1::GetInt64Field(row_, offset);
    return 0;
}

int32_t RowView::GetInt16(uint32_t idx, int16_t* val) {
    if (val == NULL) {
        return -1;
    }
    if (!CheckValid(idx, ::rtidb::type::kSmallInt)) {
        return -1;
    }
    if (IsNULL(row_, idx)) {
        return 1;
    }
    uint32_t offset = offset_vec_.at(idx);
    *val = v1::GetInt16Field(row_, offset);
    return 0;
}

int32_t RowView::GetFloat(uint32_t idx, float* val) {
    if (val == NULL) {
        return -1;
    }
    if (!CheckValid(idx, ::rtidb::type::kFloat)) {
        return -1;
    }
    if (IsNULL(row_, idx)) {
        return 1;
    }
    uint32_t offset = offset_vec_.at(idx);
    *val = v1::GetFloatField(row_, offset);
    return 0;
}

int32_t RowView::GetDouble(uint32_t idx, double* val) {
    if (val == NULL) {
        return -1;
    }
    if (!CheckValid(idx, ::rtidb::type::kDouble)) {
        return -1;
    }
    if (IsNULL(row_, idx)) {
        return 1;
    }
    uint32_t offset = offset_vec_.at(idx);
    *val = v1::GetDoubleField(row_, offset);
    return 0;
}

int32_t RowView::GetInteger(const int8_t* row, uint32_t idx,
                            ::rtidb::type::DataType type, int64_t* val) {
    int32_t ret = 0;
    switch (type) {
        case ::rtidb::type::kSmallInt: {
            int16_t tmp_val = 0;
            ret = GetValue(row, idx, type, &tmp_val);
            if (ret == 0) *val = tmp_val;
            break;
        }
        case ::rtidb::type::kInt: {
            int32_t tmp_val = 0;
            GetValue(row, idx, type, &tmp_val);
            if (ret == 0) *val = tmp_val;
            break;
        }
        case ::rtidb::type::kTimestamp:
        case ::rtidb::type::kBlob:
        case ::rtidb::type::kBigInt: {
            int64_t tmp_val = 0;
            GetValue(row, idx, type, &tmp_val);
            if (ret == 0) *val = tmp_val;
            break;
        }
        default:
            return -1;
    }
    return ret;
}

int32_t RowView::GetValue(const int8_t* row, uint32_t idx,
                          ::rtidb::type::DataType type, void* val) {
    if (schema_.size() == 0 || row == NULL) {
        return -1;
    }
    if ((int32_t)idx >= schema_.size()) {
        return -1;
    }
    const ::rtidb::common::ColumnDesc& column = schema_.Get(idx);
    if (column.data_type() != type) {
        return -1;
    }
    if (GetSize(row) <= HEADER_LENGTH) {
        return -1;
    }
    if (IsNULL(row, idx)) {
        return 1;
    }
    uint32_t offset = offset_vec_.at(idx);
    switch (type) {
        case ::rtidb::type::kBool: {
            int8_t v = v1::GetBoolField(row, offset);
            if (v == 1) {
                *(reinterpret_cast<bool*>(val)) = true;
            } else {
                *(reinterpret_cast<bool*>(val)) = false;
            }
            break;
        }
        case ::rtidb::type::kSmallInt:
            *(reinterpret_cast<int16_t*>(val)) = v1::GetInt16Field(row, offset);
            break;
        case ::rtidb::type::kInt:
            *(reinterpret_cast<int32_t*>(val)) = v1::GetInt32Field(row, offset);
            break;
        case ::rtidb::type::kTimestamp:
        case ::rtidb::type::kBlob:
        case ::rtidb::type::kBigInt:
            *(reinterpret_cast<int64_t*>(val)) = v1::GetInt64Field(row, offset);
            break;
        case ::rtidb::type::kFloat:
            *(reinterpret_cast<float*>(val)) = v1::GetFloatField(row, offset);
            break;
        case ::rtidb::type::kDouble:
            *(reinterpret_cast<double*>(val)) = v1::GetDoubleField(row, offset);
            break;
        case ::rtidb::type::kDate:
            *(reinterpret_cast<int32_t*>(val)) =
                static_cast<int32_t>(v1::GetInt32Field(row, offset));
            break;
        default:
            return -1;
    }
    return 0;
}

int32_t RowView::GetValue(const int8_t* row, uint32_t idx, char** val,
                          uint32_t* length) {
    if (schema_.size() == 0 || row == NULL || length == NULL) {
        return -1;
    }
    if ((int32_t)idx >= schema_.size()) {
        return -1;
    }
    const ::rtidb::common::ColumnDesc& column = schema_.Get(idx);
    if (column.data_type() != ::rtidb::type::kVarchar &&
        column.data_type() != ::rtidb::type::kString) {
        return -1;
    }
    uint32_t size = GetSize(row);
    if (size <= HEADER_LENGTH) {
        return -1;
    }
    if (IsNULL(row, idx)) {
        return 1;
    }
    uint32_t field_offset = offset_vec_.at(idx);
    uint32_t next_str_field_offset = 0;
    if (offset_vec_.at(idx) < string_field_cnt_ - 1) {
        next_str_field_offset = field_offset + 1;
    }
    return v1::GetStrField(row, field_offset, next_str_field_offset,
                           str_field_start_offset_, GetAddrLength(size),
                           reinterpret_cast<int8_t**>(val), length);
}

int32_t RowView::GetString(uint32_t idx, char** val, uint32_t* length) {
    if (val == NULL || length == NULL) {
        return -1;
    }

    if (!CheckValid(idx, ::rtidb::type::kVarchar) &&
        !CheckValid(idx, rtidb::type::kString)) {
        return -1;
    }
    if (IsNULL(row_, idx)) {
        return 1;
    }
    uint32_t field_offset = offset_vec_.at(idx);
    uint32_t next_str_field_offset = 0;
    if (offset_vec_.at(idx) < string_field_cnt_ - 1) {
        next_str_field_offset = field_offset + 1;
    }
    return v1::GetStrField(row_, field_offset, next_str_field_offset,
                           str_field_start_offset_, str_addr_length_,
                           reinterpret_cast<int8_t**>(val), length);
}

int32_t RowView::GetStrValue(uint32_t idx, std::string* val) {
    return GetStrValue(row_, idx, val);
}

int32_t RowView::GetStrValue(const int8_t* row, uint32_t idx,
                             std::string* val) {
    if (schema_.size() == 0 || row == NULL) {
        return -1;
    }
    if ((int32_t)idx >= schema_.size()) {
        return -1;
    }
    const ::rtidb::common::ColumnDesc& column = schema_.Get(idx);
    if (GetSize(row) <= HEADER_LENGTH) {
        return -1;
    }
    if (IsNULL(row, idx)) {
        val->assign("null");
        return 1;
    }
    switch (column.data_type()) {
        case ::rtidb::type::kBool: {
            bool value = false;
            GetValue(row, idx, ::rtidb::type::kBool, &value);
            value == true ? val->assign("true") : val->assign("false");
            break;
        }
        case ::rtidb::type::kSmallInt:
        case ::rtidb::type::kInt:
        case ::rtidb::type::kTimestamp:
        case ::rtidb::type::kBlob:
        case ::rtidb::type::kBigInt: {
            int64_t value = 0;
            GetInteger(row, idx, column.data_type(), &value);
            val->assign(std::to_string(value));
            break;
        }
        case ::rtidb::type::kFloat: {
            float value = 0.0;
            GetValue(row, idx, ::rtidb::type::kFloat, &value);
            val->assign(std::to_string(value));
            break;
        }
        case ::rtidb::type::kDouble: {
            double value = 0.0;
            GetValue(row, idx, ::rtidb::type::kDouble, &value);
            val->assign(std::to_string(value));
            break;
        }
        case ::rtidb::type::kDate: {
            uint32_t year = 0;
            uint32_t month = 0;
            uint32_t day = 0;
            int32_t date = 0;
            GetValue(row, idx, ::rtidb::type::kDate, &date);
            day = date & 0x0000000FF;
            date = date >> 8;
            month = 1 + (date & 0x0000FF);
            year = 1900 + (date >> 8);
            std::stringstream ss;
            ss << year << "-" << month << "-" << day;
            val->assign(ss.str());
            break;
        }
        case ::rtidb::type::kVarchar:
        case ::rtidb::type::kString: {
            char* ch = NULL;
            uint32_t size = 0;
            GetValue(row, idx, &ch, &size);
            std::string tmp(ch, size);
            val->swap(tmp);
            break;
        }
        default: {
            val->assign("-");
            return -1;
        }
    }
    return 0;
}

namespace v1 {
int32_t GetStrField(const int8_t* row, uint32_t field_offset,
                    uint32_t next_str_field_offset, uint32_t str_start_offset,
                    uint32_t addr_space, int8_t** data, uint32_t* size) {
    if (row == NULL || data == NULL || size == NULL) return -1;
    const int8_t* row_with_offset = row + str_start_offset;
    uint32_t str_offset = 0;
    uint32_t next_str_offset = 0;
    switch (addr_space) {
        case 1: {
            str_offset =
                (uint8_t)(*(row_with_offset + field_offset * addr_space));
            if (next_str_field_offset > 0) {
                next_str_offset = (uint8_t)(
                    *(row_with_offset + next_str_field_offset * addr_space));
            }
            break;
        }
        case 2: {
            str_offset = *(reinterpret_cast<const uint16_t*>(
                row_with_offset + field_offset * addr_space));
            if (next_str_field_offset > 0) {
                next_str_offset = *(reinterpret_cast<const uint16_t*>(
                    row_with_offset + next_str_field_offset * addr_space));
            }
            break;
        }
        case 3: {
            const int8_t* cur_row_with_offset =
                row_with_offset + field_offset * addr_space;
            str_offset = (uint8_t)(*cur_row_with_offset);
            str_offset =
                (str_offset << 8) + (uint8_t)(*(cur_row_with_offset + 1));
            str_offset =
                (str_offset << 8) + (uint8_t)(*(cur_row_with_offset + 2));
            if (next_str_field_offset > 0) {
                const int8_t* next_row_with_offset =
                    row_with_offset + next_str_field_offset * addr_space;
                next_str_offset = (uint8_t)(*(next_row_with_offset));
                next_str_offset = (next_str_offset << 8) +
                                  (uint8_t)(*(next_row_with_offset + 1));
                next_str_offset = (next_str_offset << 8) +
                                  (uint8_t)(*(next_row_with_offset + 2));
            }
            break;
        }
        case 4: {
            str_offset = *(reinterpret_cast<const uint32_t*>(
                row_with_offset + field_offset * addr_space));
            if (next_str_field_offset > 0) {
                next_str_offset = *(reinterpret_cast<const uint32_t*>(
                    row_with_offset + next_str_field_offset * addr_space));
            }
            break;
        }
        default: {
            return -2;
        }
    }
    const int8_t* ptr = row + str_offset;
    *data = (int8_t*)(ptr);  // NOLINT
    if (next_str_field_offset <= 0) {
        uint32_t total_length =
            *(reinterpret_cast<const uint32_t*>(row + VERSION_LENGTH));
        *size = total_length - str_offset;
    } else {
        *size = next_str_offset - str_offset;
    }
    return 0;
}

}  // namespace v1

RowProject::RowProject(const Schema& schema, const ProjectList& plist)
    : schema_(schema),
      plist_(plist),
      output_schema_(),
      row_builder_(NULL),
      row_view_(NULL) {}

RowProject::~RowProject() {
    delete row_builder_;
    delete row_view_;
}

bool RowProject::Init() {
    if (plist_.size() <= 0) return false;
    for (int32_t i = 0; i < plist_.size(); i++) {
        uint32_t idx = plist_.Get(i);
        if (idx >= (uint32_t)schema_.size()) {
            PDLOG(WARNING, "index %u out of schema size %d", idx,
                  schema_.size());
            return false;
        }
        const ::rtidb::common::ColumnDesc& column = schema_.Get(idx);
        output_schema_.Add()->CopyFrom(column);
    }
    row_builder_ = new RowBuilder(output_schema_);
    row_view_ = new RowView(schema_);
    return true;
}

bool RowProject::Project(const int8_t* row_ptr, uint32_t size,
                         int8_t** output_ptr, uint32_t* out_size) {
    if (row_ptr == NULL || output_ptr == NULL || out_size == NULL) return false;
    bool ok = row_view_->Reset(row_ptr, size);
    if (!ok) return false;
    uint32_t str_size = 0;
    for (int32_t i = 0; i < plist_.size(); i++) {
        uint32_t idx = plist_.Get(i);
        const ::rtidb::common::ColumnDesc& column = schema_.Get(idx);
        if (column.data_type() == ::rtidb::type::kVarchar ||
            column.data_type() == ::rtidb::type::kString) {
            if (row_view_->IsNULL(idx)) continue;
            uint32_t length = 0;
            char* content = nullptr;
            int32_t ret = row_view_->GetString(idx, &content, &length);
            if (ret != 0) {
                return false;
            }
            str_size += length;
        }
    }
    uint32_t total_size = row_builder_->CalTotalLength(str_size);
    char* ptr = new char[total_size];
    row_builder_->SetBuffer(reinterpret_cast<int8_t*>(ptr), total_size);
    for (int32_t i = 0; i < plist_.size(); i++) {
        uint32_t idx = plist_.Get(i);
        const ::rtidb::common::ColumnDesc& column = schema_.Get(idx);
        int32_t ret = 0;
        if (row_view_->IsNULL(idx)) {
            row_builder_->AppendNULL();
            continue;
        }
        switch (column.data_type()) {
            case ::rtidb::type::kBool: {
                bool val = false;
                ret = row_view_->GetBool(idx, &val);
                if (ret == 0) row_builder_->AppendBool(val);
                break;
            }
            case ::rtidb::type::kSmallInt: {
                int16_t val = 0;
                ret = row_view_->GetInt16(idx, &val);
                if (ret == 0) row_builder_->AppendInt16(val);
                break;
            }
            case ::rtidb::type::kInt: {
                int32_t val = 0;
                ret = row_view_->GetInt32(idx, &val);
                if (ret == 0) row_builder_->AppendInt32(val);
                break;
            }
            case ::rtidb::type::kDate: {
                int32_t val = 0;
                ret = row_view_->GetDate(idx, &val);
                if (ret == 0) row_builder_->AppendDate(val);
                break;
            }
            case ::rtidb::type::kBlob: {
                int64_t val = 0;
                ret = row_view_->GetBlob(idx, &val);
                if (ret == 0) row_builder_->AppendBlob(val);
                break;
            }
            case ::rtidb::type::kBigInt: {
                int64_t val = 0;
                ret = row_view_->GetInt64(idx, &val);
                if (ret == 0) row_builder_->AppendInt64(val);
                break;
            }
            case ::rtidb::type::kTimestamp: {
                int64_t val = 0;
                ret = row_view_->GetTimestamp(idx, &val);
                if (ret == 0) row_builder_->AppendTimestamp(val);
                break;
            }
            case ::rtidb::type::kFloat: {
                float val = 0;
                ret = row_view_->GetFloat(idx, &val);
                if (ret == 0) row_builder_->AppendFloat(val);
                break;
            }
            case ::rtidb::type::kDouble: {
                double val = 0;
                ret = row_view_->GetDouble(idx, &val);
                if (ret == 0) row_builder_->AppendDouble(val);
                break;
            }
            case ::rtidb::type::kString:
            case ::rtidb::type::kVarchar: {
                char* val = NULL;
                uint32_t size = 0;
                ret = row_view_->GetString(idx, &val, &size);
                if (ret == 0) row_builder_->AppendString(val, size);
                break;
            }
            default: {
                PDLOG(WARNING, "not supported type");
            }
        }
        if (ret != 0) {
            delete[] ptr;
            PDLOG(WARNING, "fail to project column %s with idx %u",
                  column.name().c_str(), idx);
            return false;
        }
    }
    *output_ptr = reinterpret_cast<int8_t*>(ptr);
    *out_size = total_size;
    return true;
}

}  // namespace codec
}  // namespace rtidb