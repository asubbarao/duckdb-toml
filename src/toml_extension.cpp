#define DUCKDB_EXTENSION_MAIN

#include "toml_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"

#define TOML_ENABLE_FORMATTERS 0
#include "toml.hpp"

// DuckDB's embedded copy of yyjson, shared with the core json extension
#include "yyjson.hpp"

#include <cmath>
#include <cstdlib>
#include <memory>

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

namespace {

std::string FormatDate(const toml::date &date) {
	return StringUtil::Format("%04u-%02u-%02u", date.year, date.month, date.day);
}

std::string FormatFractionalSeconds(uint32_t nanosecond) {
	auto frac = StringUtil::Format(".%09u", nanosecond);
	auto last_nonzero = frac.find_last_not_of('0');
	return frac.substr(0, last_nonzero + 1);
}

std::string FormatTime(const toml::time &time) {
	auto out = StringUtil::Format("%02u:%02u:%02u", time.hour, time.minute, time.second);
	if (time.nanosecond != 0) {
		out += FormatFractionalSeconds(time.nanosecond);
	}
	return out;
}

std::string FormatUtcOffset(int16_t minutes) {
	if (minutes == 0) {
		return "Z";
	}
	auto abs_minutes = std::abs(minutes);
	return StringUtil::Format("%s%02d:%02d", minutes < 0 ? "-" : "+", abs_minutes / 60, abs_minutes % 60);
}

std::string FormatDateTime(const toml::date_time &dt) {
	auto out = FormatDate(dt.date) + "T" + FormatTime(dt.time);
	if (dt.offset.has_value()) {
		out += FormatUtcOffset(dt.offset->minutes);
	}
	return out;
}

yyjson_mut_val *StrCopy(yyjson_mut_doc *doc, std::string_view str) {
	return yyjson_mut_strncpy(doc, str.data(), str.size());
}

yyjson_mut_val *TomlToJson(yyjson_mut_doc *doc, const toml::node &node) {
	switch (node.type()) {
	case toml::node_type::table: {
		auto obj = yyjson_mut_obj(doc);
		for (auto &&[key, value] : *node.as_table()) {
			yyjson_mut_obj_add(obj, StrCopy(doc, key.str()), TomlToJson(doc, value));
		}
		return obj;
	}
	case toml::node_type::array: {
		auto arr = yyjson_mut_arr(doc);
		for (auto &&value : *node.as_array()) {
			yyjson_mut_arr_append(arr, TomlToJson(doc, value));
		}
		return arr;
	}
	case toml::node_type::string:
		return StrCopy(doc, node.as_string()->get());
	case toml::node_type::integer:
		return yyjson_mut_sint(doc, node.as_integer()->get());
	case toml::node_type::floating_point: {
		auto value = node.as_floating_point()->get();
		// inf/-inf/nan are legal in TOML but not in JSON; emit them as strings,
		// which DuckDB still casts cleanly to DOUBLE
		if (std::isnan(value)) {
			return yyjson_mut_str(doc, "nan");
		}
		if (std::isinf(value)) {
			return yyjson_mut_str(doc, value < 0 ? "-inf" : "inf");
		}
		return yyjson_mut_real(doc, value);
	}
	case toml::node_type::boolean:
		return yyjson_mut_bool(doc, node.as_boolean()->get());
	case toml::node_type::date:
		return StrCopy(doc, FormatDate(node.as_date()->get()));
	case toml::node_type::time:
		return StrCopy(doc, FormatTime(node.as_time()->get()));
	case toml::node_type::date_time:
		return StrCopy(doc, FormatDateTime(node.as_date_time()->get()));
	default:
		throw InternalException("parse_toml: unexpected TOML node type");
	}
}

// Parses one TOML document and adds it to result's string heap as JSON; source names the file in errors
string_t TomlDocumentToJson(Vector &result, std::string_view input, const std::string &source) {
	toml::table table;
	try {
		table = toml::parse(input);
	} catch (const toml::parse_error &err) {
		auto &region = err.source();
		throw InvalidInputException("Invalid TOML%s at line %llu, column %llu: %s",
		                            source.empty() ? "" : " in " + source,
		                            static_cast<unsigned long long>(region.begin.line),
		                            static_cast<unsigned long long>(region.begin.column), std::string(err.description()));
	}
	std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> doc(yyjson_mut_doc_new(nullptr),
	                                                                    yyjson_mut_doc_free);
	yyjson_mut_doc_set_root(doc.get(), TomlToJson(doc.get(), table));
	size_t len;
	char *json = yyjson_mut_write(doc.get(), 0, &len);
	if (!json) {
		throw InternalException("toml: failed to serialize JSON");
	}
	auto out = StringVector::AddString(result, json, len);
	free(json);
	return out;
}

void ParseTomlFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
		return TomlDocumentToJson(result, std::string_view(input.GetData(), input.GetSize()), "");
	});
}

//------------------------------------------------------------------------------
// read_toml(path or glob): one row per file, (filename VARCHAR, toml JSON)
//------------------------------------------------------------------------------

struct ReadTomlBindData : public TableFunctionData {
	vector<OpenFileInfo> files;
};

struct ReadTomlGlobalState : public GlobalTableFunctionState {
	idx_t next_file = 0;
};

unique_ptr<FunctionData> ReadTomlBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<ReadTomlBindData>();
	auto &fs = FileSystem::GetFileSystem(context);
	result->files = fs.GlobFiles(StringValue::Get(input.inputs[0]), FileGlobOptions::DISALLOW_EMPTY);
	return_types = {LogicalType::VARCHAR, LogicalType::JSON()};
	names.emplace_back("filename");
	names.emplace_back("toml");
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> ReadTomlInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ReadTomlGlobalState>();
}

void ReadTomlFun(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<ReadTomlBindData>();
	auto &state = data.global_state->Cast<ReadTomlGlobalState>();
	auto &fs = FileSystem::GetFileSystem(context);
	auto filenames = FlatVector::GetDataMutable<string_t>(output.data[0]);
	auto documents = FlatVector::GetDataMutable<string_t>(output.data[1]);
	idx_t count = 0;
	while (state.next_file < bind_data.files.size() && count < STANDARD_VECTOR_SIZE) {
		auto &file = bind_data.files[state.next_file++];
		auto handle = fs.OpenFile(file, FileFlags::FILE_FLAGS_READ);
		std::string content(handle->GetFileSize(), '\0');
		handle->Read(content.data(), content.size());
		filenames[count] = StringVector::AddString(output.data[0], file.path);
		documents[count] = TomlDocumentToJson(output.data[1], content, file.path);
		count++;
	}
	output.SetChildCardinality(count);
}

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	ScalarFunction parse_toml_function("parse_toml", {LogicalType::VARCHAR}, LogicalType::JSON(), ParseTomlFun);
	// invalid TOML throws InvalidInputException; 2.0 turns a throw from a non-fallible function into an internal error
	parse_toml_function.SetFallible();
	loader.RegisterFunction(parse_toml_function);
	TableFunction read_toml_function("read_toml", {LogicalType::VARCHAR}, ReadTomlFun, ReadTomlBind, ReadTomlInit);
	loader.RegisterFunction(read_toml_function);
}

void TomlExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string TomlExtension::Name() {
	return "toml";
}

std::string TomlExtension::Version() const {
#ifdef EXT_VERSION_TOML
	return EXT_VERSION_TOML;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(toml, loader) {
	duckdb::LoadInternal(loader);
}
}
