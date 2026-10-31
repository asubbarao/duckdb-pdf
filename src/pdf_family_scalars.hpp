// Consolidated scalar PDF operations. Included after PdfLoader and the shared helpers.
static void PdfFamilyResolveOptions(ResolveScalarFunctionTypesInput &input) {
	auto &function = input.GetBoundFunction();
	auto index = input.GetArgumentCount() - 1;
	auto &type = input.GetArgumentType(index);
	if (type.id() == LogicalTypeId::UNKNOWN)
		throw ParameterNotResolvedException();
	if (type.id() == LogicalTypeId::SQLNULL)
		return;
	if (type.id() != LogicalTypeId::STRUCT)
		throw BinderException("%s: options must be a STRUCT", function.GetName());
	auto name = function.GetName().GetIdentifierName();
	for (auto &field : StructType::GetChildTypes(type)) {
		auto key = StringUtil::Lower(field.first.GetIdentifierName());
		bool allowed =
		    (name == "pdf_text" && (key == "format" || key == "layout" || key == "password")) ||
		    (name == "pdf_render" && (key == "format" || key == "dpi")) ||
		    (name == "pdf_pages" && (key == "rotate" || key == "rotate_pages")) ||
		    (name == "pdf_stamp" &&
		     (key == "mode" || key == "text" || key == "opacity" || key == "prefix" || key == "start")) ||
		    (name == "pdf_rewrite" && (key == "mode" || key == "password" || key == "owner_password")) ||
		    (name == "ocr_image" &&
		     (key == "language" || key == "psm" || key == "oem" || key == "tessdata_dir" || key == "preprocess" ||
		      key == "vars" || key == "config" || key == "dpi" || key == "format" || key == "backend")) ||
		    (name == "image_barcodes" && key == "symbologies");
		if (!allowed)
			throw BinderException("%s: unknown option '%s'", name, key);
		bool numeric =
		    key == "dpi" || key == "rotate" || key == "opacity" || key == "start" || key == "psm" || key == "oem";
		bool boolean = key == "preprocess", map = key == "vars";
		if (field.second.id() != LogicalTypeId::SQLNULL && (numeric   ? !field.second.IsNumeric()
		                                                    : boolean ? field.second.id() != LogicalTypeId::BOOLEAN
		                                                    : map     ? field.second.id() != LogicalTypeId::MAP
		                                                              : field.second.id() != LogicalTypeId::VARCHAR)) {
			throw BinderException("%s: option '%s' must be %s", name, key,
			                      numeric   ? "numeric"
			                      : boolean ? "BOOLEAN"
			                      : map     ? "MAP"
			                                : "VARCHAR");
		}
	}
	function.GetArguments()[index] = type;
}

struct PdfFamilyOptions {
	unordered_map<string, Value> values;
	PdfFamilyOptions(const Value &options, const char *function) {
		if (options.IsNull() || options.type().id() != LogicalTypeId::STRUCT) {
			throw InvalidInputException("%s: options must be a non-NULL STRUCT", function);
		}
		auto &types = StructType::GetChildTypes(options.type());
		auto &children = StructValue::GetChildren(options);
		for (idx_t i = 0; i < types.size(); i++) {
			auto key = StringUtil::Lower(types[i].first.GetIdentifierName());
			values.emplace(key, children[i]);
		}
	}
	PdfFamilyOptions() = default;
	bool Has(const string &key) const {
		return values.find(key) != values.end();
	}
	bool HasNull() const {
		for (auto &item : values)
			if (item.second.IsNull())
				return true;
		return false;
	}
	string Text(const string &key, const string &fallback = "") const {
		auto it = values.find(key);
		return it == values.end() ? fallback : StringValue::Get(it->second.DefaultCastAs(LogicalType::VARCHAR));
	}
	int32_t Int(const string &key, int32_t fallback) const {
		auto it = values.find(key);
		return it == values.end() ? fallback : IntegerValue::Get(it->second.DefaultCastAs(LogicalType::INTEGER));
	}
	int64_t BigInt(const string &key, int64_t fallback) const {
		auto it = values.find(key);
		return it == values.end() ? fallback : BigIntValue::Get(it->second.DefaultCastAs(LogicalType::BIGINT));
	}
	double Double(const string &key, double fallback) const {
		auto it = values.find(key);
		return it == values.end() ? fallback : DoubleValue::Get(it->second.DefaultCastAs(LogicalType::DOUBLE));
	}
	void Validate(const char *function, const std::initializer_list<const char *> &allowed) const {
		for (auto &item : values) {
			bool found = false;
			for (auto key : allowed)
				if (item.first == key)
					found = true;
			if (!found)
				throw InvalidInputException("%s: unknown or inapplicable option '%s'", function, item.first);
		}
	}
};

static void PdfFamilyOcrImage(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &context = state.GetContext();
	for (idx_t row = 0; row < args.size(); row++) {
		context.BlockingInterruptCheck();
		auto image = args.GetValue(0, row);
		if (image.IsNull() || (args.ColumnCount() > 1 && args.GetValue(1, row).IsNull())) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		PdfFamilyOptions opt =
		    args.ColumnCount() > 1 ? PdfFamilyOptions(args.GetValue(1, row), "ocr_image") : PdfFamilyOptions();
		opt.Validate("ocr_image", {"language", "psm", "oem", "tessdata_dir", "preprocess", "vars", "config", "dpi",
		                           "format", "backend"});
		// Optional OCR arguments historically treat NULL as an omitted setting.
		for (auto setting = opt.values.begin(); setting != opt.values.end();) {
			if (setting->second.IsNull())
				setting = opt.values.erase(setting);
			else
				++setting;
		}
		pdf_ocr::Options o;
		o.language = opt.Text("language", "eng");
		if (o.language.empty())
			o.language = "eng";
		o.psm = opt.Int("psm", 3);
		o.oem = opt.Int("oem", 3);
		o.dpi = opt.Int("dpi", 300);
		o.tessdata_dir = opt.Text("tessdata_dir");
		o.config = opt.Text("config");
		if (opt.Has("preprocess"))
			o.preprocess = BooleanValue::Get(opt.values.at("preprocess"));
		if (opt.Has("vars"))
			ParseStringMapValueStd(opt.values.at("vars"), o.vars, "ocr_image: vars");
		if (o.psm < 0 || o.psm > 13)
			throw InvalidInputException("ocr_image: psm must be between 0 and 13");
		if (o.oem < 0 || o.oem > 3)
			throw InvalidInputException("ocr_image: oem must be between 0 and 3");
		if (o.dpi < 1 || o.dpi > 2400)
			throw InvalidInputException("ocr_image: dpi must be between 1 and 2400");
		if (!o.tessdata_dir.empty())
			PdfCheckAccess(context, o.tessdata_dir, FileType::FILE_TYPE_DIR);
		if (!o.config.empty())
			PdfCheckAccess(context, o.config);
		try {
			o.backend = pdf_ocr::BackendFromString(opt.Text("backend", "tesseract"));
			o.format = pdf_ocr::NormalizeOutputFormat(opt.Text("format", "text"));
			string_t bytes = StringValue::Get(image);
			auto recognized = pdf_ocr::RecognizeImageBlob(
			    reinterpret_cast<const unsigned char *>(bytes.GetDataUnsafe()), bytes.GetSize(), o);
			result.SetValue(row, Value(recognized.text));
		} catch (const std::exception &e) {
			RethrowOcrImageError(e);
		}
	}
}

// image_barcodes(image_blob [, {symbologies: 'EAN13,UPCE'}]) -> LIST(STRUCT)
// through Apple Vision; see vision_ocr.hpp for names, boxes and confidence.
static LogicalType ImageBarcodeType() {
	return LogicalType::STRUCT({{"symbology", LogicalType::VARCHAR},
	                            {"payload", LogicalType::VARCHAR},
	                            {"raw_payload", LogicalType::BLOB},
	                            {"supplemental_payload", LogicalType::VARCHAR},
	                            {"is_gs1", LogicalType::BOOLEAN},
	                            {"color_inverted", LogicalType::BOOLEAN},
	                            {"confidence", LogicalType::DOUBLE},
	                            {"x0", LogicalType::DOUBLE},
	                            {"y0", LogicalType::DOUBLE},
	                            {"x1", LogicalType::DOUBLE},
	                            {"y1", LogicalType::DOUBLE}});
}

static void PdfFamilyImageBarcodes(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &context = state.GetContext();
	auto barcode_type = ImageBarcodeType();
	for (idx_t row = 0; row < args.size(); row++) {
		context.BlockingInterruptCheck();
		auto image = args.GetValue(0, row);
		if (image.IsNull() || (args.ColumnCount() > 1 && args.GetValue(1, row).IsNull())) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		PdfFamilyOptions opt =
		    args.ColumnCount() > 1 ? PdfFamilyOptions(args.GetValue(1, row), "image_barcodes") : PdfFamilyOptions();
		opt.Validate("image_barcodes", {"symbologies"});
		std::vector<std::string> symbologies;
		if (opt.Has("symbologies") && !opt.values.at("symbologies").IsNull()) {
			for (auto &name : StringUtil::Split(opt.Text("symbologies"), ',')) {
				StringUtil::Trim(name);
				if (!name.empty())
					symbologies.push_back(name);
			}
		}
		string_t bytes = StringValue::Get(image);
		std::vector<pdf_ocr::Barcode> found;
		try {
			found = pdf_ocr::DetectVisionBarcodes(reinterpret_cast<const unsigned char *>(bytes.GetDataUnsafe()),
			                                      bytes.GetSize(), symbologies);
		} catch (const std::exception &e) {
			throw IOException("image_barcodes: %s", string(e.what()));
		}
		vector<Value> barcodes;
		for (auto &b : found) {
			barcodes.push_back(Value::STRUCT(
			    barcode_type,
			    {Value(b.symbology), b.has_payload ? Value(b.payload) : Value(LogicalType::VARCHAR),
			     b.has_payload_bytes ? Value::BLOB(const_data_ptr_cast(b.payload_bytes.data()), b.payload_bytes.size())
			                         : Value(LogicalType::BLOB),
			     b.has_supplemental ? Value(b.supplemental) : Value(LogicalType::VARCHAR), Value::BOOLEAN(b.gs1),
			     Value::BOOLEAN(b.color_inverted), Value::DOUBLE(b.confidence), Value::DOUBLE(b.x0),
			     Value::DOUBLE(b.y0), Value::DOUBLE(b.x1), Value::DOUBLE(b.y1)}));
		}
		result.SetValue(row, Value::LIST(barcode_type, std::move(barcodes)));
	}
}

static void PdfFamilyText(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &context = state.GetContext();
	bool blob = args.data[0].GetType().id() == LogicalTypeId::BLOB;
	for (idx_t row = 0; row < args.size(); row++) {
		context.BlockingInterruptCheck();
		auto input = args.GetValue(0, row);
		if (input.IsNull() || (args.ColumnCount() > 1 && args.GetValue(1, row).IsNull())) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		PdfFamilyOptions opt =
		    args.ColumnCount() > 1 ? PdfFamilyOptions(args.GetValue(1, row), "pdf_text") : PdfFamilyOptions();
		if (!opt.Has("format") || !opt.values.at("format").IsNull()) {
			auto f = StringUtil::Lower(opt.Text("format", "text"));
			if (f == "text")
				opt.Validate("pdf_text", {"format", "layout", "password"});
			else if (f == "html" || f == "xml" || f == "json")
				opt.Validate("pdf_text", {"format", "password"});
			else if (f == "markdown")
				opt.Validate("pdf_text", {"format"});
			else
				throw InvalidInputException("pdf_text: unknown format '%s'", f);
		}
		if (opt.HasNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		auto format = StringUtil::Lower(opt.Text("format", "text"));
		auto value = StringValue::Get(input);
		if (!blob)
			PdfCheckAccess(context, value);
		if (format == "markdown") {
			if (blob)
				throw InvalidInputException("pdf_text: markdown requires a path");
			result.SetValue(row, Value(DocToMarkdown(context, value)));
			continue;
		}
		string bytes;
		if (blob)
			bytes = value;
		else
			ReadAllBytes(context, value, bytes);
		if (format == "json") {
			try {
				result.SetValue(row, Value(pdf_qpdf::WriteJson(bytes, opt.Text("password"), 2)));
			} catch (const std::exception &e) {
				throw IOException("pdf_text: %s", string(e.what()));
			}
			continue;
		}
		auto doc = LoadDoc(bytes, opt.Text("password"), blob ? "<blob>" : value);
		string output = format == "text"   ? DocToText(*doc, opt.Text("layout", "reading"))
		                : format == "html" ? DocToHtml(*doc)
		                                   : DocToXml(*doc);
		result.SetValue(row, Value(output));
	}
}

// Preserve pdf_to_text(input[, layout]) while sharing the consolidated executor.
static void PdfLegacyText(DataChunk &args, ExpressionState &state, Vector &result) {
	if (args.ColumnCount() == 1) {
		PdfFamilyText(args, state, result);
		return;
	}
	DataChunk forwarded;
	forwarded.Initialize(Allocator::DefaultAllocator(),
	                     {args.data[0].GetType(), LogicalType::STRUCT({{"layout", LogicalType::VARCHAR}})});
	forwarded.data[0].Reference(args.data[0]);
	for (idx_t row = 0; row < args.size(); ++row)
		forwarded.SetValue(1, row, Value::STRUCT({{"layout", args.GetValue(1, row)}}));
	forwarded.SetCardinality(args.size());
	PdfFamilyText(forwarded, state, result);
}

static void PdfFamilyRender(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &context = state.GetContext();
	bool blob = args.data[0].GetType().id() == LogicalTypeId::BLOB;
	for (idx_t row = 0; row < args.size(); row++) {
		context.BlockingInterruptCheck();
		auto input = args.GetValue(0, row), page = args.GetValue(1, row);
		if (input.IsNull() || page.IsNull() || (args.ColumnCount() > 2 && args.GetValue(2, row).IsNull())) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		PdfFamilyOptions opt =
		    args.ColumnCount() > 2 ? PdfFamilyOptions(args.GetValue(2, row), "pdf_render") : PdfFamilyOptions();
		opt.Validate("pdf_render", {"format", "dpi"});
		if (opt.HasNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		auto format = StringUtil::Lower(opt.Text("format", "png"));
		if (format != "png" && format != "svg")
			throw InvalidInputException("pdf_render: unknown format '%s'", format);
		string value = StringValue::Get(input), bytes;
		if (!blob)
			PdfCheckAccess(context, value);
		if (blob)
			bytes = value;
		else
			ReadAllBytes(context, value, bytes);
		auto doc = LoadDoc(bytes, "", blob ? "<blob>" : value);
		auto page_no = IntegerValue::Get(page), dpi = opt.Int("dpi", 150);
		auto output = format == "png" ? DocToPng(*doc, page_no, dpi) : DocToSvg(*doc, page_no, dpi);
		result.SetValue(row, Value::BLOB(const_data_ptr_cast(output.data()), output.size()));
	}
}

static void PdfFamilyPages(DataChunk &args, ExpressionState &state, Vector &result) {
	PdfCheckPairArgs(state.GetContext(), args);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < args.size(); row++) {
		state.GetContext().BlockingInterruptCheck();
		auto input = args.GetValue(0, row), output = args.GetValue(1, row), ranges = args.GetValue(2, row);
		if (input.IsNull() || output.IsNull() || ranges.IsNull() ||
		    (args.ColumnCount() > 3 && args.GetValue(3, row).IsNull())) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		if (ranges.type().id() == LogicalTypeId::STRUCT) {
			PdfFamilyOptions opt(ranges, "pdf_pages");
			opt.Validate("pdf_pages", {"rotate", "rotate_pages"});
			if (opt.HasNull()) {
				FlatVector::SetNull(result, row, true);
				continue;
			}
			if (!opt.Has("rotate"))
				throw InvalidInputException("pdf_pages: rotation requires rotate");
			result.SetValue(row, Value(PdfRotateImpl(StringValue::Get(input), StringValue::Get(output),
			                                         opt.Int("rotate", 0), opt.Text("rotate_pages", "all"))));
			continue;
		}
		PdfFamilyOptions opt =
		    args.ColumnCount() > 3 ? PdfFamilyOptions(args.GetValue(3, row), "pdf_pages") : PdfFamilyOptions();
		opt.Validate("pdf_pages", {"rotate", "rotate_pages"});
		if (opt.HasNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		if (opt.Has("rotate_pages") && !opt.Has("rotate"))
			throw InvalidInputException("pdf_pages: rotate_pages requires rotate");
		string in = StringValue::Get(input), out = StringValue::Get(output), range = StringValue::Get(ranges);
		if (!opt.Has("rotate")) {
			result.SetValue(row, Value(PdfPagesImpl(in, out, range)));
			continue;
		}
		auto degrees = opt.Int("rotate", 0);
		if (degrees % 90 != 0)
			throw InvalidInputException("pdf_pages: rotate must be a multiple of 90");
		PdfOpsCheckInOut("pdf_pages", in, out);
		try {
			AtomicPdfOutput intermediate(out), staged(out);
			pdf_qpdf::Pages(in, intermediate.TempPath(), range);
			auto pages = opt.Text("rotate_pages", "all");
			pdf_qpdf::Rotate(intermediate.TempPath(), staged.TempPath(), degrees, StringUtil::Lower(pages) == "all",
			                 pages);
			staged.Commit();
		} catch (const std::exception &e) {
			throw InvalidInputException("pdf_pages: %s", string(e.what()));
		}
		result.SetValue(row, Value(out));
	}
}

static void PdfFamilyStamp(DataChunk &args, ExpressionState &state, Vector &result) {
	PdfCheckPairArgs(state.GetContext(), args);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < args.size(); row++) {
		state.GetContext().BlockingInterruptCheck();
		auto input = args.GetValue(0, row), output = args.GetValue(1, row), options = args.GetValue(2, row);
		if (input.IsNull() || output.IsNull() || options.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		PdfFamilyOptions opt(options, "pdf_stamp");
		if (opt.Has("mode") && !opt.values.at("mode").IsNull()) {
			auto m = StringUtil::Lower(opt.Text("mode"));
			if (m == "watermark")
				opt.Validate("pdf_stamp", {"mode", "text", "opacity"});
			else if (m == "bates")
				opt.Validate("pdf_stamp", {"mode", "prefix", "start"});
			else
				throw InvalidInputException("pdf_stamp: mode must be watermark or bates");
		}
		if (opt.HasNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		auto mode = StringUtil::Lower(opt.Text("mode"));
		string in = StringValue::Get(input), out = StringValue::Get(output);
		if (mode == "watermark") {
			if (!opt.Has("text"))
				throw InvalidInputException("pdf_stamp: watermark requires text");
			result.SetValue(row, Value(PdfWatermarkImpl(in, out, opt.Text("text"), opt.Double("opacity", 0.30))));
		} else if (mode == "bates") {
			if (!opt.Has("prefix") || !opt.Has("start"))
				throw InvalidInputException("pdf_stamp: bates requires prefix and start");
			result.SetValue(row, Value(PdfBatesImpl(in, out, opt.Text("prefix"), opt.BigInt("start", 0))));
		} else
			throw InvalidInputException("pdf_stamp: mode must be watermark or bates");
	}
}

static void PdfFamilyRewrite(DataChunk &args, ExpressionState &state, Vector &result) {
	PdfCheckPairArgs(state.GetContext(), args);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < args.size(); row++) {
		state.GetContext().BlockingInterruptCheck();
		auto input = args.GetValue(0, row), output = args.GetValue(1, row), options = args.GetValue(2, row);
		if (input.IsNull() || output.IsNull() || options.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		PdfFamilyOptions opt(options, "pdf_rewrite");
		if (opt.Has("mode") && !opt.values.at("mode").IsNull()) {
			auto m = StringUtil::Lower(opt.Text("mode"));
			if (m == "compress")
				opt.Validate("pdf_rewrite", {"mode"});
			else if (m == "encrypt")
				opt.Validate("pdf_rewrite", {"mode", "password", "owner_password"});
			else if (m == "decrypt" || m == "repair")
				opt.Validate("pdf_rewrite", {"mode", "password"});
			else
				throw InvalidInputException("pdf_rewrite: mode must be compress, repair, encrypt or decrypt");
		}
		if (opt.HasNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		auto mode = StringUtil::Lower(opt.Text("mode"));
		string in = StringValue::Get(input), out = StringValue::Get(output);
		if (mode == "compress") {
			result.SetValue(row, Value(PdfCompressImpl(in, out)));
		} else if (mode == "encrypt") {
			if (!opt.Has("password"))
				throw InvalidInputException("pdf_rewrite: encrypt requires password");
			result.SetValue(row, Value(PdfEncryptImpl(in, out, opt.Text("password"), opt.Text("owner_password"))));
		} else if (mode == "decrypt") {
			if (!opt.Has("password"))
				throw InvalidInputException("pdf_rewrite: decrypt requires password");
			result.SetValue(row, Value(PdfDecryptImpl(in, out, opt.Text("password"))));
		} else if (mode == "repair") {
			PdfOpsCheckInOut("pdf_rewrite", in, out);
			try {
				AtomicPdfOutput staged(out);
				pdf_qpdf::Repair(in, staged.TempPath(), opt.Text("password"));
				staged.Commit();
			} catch (const std::exception &e) {
				throw IOException("pdf_rewrite: %s", string(e.what()));
			}
			result.SetValue(row, Value(out));
		} else
			throw InvalidInputException("pdf_rewrite: mode must be compress, repair, encrypt or decrypt");
	}
}

static void RegisterPdfFamilyScalars(PdfLoader &loader) {
	ScalarFunctionSet image_ocr("ocr_image");
	image_ocr.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::VARCHAR, PdfFamilyOcrImage));
	ScalarFunction image_ocr_options({LogicalType::BLOB, LogicalType::ANY}, LogicalType::VARCHAR, PdfFamilyOcrImage);
	image_ocr_options.SetResolveTypesCallback(PdfFamilyResolveOptions);
	image_ocr.AddFunction(std::move(image_ocr_options));
	loader.RegisterFunction(std::move(image_ocr));
	ScalarFunctionSet image_barcodes("image_barcodes");
	image_barcodes.AddFunction(
	    ScalarFunction({LogicalType::BLOB}, LogicalType::LIST(ImageBarcodeType()), PdfFamilyImageBarcodes));
	ScalarFunction image_barcodes_options({LogicalType::BLOB, LogicalType::ANY}, LogicalType::LIST(ImageBarcodeType()),
	                                      PdfFamilyImageBarcodes);
	image_barcodes_options.SetResolveTypesCallback(PdfFamilyResolveOptions);
	image_barcodes.AddFunction(std::move(image_barcodes_options));
	loader.RegisterFunction(std::move(image_barcodes));
	ScalarFunctionSet text_set("pdf_text"), render_set("pdf_render");
	for (auto input : {LogicalType::VARCHAR, LogicalType::BLOB}) {
		text_set.AddFunction(ScalarFunction({input}, LogicalType::VARCHAR, PdfFamilyText));
		text_set.AddFunction(ScalarFunction({input, LogicalType::ANY}, LogicalType::VARCHAR, PdfFamilyText));
		render_set.AddFunction(ScalarFunction({input, LogicalType::INTEGER}, LogicalType::BLOB, PdfFamilyRender));
		render_set.AddFunction(
		    ScalarFunction({input, LogicalType::INTEGER, LogicalType::ANY}, LogicalType::BLOB, PdfFamilyRender));
	}
	text_set.ApplyToFunctions([](ScalarFunction &f) {
		f.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		if (f.GetSignature().GetParameterCount() == 2)
			f.SetResolveTypesCallback(PdfFamilyResolveOptions);
	});
	render_set.ApplyToFunctions([](ScalarFunction &f) {
		f.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		if (f.GetSignature().GetParameterCount() == 3)
			f.SetResolveTypesCallback(PdfFamilyResolveOptions);
	});
	loader.RegisterFunction(std::move(text_set));
	ScalarFunctionSet legacy_text("pdf_to_text");
	for (auto input : {LogicalType::VARCHAR, LogicalType::BLOB}) {
		legacy_text.AddFunction(ScalarFunction({input}, LogicalType::VARCHAR, PdfLegacyText));
		legacy_text.AddFunction(ScalarFunction({input, LogicalType::VARCHAR}, LogicalType::VARCHAR, PdfLegacyText));
	}
	legacy_text.ApplyToFunctions([](ScalarFunction &f) { f.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY); });
	loader.RegisterFunction(std::move(legacy_text));
	loader.RegisterFunction(std::move(render_set));
	ScalarFunctionSet pages("pdf_pages");
	pages.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                 LogicalType::VARCHAR, PdfFamilyPages));
	ScalarFunction rotate({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::VARCHAR,
	                      PdfFamilyPages);
	rotate.SetResolveTypesCallback(PdfFamilyResolveOptions);
	pages.AddFunction(std::move(rotate));
	pages.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
	                   LogicalType::VARCHAR, PdfFamilyPages));
	pages.ApplyToFunctions([](ScalarFunction &f) {
		f.SetVolatile();
		if (f.GetSignature().GetParameterCount() == 4)
			f.SetResolveTypesCallback(PdfFamilyResolveOptions);
	});
	loader.RegisterFunction(std::move(pages));
	ScalarFunction stamp("pdf_stamp", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
	                     LogicalType::VARCHAR, PdfFamilyStamp);
	stamp.SetVolatile();
	stamp.SetResolveTypesCallback(PdfFamilyResolveOptions);
	loader.RegisterFunction(std::move(stamp));
	ScalarFunction rewrite("pdf_rewrite", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
	                       LogicalType::VARCHAR, PdfFamilyRewrite);
	rewrite.SetVolatile();
	rewrite.SetResolveTypesCallback(PdfFamilyResolveOptions);
	loader.RegisterFunction(std::move(rewrite));
}
