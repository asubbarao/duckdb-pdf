// Bind a public family to its selected native implementation. Selection happens
// before planning, so the existing schema, projection and streaming callbacks
// remain in charge of execution; no hidden SQL functions or nested queries.
struct PdfTableFamilyInfo : public TableFunctionInfo {
	string name;
	string selector;
	string default_mode;
	vector<std::pair<string, TableFunction>> variants;
};

static unique_ptr<FunctionData> PdfTableFamilyBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &types, vector<Identifier> &names) {
	// Replacing the bound function also replaces function_info. Retain ownership
	// of the descriptor until the delegated binder has finished using it.
	auto owner = input.table_function.function_info;
	auto &info = owner->Cast<PdfTableFamilyInfo>();
	auto parameters = input.named_parameters;
	auto inputs = input.inputs;
	auto mode = info.default_mode;
	if (!info.selector.empty()) {
		auto selected = parameters.find(Identifier(info.selector));
		if (selected != parameters.end()) {
			if (selected->second.IsNull())
				throw BinderException("%s: %s must not be NULL", info.name, info.selector);
			mode = StringUtil::Lower(StringValue::Get(selected->second));
			parameters.erase(selected);
		}
	}
	if (info.name == "pdf_render") {
		auto directory = parameters.find(Identifier("output_dir"));
		if (directory != parameters.end()) {
			if (directory->second.IsNull())
				throw BinderException("pdf_render: output_dir must not be NULL");
			mode = "write";
			inputs.push_back(directory->second);
			parameters.erase(directory);
		}
	}
	for (auto &variant : info.variants) {
		if (variant.first != mode || variant.second.GetSignature().GetParameter(0).GetType() != inputs[0].type())
			continue;
		auto function = variant.second;
		auto options = function.GetSignature().GetTypedKwargs();
		for (auto &parameter : parameters) {
			if (!options || !options->Find(parameter.first))
				throw BinderException("%s: option '%s' is not supported for %s '%s'", info.name,
				                      parameter.first.GetIdentifierName(),
				                      info.selector.empty() ? "mode" : info.selector, mode);
			if (parameter.second.IsNull())
				throw BinderException("%s: option '%s' must not be NULL", info.name,
				                      parameter.first.GetIdentifierName());
		}
		function.name = Identifier(info.name);
		input.table_function = BoundTableFunction(function);
		input.table_function.SetCallArguments(inputs, parameters);
		TableFunctionBindInput delegated(inputs, parameters, input.input_table_types, input.input_table_names,
		                                 function.function_info.get(), input.binder, input.table_function, input.ref,
		                                 input.input_plan);
		delegated.multi_file_input = input.multi_file_input;
		return function.bind(context, delegated, types, names);
	}
	throw BinderException("%s: unsupported %s '%s' for input type %s", info.name,
	                      info.selector.empty() ? "mode" : info.selector, mode, inputs[0].type().ToString());
}

struct PdfTableFamily {
	shared_ptr<PdfTableFamilyInfo> info;
	PdfTableFamily(const char *name, const char *selector, const char *default_mode)
	    : info(make_shared_ptr<PdfTableFamilyInfo>()) {
		info->name = name;
		info->selector = selector;
		info->default_mode = default_mode;
	}
	void Add(const char *mode, TableFunction function) {
		info->variants.emplace_back(mode, std::move(function));
	}
	TableFunction Function(const vector<LogicalType> &arguments) const {
		TableFunction function(Identifier(info->name), arguments, nullptr, PdfTableFamilyBind);
		function.function_info = info;
		if (!info->selector.empty())
			AddNamedParameter(function, info->selector.c_str(), LogicalType::VARCHAR);
		if (info->name == "pdf_render")
			AddNamedParameter(function, "output_dir", LogicalType::VARCHAR);
		std::set<string> added;
		for (auto &variant : info->variants) {
			auto options = variant.second.GetSignature().GetTypedKwargs();
			if (!options)
				continue;
			for (auto &name : options->GetNames()) {
				if (added.insert(name.GetIdentifierName()).second)
					AddNamedParameter(function, name.GetIdentifierName().c_str(), options->Find(name)->type);
			}
		}
		return function;
	}
};
