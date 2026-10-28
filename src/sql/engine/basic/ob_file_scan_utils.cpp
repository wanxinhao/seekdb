/**
 * (c) 2025 OceanBase
 * This file contains utilities for reading local files as tables via SQL.
 * It implements option parsing, deterministic type inference, and change detection.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_scan_utils.h"

#include <sys/stat.h>
#include <stdlib.h>

#include "lib/allocator/ob_malloc.h"
#include "lib/charset/ob_charset.h"
#include "lib/file/ob_file.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
namespace sql
{

using namespace common;

namespace
{

/// read a whole local file into a buffer; used by schema inference only
int read_whole_file(const ObString &file_url, ObArenaAllocator &allocator, ObString &content)
{
  int ret = OB_SUCCESS;
  const int64_t file_size = get_file_size(file_url.ptr());
  if (file_size < 0) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_FILE_NOT_EXIST, file_url.length(), file_url.ptr());
  } else if (false) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("invalid file size", K(file_size), K(file_url));
  } else {
    char *buf = NULL;
    if (file_size == 0) {
      buf = static_cast<char *>(allocator.alloc(1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(file_size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ObFileReader reader;
      int64_t read_size = 0;
      if (OB_FAIL(reader.open(file_url, false))) {
        ret = OB_FILE_NOT_EXIST;
        LOG_USER_ERROR(OB_FILE_NOT_EXIST, file_url.length(), file_url.ptr());
      } else if (OB_FAIL(reader.pread(buf, file_size, 0, read_size))) {
        ret = OB_IO_ERROR;
        LOG_WARN("failed to read file", K(ret), K(file_url));
      } else {
        content.assign(buf, read_size);
      }
      reader.close();
    }
  }
  return ret;
}

/// compare a string with a literal, ignoring the case
inline bool is_equal_ignore_case(const ObString &value, const char *literal)
{
  const int64_t len = strlen(literal);
  return value.length() == len && 0 == strncasecmp(value.ptr(), literal, len);
}

/// check whether a value is a valid decimal integer
bool is_integer_literal(const ObString &value)
{
  if (value.length() == 0) {
    return false;
  }
  int64_t i = (value.ptr()[0] == '-' || value.ptr()[0] == '+') ? 1 : 0;
  if (i >= value.length()) {
    return false;
  }
  for (; i < value.length(); i++) {
    if (value.ptr()[i] < '0' || value.ptr()[i] > '9') {
      return false;
    }
  }
  return true;
}

/// check whether a value is a valid floating point number
bool is_double_literal(const ObString &value)
{
  if (value.length() == 0) {
    return false;
  }
  int64_t i = (value.ptr()[0] == '-' || value.ptr()[0] == '+') ? 1 : 0;
  bool has_digit = false;
  bool has_dot = false;
  bool has_exp = false;
  for (; i < value.length(); i++) {
    const char c = value.ptr()[i];
    if (c >= '0' && c <= '9') {
      has_digit = true;
    } else if (c == '.' && !has_dot && !has_exp) {
      has_dot = true;
    } else if ((c == 'e' || c == 'E') && has_digit && !has_exp) {
      has_exp = true;
      has_digit = false;
    } else {
      return false;
    }
  }
  return has_digit;
}

/// check whether a value is a boolean literal
bool is_boolean_literal(const ObString &value)
{
  return (value.length() == 4 && 0 == strncasecmp(value.ptr(), "true", 4))
         || (value.length() == 5 && 0 == strncasecmp(value.ptr(), "false", 5));
}

/// check whether a value looks like a date, i.e. YYYY-MM-DD
bool is_date_literal(const ObString &value)
{
  if (value.length() != 10) {
    return false;
  }
  return value.ptr()[4] == '-' && value.ptr()[7] == '-'
         && is_integer_literal(ObString(4, value.ptr()))
         && is_integer_literal(ObString(2, value.ptr() + 5))
         && is_integer_literal(ObString(2, value.ptr() + 8));
}

/// check whether a value looks like a datetime, i.e. YYYY-MM-DD HH:MM:SS
bool is_datetime_literal(const ObString &value)
{
  // the separator between the date and the time part may be a space or a 'T'
  if (value.length() < 19) {
    return false;
  }
  const char sep = value.ptr()[10];
  if (sep != ' ' && sep != 'T') {
    return false;
  }
  if (!is_date_literal(ObString(10, value.ptr()))) {
    return false;
  }
  return is_integer_literal(ObString(2, value.ptr() + 11))
         && value.ptr()[13] == ':'
         && is_integer_literal(ObString(2, value.ptr() + 14))
         && value.ptr()[16] == ':'
         && is_integer_literal(ObString(2, value.ptr() + 17));
}

/// find a named option in the argument list
int find_named_arg(const ObIArray<ObFileSqlArg> &args, const char *name, const ObFileSqlArg *&arg)
{
  int ret = OB_SUCCESS;
  arg = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < args.count(); i++) {
    if (args.at(i).name_.length() > 0 && is_equal_ignore_case(args.at(i).name_, name)) {
      if (NULL != arg) {
        ret = OB_ERR_PARSE_SQL;
        LOG_USER_ERROR(OB_ERR_PARSE_SQL, "duplicate option");
      } else {
        arg = &args.at(i);
      }
    }
  }
  return ret;
}

/// copy a string option value into the allocator
int dup_string_arg(const ObFileSqlArg &arg, ObArenaAllocator &allocator, ObString &value)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  if (arg.str_value_.length() > 0) {
    if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(arg.str_value_.length())))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, arg.str_value_.ptr(), arg.str_value_.length());
      value.assign(buf, arg.str_value_.length());
    }
  }
  return ret;
}

/// resolve the compression option
int parse_compression_arg(const ObString &value, ObCSVGeneralFormat::ObCSVCompression &compression)
{
  int ret = OB_SUCCESS;
  if (is_equal_ignore_case(value, "none")) {
    compression = ObCSVGeneralFormat::ObCSVCompression::NONE;
  } else if (is_equal_ignore_case(value, "gzip")) {
    compression = ObCSVGeneralFormat::ObCSVCompression::GZIP;
  } else if (is_equal_ignore_case(value, "deflate")) {
    compression = ObCSVGeneralFormat::ObCSVCompression::DEFLATE;
  } else if (is_equal_ignore_case(value, "zstd")) {
    compression = ObCSVGeneralFormat::ObCSVCompression::ZSTD;
  } else if (is_equal_ignore_case(value, "auto")) {
    compression = ObCSVGeneralFormat::ObCSVCompression::AUTO;
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, value.length(), value.ptr());
  }
  return ret;
}

/// resolve the encoding option
int parse_encoding_arg(const ObString &value, ObCharsetType &cs_type)
{
  int ret = OB_SUCCESS;
  const ObCharsetType type = ObCharset::charset_type(value.ptr());
  if (CHARSET_INVALID == type) {
    ret = OB_ERR_UNKNOWN_CHARSET;
    LOG_USER_ERROR(OB_ERR_UNKNOWN_CHARSET, value.length(), value.ptr());
  } else {
    cs_type = type;
  }
  return ret;
}

} // namespace

int ObFileSqlUtils::get_format_type(const ObString &func_name, ObFileScanFormat &type)
{
  int ret = OB_SUCCESS;
  if (is_equal_ignore_case(func_name, "read_csv")) {
    type = ObFileScanFormat::CSV;
  } else if (is_equal_ignore_case(func_name, "read_json") || is_equal_ignore_case(func_name, "read_ndjson")) {
    type = ObFileScanFormat::JSON;
  } else if (is_equal_ignore_case(func_name, "read_parquet")) {
    type = ObFileScanFormat::PARQUET;
  } else if (is_equal_ignore_case(func_name, "glob")) {
    type = ObFileScanFormat::GLOB;
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, func_name.length(), func_name.ptr());
  }
  return ret;
}

int ObFileSqlUtils::build_csv_format(const ObIArray<ObFileSqlArg> &args,
                                     ObArenaAllocator &allocator,
                                     ObCSVGeneralFormat &csv_format)
{
  int ret = OB_SUCCESS;
  const ObFileSqlArg *arg = NULL;
  // DuckDB defaults: comma separated, double quoted fields, backslash escape, header row present
  csv_format.field_term_str_ = ObString(1, ",");
  csv_format.line_term_str_ = ObString(1, "\n");
  csv_format.field_enclosed_char_ = '"';
  csv_format.field_escaped_char_ = '\\';
  csv_format.cs_type_ = CHARSET_UTF8MB4;
  csv_format.empty_field_as_null_ = true;
  csv_format.skip_blank_lines_ = true;

  if (OB_FAIL(find_named_arg(args, "header", arg))) {
  } else if (NULL != arg) {
    csv_format.parse_header_ = (arg->kind_ == ObFileSqlArg::BOOL) ? arg->int_value_ != 0 : false;
    csv_format.skip_header_lines_ = csv_format.parse_header_ ? 1 : 0;
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "delimiter", arg))) {
  } else if (NULL != arg) {
    if (OB_FAIL(dup_string_arg(*arg, allocator, csv_format.field_term_str_))) {
    } else if (csv_format.field_term_str_.length() != 1) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "delimiter must be a single character");
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "quote", arg))) {
  } else if (NULL != arg) {
    ObString quote;
    if (OB_FAIL(dup_string_arg(*arg, allocator, quote))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      csv_format.field_enclosed_char_ = quote.length() == 1 ? quote.ptr()[0] : INT64_MAX;
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "escape", arg))) {
  } else if (NULL != arg) {
    ObString escape;
    if (OB_FAIL(dup_string_arg(*arg, allocator, escape))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      csv_format.field_escaped_char_ = escape.length() == 1 ? escape.ptr()[0] : INT64_MAX;
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "null", arg))) {
  } else if (NULL != arg) {
    ObString null_str;
    if (OB_FAIL(dup_string_arg(*arg, allocator, null_str))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(csv_format.null_if_.allocate_array(allocator, 1))) {
    } else {
      csv_format.null_if_.at(0) = null_str;
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "compression", arg))) {
  } else if (NULL != arg) {
    if (OB_FAIL(parse_compression_arg(arg->str_value_, csv_format.compression_algorithm_))) {
    } else if (csv_format.compression_algorithm_ == ObCSVGeneralFormat::ObCSVCompression::AUTO) {
      csv_format.compression_algorithm_ = ObCSVGeneralFormat::ObCSVCompression::NONE;
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "encoding", arg))) {
  } else if (NULL != arg) {
    if (OB_FAIL(parse_encoding_arg(arg->str_value_, csv_format.cs_type_))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "skip_blank_lines", arg))) {
  } else if (NULL != arg) {
    csv_format.skip_blank_lines_ = (arg->kind_ == ObFileSqlArg::BOOL) ? arg->int_value_ != 0 : false;
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "trim_space", arg))) {
  } else if (NULL != arg) {
    csv_format.trim_space_ = (arg->kind_ == ObFileSqlArg::BOOL) ? arg->int_value_ != 0 : false;
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "sample_size", arg))) {
  } else if (NULL != arg) {
    // only a full scan is supported, so the sample size must be -1
    if (arg->kind_ != ObFileSqlArg::INT || arg->int_value_ != -1) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size");
    }
  }
  return ret;
}

int ObFileSqlUtils::build_json_format(const ObIArray<ObFileSqlArg> &args,
                                      ObArenaAllocator &allocator,
                                      ObCSVGeneralFormat &json_format)
{
  int ret = OB_SUCCESS;
  const ObFileSqlArg *arg = NULL;
  json_format.line_term_str_ = ObString(1, "\n");
  json_format.cs_type_ = CHARSET_UTF8MB4;

  if (OB_FAIL(find_named_arg(args, "sample_size", arg))) {
  } else if (NULL != arg) {
    // only a full scan is supported, so the sample size must be -1
    if (arg->kind_ != ObFileSqlArg::INT || arg->int_value_ != -1) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size");
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "compression", arg))) {
  } else if (NULL != arg) {
    if (OB_FAIL(parse_compression_arg(arg->str_value_, json_format.compression_algorithm_))) {
    } else if (json_format.compression_algorithm_ == ObCSVGeneralFormat::ObCSVCompression::AUTO) {
      json_format.compression_algorithm_ = ObCSVGeneralFormat::ObCSVCompression::NONE;
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "encoding", arg))) {
  } else if (NULL != arg) {
    if (OB_FAIL(parse_encoding_arg(arg->str_value_, json_format.cs_type_))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(find_named_arg(args, "format", arg))) {
  } else if (NULL != arg) {
    if (!is_equal_ignore_case(arg->str_value_, "newline_delimited") && !is_equal_ignore_case(arg->str_value_, "array")) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, arg->str_value_.length(), arg->str_value_.ptr());
    }
  }
  return ret;
}

bool ObFileSqlUtils::ColInfer::feed_field(const ObString &value, const ObCharsetType cs_type)
{
  (void)cs_type;
  const State old_state = state_;
  if (value.length() == 0) {
    // a NULL value does not narrow the type
    return false;
  }
  if (state_ == ST_NULL) {
    if (is_boolean_literal(value)) {
      state_ = ST_BOOLEAN;
    } else if (is_integer_literal(value)) {
      state_ = ST_INT;
      const int64_t v = strtoll(value.ptr(), nullptr, 10);
      bigint_min_ = std::min(bigint_min_, v);
      bigint_max_ = std::max(bigint_max_, v);
    } else if (is_double_literal(value)) {
      state_ = ST_DOUBLE;
    } else if (is_date_literal(value)) {
      state_ = ST_DATE;
    } else if (is_datetime_literal(value)) {
      state_ = ST_DATETIME;
    } else {
      state_ = ST_VARCHAR;
      max_varchar_len_ = value.length();
    }
  } else if (state_ == ST_BOOLEAN) {
    // a boolean mixed with anything else becomes a string
    if (!is_boolean_literal(value)) {
      state_ = ST_VARCHAR;
      max_varchar_len_ = value.length();
    }
  } else if (state_ == ST_INT) {
    if (is_integer_literal(value)) {
      const int64_t v = strtoll(value.ptr(), nullptr, 10);
      bigint_min_ = std::min(bigint_min_, v);
      bigint_max_ = std::max(bigint_max_, v);
    } else if (is_double_literal(value)) {
      state_ = ST_DOUBLE;
    } else {
      state_ = ST_VARCHAR;
      max_varchar_len_ = value.length();
    }
  } else if (state_ == ST_DOUBLE) {
    if (!is_double_literal(value)) {
      state_ = ST_VARCHAR;
      max_varchar_len_ = value.length();
    }
  } else if (state_ == ST_DATE) {
    if (is_date_literal(value)) {
      // stays a date
    } else if (is_datetime_literal(value)) {
      state_ = ST_DATETIME;
    } else {
      state_ = ST_VARCHAR;
      max_varchar_len_ = value.length();
    }
  } else if (state_ == ST_DATETIME) {
    if (!is_datetime_literal(value)) {
      state_ = ST_VARCHAR;
      max_varchar_len_ = value.length();
    }
  } else if (state_ == ST_VARCHAR) {
    max_varchar_len_ = std::max(max_varchar_len_, value.length());
  }
  return old_state != state_;
}

void ObFileSqlUtils::ColInfer::finish(ObObjType &data_type,
                                      int32_t &max_varchar_len,
                                      ObCollationType &collation_type)
{
  switch (state_) {
    case ST_NULL:
      data_type = ObNullType;
      max_varchar_len = 0;
      break;
    case ST_BOOLEAN:
      data_type = ObTinyIntType;
      max_varchar_len = 0;
      break;
    case ST_INT:
      data_type = ObIntType;
      max_varchar_len = 0;
      break;
    case ST_DOUBLE:
      data_type = ObDoubleType;
      max_varchar_len = 0;
      break;
    case ST_DATE:
      data_type = ObDateType;
      max_varchar_len = 0;
      break;
    case ST_DATETIME:
      data_type = ObTimestampType;
      max_varchar_len = 0;
      break;
    case ST_VARCHAR:
    default:
      data_type = ObVarcharType;
      max_varchar_len = std::max(max_varchar_len_, 1);
      break;
  }
}

int ObFileSqlUtils::infer_column_schemas(const ObString &file_url,
                                         ObFileScanFormat format,
                                         const ObCSVGeneralFormat &format_opt,
                                         common::ObIAllocator &allocator,
                                         common::ObIArray<ObFileScanColMeta> &col_metas)
{
  int ret = OB_SUCCESS;
  ObString content;
  ObArenaAllocator arena(allocator);
  if (OB_FAIL(read_whole_file(file_url, arena, content))) {
  } else if (format == ObFileScanFormat::CSV) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "infer schema");
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "infer schema");
  }
  return ret;
}

int ObFileSqlUtils::compute_file_signature(const ObString &file_url,
                                           ObString &signature,
                                           int64_t &size,
                                           int64_t &mtime)
{
  int ret = OB_SUCCESS;
  struct stat stat_info;
  if (0 != stat(file_url.ptr(), &stat_info)) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_FILE_NOT_EXIST, file_url.length(), file_url.ptr());
  } else {
    size = stat_info.st_size;
    mtime = stat_info.st_mtime;
    ObSqlString sig;
    if (OB_FAIL(sig.append_fmt("%s:%ld:%ld", file_url.ptr(), size, mtime))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      signature.assign(sig.ptr(), sig.length());
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
