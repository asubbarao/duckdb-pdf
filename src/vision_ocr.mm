//===--------------------------------------------------------------------===//
// vision_ocr.mm — Apple Vision implementation (macOS only, compiled with ARC).
// See vision_ocr.hpp for the contract. No duckdb, poppler or tesseract
// headers here.
//
// Nothing throws inside an @autoreleasepool or while a CGImage is held:
// RecognizeLines and DetectBarcodes report failure as a message and the
// public functions throw after releasing everything.
//===--------------------------------------------------------------------===//
#include "vision_ocr.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Vision/Vision.h>

#include <cmath>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pdf_ocr {
namespace {

std::string FromNSString(NSString *s) {
	const char *utf8 = s ? [s UTF8String] : nullptr;
	return utf8 ? std::string(utf8) : std::string();
}

// Tesseract language codes -> Vision BCP-47 identifiers.
const std::map<std::string, std::string> &TesseractToVision() {
	static const std::map<std::string, std::string> table = {
	    {"eng", "en-US"}, {"fra", "fr-FR"}, {"ita", "it-IT"}, {"deu", "de-DE"}, {"spa", "es-ES"},
	    {"por", "pt-BR"}, {"chi_sim", "zh-Hans"}, {"chi_tra", "zh-Hant"}, {"jpn", "ja-JP"}, {"kor", "ko-KR"},
	    {"rus", "ru-RU"}, {"ukr", "uk-UA"}, {"tha", "th-TH"}, {"vie", "vi-VT"}, {"ara", "ar-SA"},
	    {"tur", "tr-TR"}, {"ind", "id-ID"}, {"ces", "cs-CZ"}, {"dan", "da-DK"}, {"nld", "nl-NL"},
	    {"nor", "no-NO"}, {"nob", "nb-NO"}, {"nno", "nn-NO"}, {"msa", "ms-MY"}, {"pol", "pl-PL"},
	    {"ron", "ro-RO"}, {"swe", "sv-SE"},
	};
	return table;
}

struct Token {
	std::string text;
	CGRect box; // normalized, origin bottom-left
};

struct Line {
	std::string text;
	double confidence = 0; // 0..1
	std::vector<Token> tokens;
};

NSArray<NSString *> *SupportedLanguages(VNRecognizeTextRequest *request) API_AVAILABLE(macos(10.15)) {
	NSError *error = nil;
	NSArray<NSString *> *supported = nil;
	if (@available(macOS 12.0, *)) {
		supported = [request supportedRecognitionLanguagesAndReturnError:&error];
	} else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		supported = [VNRecognizeTextRequest supportedRecognitionLanguagesForTextRecognitionLevel:request.recognitionLevel
		                                                                                revision:request.revision
		                                                                                   error:&error];
#pragma clang diagnostic pop
	}
	return supported ? supported : @[];
}

// "eng+fra" -> @[@"en-US", @"fr-FR"]; BCP-47 names pass through. Every entry
// must be one Vision supports. '~name' (Tesseract's exclusion) is skipped.
// Returns nil and sets `error` when a language is unsupported.
NSArray<NSString *> *VisionLanguages(VNRecognizeTextRequest *request, const std::string &language, std::string &error)
    API_AVAILABLE(macos(10.15)) {
	NSArray<NSString *> *supported = SupportedLanguages(request);
	NSMutableArray<NSString *> *out = [NSMutableArray array];
	const std::string spec = language.empty() ? "eng" : language;
	size_t start = 0;
	while (start <= spec.size()) {
		size_t end = spec.find('+', start);
		if (end == std::string::npos) {
			end = spec.size();
		}
		const std::string name = spec.substr(start, end - start);
		start = end + 1;
		if (name.empty() || name[0] == '~') {
			continue;
		}
		auto mapped = TesseractToVision().find(name);
		const std::string wanted = mapped == TesseractToVision().end() ? name : mapped->second;
		NSString *candidate = [NSString stringWithUTF8String:wanted.c_str()];
		NSString *match = nil;
		for (NSString *s in supported) {
			if ([s caseInsensitiveCompare:candidate] == NSOrderedSame) {
				match = s;
				break;
			}
		}
		if (!match) {
			error = "language '" + name + "' is not supported by Vision on this system; supported: " +
			        FromNSString([supported componentsJoinedByString:@", "]);
			return nil;
		}
		[out addObject:match];
	}
	if (out.count == 0) {
		error = "no language selected in '" + language + "'";
		return nil;
	}
	return out;
}

// Vision request settings, from Options::vars (the same MAP that carries
// Tesseract variables). Unknown keys are errors: unlike Tesseract's
// SetVariable, there is no long tail of engine variables to pass through.
struct Settings {
	bool fast = false;
	bool language_correction = true;
	double minimum_text_height = 0;
	std::vector<std::string> custom_words;
};

bool ParseBool(const std::string &value, bool &out) {
	if (value == "true" || value == "1") {
		out = true;
		return true;
	}
	if (value == "false" || value == "0") {
		out = false;
		return true;
	}
	return false;
}

// Returns an error message, or an empty string when every key is valid.
std::string ParseSettings(const Options &opt, Settings &settings) {
	for (auto &kv : opt.vars) {
		const std::string &key = kv.first;
		const std::string &value = kv.second;
		if (key == "recognition_level") {
			if (value != "accurate" && value != "fast") {
				return "recognition_level must be 'accurate' or 'fast' (got '" + value + "')";
			}
			settings.fast = value == "fast";
		} else if (key == "language_correction") {
			if (!ParseBool(value, settings.language_correction)) {
				return "language_correction must be true or false (got '" + value + "')";
			}
		} else if (key == "minimum_text_height") {
			char *end = nullptr;
			settings.minimum_text_height = std::strtod(value.c_str(), &end);
			if (value.empty() || *end != '\0' || settings.minimum_text_height < 0 || settings.minimum_text_height > 1) {
				return "minimum_text_height must be a fraction of the image height from 0 to 1 (got '" + value + "')";
			}
		} else if (key == "custom_words") {
			size_t start = 0;
			while (start <= value.size()) {
				size_t end = value.find(',', start);
				if (end == std::string::npos) {
					end = value.size();
				}
				std::string word = value.substr(start, end - start);
				start = end + 1;
				const size_t first = word.find_first_not_of(' ');
				if (first != std::string::npos) {
					settings.custom_words.push_back(word.substr(first, word.find_last_not_of(' ') - first + 1));
				}
			}
		} else {
			return "unknown ocr_vars key '" + key +
			       "' (Vision accepts recognition_level, language_correction, minimum_text_height, custom_words)";
		}
	}
	return std::string();
}

// One recognition pass with the request configured from `opt`. Returns an
// error message, or an empty string on success.
std::string RecognizeLines(CGImageRef image, CGImagePropertyOrientation orientation, const Options &opt,
                           bool want_tokens, std::vector<Line> &lines) {
	if (@available(macOS 10.15, *)) {
		Settings settings;
		std::string error = ParseSettings(opt, settings);
		if (!error.empty()) {
			return error;
		}
		@autoreleasepool {
			VNRecognizeTextRequest *request = [[VNRecognizeTextRequest alloc] init];
			// The level comes first: the two levels support different languages.
			request.recognitionLevel =
			    settings.fast ? VNRequestTextRecognitionLevelFast : VNRequestTextRecognitionLevelAccurate;
			request.usesLanguageCorrection = settings.language_correction ? YES : NO;
			request.minimumTextHeight = static_cast<float>(settings.minimum_text_height);
			if (!settings.custom_words.empty()) {
				NSMutableArray<NSString *> *words = [NSMutableArray array];
				for (auto &word : settings.custom_words) {
					[words addObject:[NSString stringWithUTF8String:word.c_str()]];
				}
				request.customWords = words;
			}
			if (opt.language == "auto") {
				if (@available(macOS 13.0, *)) {
					request.automaticallyDetectsLanguage = YES;
				} else {
					return std::string("ocr_language 'auto' requires macOS 13 or later");
				}
			} else {
				NSArray<NSString *> *languages = VisionLanguages(request, opt.language, error);
				if (!languages) {
					return error;
				}
				request.recognitionLanguages = languages;
			}
			VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:image
			                                                                    orientation:orientation
			                                                                        options:@{}];
			NSError *failure = nil;
			if (![handler performRequests:@[ request ] error:&failure]) {
				return "recognition failed: " + FromNSString(failure.localizedDescription);
			}
			NSCharacterSet *space = [NSCharacterSet whitespaceAndNewlineCharacterSet];
			for (VNRecognizedTextObservation *observation in request.results) {
				VNRecognizedText *top = [[observation topCandidates:1] firstObject];
				if (!top) {
					continue;
				}
				Line line;
				line.text = FromNSString(top.string);
				line.confidence = top.confidence;
				if (want_tokens) {
					NSString *s = top.string;
					const NSUInteger length = s.length;
					NSUInteger i = 0;
					while (i < length) {
						while (i < length && [space characterIsMember:[s characterAtIndex:i]]) {
							i++;
						}
						const NSUInteger begin = i;
						while (i < length && ![space characterIsMember:[s characterAtIndex:i]]) {
							i++;
						}
						if (i == begin) {
							continue;
						}
						const NSRange range = NSMakeRange(begin, i - begin);
						VNRectangleObservation *box = [top boundingBoxForRange:range error:nil];
						line.tokens.push_back(
						    {FromNSString([s substringWithRange:range]), box ? box.boundingBox : observation.boundingBox});
					}
				}
				lines.push_back(std::move(line));
			}
		}
		return error;
	}
	return "requires macOS 10.15 or later";
}

CGImageRef CreateImage(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format) {
	const bool rgb24 = format == ImageFormat::RGB24;
	CGDataProviderRef provider = CGDataProviderCreateWithData(
	    nullptr, data, static_cast<size_t>(bytes_per_row) * static_cast<size_t>(height), nullptr);
	if (!provider) {
		return nullptr;
	}
	CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	// poppler ARGB32 is B,G,R,A in little-endian memory; rendered pages are opaque.
	const CGBitmapInfo info = rgb24 ? static_cast<CGBitmapInfo>(kCGImageAlphaNone)
	                                : static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
	CGImageRef image = CGImageCreate(static_cast<size_t>(width), static_cast<size_t>(height), 8, rgb24 ? 24 : 32,
	                                 static_cast<size_t>(bytes_per_row), space, info, provider, nullptr, false,
	                                 kCGRenderingIntentDefault);
	CGColorSpaceRelease(space);
	CGDataProviderRelease(provider);
	return image;
}

// Decodes with ImageIO and reads the EXIF orientation so photos come out upright.
CGImageRef DecodeImage(const unsigned char *data, size_t size, CGImagePropertyOrientation &orientation) {
	orientation = kCGImagePropertyOrientationUp;
	CFDataRef bytes = CFDataCreateWithBytesNoCopy(nullptr, data, static_cast<CFIndex>(size), kCFAllocatorNull);
	CGImageSourceRef source = bytes ? CGImageSourceCreateWithData(bytes, nullptr) : nullptr;
	CGImageRef image = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
	if (image) {
		CFDictionaryRef properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr);
		if (properties) {
			auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(properties, kCGImagePropertyOrientation));
			uint32_t raw = 0;
			if (value && CFNumberGetValue(value, kCFNumberSInt32Type, &raw) && raw >= 1 && raw <= 8) {
				orientation = static_cast<CGImagePropertyOrientation>(raw);
			}
			CFRelease(properties);
		}
	}
	if (source) {
		CFRelease(source);
	}
	if (bytes) {
		CFRelease(bytes);
	}
	return image;
}

int MeanConfidence(const std::vector<Line> &lines) {
	if (lines.empty()) {
		return 0;
	}
	double sum = 0;
	for (auto &line : lines) {
		sum += line.confidence;
	}
	return static_cast<int>(std::lround(100.0 * sum / lines.size()));
}

void FillText(const std::vector<Line> &lines, TextResult &out) {
	for (auto &line : lines) {
		out.text += line.text;
		out.text.push_back('\n');
	}
	out.confidence = MeanConfidence(lines);
}

// "VNBarcodeSymbologyEAN13" -> "EAN13".
NSString *ShortSymbology(NSString *symbology) {
	NSString *prefix = @"VNBarcodeSymbology";
	return [symbology hasPrefix:prefix] ? [symbology substringFromIndex:prefix.length] : symbology;
}

std::string SymbologyName(NSString *symbology) {
	return FromNSString(ShortSymbology(symbology));
}

NSArray<VNBarcodeSymbology> *SupportedSymbologies(VNDetectBarcodesRequest *request) API_AVAILABLE(macos(10.15)) {
	NSArray<VNBarcodeSymbology> *supported = nil;
	if (@available(macOS 12.0, *)) {
		NSError *error = nil;
		supported = [request supportedSymbologiesAndReturnError:&error];
	} else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		supported = [VNDetectBarcodesRequest supportedSymbologies];
#pragma clang diagnostic pop
	}
	return supported ? supported : @[];
}

// One detection pass. Returns an error message, or an empty string on success.
std::string DetectBarcodes(CGImageRef image, CGImagePropertyOrientation orientation,
                           const std::vector<std::string> &wanted, std::vector<Barcode> &barcodes) {
	if (@available(macOS 10.15, *)) {
		@autoreleasepool {
			VNDetectBarcodesRequest *request = [[VNDetectBarcodesRequest alloc] init];
			if (!wanted.empty()) {
				NSArray<VNBarcodeSymbology> *supported = SupportedSymbologies(request);
				NSMutableArray<VNBarcodeSymbology> *selected = [NSMutableArray array];
				for (auto &name : wanted) {
					NSString *candidate = [NSString stringWithUTF8String:name.c_str()];
					VNBarcodeSymbology match = nil;
					for (VNBarcodeSymbology s in supported) {
						if (candidate && [ShortSymbology(s) caseInsensitiveCompare:candidate] == NSOrderedSame) {
							match = s;
							break;
						}
					}
					if (!match) {
						std::string names;
						for (VNBarcodeSymbology s in supported) {
							names += (names.empty() ? "" : ", ") + SymbologyName(s);
						}
						return "symbology '" + name + "' is not supported by Vision on this system; supported: " + names;
					}
					[selected addObject:match];
				}
				request.symbologies = selected;
			}
			VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:image
			                                                                    orientation:orientation
			                                                                        options:@{}];
			NSError *failure = nil;
			if (![handler performRequests:@[ request ] error:&failure]) {
				return "barcode detection failed: " + FromNSString(failure.localizedDescription);
			}
			// Vision's boxes are normalized to the upright image, origin bottom-left.
			// EXIF orientations 5-8 turn the stored image a quarter turn.
			double width = static_cast<double>(CGImageGetWidth(image));
			double height = static_cast<double>(CGImageGetHeight(image));
			if (orientation >= kCGImagePropertyOrientationLeftMirrored) {
				std::swap(width, height);
			}
			for (VNBarcodeObservation *observation in request.results) {
				Barcode b;
				b.symbology = SymbologyName(observation.symbology);
				NSString *payload = observation.payloadStringValue;
				if (payload) {
					b.has_payload = true;
					b.payload = FromNSString(payload);
				}
				if (@available(macOS 14.0, *)) {
					NSData *bytes = observation.payloadData;
					if (bytes) {
						b.has_payload_bytes = true;
						b.payload_bytes.assign(static_cast<const char *>(bytes.bytes), bytes.length);
					}
					NSString *supplemental = observation.supplementalPayloadString;
					if (supplemental) {
						b.has_supplemental = true;
						b.supplemental = FromNSString(supplemental);
					}
					b.gs1 = observation.isGS1DataCarrier;
					b.color_inverted = observation.isColorInverted;
				}
				b.confidence = 100.0 * observation.confidence;
				const CGRect box = observation.boundingBox;
				b.x0 = CGRectGetMinX(box) * width;
				b.x1 = CGRectGetMaxX(box) * width;
				b.y0 = (1.0 - CGRectGetMaxY(box)) * height;
				b.y1 = (1.0 - CGRectGetMinY(box)) * height;
				barcodes.push_back(std::move(b));
			}
		}
		return std::string();
	}
	return "requires macOS 10.15 or later";
}

// Throws, unless best_effort asks for an empty result instead.
bool Failed(const std::string &error, const Options &opt) {
	if (error.empty()) {
		return false;
	}
	if (opt.best_effort) {
		return true;
	}
	throw std::runtime_error("ocr backend 'vision': " + error);
}

} // namespace

bool VisionAvailable() {
	if (@available(macOS 10.15, *)) {
		return true;
	}
	return false;
}

TextResult RecognizeVisionText(const unsigned char *data, int width, int height, int bytes_per_row,
                               ImageFormat format, const Options &opt) {
	TextResult out;
	out.backend_used = Backend::Vision;
	if (!data || width <= 0 || height <= 0) {
		return out;
	}
	std::vector<Line> lines;
	CGImageRef image = CreateImage(data, width, height, bytes_per_row, format);
	std::string error = image ? RecognizeLines(image, kCGImagePropertyOrientationUp, opt, false, lines)
	                          : "could not wrap the page bitmap as an image";
	if (image) {
		CGImageRelease(image);
	}
	if (!Failed(error, opt)) {
		FillText(lines, out);
	}
	return out;
}

WordsResult RecognizeVisionWords(const unsigned char *data, int width, int height, int bytes_per_row,
                                 ImageFormat format, const Options &opt) {
	WordsResult out;
	out.backend_used = Backend::Vision;
	if (!data || width <= 0 || height <= 0) {
		return out;
	}
	std::vector<Line> lines;
	CGImageRef image = CreateImage(data, width, height, bytes_per_row, format);
	std::string error = image ? RecognizeLines(image, kCGImagePropertyOrientationUp, opt, true, lines)
	                          : "could not wrap the page bitmap as an image";
	if (image) {
		CGImageRelease(image);
	}
	if (Failed(error, opt)) {
		return out;
	}
	// Vision's normalized boxes share PDF user space's bottom-left origin.
	const double dpi = opt.dpi > 0 ? opt.dpi : 300;
	const double page_w = width * 72.0 / dpi;
	const double page_h = height * 72.0 / dpi;
	for (auto &line : lines) {
		for (auto &token : line.tokens) {
			if (!HasGlyphs(token.text)) {
				continue;
			}
			Word w;
			w.text = token.text;
			w.x0 = CGRectGetMinX(token.box) * page_w;
			w.x1 = CGRectGetMaxX(token.box) * page_w;
			w.y0 = CGRectGetMinY(token.box) * page_h;
			w.y1 = CGRectGetMaxY(token.box) * page_h;
			w.confidence = static_cast<float>(100.0 * line.confidence);
			out.words.push_back(std::move(w));
		}
	}
	out.confidence = MeanConfidence(lines);
	return out;
}

TextResult RecognizeVisionImageBlob(const unsigned char *data, size_t size, const Options &opt) {
	TextResult out;
	out.backend_used = Backend::Vision;
	if (opt.format != "text") {
		throw std::runtime_error("ocr backend 'vision': format must be 'text' (hocr and tsv are Tesseract formats)");
	}
	CGImagePropertyOrientation orientation;
	CGImageRef image = DecodeImage(data, size, orientation);
	std::vector<Line> lines;
	std::string error = image ? RecognizeLines(image, orientation, opt, false, lines)
	                          : "could not decode image BLOB (expected a format ImageIO reads, e.g. PNG or JPEG)";
	if (image) {
		CGImageRelease(image);
	}
	if (!Failed(error, opt)) {
		FillText(lines, out);
	}
	return out;
}

std::vector<Barcode> DetectVisionBarcodes(const unsigned char *data, size_t size,
                                          const std::vector<std::string> &symbologies) {
	std::vector<Barcode> barcodes;
	CGImagePropertyOrientation orientation = kCGImagePropertyOrientationUp;
	CGImageRef image = data && size > 0 ? DecodeImage(data, size, orientation) : nullptr;
	std::string error = image ? DetectBarcodes(image, orientation, symbologies, barcodes)
	                          : "could not decode image BLOB (expected a format ImageIO reads, e.g. PNG or JPEG)";
	if (image) {
		CGImageRelease(image);
	}
	if (!error.empty()) {
		throw std::runtime_error(error);
	}
	return barcodes;
}

} // namespace pdf_ocr
